/*
 * Startup screen. Shows the startup picture, or the name if there is no art,
 * and leaves the console in graphics mode so boot messages stay hidden.
 * "splash --text" gives the console back, for the login prompt.
 * "splash --done" says shutdown has finished, as the last step before halting.
 */
#include <fcntl.h>
#include <linux/kd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "ui.h"

int main(int argc, char **argv)
{
	static const char name[] = "Pocket Reality";
	struct ui_image picture;

	if (argc > 1 && !strcmp(argv[1], "--text")) {
		int fd = open("/dev/tty0", O_RDWR);

		if (fd >= 0)
			ioctl(fd, KDSETMODE, KD_TEXT);
		return 0;
	}
	if (ui_open())
		return 1;
	if (argc > 1 && !strcmp(argv[1], "--done")) {
		static const char line1[] = "Everything is saved <3";
		static const char line2[] = "It is now safe to press the reset button.";
		int w = ui_text_width(line2, strlen(line2)) + 48;
		int x = (SCREEN_W - w) / 2;

		ui_fill(0, 0, SCREEN_W, SCREEN_H, C_TITLE);
		ui_fill(x + 3, 83, w, 70, C_SHADE);
		ui_fill(x - 1, 79, w + 2, 72, C_INK);
		ui_fill(x, 80, w, 70, C_WINDOW);
		ui_text(x + 24, 92, line1, strlen(line1), C_INK);
		ui_text(x + 24, 112, line2, strlen(line2), C_INK);
		ui_present();
		return 0;
	}
	ui_fill(0, 0, SCREEN_W, SCREEN_H, C_TITLE);
	if (!ui_load_image("happy-mac", &picture))
		ui_draw_image((SCREEN_W - picture.w * 2) / 2, (SCREEN_H - picture.h * 2) / 2,
			      &picture, 2);
	else
		ui_text((SCREEN_W - ui_text_width(name, strlen(name))) / 2, 110, name,
			strlen(name), C_INK);
	ui_present();
	return 0;
}
