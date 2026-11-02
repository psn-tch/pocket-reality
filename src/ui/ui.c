/*
 * Screen, font and input for the real Jornada, or for PC tests.
 *
 * Programs draw into a RAM copy of the screen. ui_present() then writes only
 * the pixels that changed, because the screen bus manages about 4 MB/s.
 *
 * PC tests set POCKET_FB_FILE for the screen and POCKET_KEYS for the keyboard.
 */
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#include "font_data.h"
#include "ui.h"

static uint16_t back[SCREEN_W * SCREEN_H];
static uint16_t shown[SCREEN_W * SCREEN_H];
static volatile uint16_t *screen;
static size_t screen_len;
static int stride_px = SCREEN_W;
static int first_present = 1;
static int tty_fd = -1;
static int inputs[4];
static int input_count;
static int held_mods;
static FILE *script;

/*
 * One press of the stylus reaches us as a burst of short presses, so a tap is
 * only finished once the stylus has stayed up for TAP_SETTLE_MS. Its position
 * is the median of the burst, because single readings can be far off.
 */
#define TAP_SETTLE_MS 150
#define TAP_SAMPLES 64
#define BATTERY_EVERY_MS 60000
#define BRIGHTNESS_STEPS 10
#define BRIGHTNESS_USEFUL 161
#define CONTRAST_TOP 15
#define CONTRAST_WAIT 10
#define SCREEN_BOX_Y 60
#define SCREEN_BOX_H 104
#define GPIO_PAGE 0x10004000	/* The HD64461's GPIO, by physical address. */
#define GPBCR 1			/* 16-bit registers within that page. */
#define GPBDR 9

static int sample_x[TAP_SAMPLES];
static int sample_y[TAP_SAMPLES];
static int x_count;
static int y_count;
static int touch_held;
static long long touch_up_ms = -1;
static int vt_fd = -1;
static int sleep_log = -1;
static volatile sig_atomic_t wake_target;
static volatile sig_atomic_t woke;
static long long cal[7];	/* x = (a*rx + b*ry + c) / g, y = (d*rx + e*ry + f) / g */
static int have_cal;
static int battery_now = -1;
static int battery_drawn = -1;
static long long battery_due;
static volatile uint16_t *lcd_gpio;

static void catch_signal(int sig, void (*handler)(int));

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000ll + ts.tv_nsec / 1000000;
}

/* Whatever is in front gets the news, even if that is a program this one started. */
static void on_wake(int sig)
{
	(void)sig;
	if (wake_target > 0)
		kill(wake_target, SIGWINCH);
	else
		woke = 1;
}

static int open_screen(void)
{
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	const char *file = getenv("POCKET_FB_FILE");
	int fd;

	if (file) {
		screen_len = sizeof back;
		fd = open(file, O_RDWR | O_CREAT | O_TRUNC, 0644);
		if (fd < 0 || ftruncate(fd, screen_len))
			return -1;
	} else {
		fd = open("/dev/fb0", O_RDWR);
		if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &var) ||
		    ioctl(fd, FBIOGET_FSCREENINFO, &fix))
			return -1;
		if (var.bits_per_pixel != 16) {
			fprintf(stderr, "need a 16 bpp screen, found %u\n", var.bits_per_pixel);
			return -1;
		}
		stride_px = fix.line_length / 2;
		screen_len = (size_t)fix.line_length * SCREEN_H;
		tty_fd = open("/dev/tty0", O_RDWR);
		if (tty_fd >= 0)
			ioctl(tty_fd, KDSETMODE, KD_GRAPHICS);
	}
	screen = mmap(NULL, screen_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	return screen == MAP_FAILED ? -1 : 0;
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
		ioctl(fd, EVIOCGRAB, 1);
		inputs[input_count++] = fd;
	}
	closedir(d);
}

const char *ui_root(void)
{
	const char *root = getenv("POCKET_ROOT");

	return root ? root : "/media/pocket";
}

static void load_calibration(void)
{
	char path[300];
	FILE *f;

	snprintf(path, sizeof path, "%s/system/touch.cal", ui_root());
	f = fopen(path, "r");
	if (!f)
		return;
	have_cal = fscanf(f, "%lld %lld %lld %lld %lld %lld %lld", &cal[0], &cal[1], &cal[2],
			  &cal[3], &cal[4], &cal[5], &cal[6]) == 7 && cal[6] != 0;
	fclose(f);
}

int ui_has_calibration(void)
{
	return have_cal;
}

