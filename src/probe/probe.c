/*
 * probe: reports what the real Jornada can do.
 * Prints to the console and to a log file, so results survive a bad screen.
 * Usage: probe [logfile] [input-seconds]
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define BENCH_MS 1000
#define DOOM_FRAME (320 * 200)

static FILE *logf;

static void say(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
	if (logf) {
		va_start(ap, fmt);
		vfprintf(logf, fmt, ap);
		va_end(ap);
		fflush(logf);
		fsync(fileno(logf));
	}
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void show_file(const char *path)
{
	char line[256];
	FILE *f = fopen(path, "r");

	if (!f) {
		say("%s: %s\n", path, strerror(errno));
		return;
	}
	while (fgets(line, sizeof line, f))
		say("  %s", line);
	fclose(f);
}

/* Same operation as Doom's FixedMul, the hottest maths in the renderer. */
static inline int32_t fixed_mul(int32_t a, int32_t b)
{
	return (int32_t)(((int64_t)a * b) >> 16);
}

static void bench_cpu(void)
{
	volatile int32_t x = 0x12345;
	int32_t v = 0x10000;
	unsigned long n = 0;
	double t = now(), e;
	int i;

	do {
		for (i = 0; i < 10000; i++)
			v = fixed_mul(v + x, 0x10003);
		n += 10000;
		e = now() - t;
	} while (e * 1000 < BENCH_MS);
	x = v;
	say("cpu: FixedMul %.2f million/s\n", n / e / 1e6);
}

/* Copies len bytes in a loop for BENCH_MS and reports MB/s. */
static void bench_copy(const char *name, void *dst, const void *src, size_t len)
{
	unsigned long n = 0;
	double t = now(), e;

	do {
		memcpy(dst, src, len);
		n++;
		e = now() - t;
	} while (e * 1000 < BENCH_MS);
	say("%s: %.2f MB/s, %.1f copies/s of %lu bytes\n", name,
	    n * (double)len / e / 1e6, n / e, (unsigned long)len);
}

static void draw_bars(uint8_t *fb, const struct fb_var_screeninfo *v, int stride)
{
	int bytes = (v->bits_per_pixel + 7) / 8;
	int x, y, i;

	for (y = 0; y < (int)v->yres; y++) {
		for (x = 0; x < (int)v->xres; x++) {
			unsigned val = x * 256 / v->xres;
			uint8_t *p = fb + y * stride + x * bytes;

			if (bytes == 1) {
				*p = val;
			} else {
				uint16_t c = (val >> 3) << 11 | (val >> 2) << 5 | val >> 3;

				for (i = 0; i < 2; i++)
					p[i] = c >> (8 * i);
			}
		}
	}
}

static void test_fb(const char *dev)
{
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	uint8_t *fb, *ram;
	size_t screen;
	int fd = open(dev, O_RDWR);

	if (fd < 0) {
		say("fb: %s: %s\n", dev, strerror(errno));
		return;
	}
	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) || ioctl(fd, FBIOGET_FSCREENINFO, &fix)) {
		say("fb: ioctl: %s\n", strerror(errno));
		close(fd);
		return;
	}
	say("fb: id=%s %ux%u virtual %ux%u depth=%u line=%u mem=%u visual=%u\n",
	    fix.id, var.xres, var.yres, var.xres_virtual, var.yres_virtual,
	    var.bits_per_pixel, fix.line_length, fix.smem_len, fix.visual);
	say("fb: red %u/%u green %u/%u blue %u/%u\n", var.red.offset, var.red.length,
	    var.green.offset, var.green.length, var.blue.offset, var.blue.length);

	fb = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) {
		say("fb: mmap: %s\n", strerror(errno));
		close(fd);
		return;
	}
	screen = (size_t)fix.line_length * var.yres;
	ram = calloc(1, screen);
	if (!ram) {
		say("fb: no memory\n");
	} else {
		draw_bars(fb, &var, fix.line_length);
		bench_copy("fb write, full screen", fb, ram, screen);
		if (screen > DOOM_FRAME)
			bench_copy("fb write, 320x200 amount", fb, ram, DOOM_FRAME);
		draw_bars(ram, &var, fix.line_length);
		bench_copy("fb read", ram, fb, screen);
		bench_copy("ram copy", ram, ram + screen / 2, screen / 2);
		draw_bars(fb, &var, fix.line_length);
		free(ram);
	}
	munmap(fb, fix.smem_len);
	close(fd);
}

static int open_inputs(int *fds, int max)
{
	char path[280];
	char name[80];
	struct dirent *e;
	int n = 0;
	DIR *d = opendir("/dev/input");

	if (!d) {
		say("input: /dev/input: %s\n", strerror(errno));
		return 0;
	}
	while ((e = readdir(d)) && n < max) {
		if (strncmp(e->d_name, "event", 5))
			continue;
		snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
		fds[n] = open(path, O_RDONLY | O_NONBLOCK);
		if (fds[n] < 0) {
			say("input: %s: %s\n", path, strerror(errno));
			continue;
		}
		strcpy(name, "?");
		ioctl(fds[n], EVIOCGNAME(sizeof name), name);
		say("input: fd%d %s \"%s\"\n", n, path, name);
		n++;
	}
	closedir(d);
	return n;
}

static void test_input(int seconds)
{
	struct input_event ev;
	int fds[8];
	int n = open_inputs(fds, 8);
	double end = now() + seconds;
	int i, events = 0;

	if (!n) {
		say("input: no event devices found\n");
		return;
	}
	say("input: press keys and tap the screen for %d seconds\n", seconds);
	while (now() < end) {
		struct timeval tv = { 0, 100000 };
		fd_set set;
		int top = 0;

		FD_ZERO(&set);
		for (i = 0; i < n; i++) {
			FD_SET(fds[i], &set);
			if (fds[i] > top)
				top = fds[i];
		}
		if (select(top + 1, &set, NULL, NULL, &tv) <= 0)
			continue;
		for (i = 0; i < n; i++) {
			while (read(fds[i], &ev, sizeof ev) == sizeof ev) {
				if (ev.type == EV_SYN)
					continue;
				say("  fd%d type=%u code=%u value=%d\n", i, ev.type, ev.code, ev.value);
				events++;
			}
		}
	}
	say("input: %d events\n", events);
	for (i = 0; i < n; i++)
		close(fds[i]);
}

int main(int argc, char **argv)
{
	const char *log_path = argc > 1 ? argv[1] : "probe.log";
	int seconds = argc > 2 ? atoi(argv[2]) : 20;
	struct utsname u;

	logf = fopen(log_path, "w");
	say("pocket reality probe\n");
	if (!logf)
		say("log: %s: %s\n", log_path, strerror(errno));
	if (!uname(&u))
		say("kernel: %s %s %s\n", u.sysname, u.release, u.machine);
	show_file("/proc/cpuinfo");
	show_file("/proc/meminfo");
	bench_cpu();
	test_fb("/dev/fb0");
	test_input(seconds);
	say("done\n");
	if (logf)
		fclose(logf);
	return 0;
}
