/*
 * Runs a Lua program full screen, for Pocket Code.
 *
 * Every program is its own process, so a bad one cannot hurt the editor or the
 * story being written. Esc always stops it. Lua is built with 32-bit numbers,
 * because whole numbers are fast here and fractions are done in software.
 *
 * On an error, the line number goes to the file descriptor in POCKET_LINE_FD,
 * so the editor can jump to it.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include "ui.h"

#define MEMORY_LIMIT (5 * 1024 * 1024)
#define POLL_EVERY 100000	/* Lua instructions between looks at the keyboard */
#define QUEUE 32
#define CONSOLE_ROWS 16
#define CONSOLE_COLS 110
#define ROW_H 15

struct queued {
	char name[12];
	int x, y;		/* set for a tap */
};

static struct queued queue[QUEUE];
static int queue_head;
static int queue_tail;
static int stopping;
static int stop_raised;
static int input_ended;
static size_t memory_used;

static char console[CONSOLE_ROWS][CONSOLE_COLS];
static int console_used;

/* Lua's memory comes through here, so a runaway program gets an error, not the swap. */
static void *limited_alloc(void *ud, void *ptr, size_t old, size_t size)
{
	void *moved;

	(void)ud;
	if (!ptr)
		old = 0;
	if (!size) {
		memory_used -= old;
		free(ptr);
		return NULL;
	}
	if (size > old && memory_used + (size - old) > MEMORY_LIMIT)
		return NULL;
	moved = realloc(ptr, size);
	if (moved)
		memory_used += size - old;
	return moved;
}

static void key_name(const struct ui_event *ev, char *name, size_t size)
{
	static const struct { enum ui_key key; const char *name; } names[] = {
		{ K_ENTER, "enter" }, { K_BACKSPACE, "backspace" }, { K_DELETE, "delete" },
		{ K_TAB, "tab" }, { K_UP, "up" }, { K_DOWN, "down" }, { K_LEFT, "left" },
		{ K_RIGHT, "right" }, { K_HOME, "home" }, { K_END, "end" },
		{ K_PAGEUP, "pageup" }, { K_PAGEDOWN, "pagedown" },
	};
	unsigned i;

	name[0] = 0;
	if (ev->key == K_CHAR) {
		snprintf(name, size, "%c", ev->ch);
		return;
	}
	for (i = 0; i < sizeof names / sizeof names[0]; i++) {
		if (ev->key == names[i].key)
			snprintf(name, size, "%s", names[i].name);
	}
}

/* Moves whatever has happened into the queue. Esc, or the end of input, stops the program. */
static void take_input(int wait_ms)
{
	struct ui_event ev;
	int got;

	while ((got = ui_wait(&ev, wait_ms)) != 0) {
		struct queued *q = &queue[queue_tail];
		int next = (queue_tail + 1) % QUEUE;

		wait_ms = 0;
		if (got < 0) {
			input_ended = 1;
			stopping = 1;
			return;
		}
		if (ev.key == K_ESC) {
			stopping = 1;
			return;
		}
		if (next == queue_head)
			continue;
		if (ev.key == K_TAP) {
			strcpy(q->name, "tap");
			q->x = ev.x;
			q->y = ev.y;
		} else {
			key_name(&ev, q->name, sizeof q->name);
			if (!q->name[0])
				continue;
		}
		queue_tail = next;
	}
}

static void finish_screen(const char *top, const char *message);

/* A program can catch the error with pcall, so if it is still going, it ends here. */
static void stop_if_asked(lua_State *L)
{
	if (!stopping)
		return;
	if (stop_raised) {
		finish_screen("Stopped. Press a key.", NULL);
		ui_close();
		_exit(0);
	}
	stop_raised = 1;
	luaL_error(L, "stopped");
}

static void poll_hook(lua_State *L, lua_Debug *ar)
{
	(void)ar;
	take_input(0);
	stop_if_asked(L);
}