void ui_set_calibration(const int s[6], const int r[6])
{
	long long g = (long long)(r[0] - r[4]) * (r[3] - r[5]) -
		      (long long)(r[2] - r[4]) * (r[1] - r[5]);
	char path[300];
	FILE *f;
	int axis;

	if (!g)
		return;
	for (axis = 0; axis < 2; axis++) {
		long long p0 = s[axis];
		long long p1 = s[2 + axis];
		long long p2 = s[4 + axis];
		long long a = (p0 - p2) * (r[3] - r[5]) - (p1 - p2) * (r[1] - r[5]);
		long long b = (r[0] - r[4]) * (p1 - p2) - (r[2] - r[4]) * (p0 - p2);

		cal[axis * 3] = a;
		cal[axis * 3 + 1] = b;
		cal[axis * 3 + 2] = p0 * g - a * r[0] - b * r[1];
	}
	cal[6] = g;
	have_cal = 1;
	snprintf(path, sizeof path, "%s/system", ui_root());
	mkdir(path, 0755);
	snprintf(path, sizeof path, "%s/system/touch.cal", ui_root());
	f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "%lld %lld %lld %lld %lld %lld %lld\n", cal[0], cal[1], cal[2], cal[3], cal[4],
		cal[5], cal[6]);
	fflush(f);
	fsync(fileno(f));
	fclose(f);
}

int ui_open(void)
{
	const char *keys = getenv("POCKET_KEYS");

	if (open_screen())
		return -1;
	catch_signal(SIGWINCH, on_wake);
	load_calibration();
	if (keys)
		script = fopen(keys, "r");
	else
		open_inputs();
	return 0;
}

void ui_close(void)
{
	if (tty_fd >= 0) {
		ioctl(tty_fd, KDSETMODE, KD_TEXT);
		close(tty_fd);
		tty_fd = -1;
	}
}

static void log_sleep(const char *text)
{
	if (sleep_log >= 0 && write(sleep_log, text, strlen(text)) < 0)
		return;
}

static void on_console_release(int sig)
{
	(void)sig;
	log_sleep("asked to sleep\n");
	ioctl(vt_fd, VT_RELDISP, 1);
}

static void on_console_acquire(int sig)
{
	(void)sig;
	ioctl(vt_fd, VT_RELDISP, VT_ACKACQ);
	log_sleep("awake\n");
	if (wake_target > 0)
		kill(wake_target, SIGWINCH);
	else
		woke = 1;
}

static void catch_signal(int sig, void (*handler)(int))
{
	struct sigaction sa;

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = handler;
	sa.sa_flags = SA_RESTART;
	sigaction(sig, &sa, NULL);
}

void ui_own_console(void)
{
	struct vt_mode mode;
	char path[300];

	if (getenv("POCKET_FB_FILE"))
		return;
	vt_fd = open("/dev/tty0", O_RDWR);
	if (vt_fd < 0)
		return;
	fcntl(vt_fd, F_SETFD, FD_CLOEXEC);
	snprintf(path, sizeof path, "%s/system/sleep.log", ui_root());
	sleep_log = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (sleep_log >= 0)
		fcntl(sleep_log, F_SETFD, FD_CLOEXEC);
	catch_signal(SIGUSR1, on_console_release);
	catch_signal(SIGUSR2, on_console_acquire);
	memset(&mode, 0, sizeof mode);
	mode.mode = VT_PROCESS;
	mode.relsig = SIGUSR1;
	mode.acqsig = SIGUSR2;
	ioctl(vt_fd, VT_SETMODE, &mode);
}

void ui_give_console_back(void)
{
	struct vt_mode mode;

	if (vt_fd < 0)
		return;
	memset(&mode, 0, sizeof mode);
	mode.mode = VT_AUTO;
	ioctl(vt_fd, VT_SETMODE, &mode);
	close(vt_fd);
	vt_fd = -1;
	if (sleep_log >= 0)
		close(sleep_log);
	sleep_log = -1;
}

void ui_wake_target(int pid)
{
	wake_target = pid;
}

void ui_suspend(void)
{
	while (input_count > 0)
		close(inputs[--input_count]);
	held_mods = 0;
	ui_close();
}

int ui_resume(void)
{
	if (!getenv("POCKET_FB_FILE")) {
		tty_fd = open("/dev/tty0", O_RDWR);
		if (tty_fd >= 0)
			ioctl(tty_fd, KDSETMODE, KD_GRAPHICS);
		open_inputs();
	}
	load_calibration();
	first_present = 1;
	return 0;
}

int ui_switch_away(void)
{
	if (!getenv("POCKET_SWITCHER"))
		return 0;
	ui_suspend();
	raise(SIGSTOP);
	ui_resume();
	return 1;
}

uint16_t *ui_pixels(void)
{
	return back;
}

void ui_present(void)
{
	int y, x;

	for (y = 0; y < SCREEN_H; y++) {
		const uint16_t *from = back + y * SCREEN_W;
		uint16_t *was = shown + y * SCREEN_W;
		volatile uint16_t *to = screen + y * stride_px;

		if (!first_present && !memcmp(from, was, SCREEN_W * 2))
			continue;
		for (x = 0; x < SCREEN_W; x++) {
			if (first_present || from[x] != was[x]) {
				to[x] = from[x];
				was[x] = from[x];
			}
		}
	}
	first_present = 0;
}

