/*
 * The small part of SDL that Wolf4SDL needs, on the Jornada's screen and keyboard.
 * Key values stay below 256 so they index Wolf's key table directly.
 */
#ifndef POCKET_SDL_H
#define POCKET_SDL_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef int16_t Sint16;
typedef int32_t Sint32;

typedef enum { SDL_FALSE, SDL_TRUE } SDL_bool;

typedef struct { uint8_t r, g, b, a; } SDL_Color;
typedef struct { int x, y, w, h; } SDL_Rect;
typedef struct { SDL_Color colors[256]; } SDL_Palette;
typedef struct { uint8_t BytesPerPixel; SDL_Palette *palette; } SDL_PixelFormat;
typedef struct {
	SDL_PixelFormat *format;
	int w, h, pitch;
	void *pixels;
} SDL_Surface;

typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;
typedef struct SDL_Joystick SDL_Joystick;
typedef int SDL_Keymod;

typedef struct { int sym, scancode, mod; } SDL_Keysym;
typedef struct { int event, windowID; } SDL_WindowEvent;
typedef struct { int type, button; } SDL_MouseButtonEvent;
typedef struct { int y; } SDL_MouseWheelEvent;
typedef struct { SDL_Keysym keysym; } SDL_KeyboardEvent;
typedef struct {
	int type;
	SDL_KeyboardEvent key;
	SDL_WindowEvent window;
	SDL_MouseButtonEvent button;
	SDL_MouseWheelEvent wheel;
} SDL_Event;

enum { SDL_QUIT = 1, SDL_KEYDOWN, SDL_KEYUP, SDL_MOUSEWHEEL, SDL_MOUSEBUTTONDOWN,
       SDL_MOUSEBUTTONUP, SDL_WINDOWEVENT };
enum { SDL_WINDOWEVENT_RESIZED = 1, SDL_WINDOWEVENT_MINIMIZED, SDL_WINDOWEVENT_MAXIMIZED,
       SDL_WINDOWEVENT_RESTORED, SDL_WINDOWEVENT_FOCUS_GAINED, SDL_WINDOWEVENT_FOCUS_LOST };

#define SDL_INIT_VIDEO 1
#define SDL_INIT_AUDIO 2
#define SDL_INIT_JOYSTICK 4
#define SDL_WINDOWPOS_CENTERED 0
#define SDL_WINDOW_ALLOW_HIGHDPI 0
#define SDL_WINDOW_RESIZABLE 0
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0
#define SDL_RENDERER_PRESENTVSYNC 0
#define SDL_RENDERER_SOFTWARE 0
#define SDL_TEXTUREACCESS_STREAMING 0
#define SDL_HINT_RENDER_SCALE_QUALITY ""
#define SDL_BUTTON_LEFT 1
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT 3
#define SDL_BUTTON(x) (1 << ((x) - 1))
#define SDL_HAT_UP 1
#define SDL_HAT_RIGHT 2
#define SDL_HAT_DOWN 4
#define SDL_HAT_LEFT 8
#define SDL_SCANCODE_RETURN 1
#define SDL_SCANCODE_KP_ENTER 2
#define KMOD_ALT 1
#define KMOD_LALT 1
#define KMOD_RALT 1
#define KMOD_SHIFT 2
#define KMOD_CAPS 4
#define KMOD_NUM 8
#define KMOD_GUI 0
#define KMOD_LGUI 0
#define KMOD_RGUI 0
#define KMOD_CTRL 0

enum {
	SDLK_UNKNOWN = 0, SDLK_BACKSPACE = 8, SDLK_TAB = 9, SDLK_RETURN = 13, SDLK_ESCAPE = 27,
	SDLK_SPACE = 32, SDLK_EXCLAIM = 33, SDLK_QUOTEDBL, SDLK_HASH, SDLK_DOLLAR,
	SDLK_AMPERSAND = 38, SDLK_QUOTE = 39, SDLK_LEFTPAREN, SDLK_RIGHTPAREN, SDLK_ASTERISK,
	SDLK_PLUS, SDLK_COMMA = 44, SDLK_MINUS = 45, SDLK_PERIOD = 46,
	SDLK_SLASH = 47, SDLK_0 = '0', SDLK_1, SDLK_2, SDLK_3, SDLK_4, SDLK_5, SDLK_6, SDLK_7,
	SDLK_8, SDLK_9, SDLK_COLON = 58, SDLK_SEMICOLON = 59, SDLK_LESS, SDLK_EQUALS, SDLK_GREATER, SDLK_QUESTION,
	SDLK_AT, SDLK_LEFTBRACKET = 91,
	SDLK_BACKSLASH = 92, SDLK_RIGHTBRACKET = 93, SDLK_BACKQUOTE = 96,
	SDLK_a = 'a', SDLK_b, SDLK_c, SDLK_d, SDLK_e, SDLK_f, SDLK_g, SDLK_h, SDLK_i, SDLK_j,
	SDLK_k, SDLK_l, SDLK_m, SDLK_n, SDLK_o, SDLK_p, SDLK_q, SDLK_r, SDLK_s, SDLK_t, SDLK_u,
	SDLK_v, SDLK_w, SDLK_x, SDLK_y, SDLK_z,
	SDLK_DELETE = 127,
	SDLK_UP = 128, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT, SDLK_INSERT, SDLK_HOME, SDLK_END,
	SDLK_PAGEUP, SDLK_PAGEDOWN, SDLK_F1, SDLK_F2, SDLK_F3, SDLK_F4, SDLK_F5, SDLK_F6,
	SDLK_F7, SDLK_F8, SDLK_F9, SDLK_F10, SDLK_F11, SDLK_F12, SDLK_LSHIFT, SDLK_RSHIFT,
	SDLK_LCTRL, SDLK_RCTRL, SDLK_LALT, SDLK_RALT, SDLK_CAPSLOCK, SDLK_NUMLOCKCLEAR,
	SDLK_SCROLLLOCK, SDLK_PRINTSCREEN, SDLK_PAUSE, SDLK_KP_ENTER, SDLK_KP_0, SDLK_KP_1,
	SDLK_KP_2, SDLK_KP_3, SDLK_KP_4, SDLK_KP_5, SDLK_KP_6, SDLK_KP_7, SDLK_KP_8, SDLK_KP_9,
	SDLK_KP_PERIOD, SDLK_KP_DIVIDE, SDLK_KP_MULTIPLY, SDLK_KP_MINUS, SDLK_KP_PLUS,
	SDLK_LGUI, SDLK_RGUI, SDLK_F13, SDLK_F14, SDLK_F15, SDLK_F16, SDLK_F17, SDLK_F18,
	SDLK_F19, SDLK_LAST
};

