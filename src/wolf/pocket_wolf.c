/*
 * Wolfenstein 3D on the Jornada 680: the screen, keys and clock behind the SDL
 * calls in shim/SDL.h. Shares Doom's sizes, keys and app switching.
 *
 * PC tests set POCKET_FB_FILE for the screen, POCKET_KEYS for a script of
 * "ms key 1|0" lines, and POCKET_BENCH to count frames.
 */
#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "SDL.h"
#undef main

#define WIDTH 320
#define HEIGHT 200
#define MAX_ROWS 240

enum { BOX, TALL, WIDE, FIT };

static int mode = BOX;
static uint8_t *fb;
static size_t fb_len;
static int stride;
static int origin_x;
static int origin_y;
static uint8_t previous[WIDTH * HEIGHT];
static uint16_t lut[256];
static SDL_Color colors[256];
static int repaint = 1;
static volatile sig_atomic_t woke;
static int tty_fd = -1;

static SDL_Surface *screen_surface;	/* what Wolf calls the screen */
static SDL_Surface *game_surface;	/* where Wolf draws */
static const SDL_Surface *pending;	/* blitted but not yet shown */

static int inputs[4];
static int input_count;
static int ctrl_held;
static int shift_held;
static int want_switch;
static Uint32 start_ms;
static uint32_t paused_ms;
static unsigned char key_down[256];

#define QUEUE 32
static uint16_t queue[QUEUE];
static unsigned queue_head;
static unsigned queue_tail;

static void push_key(int pressed, int key)
{
	unsigned next = (queue_head + 1) % QUEUE;

	if (next != queue_tail) {
		queue[queue_head] = pressed << 8 | key;
		queue_head = next;
		key_down[key] = pressed;
	}
}

static int map_key(int code)
{
	static const char row1[] = "1234567890";
	static const char row2[] = "qwertyuiop";
	static const char row3[] = "asdfghjkl";
	static const char row4[] = "zxcvbnm";

	if (code >= KEY_1 && code <= KEY_0)
		return row1[code - KEY_1];
	if (code >= KEY_Q && code <= KEY_P)
		return row2[code - KEY_Q];
	if (code >= KEY_A && code <= KEY_L)
		return row3[code - KEY_A];
	if (code >= KEY_Z && code <= KEY_M)
		return row4[code - KEY_Z];
	if (code >= KEY_F1 && code <= KEY_F10)
		return SDLK_F1 + (code - KEY_F1);

	switch (code) {
	/* JLime's keymap puts the Jornada's arrows on the keypad codes. */
	case KEY_UP:
	case KEY_KP8: return SDLK_UP;
	case KEY_DOWN:
	case KEY_KP2: return SDLK_DOWN;
	case KEY_LEFT:
	case KEY_KP4: return SDLK_LEFT;
	case KEY_RIGHT:
	case KEY_KP6: return SDLK_RIGHT;
	case KEY_LEFTCTRL:
	case KEY_RIGHTCTRL: return SDLK_LCTRL;
	case KEY_LEFTSHIFT:
	case KEY_RIGHTSHIFT: return SDLK_LSHIFT;
	case KEY_LEFTALT:
	case KEY_RIGHTALT: return SDLK_LALT;
	case KEY_SPACE: return SDLK_SPACE;
	case KEY_ESC: return SDLK_ESCAPE;
	case KEY_ENTER: return SDLK_RETURN;
	case KEY_TAB: return SDLK_TAB;
	case KEY_BACKSPACE: return SDLK_BACKSPACE;
	case KEY_MINUS: return SDLK_MINUS;
	case KEY_EQUAL: return SDLK_EQUALS;
	case KEY_COMMA: return SDLK_COMMA;
	case KEY_DOT: return SDLK_PERIOD;
	default: return 0;
	}
}

/* Plays out a key script, each line waiting for its time. */
static void poll_script(void)
{
	static FILE *script;
	static int opened;
	static int due = -1, key, down;

	if (!opened) {
		const char *name = getenv("POCKET_KEYS");

		opened = 1;
		script = name ? fopen(name, "r") : NULL;
	}
	if (!script)
		return;
	for (;;) {
		if (due < 0 && fscanf(script, "%d %d %d", &due, &key, &down) != 3) {
			fclose(script);
			script = NULL;
			return;
		}
		if ((Uint32)due > SDL_GetTicks() - start_ms)
			return;
		push_key(down, key);
		due = -1;
	}
}