void ui_fill(int x, int y, int w, int h, uint16_t color)
{
	int i;

	if (x < 0) {
		w += x;
		x = 0;
	}
	if (y < 0) {
		h += y;
		y = 0;
	}
	if (x + w > SCREEN_W)
		w = SCREEN_W - x;
	if (y + h > SCREEN_H)
		h = SCREEN_H - y;
	for (; h > 0; h--, y++) {
		uint16_t *p = back + y * SCREEN_W + x;

		for (i = 0; i < w; i++)
			p[i] = color;
	}
}

int ui_char_width(unsigned char c)
{
	if (c < 32 || c > 126)
		c = '?';
	return font_width[c - 32];
}

int ui_text_width(const char *text, int len)
{
	int w = 0;

	while (len-- > 0)
		w += ui_char_width((unsigned char)*text++);
	return w;
}

/* y is the top of the line. Returns the x after the text. */
int ui_text(int x, int y, const char *text, int len, uint16_t color)
{
	for (; len > 0; len--, text++) {
		unsigned char c = *text;
		int row;

		if (c < 32 || c > 126)
			c = '?';
		for (row = 0; row < FONT_HEIGHT; row++) {
			unsigned bits = font_bits[c - 32][row];
			int py = y + row;
			int bit;

			if (!bits || py < 0 || py >= SCREEN_H)
				continue;
			for (bit = 0; bits; bit++, bits >>= 1) {
				if ((bits & 1) && x + bit >= 0 && x + bit < SCREEN_W)
					back[py * SCREEN_W + x + bit] = color;
			}
		}
		x += font_width[c - 32];
	}
	return x;
}

