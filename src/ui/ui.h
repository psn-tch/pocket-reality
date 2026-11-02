/* Shared screen, font and input layer for the Pocket Reality programs. */
#ifndef POCKET_UI_H
#define POCKET_UI_H

#include <stdint.h>

#define SCREEN_W 640
#define SCREEN_H 240

#define RGB(r, g, b) ((uint16_t)(((r) & 0xf8) << 8 | ((g) & 0xfc) << 3 | (b) >> 3))

/* Platinum Reality's palette. */
#define C_WHITE RGB(0xff, 0xff, 0xff)
#define C_INK RGB(0x22, 0x22, 0x22)
#define C_WINDOW RGB(0xdd, 0xdd, 0xdd)
#define C_TITLE RGB(0xcc, 0xcc, 0xcc)
#define C_MID RGB(0x88, 0x88, 0x88)
#define C_SHADE RGB(0x55, 0x55, 0x55)
#define C_SELECT RGB(0x33, 0x33, 0x66)
#define C_MISSPELT RGB(0xd0, 0x10, 0x10)

enum ui_key {
	K_NONE = 0,
	K_CHAR,
	K_ENTER,
	K_BACKSPACE,
	K_DELETE,
	K_TAB,
	K_ESC,
	K_UP,
	K_DOWN,
	K_LEFT,
	K_RIGHT,
	K_HOME,
	K_END,
	K_PAGEUP,
	K_PAGEDOWN,
	K_TAP,
};

#define MOD_SHIFT 1
#define MOD_CTRL 2

struct ui_event {
	enum ui_key key;
	int ch;		/* K_CHAR only: the character typed */
	int mods;
	int x, y;	/* K_TAP only: screen position */
	int raw_x, raw_y;	/* K_TAP only: before calibration */
};

struct ui_image {
	int w, h;
	uint16_t *pixels;
	uint8_t *mask;		/* 1 where the image is solid */
};

/* One title in a menu bar. keys, when set, names a Ctrl shortcut per item. */
struct ui_menu {
	const char *title;
	const char *labels[6];
	const char *keys;
};

int ui_open(void);
void ui_close(void);

/*
 * The kernel can only sleep if it can switch away from our graphics console.
 * The desktop owns that switch for every program. After a wake it sends
 * SIGWINCH to the pid in ui_wake_target, or redraws itself if that is 0.
 */
void ui_own_console(void);
void ui_give_console_back(void);
void ui_wake_target(int pid);

/* Hands the screen and input to another program, then takes them back. */
void ui_suspend(void);
int ui_resume(void);

/*
 * Pauses this program so the Finder can run, until the Finder resumes it.
 * Returns 0, doing nothing, if the Finder did not start this program.
 */
int ui_switch_away(void);

/* Where the card is mounted, normally /media/pocket. */
const char *ui_root(void);
uint16_t *ui_pixels(void);
void ui_present(void);

void ui_fill(int x, int y, int w, int h, uint16_t color);
int ui_text(int x, int y, const char *text, int len, uint16_t color);
int ui_text_width(const char *text, int len);
int ui_char_width(unsigned char c);

/* The window furniture shared by every program. */
void ui_menu_bar(const struct ui_menu *menus, int count, const char *right);
void ui_title_bar(int x, int y, int w, const char *title);

/* True when the percentage in the menu bar is out of date and needs a repaint. */
int ui_battery_changed(void);

/* Sets the saved brightness and contrast, then lets the viewer change them. */
void ui_screen_load(void);
void ui_screen_dialog(void (*paint)(void));

int ui_load_image(const char *name, struct ui_image *image);
void ui_draw_image(int x, int y, const struct ui_image *image, int scale);

/*
 * Menus and lists run until a choice is made, repainting with paint() each time.
 * ui_run_menus returns menu * 256 + item, or -1 if cancelled.
 * ui_pick returns the item, or -1 if cancelled.
 */
int ui_run_menus(const struct ui_menu *menus, int count, int start, void (*paint)(void));
int ui_pick(int x, int y, int w, const char *const *items, int count, void (*paint)(void));
int ui_menu_at(const struct ui_menu *menus, int count, int x);

/* Touch calibration, from three screen points and the raw taps on them. */
int ui_has_calibration(void);
void ui_set_calibration(const int screen[6], const int raw[6]);

/* Waits up to timeout_ms. Returns 1 for an event, 0 on timeout, -1 at the end of a test script. */
int ui_wait(struct ui_event *ev, int timeout_ms);

#endif
