/*
 * doomgeneric backend for the HP Jornada 680: Linux framebuffer and evdev.
 *
 * The screen bus is the bottleneck (about 4.4 MB/s), so only pixels that
 * changed since the last frame are written, four at a time.
 *
 * Started from the Finder, the ` key or Ctrl+Tab pauses the game and goes back
 * to the Finder. The game clock stops too, so nothing happens while it is away.
 *
 * -pmode box   320x200, centred. Least data, the default.
 * -pmode tall  320x240, DOS aspect ratio, every fifth line repeated.
 * -pmode wide  640x200, every pixel doubled across.
 * -pmode fit   640x240, both of the above. Fills the screen, and is the slowest.
 */
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#include "doomgeneric.h"

#define WIDTH 320
#define HEIGHT 200
#define MAX_ROWS 240

/* Doom key codes. doomkeys.h clashes with linux/input.h. */
#define D_RIGHT 0xae
#define D_LEFT 0xac
#define D_UP 0xad
#define D_DOWN 0xaf
#define D_USE 0xa2
#define D_FIRE 0xa3
#define D_ESC 27
#define D_ENTER 13
#define D_TAB 9
#define D_BACKSPACE 0x7f
#define D_RSHIFT (0x80 + 0x36)
#define D_RALT (0x80 + 0x38)
#define D_F1 (0x80 + 0x3b)

struct color {
	uint32_t b:8;
	uint32_t g:8;
	uint32_t r:8;
	uint32_t a:8;
};

extern struct color colors[256];
extern int palette_changed;
extern uint8_t *I_VideoBuffer;
extern int screenblocks;
extern int detailLevel;

enum { BOX, TALL, WIDE, FIT };

static int mode = BOX;
static uint8_t *screen;
static size_t screen_len;
static int stride;
static int origin_x;
static int origin_y;
static uint8_t previous[WIDTH * HEIGHT];
static uint16_t lut[256];
static int repaint = 1;
static volatile sig_atomic_t woke;
static int tty_fd = -1;
static unsigned long pixels_written;

static int inputs[4];
static int input_count;
static int ctrl_held;
static int want_switch;
static uint32_t paused_ms;
static unsigned char key_down[256];

#define QUEUE 32
static uint16_t queue[QUEUE];
static unsigned queue_head;
static unsigned queue_tail;

static void push_key(int pressed, unsigned char key)
{
	unsigned next = (queue_head + 1) % QUEUE;

	if (next != queue_tail) {
		queue[queue_head] = pressed << 8 | key;
		queue_head = next;
		key_down[key] = pressed;
	}
}

static unsigned char map_key(int code)
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
		return D_F1 + (code - KEY_F1);

	switch (code) {
	/* JLime's keymap puts the Jornada's arrows on the keypad codes. */
	case KEY_UP:
	case KEY_KP8: return D_UP;
	case KEY_DOWN:
	case KEY_KP2: return D_DOWN;
	case KEY_LEFT:
	case KEY_KP4: return D_LEFT;
	case KEY_RIGHT:
	case KEY_KP6: return D_RIGHT;
	case KEY_LEFTCTRL:
	case KEY_RIGHTCTRL: return D_FIRE;
	case KEY_SPACE: return D_USE;
	case KEY_LEFTSHIFT:
	case KEY_RIGHTSHIFT: return D_RSHIFT;
	case KEY_LEFTALT:
	case KEY_RIGHTALT: return D_RALT;
	case KEY_ESC: return D_ESC;
	case KEY_ENTER: return D_ENTER;
	case KEY_TAB: return D_TAB;
	case KEY_BACKSPACE: return D_BACKSPACE;
	case KEY_MINUS: return '-';
	case KEY_EQUAL: return '=';
	case KEY_COMMA: return ',';
	case KEY_DOT: return '.';
	default: return 0;
	}
}