/* The window, renderer and joystick do not exist here. */
#define SDL_Init(flags) 0
#define SDL_Quit pocket_quit
#define SDL_GetError() "no such feature"
#define SDL_NumJoysticks() 0
#define SDL_JoystickOpen(i) ((SDL_Joystick *)0)
#define SDL_JoystickClose(j) ((void)0)
#define SDL_JoystickUpdate() ((void)0)
#define SDL_ENABLE 1
#define SDL_JoystickEventState(x) 0
#define SDL_JoystickGetAxis(j, a) 0
#define SDL_JoystickGetHat(j, h) 0
#define SDL_JoystickGetButton(j, b) 0
#define SDL_JoystickNumButtons(j) 0
#define SDL_JoystickNumHats(j) 0
#define SDL_CreateWindow(t, x, y, w, h, f) ((SDL_Window *)1)
#define SDL_CreateRenderer(w, i, f) ((SDL_Renderer *)1)
#define SDL_CreateTexture(r, f, a, w, h) ((SDL_Texture *)1)
#define SDL_SetWindowMinimumSize(w, x, y) ((void)0)
#define SDL_SetWindowSize(w, x, y) ((void)0)
#define SDL_SetWindowFullscreen(w, f) ((void)0)
#define SDL_SetWindowTitle(w, t) ((void)0)
#define SDL_GetWindowSize(w, x, y) (*(x) = 320, *(y) = 200)
#define SDL_GetWindowPixelFormat(w) 0
#define SDL_GetWindowID(w) 0
#define SDL_RenderSetLogicalSize(r, w, h) ((void)0)
#define SDL_RenderClear(r) ((void)0)
#define SDL_RenderCopy(r, t, a, b) ((void)0)
#define SDL_UpdateTexture(t, r, p, n) ((void)0)
#define SDL_SetHint(a, b) ((void)0)
#define SDL_ShowCursor(x) 0
#define SDL_SetRelativeMouseMode(x) 0
#define SDL_GetRelativeMouseState(x, y) 0
#define SDL_GetMouseState(x, y) 0
#define SDL_WarpMouseInWindow(w, x, y) ((void)0)
#define SDL_PushEvent(e) 0
#define SDL_SaveBMP(s, f) 0
#define SDL_PixelFormatEnumToMasks(f, bits, r, g, b, a) (*(bits) = 8, *(r) = 0, 1)
#define SDL_MUSTLOCK(s) pocket_must_lock(s)
#define SDL_LockSurface(s) pocket_lock(s)
#define SDL_UnlockSurface(s) ((void)0)
#define SDL_FreePalette(p) free(p)
#define SDL_AllocPalette(n) ((SDL_Palette *)calloc(1, sizeof(SDL_Palette)))

int pocket_must_lock(SDL_Surface *s);
int pocket_lock(SDL_Surface *s);
void pocket_quit(void);

SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int w, int h, int bits, Uint32 r, Uint32 g,
				  Uint32 b, Uint32 a);
void SDL_FreeSurface(SDL_Surface *s);
int SDL_FillRect(SDL_Surface *s, const SDL_Rect *rect, Uint32 color);
int SDL_BlitSurface(SDL_Surface *src, const SDL_Rect *from, SDL_Surface *dst, SDL_Rect *to);
int SDL_SetPaletteColors(SDL_Palette *p, const SDL_Color *colors, int first, int count);
int SDL_SetSurfacePalette(SDL_Surface *s, SDL_Palette *p);
Uint32 SDL_MapRGB(const SDL_PixelFormat *f, Uint8 r, Uint8 g, Uint8 b);
void SDL_RenderPresent(SDL_Renderer *r);
int SDL_PollEvent(SDL_Event *e);
int SDL_WaitEvent(SDL_Event *e);
SDL_Keymod SDL_GetModState(void);
Uint32 SDL_GetTicks(void);
void SDL_Delay(Uint32 ms);

/* As in real SDL, the program's main is renamed and pocket_wolf.c starts it. */
int SDL_main(int argc, char **argv);
#define main SDL_main

#ifdef __cplusplus
}
#endif
#endif