static uint16_t color_arg(lua_State *L, int index, uint16_t fallback)
{
	return lua_isnoneornil(L, index) ? fallback : (uint16_t)luaL_checkinteger(L, index);
}

static int l_rgb(lua_State *L)
{
	lua_pushinteger(L, RGB(luaL_checkinteger(L, 1), luaL_checkinteger(L, 2),
			       luaL_checkinteger(L, 3)));
	return 1;
}

static int l_cls(lua_State *L)
{
	ui_fill(0, 0, SCREEN_W, SCREEN_H, color_arg(L, 1, C_WHITE));
	console_used = 0;
	return 0;
}

static int l_pixel(lua_State *L)
{
	ui_fill(luaL_checkinteger(L, 1), luaL_checkinteger(L, 2), 1, 1, color_arg(L, 3, C_INK));
	return 0;
}

static int l_fill(lua_State *L)
{
	ui_fill(luaL_checkinteger(L, 1), luaL_checkinteger(L, 2), luaL_checkinteger(L, 3),
		luaL_checkinteger(L, 4), color_arg(L, 5, C_INK));
	return 0;
}

static int l_rect(lua_State *L)
{
	int x = luaL_checkinteger(L, 1);
	int y = luaL_checkinteger(L, 2);
	int w = luaL_checkinteger(L, 3);
	int h = luaL_checkinteger(L, 4);
	uint16_t c = color_arg(L, 5, C_INK);

	ui_fill(x, y, w, 1, c);
	ui_fill(x, y + h - 1, w, 1, c);
	ui_fill(x, y, 1, h, c);
	ui_fill(x + w - 1, y, 1, h, c);
	return 0;
}

static int l_line(lua_State *L)
{
	int x = luaL_checkinteger(L, 1);
	int y = luaL_checkinteger(L, 2);
	int x2 = luaL_checkinteger(L, 3);
	int y2 = luaL_checkinteger(L, 4);
	uint16_t c = color_arg(L, 5, C_INK);
	int dx = abs(x2 - x);
	int dy = -abs(y2 - y);
	int sx = x < x2 ? 1 : -1;
	int sy = y < y2 ? 1 : -1;
	int err = dx + dy;

	if (dx > 2 * SCREEN_W || -dy > 2 * SCREEN_H)
		return luaL_error(L, "that line goes too far off the screen");
	for (;;) {
		int twice = 2 * err;

		ui_fill(x, y, 1, 1, c);
		if (x == x2 && y == y2)
			break;
		if (twice >= dy) {
			err += dy;
			x += sx;
		}
		if (twice <= dx) {
			err += dx;
			y += sy;
		}
	}
	return 0;
}

static int l_text(lua_State *L)
{
	size_t len;
	int x = luaL_checkinteger(L, 1);
	int y = luaL_checkinteger(L, 2);
	const char *s = luaL_tolstring(L, 3, &len);

	lua_pushinteger(L, ui_text(x, y, s, len, color_arg(L, 4, C_INK)));
	return 1;
}

static int l_show(lua_State *L)
{
	ui_present();
	take_input(0);
	stop_if_asked(L);
	return 0;
}

static void push_next(lua_State *L, struct queued *q)
{
	lua_pushstring(L, q->name);
	queue_head = (queue_head + 1) % QUEUE;
}

static int l_key(lua_State *L)
{
	take_input(0);
	stop_if_asked(L);
	if (queue_head == queue_tail) {
		lua_pushnil(L);
		return 1;
	}
	push_next(L, &queue[queue_head]);
	return 1;
}

static int pop_with_tap(lua_State *L)
{
	struct queued *q = &queue[queue_head];

	push_next(L, q);
	if (strcmp(q->name, "tap"))
		return 1;
	lua_pushinteger(L, q->x);
	lua_pushinteger(L, q->y);
	return 3;
}