static void poll_inputs(void)
{
	struct input_event ev;
	int i;

	for (i = 0; i < input_count; i++) {
		while (read(inputs[i], &ev, sizeof ev) == sizeof ev) {
			unsigned char key;

			if (ev.type != EV_KEY || ev.value == 2)
				continue;
			if (ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL)
				ctrl_held = ev.value != 0;
			if (getenv("POCKET_SWITCHER") && ev.value &&
			    (ev.code == 123 || (ev.code == KEY_TAB && ctrl_held))) {
				want_switch = 1;
				continue;
			}
			/* A stylus tap fires, for when the Ctrl key is awkward. */
			key = ev.code == BTN_TOUCH ? D_FIRE : map_key(ev.code);
			if (key)
				push_key(ev.value != 0, key);
			else if (ev.value)
				/* Lands in play.log, to identify any key still unmapped. */
				fprintf(stderr, "unmapped key %u\n", ev.code);
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

static void restore_console(void)
{
	/* The file screen keeps its last frame so a PC test can inspect it. */
	if (tty_fd >= 0) {
		memset(screen, 0, screen_len);
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
	screen_len = (size_t)stride * var.yres;
	screen = mmap(NULL, screen_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (screen == MAP_FAILED) {
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
	screen_len = (size_t)stride * MAX_ROWS;
	if (fd < 0 || ftruncate(fd, screen_len)) {
		perror(path);
		exit(1);
	}
	screen = mmap(NULL, screen_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (screen == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
}

uint32_t DG_GetTicksMs(void);

static void close_inputs(void)
{
	while (input_count > 0)
		close(inputs[--input_count]);
}

/* Nothing is held once we return, so let go of every key the game thinks is down. */
static void release_keys(void)
{
	int i;

	for (i = 0; i < 256; i++) {
		if (key_down[i])
			push_key(0, i);
	}
	ctrl_held = 0;
}

static void switch_to_finder(void)
{
	uint32_t left;

	want_switch = 0;
	release_keys();
	close_inputs();
	if (tty_fd >= 0)
		ioctl(tty_fd, KDSETMODE, KD_TEXT);
	left = DG_GetTicksMs();
	raise(SIGSTOP);
	paused_ms += DG_GetTicksMs() - left;
	if (tty_fd >= 0)
		ioctl(tty_fd, KDSETMODE, KD_GRAPHICS);
	memset(screen, 0, screen_len);
	repaint = 1;
	open_inputs();
}

void DG_Init(void)
{
	const char *file = getenv("POCKET_FB_FILE");
	int rows;

	if (file)
		open_file_screen(file);
	else
		open_real_screen();

	rows = mode == TALL || mode == FIT ? MAX_ROWS : HEIGHT;
	origin_x = mode == WIDE || mode == FIT ? 0 : (640 - WIDTH) / 2;
	origin_y = (MAX_ROWS - rows) / 2;
	memset(screen, 0, screen_len);
	atexit(restore_console);
	if (!file)
		open_inputs();
}

static void update_lut(void)
{
	int i;

	for (i = 0; i < 256; i++)
		lut[i] = (colors[i].r & 0xf8) << 8 | (colors[i].g & 0xfc) << 3 | colors[i].b >> 3;
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
		pixels_written += 4;
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

uint32_t DG_GetTicksMs(void);

static uint64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
}

static uint64_t spent_blit;
static uint64_t spent_input;

/* POCKET_BENCH=n measures n frames, after skipping level load, then quits. */
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
	if (++frames == 10) {
		start = DG_GetTicksMs();
		spent_blit = spent_input = 0;
		pixels_written = 0;
	}
	if (frames == 10 + limit) {
		uint32_t ms = DG_GetTicksMs() - start;

		fprintf(stderr, "bench: %d frames in %u ms = %u.%u fps\n", limit, ms,
			limit * 1000 / ms, limit * 10000 / ms % 10);
		fprintf(stderr, "bench: per frame us: blit %lu input %lu, other %lu\n",
			(unsigned long)(spent_blit / limit), (unsigned long)(spent_input / limit),
			(unsigned long)((ms * 1000ull - spent_blit - spent_input) / limit));
		fprintf(stderr, "bench: pixels written per frame %lu, screenblocks %d detail %d\n",
			pixels_written / limit, screenblocks, detailLevel);
		exit(0);
	}
}

void DG_DrawFrame(void)
{
	const uint8_t *src = I_VideoBuffer;
	uint8_t *old = previous;
	uint8_t *dst = screen + origin_y * stride + origin_x * 2;
	uint64_t t0 = now_us(), t1, t2;
	int y;

	if (palette_changed) {
		update_lut();
		palette_changed = 0;
		repaint = 1;
	}
	if (woke) {
		woke = 0;
		memset(screen, 0, screen_len);
		repaint = 1;
	}
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
	t1 = now_us();
	poll_inputs();
	t2 = now_us();
	spent_blit += t1 - t0;
	spent_input += t2 - t1;
	bench_frame();
	if (want_switch)
		switch_to_finder();
}

void DG_SleepMs(uint32_t ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

uint32_t DG_GetTicksMs(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000u + ts.tv_nsec / 1000000u - paused_ms;
}

int DG_GetKey(int *pressed, unsigned char *key)
{
	uint16_t entry;

	if (queue_tail == queue_head)
		poll_inputs();
	if (queue_tail == queue_head)
		return 0;
	entry = queue[queue_tail];
	queue_tail = (queue_tail + 1) % QUEUE;
	*pressed = entry >> 8;
	*key = entry & 0xff;
	return 1;
}

void DG_SetWindowTitle(const char *title)
{
	(void)title;
}

#ifdef __SH3__
extern const char __executable_start[];
extern const char etext[];

/*
 * The kernel can kill us over a bad address without saying which, so this logs
 * every register, the code at the crash, and whether the code in memory still
 * matches the file. A mismatch means the card or RAM handed back bad data.
 */
static void log_crash_detail(const ucontext_t *uc)
{
	const unsigned long *r = uc->uc_mcontext.gregs;
	const unsigned short *code = (const unsigned short *)(uc->uc_mcontext.pc & ~1ul);
	const char *at = __executable_start;
	char page[4096];
	char line[200];
	int bad = 0;
	int fd;
	int i;

	for (i = 0; i < 16; i += 4) {
		int n = snprintf(line, sizeof line, "r%d %08lx %08lx %08lx %08lx\n", i, r[i],
				 r[i + 1], r[i + 2], r[i + 3]);

		write(2, line, n);
	}
	write(2, line, snprintf(line, sizeof line, "code %04x %04x %04x %04x\n", code[-1], code[0],
				code[1], code[2]));
	fd = open("/proc/self/exe", O_RDONLY);
	if (fd < 0) {
		write(2, "no code check\n", 14);
		return;
	}
	for (; at < etext; at += sizeof page) {
		if (read(fd, page, sizeof page) != sizeof page)
			break;
		if (memcmp(page, at, sizeof page))
			bad++;
	}
	close(fd);
	write(2, line, snprintf(line, sizeof line, "code check: %d bad pages\n", bad));
}
#endif

/* Records where a crash happened, then lets the shell report it as usual. */
static void on_crash(int sig, siginfo_t *info, void *context)
{
	char line[160];
	int n = snprintf(line, sizeof line, "crash: signal %d code %d address %p", sig,
			 info->si_code, info->si_addr);
#ifdef __SH3__
	ucontext_t *uc = context;

	n += snprintf(line + n, sizeof line - n, " pc %08lx pr %08lx sp %08lx",
		      (unsigned long)uc->uc_mcontext.pc, (unsigned long)uc->uc_mcontext.pr,
		      (unsigned long)uc->uc_mcontext.gregs[15]);
#else
	(void)context;
#endif
	line[n++] = '\n';
	write(2, line, n);
#ifdef __SH3__
	log_crash_detail(uc);
#endif
	signal(sig, SIG_DFL);
	raise(sig);
}

static void catch_crashes(void)
{
	static const int signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE };
	struct sigaction sa;
	unsigned i;

	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = on_crash;
	sa.sa_flags = SA_SIGINFO;
	for (i = 0; i < sizeof signals / sizeof signals[0]; i++)
		sigaction(signals[i], &sa, NULL);
}

/* Sent by the desktop after a sleep, which can lose the picture. */
static void on_wake(int sig)
{
	(void)sig;
	woke = 1;
}

int main(int argc, char **argv)
{
	char **args = calloc(argc + 3, sizeof *args);
	int i, n = 0;

	catch_crashes();
	signal(SIGWINCH, on_wake);
	for (i = 0; i < argc; i++) {
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
		/* doomgeneric never reads its config file, so settings arrive here. */
		if (!strcmp(argv[i], "-pblocks") && i + 1 < argc) {
			screenblocks = atoi(argv[++i]);
			continue;
		}
		if (!strcmp(argv[i], "-pdetail") && i + 1 < argc) {
			detailLevel = atoi(argv[++i]);
			continue;
		}
		args[n++] = argv[i];
	}
	/* Scaling 0 skips doomgeneric's extra copy; DG_DrawFrame reads Doom's buffer. */
	args[n++] = "-scaling";
	args[n++] = "0";

	doomgeneric_Create(n, args);
	for (;;)
		doomgeneric_Tick();
	return 0;
}