static void poll_inputs(void)
{
	struct input_event ev;
	int i;

	poll_script();
	for (i = 0; i < input_count; i++) {
		while (read(inputs[i], &ev, sizeof ev) == sizeof ev) {
			int key;

			if (ev.type != EV_KEY || ev.value == 2)
				continue;
			if (ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL)
				ctrl_held = ev.value != 0;
			if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT)
				shift_held = ev.value != 0;
			if (getenv("POCKET_SWITCHER") && ev.value &&
			    (ev.code == 123 || (ev.code == KEY_TAB && ctrl_held))) {
				want_switch = 1;
				continue;
			}
			/* A stylus tap fires, for when the Ctrl key is awkward. */
			key = ev.code == BTN_TOUCH ? SDLK_LCTRL : map_key(ev.code);
			if (key)
				push_key(ev.value != 0, key);
		}
	}
}

static void open_inputs(void)
{
	char path[280];
	struct dirent *e;
	DIR *d = opendir("/dev/input");

	if (!d)
		return;
	while ((e = readdir(d)) && input_count < 4) {
		int fd;

		if (strncmp(e->d_name, "event", 5))
			continue;
		snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		/* Stops key presses reaching the login prompt underneath. */
		ioctl(fd, EVIOCGRAB, 1);
		inputs[input_count++] = fd;
	}
	closedir(d);
}

static void close_inputs(void)
{
	while (input_count > 0)
		close(inputs[--input_count]);
}

static void restore_console(void)
{
	/* The file screen keeps its last frame so a PC test can inspect it. */
	if (tty_fd >= 0) {
		memset(fb, 0, fb_len);
		ioctl(tty_fd, KDSETMODE, KD_TEXT);
	}
}

static void open_real_screen(void)
{
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	int fd = open("/dev/fb0", O_RDWR);

	if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &var) ||
	    ioctl(fd, FBIOGET_FSCREENINFO, &fix)) {
		perror("/dev/fb0");
		exit(1);
	}
	if (var.bits_per_pixel != 16) {
		fprintf(stderr, "need a 16 bpp screen, found %u\n", var.bits_per_pixel);
		exit(1);
	}
	stride = fix.line_length;
	fb_len = (size_t)stride * var.yres;
	fb = mmap(NULL, fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
	tty_fd = open("/dev/tty0", O_RDWR);
	if (tty_fd >= 0)
		ioctl(tty_fd, KDSETMODE, KD_GRAPHICS);
}

/* Lets the PC test the picture under qemu, with a file standing in for the screen. */
static void open_file_screen(const char *path)
{
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);

	stride = 1280;
	fb_len = (size_t)stride * MAX_ROWS;
	if (fd < 0 || ftruncate(fd, fb_len)) {
		perror(path);
		exit(1);
	}
	fb = mmap(NULL, fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
}

static void open_screen(void)
{
	const char *file = getenv("POCKET_FB_FILE");
	int rows = mode == TALL || mode == FIT ? MAX_ROWS : HEIGHT;

	if (file)
		open_file_screen(file);
	else
		open_real_screen();
	origin_x = mode == WIDE || mode == FIT ? 0 : (640 - WIDTH) / 2;
	origin_y = (MAX_ROWS - rows) / 2;
	memset(fb, 0, fb_len);
	atexit(restore_console);
	if (!file)
		open_inputs();
}

/* Writes one source row, skipping groups of four pixels that have not changed. */
static void draw_row(const uint8_t *src, uint8_t *old, uint8_t *dst, int force, int keep)
{
	const uint32_t *a = (const uint32_t *)src;
	uint32_t *b = (uint32_t *)old;
	volatile uint16_t *out = (volatile uint16_t *)dst;
	int doubled = mode == WIDE || mode == FIT;
	int x;

	for (x = 0; x < WIDTH / 4; x++, src += 4, out += doubled ? 8 : 4) {
		uint16_t c0, c1, c2, c3;

		if (!force && a[x] == b[x])
			continue;
		c0 = lut[src[0]];
		c1 = lut[src[1]];
		c2 = lut[src[2]];
		c3 = lut[src[3]];
		if (keep)
			b[x] = a[x];
		if (doubled) {
			out[0] = c0; out[1] = c0;
			out[2] = c1; out[3] = c1;
			out[4] = c2; out[5] = c2;
			out[6] = c3; out[7] = c3;
		} else {
			out[0] = c0;
			out[1] = c1;
			out[2] = c2;
			out[3] = c3;
		}
	}
}