static int l_waitkey(lua_State *L)
{
	ui_present();
	while (queue_head == queue_tail) {
		take_input(1000);
		stop_if_asked(L);
	}
	return pop_with_tap(L);
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000ll + ts.tv_nsec / 1000000;
}

static int l_wait(lua_State *L)
{
	long long until = now_ms() + luaL_checkinteger(L, 1);

	ui_present();
	for (;;) {
		long long left = until - now_ms();

		if (left <= 0)
			break;
		take_input(left > 100 ? 100 : left);
		stop_if_asked(L);
	}
	return 0;
}

static int l_ticks(lua_State *L)
{
	lua_pushinteger(L, (lua_Integer)(now_ms() & 0x7fffffff));
	return 1;
}

static void draw_console(void)
{
	int i;

	ui_fill(0, 0, SCREEN_W, console_used * ROW_H + 2, C_WHITE);
	for (i = 0; i < console_used; i++)
		ui_text(6, i * ROW_H - 2, console[i], strlen(console[i]), C_INK);
	ui_present();
}

static void console_line(const char *text, size_t len)
{
	if (console_used == CONSOLE_ROWS) {
		memmove(console[0], console[1], sizeof console[0] * (CONSOLE_ROWS - 1));
		console_used--;
	}
	snprintf(console[console_used++], CONSOLE_COLS, "%.*s", (int)len, text);
}

/* Breaks long text at the edge of the screen and at line ends. */
static void console_text(const char *s, size_t len)
{
	for (;;) {
		size_t n = 0;

		while (n < len && s[n] != '\n' && n < CONSOLE_COLS - 1 &&
		       ui_text_width(s, n + 1) < SCREEN_W - 12)
			n++;
		console_line(s, n);
		if (n == len)
			return;
		if (s[n] == '\n')
			n++;
		s += n;
		len -= n;
		if (!len)
			return;
	}
}

static int l_print(lua_State *L)
{
	luaL_Buffer b;
	int n = lua_gettop(L);
	int i;

	luaL_buffinit(L, &b);
	for (i = 1; i <= n; i++) {
		if (i > 1)
			luaL_addchar(&b, ' ');
		luaL_tolstring(L, i, NULL);
		luaL_addvalue(&b);
	}
	luaL_pushresult(&b);
	{
		size_t len;
		const char *s = lua_tolstring(L, -1, &len);

		console_text(s, len);
	}
	draw_console();
	return 0;
}

static const luaL_Reg screen_funcs[] = {
	{ "rgb", l_rgb }, { "cls", l_cls }, { "pixel", l_pixel }, { "fill", l_fill },
	{ "rect", l_rect }, { "line", l_line }, { "text", l_text }, { "show", l_show },
	{ "key", l_key }, { "waitkey", l_waitkey }, { "wait", l_wait }, { "ticks", l_ticks },
	{ "print", l_print }, { NULL, NULL },
};

static void set_color(lua_State *L, const char *name, uint16_t color)
{
	lua_pushinteger(L, color);
	lua_setglobal(L, name);
}

static void add_screen_api(lua_State *L)
{
	lua_pushglobaltable(L);
	luaL_setfuncs(L, screen_funcs, 0);
	lua_pop(L, 1);
	lua_pushinteger(L, SCREEN_W);
	lua_setglobal(L, "WIDTH");
	lua_pushinteger(L, SCREEN_H);
	lua_setglobal(L, "HEIGHT");
	set_color(L, "BLACK", RGB(0, 0, 0));
	set_color(L, "WHITE", C_WHITE);
	set_color(L, "GRAY", RGB(0x88, 0x88, 0x88));
	set_color(L, "RED", RGB(0xe0, 0x20, 0x20));
	set_color(L, "GREEN", RGB(0x20, 0xb0, 0x40));
	set_color(L, "BLUE", RGB(0x20, 0x40, 0xd0));
	set_color(L, "YELLOW", RGB(0xf0, 0xd0, 0x20));
	set_color(L, "PURPLE", RGB(0x90, 0x40, 0xc0));
}