int ui_load_image(const char *name, struct ui_image *image)
{
	const char *dir = getenv("POCKET_ART");
	char path[300];
	unsigned char head[4];
	FILE *f;
	size_t n;

	memset(image, 0, sizeof *image);
	snprintf(path, sizeof path, "%s/%s.img", dir ? dir : "/usr/share/pocket/art", name);
	f = fopen(path, "rb");
	if (!f)
		return -1;
	if (fread(head, 1, 4, f) != 4) {
		fclose(f);
		return -1;
	}
	image->w = head[0] | head[1] << 8;
	image->h = head[2] | head[3] << 8;
	n = (size_t)image->w * image->h;
	image->pixels = malloc(n * 2);
	image->mask = malloc(n);
	if (!image->pixels || !image->mask || fread(image->pixels, 2, n, f) != n ||
	    fread(image->mask, 1, n, f) != n) {
		free(image->pixels);
		free(image->mask);
		memset(image, 0, sizeof *image);
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

void ui_draw_image(int x, int y, const struct ui_image *image, int scale)
{
	int sy, sx, i, j;

	for (sy = 0; sy < image->h; sy++) {
		for (sx = 0; sx < image->w; sx++) {
			size_t at = (size_t)sy * image->w + sx;

			if (!image->mask[at])
				continue;
			for (j = 0; j < scale; j++) {
				for (i = 0; i < scale; i++) {
					int px = x + sx * scale + i;
					int py = y + sy * scale + j;

					if (px >= 0 && px < SCREEN_W && py >= 0 && py < SCREEN_H)
						back[py * SCREEN_W + px] = image->pixels[at];
				}
			}
		}
	}
}

static int menu_title_x(const struct ui_menu *menus, int index)
{
	int x = 10;
	int i;

	for (i = 0; i < index; i++)
		x += ui_text_width(menus[i].title, strlen(menus[i].title)) + 16;
	return x;
}

int ui_menu_at(const struct ui_menu *menus, int count, int x)
{
	int i;

	for (i = 0; i < count; i++) {
		int left = menu_title_x(menus, i);

		if (x >= left - 6 &&
		    x < left + ui_text_width(menus[i].title, strlen(menus[i].title)) + 6)
			return i;
	}
	return -1;
}

/* The kernel works this out from the battery voltage, so it can read past 0 and 100. */
static void read_battery(void)
{
	const char *name = getenv("POCKET_APM");
	FILE *file = fopen(name ? name : "/proc/apm", "r");
	int percent;

	battery_now = -1;
	if (!file)
		return;
	if (fscanf(file, "%*s %*s %*s %*s %*s %*s %d%%", &percent) == 1)
		battery_now = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	fclose(file);
}

static int battery_percent(void)
{
	long long now = now_ms();

	if (now >= battery_due) {
		read_battery();
		battery_due = now + BATTERY_EVERY_MS;
	}
	return battery_now;
}

int ui_battery_changed(void)
{
	return battery_percent() != battery_drawn;
}

void ui_menu_bar(const struct ui_menu *menus, int count, const char *right)
{
	char percent[8];
	int right_edge = SCREEN_W - 8;
	int i;

	ui_fill(0, 0, SCREEN_W, 16, C_WINDOW);
	ui_fill(0, 0, SCREEN_W, 1, C_WHITE);
	ui_fill(0, 16, SCREEN_W, 1, C_INK);
	for (i = 0; i < count; i++)
		ui_text(menu_title_x(menus, i), 0, menus[i].title, strlen(menus[i].title), C_INK);
	battery_drawn = battery_percent();
	if (battery_drawn >= 0) {
		int len = snprintf(percent, sizeof percent, "%d%%", battery_drawn);

		ui_text(right_edge - ui_text_width(percent, len), 0, percent, len, C_INK);
		right_edge -= ui_text_width("100%", 4) + 14;
	}
	if (right)
		ui_text(right_edge - ui_text_width(right, strlen(right)), 0, right,
			strlen(right), C_INK);
}

/* The striped Platinum title bar, with the close box at the left. */
void ui_title_bar(int x, int y, int w, const char *title)
{
	int len = strlen(title);
	int text_w = ui_text_width(title, len);
	int text_x = x + (w - text_w) / 2;
	int row;

	ui_fill(x, y, w, 16, C_TITLE);
	ui_fill(x, y, w, 1, C_WHITE);
	ui_fill(x, y + 15, w, 1, C_MID);
	for (row = 4; row < 12; row += 2) {
		ui_fill(x + 22, y + row, w - 30, 1, C_WHITE);
		ui_fill(x + 22, y + row + 1, w - 30, 1, C_MID);
	}
	ui_fill(text_x - 8, y + 1, text_w + 16, 14, C_TITLE);
	ui_text(text_x, y - 1, title, len, C_INK);

	ui_fill(x + 6, y + 2, 12, 12, C_MID);
	ui_fill(x + 7, y + 3, 11, 11, C_WHITE);
	ui_fill(x + 7, y + 3, 10, 10, C_INK);
	ui_fill(x + 8, y + 4, 8, 8, C_TITLE);
}

#define LIST_ROW 15

static void draw_list(int x, int y, int w, const char *const *items, int count,
		      const char *keys, int chosen)
{
	int h = count * LIST_ROW + 4;
	int i;

	ui_fill(x + 2, y + 2, w, h, C_SHADE);
	ui_fill(x, y, w, h, C_INK);
	ui_fill(x + 1, y + 1, w - 2, h - 2, C_WINDOW);
	for (i = 0; i < count; i++) {
		int row = y + 2 + i * LIST_ROW;
		uint16_t ink = i == chosen ? C_WHITE : C_INK;

		if (i == chosen)
			ui_fill(x + 1, row, w - 2, LIST_ROW, C_SELECT);
		ui_text(x + 8, row - 2, items[i], strlen(items[i]), ink);
		if (keys && keys[i] != ' ') {
			char hint[8];

			snprintf(hint, sizeof hint, "Ctrl+%c", toupper((unsigned char)keys[i]));
			ui_text(x + w - 8 - ui_text_width(hint, strlen(hint)), row - 2, hint,
				strlen(hint), ink);
		}
	}
}

static int list_row_at(const struct ui_event *ev, int x, int y, int w, int count)
{
	int row = (ev->y - y - 2) / LIST_ROW;

	if (ev->x < x || ev->x >= x + w || ev->y < y + 2 || row >= count)
		return -1;
	return row;
}

int ui_pick(int x, int y, int w, const char *const *items, int count, void (*paint)(void))
{
	struct ui_event ev;
	int chosen = 0;

	for (;;) {
		int got;

		paint();
		draw_list(x, y, w, items, count, NULL, chosen);
		ui_present();
		do {
			got = ui_wait(&ev, 1000);
		} while (!got);
		if (got < 0)
			return -1;
		if (ev.key == K_UP && chosen > 0)
			chosen--;
		else if (ev.key == K_DOWN && chosen < count - 1)
			chosen++;
		else if (ev.key == K_ENTER)
			return chosen;
		else if (ev.key == K_ESC)
			return -1;
		else if (ev.key == K_TAP)
			return list_row_at(&ev, x, y, w, count);
	}
}

int ui_run_menus(const struct ui_menu *menus, int count, int start, void (*paint)(void))
{
	struct ui_event ev;
	int m = start;
	int item = 0;

	for (;;) {
		const struct ui_menu *menu = &menus[m];
		int title_w = ui_text_width(menu->title, strlen(menu->title));
		int title_x = menu_title_x(menus, m);
		int x = title_x - 6;
		int w = 0;
		int items = 0;
		int got;

		while (items < 6 && menu->labels[items]) {
			int tw = ui_text_width(menu->labels[items], strlen(menu->labels[items]));

			if (tw > w)
				w = tw;
			items++;
		}
		w += menu->keys ? 80 : 24;
		if (x + w > SCREEN_W - 4)
			x = SCREEN_W - 4 - w;
		paint();
		ui_fill(title_x - 4, 1, title_w + 8, 15, C_SELECT);
		ui_text(title_x, 0, menu->title, strlen(menu->title), C_WHITE);
		draw_list(x, 16, w, menu->labels, items, menu->keys, item);
		ui_present();

		do {
			got = ui_wait(&ev, 1000);
		} while (!got);
		if (got < 0)
			return -1;
		switch (ev.key) {
		case K_ESC:
			return -1;
		case K_ENTER:
			return m * 256 + item;
		case K_UP:
			item = item > 0 ? item - 1 : items - 1;
			break;
		case K_DOWN:
			item = item < items - 1 ? item + 1 : 0;
			break;
		case K_LEFT:
			m = m > 0 ? m - 1 : count - 1;
			item = 0;
			break;
		case K_RIGHT:
			m = m < count - 1 ? m + 1 : 0;
			item = 0;
			break;
		case K_TAP: {
			int row = list_row_at(&ev, x, 16, w, items);
			int title = ev.y < 16 ? ui_menu_at(menus, count, ev.x) : -1;

			if (row >= 0)
				return m * 256 + row;
			if (title < 0)
				return -1;
			m = title;
			item = 0;
			break;
		}
		default:
			break;
		}
	}
}

static int backlight_path(char *path, size_t size, const char *name)
{
	const char *dir = getenv("POCKET_BACKLIGHT");

	return snprintf(path, size, "%s/%s", dir ? dir : "/sys/class/backlight/hp680-bl", name) <
	       (int)size;
}

static int read_number(const char *path, int *value)
{
	FILE *file = fopen(path, "r");
	int ok;

	if (!file)
		return 0;
	ok = fscanf(file, "%d", value) == 1;
	fclose(file);
	return ok;
}

static int read_backlight(const char *name, int *value)
{
	char path[256];

	return backlight_path(path, sizeof path, name) && read_number(path, value);
}

static void write_backlight(int value)
{
	char path[256];
	FILE *file;

	if (!backlight_path(path, sizeof path, "brightness"))
		return;
	file = fopen(path, "w");
	if (!file)
		return;
	fprintf(file, "%d\n", value);
	fclose(file);
}

/*
 * Contrast has no Linux driver, so this uses the HD64461's GPIO port B the way
 * NetBSD's j6x0lcd does. Pins 6 to 3 hold the level, inverted, with pin 6 the
 * lowest bit: output mode for a 1, input mode for a 0, with the data bits set.
 * Larger levels are blacker. Only a Jornada build ever touches real hardware.
 */
static int lcd_open(void)
{
	const char *file = getenv("POCKET_LCD_FILE");
	void *page;
	int fd;

	if (lcd_gpio)
		return 1;
#ifndef __sh__
	if (!file)
		return 0;
#endif
	fd = open(file ? file : "/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0)
		return 0;
	page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, file ? 0 : GPIO_PAGE);
	close(fd);
	if (page == MAP_FAILED)
		return 0;
	lcd_gpio = page;
	return 1;
}

static const int contrast_pins[4] = { 6, 5, 4, 3 };

static int contrast_read(void)
{
	int raw = 0;
	int bit;

	for (bit = 0; bit < 4; bit++) {
		if (((lcd_gpio[GPBCR] >> (contrast_pins[bit] * 2)) & 3) == 1)
			raw |= 1 << bit;
	}
	return CONTRAST_TOP - raw;
}

static void contrast_set(int level)
{
	unsigned control = lcd_gpio[GPBCR] & 0xc03f;
	int raw = CONTRAST_TOP - level;
	int bit;

	for (bit = 0; bit < 4; bit++)
		control |= (raw >> bit & 1 ? 1u : 3u) << (contrast_pins[bit] * 2);
	lcd_gpio[GPBDR] |= 0x78;
	lcd_gpio[GPBCR] = control;
}

void ui_screen_load(void)
{
	char path[256];
	int saved;
	int max;

	snprintf(path, sizeof path, "%s/system/brightness", ui_root());
	if (read_number(path, &saved) && saved > 0 && read_backlight("max_brightness", &max))
		write_backlight(saved < max ? saved : max);
	snprintf(path, sizeof path, "%s/system/contrast", ui_root());
	if (read_number(path, &saved) && saved >= 0 && saved <= CONTRAST_TOP && lcd_open())
		contrast_set(saved);
}

static void save_setting(const char *name, int value)
{
	char path[256];
	FILE *file;

	snprintf(path, sizeof path, "%s/system", ui_root());
	mkdir(path, 0755);
	snprintf(path, sizeof path, "%s/system/%s", ui_root(), name);
	file = fopen(path, "w");
	if (!file)
		return;
	fprintf(file, "%d\n", value);
	fflush(file);
	fsync(fileno(file));
	fclose(file);
}

/*
 * Low settings matter most to the eye, so the steps start close together.
 * Never 0, which turns the backlight off. NetBSD found no visible change above
 * 161, so the steps stop there.
 */
static int level_value(int level, int steps, int top)
{
	int value = steps == top ? level : top * level * level / (steps * steps);

	return value < 1 ? 1 : value;
}

static void draw_bar(int y, int on, int steps)
{
	int w = 280 / steps;
	int i;

	for (i = 0; i < steps; i++) {
		ui_fill(180 + i * w, y, w - 2, 14, C_INK);
		ui_fill(181 + i * w, y + 1, w - 4, 12, i < on ? C_SELECT : C_WHITE);
	}
}

static void screen_box(const char *name, int y, int on, int steps, int picked)
{
	if (picked)
		ui_fill(177, y - 16, ui_text_width(name, strlen(name)) + 6, 15, C_SELECT);
	ui_text(180, y - 19, name, strlen(name), picked ? C_WHITE : C_INK);
	draw_bar(y, on, steps);
}

static void screen_frame(const char *text)
{
	ui_fill(173, SCREEN_BOX_Y + 3, 300, SCREEN_BOX_H, C_SHADE);
	ui_fill(169, SCREEN_BOX_Y - 1, 302, SCREEN_BOX_H + 2, C_INK);
	ui_fill(170, SCREEN_BOX_Y, 300, SCREEN_BOX_H, C_WINDOW);
	ui_text(180, SCREEN_BOX_Y + SCREEN_BOX_H - 19, text, strlen(text), C_SHADE);
}

void ui_screen_dialog(void (*paint)(void))
{
	struct ui_event ev;
	int have_light = 1;
	int have_contrast = lcd_open();
	int bright_start = 0;
	int max = 0;
	int top = 0;
	int steps = 0;
	int level = 1;
	int level_start = 1;
	int contrast = 0;
	int contrast_start = 0;
	unsigned control_start = 0;
	unsigned data_start = 0;
	int row;
	int idle = 0;

	if (!read_backlight("brightness", &bright_start) ||
	    !read_backlight("max_brightness", &max) || max < 1)
		have_light = 0;
	if (!have_light && !have_contrast) {
		paint();
		screen_frame("These controls are missing. Press a key.");
		ui_present();
		while (!ui_wait(&ev, 1000))
			;
		return;
	}
	if (have_light) {
		top = max < BRIGHTNESS_USEFUL ? max : BRIGHTNESS_USEFUL;
		steps = top < BRIGHTNESS_STEPS ? top : BRIGHTNESS_STEPS;
		while (level < steps && level_value(level, steps, top) < bright_start)
			level++;
		level_start = level;
	}
	if (have_contrast) {
		control_start = lcd_gpio[GPBCR];
		data_start = lcd_gpio[GPBDR];
		contrast = contrast_read();
		contrast_start = contrast;
	}
	row = have_light ? 0 : 1;
	for (;;) {
		int changed = have_contrast && contrast != contrast_start;
		char hint[64];
		int got;

		if (changed)
			snprintf(hint, sizeof hint, "Going back in %d unless you press Enter",
				 CONTRAST_WAIT - idle);
		else
			snprintf(hint, sizeof hint, "Arrows change them, Enter keeps them");
		paint();
		screen_frame(hint);
		if (have_light)
			screen_box("Brightness", SCREEN_BOX_Y + 19, level, steps, row == 0);
		if (have_contrast)
			screen_box("Contrast", SCREEN_BOX_Y + 52, contrast + 1, CONTRAST_TOP + 1,
				   row == 1);
		ui_present();
		got = ui_wait(&ev, 1000);
		if (!got) {
			/* The screen may be unreadable, so waiting is always a way out. */
			if (changed && ++idle >= CONTRAST_WAIT) {
				lcd_gpio[GPBCR] = control_start;
				lcd_gpio[GPBDR] = data_start;
				contrast = contrast_start;
				idle = 0;
			}
			continue;
		}
		idle = 0;
		if (got < 0 || ev.key == K_ESC) {
			if (have_light)
				write_backlight(bright_start);
			if (have_contrast) {
				lcd_gpio[GPBCR] = control_start;
				lcd_gpio[GPBDR] = data_start;
			}
			return;
		}
		if (ev.key == K_ENTER)
			break;
		if (ev.key == K_UP || ev.key == K_DOWN) {
			if (have_light && have_contrast)
				row = !row;
			continue;
		}
		if (ev.key == K_TAP) {
			int bar = ev.y >= SCREEN_BOX_Y + 52 ? 1 : 0;
			int bar_y = SCREEN_BOX_Y + (bar ? 52 : 19);
			int count = bar ? CONTRAST_TOP + 1 : steps;

			if (ev.x < 170 || ev.x >= 470 || ev.y < SCREEN_BOX_Y ||
			    ev.y >= SCREEN_BOX_Y + SCREEN_BOX_H)
				break;
			if ((bar ? !have_contrast : !have_light) || ev.y < bar_y ||
			    ev.y >= bar_y + 14 || ev.x < 180 ||
			    ev.x >= 180 + count * (280 / count))
				continue;
			row = bar;
			if (bar)
				contrast = (ev.x - 180) / (280 / count);
			else
				level = (ev.x - 180) / (280 / count) + 1;
		} else if (ev.key == K_LEFT) {
			if (row)
				contrast -= contrast > 0;
			else
				level -= level > 1;
		} else if (ev.key == K_RIGHT) {
			if (row)
				contrast += contrast < CONTRAST_TOP;
			else
				level += level < steps;
		} else {
			continue;
		}
		if (row)
			contrast_set(contrast);
		else
			write_backlight(level_value(level, steps, top));
	}
	if (have_light && level != level_start)
		save_setting("brightness", level_value(level, steps, top));
	if (have_contrast && contrast != contrast_start)
		save_setting("contrast", contrast);
}

/* US layout, by evdev code. */
static int key_char(int code, int shift)
{
	static const char plain[] =
		"\0\0" "1234567890-=\0\0" "qwertyuiop[]\0\0" "asdfghjkl;'`\0\\" "zxcvbnm,./";
	static const char shifted[] =
		"\0\0" "!@#$%^&*()_+\0\0" "QWERTYUIOP{}\0\0" "ASDFGHJKL:\"~\0|" "ZXCVBNM<>?";

	if (code == KEY_SPACE)
		return ' ';
	if (code == 123)	/* JLime's code for the Jornada's backtick key */
		return shift ? '~' : '`';
	if (code >= (int)sizeof plain - 1)
		return 0;
	return shift ? shifted[code] : plain[code];
}

/* JLime's keymap puts the Jornada's arrows on the keypad codes. */
static enum ui_key special_key(int code)
{
	switch (code) {
	case KEY_UP: case KEY_KP8: return K_UP;
	case KEY_DOWN: case KEY_KP2: return K_DOWN;
	case KEY_LEFT: case KEY_KP4: return K_LEFT;
	case KEY_RIGHT: case KEY_KP6: return K_RIGHT;
	case KEY_HOME: return K_HOME;
	case KEY_END: return K_END;
	case KEY_PAGEUP: return K_PAGEUP;
	case KEY_PAGEDOWN: return K_PAGEDOWN;
	case KEY_ENTER: return K_ENTER;
	case KEY_BACKSPACE: return K_BACKSPACE;
	case KEY_DELETE: case KEY_KPDOT: return K_DELETE;
	case KEY_TAB: return K_TAB;
	case KEY_ESC: return K_ESC;
	default: return K_NONE;
	}
}

static int translate(const struct input_event *in, struct ui_event *ev)
{
	int mod = 0;
	int ch;

	switch (in->code) {
	case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: mod = MOD_SHIFT; break;
	case KEY_LEFTCTRL: case KEY_RIGHTCTRL: mod = MOD_CTRL; break;
	}
	if (mod) {
		held_mods = in->value ? held_mods | mod : held_mods & ~mod;
		return 0;
	}
	if (!in->value)
		return 0;

	memset(ev, 0, sizeof *ev);
	ev->mods = held_mods;
	ev->key = special_key(in->code);
	if (ev->key == K_BACKSPACE && (held_mods & MOD_SHIFT))
		ev->key = K_DELETE;
	if (ev->key != K_NONE)
		return 1;
	ch = key_char(in->code, held_mods & MOD_SHIFT);
	if (!ch)
		return 0;
	ev->key = K_CHAR;
	ev->ch = ch;
	return 1;
}

/*
 * Test scripts have one action per line:
 *   T text    types the text
 *   K name    presses enter, tab, backspace, left, right, up, down, home, end, esc
 *   S name    the same key with Shift held
 *   C x       presses Ctrl and x
 *   P x y     taps the screen
 *   W ms      lets a timeout pass, for autosave tests
 */
static int script_event(struct ui_event *ev, int timeout_ms)
{
	static const struct { const char *name; enum ui_key key; } names[] = {
		{ "enter", K_ENTER }, { "backspace", K_BACKSPACE }, { "delete", K_DELETE },
		{ "left", K_LEFT }, { "right", K_RIGHT }, { "up", K_UP }, { "down", K_DOWN },
		{ "home", K_HOME }, { "end", K_END }, { "esc", K_ESC },
		{ "pageup", K_PAGEUP }, { "pagedown", K_PAGEDOWN }, { "tab", K_TAB },
	};
	static char line[1024];
	static char *typing;
	unsigned i;

	(void)timeout_ms;
	memset(ev, 0, sizeof *ev);
	if (typing && *typing) {
		ev->key = K_CHAR;
		ev->ch = (unsigned char)*typing++;
		return 1;
	}
	typing = NULL;
	if (!fgets(line, sizeof line, script))
		return -1;
	line[strcspn(line, "\n")] = 0;
	switch (line[0]) {
	case 'T':
		typing = line + 2;
		return script_event(ev, timeout_ms);
	case 'C':
		ev->key = K_CHAR;
		ev->ch = line[2];
		ev->mods = MOD_CTRL;
		return 1;
	case 'P':
		ev->key = K_TAP;
		if (sscanf(line + 2, "%d %d", &ev->x, &ev->y) != 2)
			return -1;
		ev->raw_x = ev->x;
		ev->raw_y = ev->y;
		return 1;
	case 'K':
	case 'S':
		for (i = 0; i < sizeof names / sizeof names[0]; i++) {
			if (!strcmp(line + 2, names[i].name)) {
				ev->key = names[i].key;
				ev->mods = line[0] == 'S' ? MOD_SHIFT : 0;
				if (ev->key == K_BACKSPACE && ev->mods)
					ev->key = K_DELETE;
				return 1;
			}
		}
		fprintf(stderr, "bad script key: %s\n", line);
		return -1;
	default:
		return 0;
	}
}

/* Each axis reports on its own, so each keeps its own readings. */
static void touch_sample_x(int value)
{
	if (touch_held && x_count < TAP_SAMPLES)
		sample_x[x_count++] = value;
}

static void touch_sample_y(int value)
{
	if (touch_held && y_count < TAP_SAMPLES)
		sample_y[y_count++] = value;
}

static void touch_press(void)
{
	touch_held = 1;
	touch_up_ms = -1;
}

static void touch_release(long long now)
{
	touch_held = 0;
	touch_up_ms = now;
}

/* True once a burst has settled and holds at least one reading. */
static int touch_finished(long long now)
{
	if (touch_held || touch_up_ms < 0 || now - touch_up_ms < TAP_SETTLE_MS)
		return 0;
	touch_up_ms = -1;
	if (x_count && y_count)
		return 1;
	x_count = 0;
	y_count = 0;
	return 0;
}

static int by_value(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

static void take_tap(int *x, int *y)
{
	qsort(sample_x, x_count, sizeof sample_x[0], by_value);
	qsort(sample_y, y_count, sizeof sample_y[0], by_value);
	*x = sample_x[x_count / 2];
	*y = sample_y[y_count / 2];
	x_count = 0;
	y_count = 0;
}

static void tap_event(struct ui_event *ev)
{
	int touch_x;
	int touch_y;

	take_tap(&touch_x, &touch_y);
	memset(ev, 0, sizeof *ev);
	ev->key = K_TAP;
	ev->raw_x = touch_x;
	ev->raw_y = touch_y;
	if (have_cal) {
		ev->x = (cal[0] * touch_x + cal[1] * touch_y + cal[2]) / cal[6];
		ev->y = (cal[3] * touch_x + cal[4] * touch_y + cal[5]) / cal[6];
	} else {
		ev->x = -1;
		ev->y = -1;
	}
}

int ui_wait(struct ui_event *ev, int timeout_ms)
{
	struct input_event in;
	int i;

	/* Sleep can lose the picture, and the console was drawn over it. */
	if (woke) {
		int bright;

		woke = 0;
		held_mods = 0;
		first_present = 1;
		ui_present();
		battery_due = 0;
		if (read_backlight("brightness", &bright) && bright > 0)
			write_backlight(bright);
	}
	if (script)
		return script_event(ev, timeout_ms);
	for (;;) {
		struct timeval tv;
		fd_set set;
		int top = 0;
		int wait_ms = timeout_ms;

		for (i = 0; i < input_count; i++) {
			while (read(inputs[i], &in, sizeof in) == sizeof in) {
				if (in.type == EV_ABS && in.code == ABS_X) {
					touch_sample_x(in.value);
				} else if (in.type == EV_ABS && in.code == ABS_Y) {
					touch_sample_y(in.value);
				} else if (in.type == EV_KEY && in.code == BTN_TOUCH) {
					if (in.value)
						touch_press();
					else
						touch_release(now_ms());
				} else if (in.type == EV_KEY && translate(&in, ev)) {
					return 1;
				}
			}
		}
		if (touch_finished(now_ms())) {
			tap_event(ev);
			return 1;
		}
		/* Wake up in time to finish a tap that is settling. */
		if (touch_up_ms >= 0 && wait_ms > TAP_SETTLE_MS)
			wait_ms = TAP_SETTLE_MS;
		tv.tv_sec = wait_ms / 1000;
		tv.tv_usec = (wait_ms % 1000) * 1000;
		FD_ZERO(&set);
		for (i = 0; i < input_count; i++) {
			FD_SET(inputs[i], &set);
			if (inputs[i] > top)
				top = inputs[i];
		}
		if (select(top + 1, &set, NULL, NULL, &tv) <= 0 && touch_up_ms < 0)
			return 0;
	}
}