static void draw_frame(const uint8_t *src)
{
	uint8_t *old = previous;
	uint8_t *dst = fb + origin_y * stride + origin_x * 2;
	int y;

	for (y = 0; y < HEIGHT; y++, src += WIDTH, old += WIDTH) {
		int twice = (mode == TALL || mode == FIT) && y % 5 == 4;

		/* A repeated row must see the same differences, so keep it for last. */
		draw_row(src, old, dst, repaint, !twice);
		dst += stride;
		if (twice) {
			draw_row(src, old, dst, repaint, 1);
			dst += stride;
		}
	}
	repaint = 0;
}

static void switch_to_finder(void)
{
	uint32_t left;
	int i;

	want_switch = 0;
	/* Nothing is held once we return, so let go of every key the game thinks is down. */
	for (i = 0; i < 256; i++) {
		if (key_down[i])
			push_key(0, i);
	}
	ctrl_held = shift_held = 0;
	close_inputs();
	if (tty_fd >= 0)
		ioctl(tty_fd, KDSETMODE, KD_TEXT);
	left = SDL_GetTicks();
	raise(SIGSTOP);
	paused_ms += SDL_GetTicks() - left;
	if (tty_fd >= 0)
		ioctl(tty_fd, KDSETMODE, KD_GRAPHICS);
	memset(fb, 0, fb_len);
	repaint = 1;
	draw_frame(previous);
	open_inputs();
}

/* POCKET_BENCH=n measures n frames, once frames start coming quickly, then quits. */
static void bench_frame(void)
{
	static int limit = -1;
	static int frames;
	static uint32_t start;

	if (limit < 0) {
		const char *s = getenv("POCKET_BENCH");

		limit = s ? atoi(s) : 0;
	}
	if (!limit)
		return;
	if (++frames == 30)
		start = SDL_GetTicks();
	if (frames == 30 + limit) {
		uint32_t ms = SDL_GetTicks() - start;

		fprintf(stderr, "bench: %d frames in %u ms = %u.%u fps\n", limit, ms,
			limit * 1000 / ms, limit * 10000 / ms % 10);
		exit(0);
	}
}

void pocket_quit(void)
{
}

int pocket_must_lock(SDL_Surface *s)
{
	return s == screen_surface;
}

/* Fading writes on the screen directly, so it must hold what is showing. */
int pocket_lock(SDL_Surface *s)
{
	if (pending) {
		memcpy(s->pixels, previous, sizeof previous);
		pending = NULL;
	}
	return 0;
}

SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int w, int h, int bits, Uint32 r, Uint32 g,
				  Uint32 b, Uint32 a)
{
	SDL_Surface *s = calloc(1, sizeof *s);

	(void)flags; (void)g; (void)b; (void)a;
	if (!s)
		return NULL;
	s->format = calloc(1, sizeof *s->format);
	s->pixels = calloc(h, w);
	if (!s->format || !s->pixels)
		return NULL;
	s->format->BytesPerPixel = bits / 8;
	s->w = w;
	s->h = h;
	s->pitch = w;
	/* id_vl.cpp makes the screen first and the drawing buffer second. */
	if (!screen_surface) {
		screen_surface = s;
		open_screen();
	} else if (!game_surface) {
		game_surface = s;
	}
	(void)r;
	return s;
}

void SDL_FreeSurface(SDL_Surface *s)
{
	if (s) {
		free(s->pixels);
		free(s->format->palette);
		free(s->format);
		free(s);
	}
}

int SDL_FillRect(SDL_Surface *s, const SDL_Rect *rect, Uint32 color)
{
	if (!rect) {
		memset(s->pixels, color, (size_t)s->pitch * s->h);
	} else {
		int y;

		for (y = 0; y < rect->h; y++)
			memset((uint8_t *)s->pixels + (rect->y + y) * s->pitch + rect->x, color,
			       rect->w);
	}
	return 0;
}