static int report_traceback(lua_State *L)
{
	if (!lua_tostring(L, 1))
		lua_pushstring(L, "the program stopped with an error that has no message");
	return 1;
}

static void report_line(const char *message)
{
	const char *fd_name = getenv("POCKET_LINE_FD");
	const char *found = strstr(message, "program:");
	char number[16];

	if (!found || !fd_name || !isdigit((unsigned char)found[8]))
		return;
	snprintf(number, sizeof number, "%d\n", atoi(found + 8));
	if (write(atoi(fd_name), number, strlen(number)) < 0)
		return;
}

/* The last words, in a strip at the bottom, until a key is pressed. */
static void finish_screen(const char *top, const char *message)
{
	struct ui_event ev;
	int rows = 1;
	int y;
	size_t len = message ? strlen(message) : 0;

	console_used = 0;
	if (message) {
		console_text(message, len);
		rows += console_used;
	}
	y = SCREEN_H - rows * ROW_H - 4;
	if (y < 0)
		y = 0;
	ui_fill(0, y, SCREEN_W, SCREEN_H - y, C_WINDOW);
	ui_fill(0, y, SCREEN_W, 1, C_INK);
	ui_text(6, y - 1, top, strlen(top), C_INK);
	for (rows = 0; message && rows < console_used; rows++)
		ui_text(6, y - 1 + (rows + 1) * ROW_H, console[rows], strlen(console[rows]),
			RGB(0xc0, 0x10, 0x10));
	ui_present();
	if (input_ended)
		return;
	while (!ui_wait(&ev, 1000))
		;
}

/* Errors name the chunk, so a short name keeps them readable and easy to parse. */
static int load_program(lua_State *L, const char *path)
{
	FILE *file = fopen(path, "rb");
	long size;
	char *text;
	int failed;

	if (!file || fseek(file, 0, SEEK_END) || (size = ftell(file)) < 0) {
		if (file)
			fclose(file);
		lua_pushstring(L, "could not read the program");
		return LUA_ERRFILE;
	}
	rewind(file);
	text = malloc(size + 1);
	if (!text || fread(text, 1, size, file) != (size_t)size) {
		free(text);
		fclose(file);
		lua_pushstring(L, "could not read the program");
		return LUA_ERRFILE;
	}
	fclose(file);
	failed = luaL_loadbufferx(L, text, size, "=program", "t");
	free(text);
	return failed;
}

int main(int argc, char **argv)
{
	lua_State *L;
	int failed;

	if (argc < 2) {
		fprintf(stderr, "usage: pocketlua program.lua\n");
		return 2;
	}
	if (ui_open()) {
		fprintf(stderr, "could not open the screen\n");
		return 1;
	}
	L = lua_newstate(limited_alloc, NULL);
	if (!L) {
		ui_close();
		return 1;
	}
	luaL_openlibs(L);
	add_screen_api(L);
	ui_fill(0, 0, SCREEN_W, SCREEN_H, C_WHITE);
	ui_present();

	lua_pushcfunction(L, report_traceback);
	failed = load_program(L, argv[1]);
	if (!failed) {
		lua_sethook(L, poll_hook, LUA_MASKCOUNT, POLL_EVERY);
		failed = lua_pcall(L, 0, 0, 1);
		lua_sethook(L, NULL, 0, 0);
	}
	if (stopping) {
		finish_screen("Stopped. Press a key.", NULL);
	} else if (failed) {
		const char *message = lua_tostring(L, -1);

		report_line(message);
		finish_screen("The program hit a problem. Press a key.", message);
	} else {
		finish_screen("Finished. Press a key.", NULL);
	}
	lua_close(L);
	ui_close();
	return failed && !stopping ? 1 : 0;
}