int SDL_BlitSurface(SDL_Surface *src, const SDL_Rect *from, SDL_Surface *dst, SDL_Rect *to)
{
	SDL_Rect a = { 0, 0, src->w, src->h };
	int x = to ? to->x : 0;
	int y = to ? to->y : 0;
	int row;

	/* The frame goes straight from Wolf's buffer to the screen, with no copy here. */
	if (dst == screen_surface && !from && !to && src->w == WIDTH && src->h == HEIGHT) {
		pending = src;
		return 0;
	}
	if (from)
		a = *from;
	if (a.w > dst->w - x)
		a.w = dst->w - x;
	if (a.h > dst->h - y)
		a.h = dst->h - y;
	for (row = 0; row < a.h; row++)
		memcpy((uint8_t *)dst->pixels + (y + row) * dst->pitch + x,
		       (const uint8_t *)src->pixels + (a.y + row) * src->pitch + a.x, a.w);
	return 0;
}

int SDL_SetPaletteColors(SDL_Palette *p, const SDL_Color *list, int first, int count)
{
	memcpy(p->colors + first, list, count * sizeof *list);
	return 0;
}

int SDL_SetSurfacePalette(SDL_Surface *s, SDL_Palette *p)
{
	int i;

	/* Pictures are drawn in the game palette, only the game buffer sets the screen's. */
	if (s != game_surface)
		return 0;
	memcpy(colors, p->colors, sizeof colors);
	for (i = 0; i < 256; i++)
		lut[i] = (colors[i].r & 0xf8) << 8 | (colors[i].g & 0xfc) << 3 | colors[i].b >> 3;
	repaint = 1;
	return 0;
}

/* Only fades use this, on colors taken from the palette. */
Uint32 SDL_MapRGB(const SDL_PixelFormat *f, Uint8 r, Uint8 g, Uint8 b)
{
	static int last;
	int i;

	(void)f;
	if (colors[last].r == r && colors[last].g == g && colors[last].b == b)
		return last;
	for (i = 0; i < 256; i++) {
		if (colors[i].r == r && colors[i].g == g && colors[i].b == b) {
			last = i;
			return i;
		}
	}
	return 0;
}

void SDL_RenderPresent(SDL_Renderer *r)
{
	(void)r;
	if (woke) {
		woke = 0;
		memset(fb, 0, fb_len);
		repaint = 1;
	}
	draw_frame(pending ? pending->pixels : screen_surface->pixels);
	bench_frame();
}

int SDL_PollEvent(SDL_Event *e)
{
	uint16_t entry;

	if (woke) {
		woke = 0;
		memset(fb, 0, fb_len);
		repaint = 1;
		draw_frame(previous);
	}
	if (queue_tail == queue_head)
		poll_inputs();
	if (want_switch)
		switch_to_finder();
	if (queue_tail == queue_head)
		return 0;
	entry = queue[queue_tail];
	queue_tail = (queue_tail + 1) % QUEUE;
	memset(e, 0, sizeof *e);
	e->type = entry >> 8 ? SDL_KEYDOWN : SDL_KEYUP;
	e->key.keysym.sym = entry & 0xff;
	return 1;
}

int SDL_WaitEvent(SDL_Event *e)
{
	while (!SDL_PollEvent(e))
		SDL_Delay(10);
	return 1;
}

SDL_Keymod SDL_GetModState(void)
{
	return KMOD_NUM | (shift_held ? KMOD_SHIFT : 0);
}

Uint32 SDL_GetTicks(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000u + ts.tv_nsec / 1000000u - paused_ms;
}

void SDL_Delay(Uint32 ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

/* Sent by the desktop after a sleep, which can lose the picture. */
static void on_wake(int sig)
{
	(void)sig;
	woke = 1;
}

int main(int argc, char **argv)
{
	char **args = calloc(argc + 8, sizeof *args);
	int i, n = 0;

	signal(SIGWINCH, on_wake);
	start_ms = SDL_GetTicks();
	args[n++] = argv[0];
	/* Wolf's own scale would mean more pixels than this bus can carry. */
	args[n++] = "--res";
	args[n++] = "320";
	args[n++] = "200";
	args[n++] = "--configdir";
	args[n++] = ".";
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-pmode") && i + 1 < argc) {
			i++;
			if (!strcmp(argv[i], "tall"))
				mode = TALL;
			else if (!strcmp(argv[i], "wide"))
				mode = WIDE;
			else if (!strcmp(argv[i], "fit"))
				mode = FIT;
			continue;
		}
		args[n++] = argv[i];
	}
	return SDL_main(n, args);
}
