/*
 * The Pocket Reality desktop: a small Platinum Finder for stories and games.
 *
 * The desktop holds Pocket HD and the Trash, and Pocket HD holds everything else.
 *
 * Keyboard: up and down choose, left and right switch between folders,
 * Enter opens, Backspace goes up a level, Esc opens the menus.
 * Stylus: tap to select, tap again to open, close box to go up a level or
 * close the window. Tab, or tapping the name at the top right, lists what is open.
 *
 * Stories and games stay paused in memory while the Finder is showing.
 * Deleted items wait in the Trash, on the card, until it is emptied.
 * Folders made in Pocket HD are kept in the card's desktop folder.
 *
 * Exits with 0 for the login prompt or 10 to power off. Art is optional and
 * comes from the art folder; without it, plain original shapes stand in.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ui.h"

#define C_DESKTOP RGB(0x70, 0x88, 0x98)

#define WIN_X 10
#define WIN_Y 24
#define WIN_W 480
#define WIN_H 204
#define INFO_H 15
#define ROW_H 16
#define LIST_Y (WIN_Y + 16 + INFO_H + 1)
#define VISIBLE ((WIN_Y + WIN_H - 2 - LIST_Y) / ROW_H)

#define ICON_X 524
#define ICON_W 92
#define MAX_ITEMS 200
#define MAX_OPEN 6
#define MAX_FOLDERS 16
#define DESKTOP_ICONS 2
#define DISK_ICON_Y 22
#define TRASH_ICON_Y 166
#define EXIT_POWER_OFF 10

enum kind { SUBFOLDER, PLACE, STORY, DOOM, DOOM_BENCH, WOLF };
enum { DISK, TRASH, STORIES, GAMES, CODE, FIXED_FOLDERS };

struct item {
	char label[64];
	char arg[300];
	enum kind kind;
};

static struct ui_menu menus[] = {
	{ "File", { "Open", "New Story", "New Folder", "Close Window" }, NULL },
	{ "Special", { "Calibrate Stylus", "Brightness & Contrast...", "Login Prompt", "Power Off" }, NULL },
	{ "Item", { "Rename...", "Move To...", "Move to Trash" }, NULL },
};

#define MENU_COUNT ((int)(sizeof menus / sizeof menus[0]))
#define ITEM_MENU 2
#define ITEM_LAST 2

/* A program paused in the background. */
struct open_app {
	pid_t pid;
	char label[64];
	char key[300];		/* what it has open, so opening it twice switches instead */
};

static struct open_app open_apps[MAX_OPEN];
static int open_count;

/* The two desktop icons, Stories and Games, then the folders made in Pocket HD. */
static char folder_names[MAX_FOLDERS][48];
static int folder_count;
static int window_open = 1;

static struct item items[MAX_ITEMS];
static int item_count;
static int current = DISK;
static int chosen;
static int top;
static char subfolder[150];	/* inside stories, empty at the top level */
static char notice[64];

static struct ui_image wallpaper;
static struct ui_image folder_big;
static struct ui_image disk_big;
static struct ui_image trash_big;
static struct ui_image doc_small;
static struct ui_image folder_small;
static struct ui_image app_small;

static void add_item(const char *label, const char *arg, enum kind kind)
{
	struct item *it;

	if (item_count == MAX_ITEMS)
		return;
	it = &items[item_count++];
	snprintf(it->label, sizeof it->label, "%s", label);
	snprintf(it->arg, sizeof it->arg, "%s", arg);
	it->kind = kind;
}

/* Folders first, then names in order. */
static int by_label(const void *a, const void *b)
{
	const struct item *x = a;
	const struct item *y = b;

	if ((x->kind == SUBFOLDER) != (y->kind == SUBFOLDER))
		return x->kind == SUBFOLDER ? -1 : 1;
	return strcasecmp(x->label, y->label);
}

static void stories_path(char *out, size_t size, const char *name)
{
	char base[120];

	if (current == TRASH)
		snprintf(base, sizeof base, "%s/trash", ui_root());
	else if (current == CODE)
		snprintf(base, sizeof base, "%s/code", ui_root());
	else if (current < FIXED_FOLDERS)
		snprintf(base, sizeof base, "%s/stories", ui_root());
	else
		snprintf(base, sizeof base, "%s/desktop/%s", ui_root(), folder_names[current]);
	snprintf(out, size, "%s%s%s%s%s", base, subfolder[0] ? "/" : "", subfolder,
		 name ? "/" : "", name ? name : "");
}

static int by_name(const void *a, const void *b)
{
	return strcasecmp(a, b);
}

static void load_desktop(void)
{
	char path[300];
	struct dirent *e;
	DIR *d;

	strcpy(folder_names[DISK], "Pocket HD");
	strcpy(folder_names[TRASH], "Trash");
	strcpy(folder_names[STORIES], "Stories");
	strcpy(folder_names[GAMES], "Games");
	strcpy(folder_names[CODE], "Code");
	folder_count = FIXED_FOLDERS;
	snprintf(path, sizeof path, "%s/trash", ui_root());
	mkdir(path, 0755);
	snprintf(path, sizeof path, "%s/desktop", ui_root());
	mkdir(path, 0755);
	d = opendir(path);
	if (!d)
		return;
	while ((e = readdir(d)) && folder_count < MAX_FOLDERS) {
		struct stat st;

		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof path, "%s/desktop/%s", ui_root(), e->d_name);
		if (stat(path, &st) || !S_ISDIR(st.st_mode))
			continue;
		snprintf(folder_names[folder_count++], sizeof folder_names[0], "%.47s", e->d_name);
	}
	closedir(d);
	qsort(folder_names[FIXED_FOLDERS], folder_count - FIXED_FOLDERS, sizeof folder_names[0],
	      by_name);
}

/* The disk at the top right and the Trash at the bottom right, like a Mac desktop. */
static int icon_y(int folder)
{
	return folder == TRASH ? TRASH_ICON_Y : DISK_ICON_Y;
}

static void load_stories(void)
{
	char path[300];
	struct dirent *e;
	DIR *d;

	snprintf(path, sizeof path, "%s/stories", ui_root());
	mkdir(path, 0755);
	stories_path(path, sizeof path, NULL);
	mkdir(path, 0755);
	d = opendir(path);
	if (d) {
		while ((e = readdir(d))) {
			size_t n = strlen(e->d_name);
			char label[64];
			struct stat st;

			if (e->d_name[0] == '.')
				continue;
			stories_path(path, sizeof path, e->d_name);
			if (stat(path, &st))
				continue;
			if (S_ISDIR(st.st_mode)) {
				add_item(e->d_name, e->d_name, SUBFOLDER);
				continue;
			}
			if (n < 5 || (strcmp(e->d_name + n - 4, ".txt") &&
				      strcmp(e->d_name + n - 4, ".lua")) ||
			    !strcmp(e->d_name, "my-words.txt"))
				continue;
			snprintf(label, sizeof label, "%.*s", (int)(n - 4), e->d_name);
			add_item(label, path, STORY);
		}
		closedir(d);
	}
	qsort(items, item_count, sizeof items[0], by_label);
}

static void load_folder(void)
{
	item_count = 0;
	if (current == DISK) {
		int i;

		for (i = STORIES; i < folder_count; i++) {
			char index[12];

			snprintf(index, sizeof index, "%d", i);
			add_item(folder_names[i], index, PLACE);
		}
	} else if (current != GAMES) {
		load_stories();
	} else {
		add_item("Doom: Normal", "box", DOOM);
		add_item("Doom: Tall", "tall", DOOM);
		add_item("Doom: Wide", "wide", DOOM);
		add_item("Doom: Fit Screen", "fit", DOOM);
		add_item("Doom: Speed Test", "bench", DOOM_BENCH);
		add_item("Wolfenstein 3D: Normal", "box", WOLF);
		add_item("Wolfenstein 3D: Tall", "tall", WOLF);
		add_item("Wolfenstein 3D: Wide", "wide", WOLF);
		add_item("Wolfenstein 3D: Fit Screen", "fit", WOLF);
	}
	if (chosen >= item_count)
		chosen = item_count ? item_count - 1 : 0;
}

static void open_folder(int folder)
{
	current = folder;
	window_open = 1;
	subfolder[0] = 0;
	chosen = 0;
	top = 0;
	load_folder();
}

/* Stand-in icons for builds with no art folder. */
static void plain_icon(int x, int y, int size, int game)
{
	uint16_t body = game ? C_WINDOW : RGB(0x93, 0xaf, 0xd2);

	if (size > 16) {
		ui_fill(x + 2, y + 6, 12, 4, C_INK);
		ui_fill(x + 2, y + 9, 28, 19, C_INK);
		ui_fill(x + 3, y + 10, 26, 17, body);
	} else {
		ui_fill(x + 1, y + 1, 13, 14, C_INK);
		ui_fill(x + 2, y + 2, 11, 12, game ? C_WINDOW : C_WHITE);
		ui_fill(x + 4, y + 5, 7, 1, C_MID);
		ui_fill(x + 4, y + 8, 7, 1, C_MID);
	}
}

static void draw_icon(int x, int y, const struct ui_image *image, int size, int game)
{
	if (image->pixels) {
		ui_draw_image(x, y, image, 1);
	} else if (image == &folder_small) {
		ui_fill(x + 1, y + 3, 6, 2, C_INK);
		ui_fill(x + 1, y + 4, 14, 10, C_INK);
		ui_fill(x + 2, y + 5, 12, 8, RGB(0x93, 0xaf, 0xd2));
	} else {
		plain_icon(x, y, size, game);
	}
}

static void draw_desktop_icon(int folder)
{
	const char *name = folder_names[folder];
	int on = folder == (current == TRASH ? TRASH : DISK);
	int w = ui_text_width(name, strlen(name));
	int x = ICON_X;
	int y = icon_y(folder);
	int label_x = x + (ICON_W - w) / 2;
	int label_y = y + 34;

	draw_icon(x + (ICON_W - 32) / 2, y,
		  folder == DISK && disk_big.pixels ? &disk_big :
		  folder == TRASH && trash_big.pixels ? &trash_big : &folder_big, 32,
		  folder == DISK);
	ui_fill(label_x - 3, label_y, w + 6, 14, on ? C_SELECT : C_WHITE);
	ui_text(label_x, label_y - 3, name, strlen(name), on ? C_WHITE : C_INK);
}

static void draw_window(void)
{
	const char *title = strrchr(subfolder, '/');
	char info[96];
	int i;

	ui_fill(WIN_X + 3, WIN_Y + 3, WIN_W, WIN_H, C_SHADE);
	ui_fill(WIN_X - 1, WIN_Y - 1, WIN_W + 2, WIN_H + 2, C_INK);
	ui_fill(WIN_X, WIN_Y, WIN_W, WIN_H, C_WHITE);
	ui_title_bar(WIN_X, WIN_Y, WIN_W,
		     subfolder[0] ? (title ? title + 1 : subfolder) : folder_names[current]);

	if (notice[0])
		snprintf(info, sizeof info, "%s", notice);
	else
		snprintf(info, sizeof info, "%d item%s", item_count, item_count == 1 ? "" : "s");
	ui_fill(WIN_X, WIN_Y + 16, WIN_W, INFO_H, C_WINDOW);
	ui_fill(WIN_X, WIN_Y + 16 + INFO_H, WIN_W, 1, C_MID);
	ui_text(WIN_X + (WIN_W - ui_text_width(info, strlen(info))) / 2, WIN_Y + 13, info,
		strlen(info), C_INK);

	for (i = 0; i < VISIBLE && top + i < item_count; i++) {
		const struct item *it = &items[top + i];
		int y = LIST_Y + i * ROW_H;
		int on = top + i == chosen;

		if (on)
			ui_fill(WIN_X, y, WIN_W, ROW_H, C_SELECT);
		draw_icon(WIN_X + 6, y,
			  it->kind == SUBFOLDER || it->kind == PLACE ? &folder_small :
			  it->kind == STORY ? &doc_small : &app_small, 16,
			  it->kind == DOOM || it->kind == WOLF);
		ui_text(WIN_X + 28, y - 2, it->label, strlen(it->label), on ? C_WHITE : C_INK);
	}
}

static void paint(void)
{
	char right[32];
	int i;

	if (wallpaper.pixels)
		ui_draw_image(0, 0, &wallpaper, 1);
	else
		ui_fill(0, 0, SCREEN_W, SCREEN_H, C_DESKTOP);
	snprintf(right, sizeof right, open_count ? "Finder (%d open)" : "Finder", open_count);
	ui_menu_bar(menus, MENU_COUNT, right);
	for (i = 0; i < DESKTOP_ICONS; i++)
		draw_desktop_icon(i);
	if (window_open)
		draw_window();
}

/* Pocket Write leaves a note if Save As gave the open story a new path. */
static void read_new_path(int index)
{
	const char *dir = getenv("POCKET_TMP");
	struct open_app *app = &open_apps[index];
	char note[64];
	char *base;
	size_t n;
	FILE *file;

	snprintf(note, sizeof note, "%.40s/pocket-open.%d", dir ? dir : "/tmp", (int)app->pid);
	file = fopen(note, "r");
	if (!file)
		return;
	n = fread(app->key, 1, sizeof app->key - 1, file);
	app->key[n] = 0;
	fclose(file);
	unlink(note);
	base = strrchr(app->key, '/');
	snprintf(app->label, sizeof app->label, "%s", base ? base + 1 : app->key);
	n = strlen(app->label);
	if (n > 4)
		app->label[n - 4] = 0;
}

/* Waits until the program pauses itself or ends. */
static void watch_app(int index)
{
	int status;
	pid_t got;

	ui_wake_target(open_apps[index].pid);
	do {
		got = waitpid(open_apps[index].pid, &status, WUNTRACED);
	} while (got < 0 && errno == EINTR);
	ui_wake_target(0);
	read_new_path(index);
	if (got > 0 && WIFSTOPPED(status))
		return;
	if (index < open_count - 1)
		memmove(&open_apps[index], &open_apps[index + 1],
			sizeof open_apps[0] * (open_count - index - 1));
	open_count--;
}

static void resume_app(int index)
{
	ui_suspend();
	kill(open_apps[index].pid, SIGCONT);
	watch_app(index);
	ui_resume();
	load_folder();
}

/* Runs a program from the bin folder, or brings it back if already open. */
static void launch(const char *label, const char *key, char *const argv[], const char *dir,
		   const char *log)
{
	const char *bin = getenv("POCKET_BIN");
	char path[300];
	pid_t pid;
	int i;

	for (i = 0; i < open_count; i++) {
		if (!strcmp(open_apps[i].key, key)) {
			resume_app(i);
			return;
		}
	}
	if (open_count == MAX_OPEN) {
		snprintf(notice, sizeof notice, "Too many things open, close one first");
		return;
	}
	snprintf(path, sizeof path, "%s/%s", bin ? bin : "/usr/bin", argv[0]);
	ui_suspend();
	pid = fork();
	if (pid == 0) {
		setenv("POCKET_SWITCHER", "1", 1);
		if (dir && chdir(dir))
			_exit(126);
		if (log) {
			int fd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);

			if (fd >= 0) {
				dup2(fd, 1);
				dup2(fd, 2);
				close(fd);
			}
		}
		execv(path, argv);
		_exit(127);
	}
	if (pid > 0) {
		i = open_count++;
		open_apps[i].pid = pid;
		snprintf(open_apps[i].label, sizeof open_apps[i].label, "%s", label);
		snprintf(open_apps[i].key, sizeof open_apps[i].key, "%s", key);
		watch_app(i);
	}
	ui_resume();
	load_folder();
}

static void open_story(const char *label, const char *path)
{
	char *argv[] = { "pocketwrite", (char *)path, NULL };

	launch(label, path, argv, NULL, NULL);
}

static void open_doom(const char *label, const char *mode)
{
	char dir[300];
	char wad[320];
	char *argv[] = { "pdoom", "-iwad", wad, "-mb", "6", "-nogui", "-pmode", (char *)mode,
			 "-pblocks", "10", "-pdetail", "0", NULL };

	snprintf(dir, sizeof dir, "%s/doom", ui_root());
	/* Freedoom is free to share, so anyone can play without buying a copy. */
	snprintf(wad, sizeof wad, "%s/Doomu.wad", dir);
	if (access(wad, R_OK))
		snprintf(wad, sizeof wad, "%s/freedoom1.wad", dir);
	if (access(wad, R_OK)) {
		snprintf(notice, sizeof notice, "Doom needs a WAD in the doom folder");
		return;
	}
	launch(label, "doom", argv, dir, "play.log");
}

static void open_wolf(const char *label, const char *mode)
{
	char dir[300];
	char data[320];
	char *argv[] = { "pwolf", "-pmode", (char *)mode, NULL };

	snprintf(dir, sizeof dir, "%s/wolf3d", ui_root());
	snprintf(data, sizeof data, "%s/vswap.wl1", dir);
	if (access(data, R_OK)) {
		snprintf(notice, sizeof notice, "Wolf 3D needs its .wl1 files in the wolf3d folder");
		return;
	}
	launch(label, "wolf", argv, dir, "play.log");
}

static void speed_test(void)
{
	char *argv[] = { "pocket-doom", "bench", NULL };

	launch("Doom Speed Test", "speed-test", argv, NULL, NULL);
}

/* Lists everything open. Choosing one brings it back. */
static void show_open_apps(void)
{
	const char *names[MAX_OPEN + 1];
	int w = 80;
	int pick;
	int i;

	if (!open_count) {
		snprintf(notice, sizeof notice, "Nothing else is open");
		return;
	}
	names[0] = "Finder";
	for (i = 0; i < open_count; i++) {
		int tw = ui_text_width(open_apps[i].label, strlen(open_apps[i].label)) + 24;

		names[i + 1] = open_apps[i].label;
		if (tw > w)
			w = tw;
	}
	if (w > SCREEN_W - 8)
		w = SCREEN_W - 8;
	pick = ui_pick(SCREEN_W - w - 4, 16, w, names, open_count + 1, paint);
	if (pick > 0)
		resume_app(pick - 1);
}

/* Stories save themselves when asked. */
static void close_everything(void)
{
	while (open_count) {
		kill(open_apps[0].pid, SIGTERM);
		kill(open_apps[0].pid, SIGCONT);
		watch_app(0);
	}
}

static void new_story(void)
{
	char path[300];
	char name[32];
	struct stat st;
	int i;

	for (i = 1; i < 10000; i++) {
		snprintf(name, sizeof name, current == CODE ? "Program %d.lua" : "Story %d.txt", i);
		stories_path(path, sizeof path, name);
		if (stat(path, &st))
			break;
	}
	name[strlen(name) - 4] = 0;
	open_story(name, path);
}

/* Characters FAT cannot store in a name. */
static int name_char_ok(int c)
{
	return c >= 32 && c < 127 && !strchr("\\/:*?\"<>|", c);
}

/* Asks for a name. Returns 1 if one was entered. */
static int ask_name(const char *question, char *name, int size)
{
	struct ui_event ev;
	int len = strlen(name);

	for (;;) {
		char shown[72];
		int got;

		paint();
		ui_fill(103, 83, 280, 62, C_SHADE);
		ui_fill(99, 79, 282, 64, C_INK);
		ui_fill(100, 80, 280, 62, C_WINDOW);
		ui_text(110, 81, question, strlen(question), C_INK);
		ui_fill(110, 102, 260, 18, C_INK);
		ui_fill(111, 103, 258, 16, C_WHITE);
		snprintf(shown, sizeof shown, "%s|", name);
		ui_text(114, 101, shown, strlen(shown), C_INK);
		ui_text(110, 121, "Enter to accept, Esc to cancel", 30, C_SHADE);
		ui_present();
		got = ui_wait(&ev, 1000);
		if (got < 0)
			return 0;
		if (!got)
			continue;
		if (ev.key == K_ESC)
			return 0;
		if (ev.key == K_ENTER && len)
			break;
		if (ev.key == K_BACKSPACE && len)
			name[--len] = 0;
		if (ev.key == K_CHAR && len < size - 1 && name_char_ok(ev.ch) &&
		    ui_text_width(name, len) < 230) {
			name[len++] = ev.ch;
			name[len] = 0;
		}
	}
	/* FAT drops trailing spaces and dots, so the folder would not match its name. */
	while (len && (name[len - 1] == ' ' || name[len - 1] == '.'))
		name[--len] = 0;
	return len > 0;
}

static void new_disk_folder(void)
{
	char name[48] = "";
	char path[300];
	int i;

	if (folder_count == MAX_FOLDERS) {
		snprintf(notice, sizeof notice, "Pocket HD is full");
		return;
	}
	if (!ask_name("Name for the new folder:", name, sizeof name))
		return;
	snprintf(path, sizeof path, "%s/desktop/%s", ui_root(), name);
	if (mkdir(path, 0755)) {
		snprintf(notice, sizeof notice, "Could not create that folder");
		return;
	}
	sync();
	load_desktop();
	open_folder(DISK);
	for (i = FIXED_FOLDERS; i < folder_count; i++) {
		if (!strcmp(folder_names[i], name))
			chosen = i - STORIES;
	}
}

static void new_folder(void)
{
	char name[48] = "";
	char path[300];
	int i;

	if (!window_open || current == DISK) {
		new_disk_folder();
		return;
	}
	if (current == GAMES || current == TRASH)
		open_folder(STORIES);
	if (!ask_name("Name for the new folder:", name, sizeof name))
		return;
	stories_path(path, sizeof path, name);
	if (mkdir(path, 0755)) {
		snprintf(notice, sizeof notice, "Could not create that folder");
		return;
	}
	sync();
	load_folder();
	for (i = 0; i < item_count; i++) {
		if (!strcmp(items[i].label, name))
			chosen = i;
	}
}

static void close_folder(void)
{
	char *cut = strrchr(subfolder, '/');

	if (!subfolder[0]) {
		if (current < STORIES) {
			window_open = 0;
		} else {
			int was = current;

			open_folder(DISK);
			chosen = was - STORIES;
		}
		return;
	}
	if (cut)
		*cut = 0;
	else
		subfolder[0] = 0;
	chosen = 0;
	top = 0;
	load_folder();
}

static void enter_subfolder(const char *name)
{
	size_t used = strlen(subfolder);

	if (used + strlen(name) + 2 >= sizeof subfolder) {
		snprintf(notice, sizeof notice, "Folders are nested too deeply");
		return;
	}
	if (used)
		strcat(subfolder, "/");
	strcat(subfolder, name);
	chosen = 0;
	top = 0;
	load_folder();
}

static void open_chosen(void)
{
	const struct item *it;

	if (!window_open) {
		open_folder(current);
		return;
	}
	if (!item_count)
		return;
	it = &items[chosen];
	if (it->kind == PLACE)
		open_folder(atoi(it->arg));
	else if (it->kind == SUBFOLDER)
		enter_subfolder(it->arg);
	else if (current == TRASH)
		snprintf(notice, sizeof notice, "Use Move To to bring it out of the Trash first");
	else if (it->kind == STORY)
		open_story(it->label, it->arg);
	else if (it->kind == DOOM)
		open_doom(it->label, it->arg);
	else if (it->kind == WOLF)
		open_wolf(it->label, it->arg);
	else
		speed_test();
}

static void show_target(int x, int y, const char *message)
{
	ui_fill(0, 0, SCREEN_W, SCREEN_H, C_WHITE);
	ui_text((SCREEN_W - ui_text_width(message, strlen(message))) / 2, 100, message,
		strlen(message), C_INK);
	ui_fill(x - 10, y, 21, 1, C_INK);
	ui_fill(x, y - 10, 1, 21, C_INK);
	ui_present();
}

/* Three taps fix any offset, scale, tilt or swapped axes in the touchscreen. */
static void calibrate(void)
{
	static const int targets[6] = { 40, 30, 600, 40, 320, 210 };
	struct ui_event ev;
	int raw[6];
	int i;

	for (i = 0; i < 3; i++) {
		show_target(targets[i * 2], targets[i * 2 + 1], "Tap the center of the cross");
		for (;;) {
			int got = ui_wait(&ev, 1000);
			int j;

			if (got < 0 || (got && ev.key == K_ESC))
				return;
			if (!got || ev.key != K_TAP)
				continue;
			/* The crosses are far apart, so a nearby tap is a repeat of the last one. */
			for (j = 0; j < i; j++) {
				if (abs(ev.raw_x - raw[j * 2]) + abs(ev.raw_y - raw[j * 2 + 1]) < 40)
					break;
			}
			if (j == i)
				break;
		}
		raw[i * 2] = ev.raw_x;
		raw[i * 2 + 1] = ev.raw_y;
	}
	ui_set_calibration(targets, raw);
}

/* True if the path, or anything inside it, is open right now. */
static int is_open(const char *path)
{
	size_t n = strlen(path);
	int i;

	for (i = 0; i < open_count; i++) {
		if (!strncmp(open_apps[i].key, path, n) &&
		    (!open_apps[i].key[n] || open_apps[i].key[n] == '/'))
			return 1;
	}
	return 0;
}

/* Finds what the Item menu acts on: the chosen row. */
static int selected_path(char *out, size_t size, int *is_story)
{
	const struct item *it;

	*is_story = 0;
	if (!window_open) {
		snprintf(notice, sizeof notice, "That one has to stay");
		return 0;
	}
	if (current == GAMES || !item_count) {
		snprintf(notice, sizeof notice, "Choose a story or folder first");
		return 0;
	}
	it = &items[chosen];
	*is_story = it->kind == STORY;
	if (it->kind == PLACE) {
		int folder = atoi(it->arg);

		if (folder < FIXED_FOLDERS) {
			snprintf(notice, sizeof notice, "That one has to stay");
			return 0;
		}
		snprintf(out, size, "%s/desktop/%s", ui_root(), folder_names[folder]);
	} else if (it->kind == SUBFOLDER)
		stories_path(out, size, it->arg);
	else
		snprintf(out, size, "%s", it->arg);
	return 1;
}

/* Moves a story with its backup copies, or a folder. Returns 0 if it worked. */
static int move_path(const char *from, const char *to, int is_story)
{
	static const char *const sides[] = { ".bak", ".autosave", ".tmp" };
	unsigned i;

	if (strcasecmp(from, to) && !access(to, F_OK)) {
		snprintf(notice, sizeof notice, "That name is already in use");
		return -1;
	}
	if (rename(from, to)) {
		snprintf(notice, sizeof notice, "Could not move that, the card said no");
		return -1;
	}
	for (i = 0; is_story && i < sizeof sides / sizeof sides[0]; i++) {
		char a[320];
		char b[320];

		snprintf(a, sizeof a, "%s%s", from, sides[i]);
		snprintf(b, sizeof b, "%s%s", to, sides[i]);
		if (!access(a, F_OK))
			rename(a, b);
	}
	sync();
	return 0;
}

/* Reloads the view after an item changed, keeping the same place when possible. */
static void refresh_after_change(void)
{
	char name[48];

	snprintf(name, sizeof name, "%s", folder_names[current]);
	load_desktop();
	if (current >= folder_count || strcmp(name, folder_names[current]))
		current = DISK;
	load_folder();
}

static int blocked_by_open(const char *path)
{
	if (!is_open(path))
		return 0;
	snprintf(notice, sizeof notice, "Close it first, it is still open");
	return 1;
}

static void rename_item(void)
{
	char path[300];
	char to[360];
	char name[48];
	char *slash;
	int is_story;
	size_t n;

	if (!selected_path(path, sizeof path, &is_story) || blocked_by_open(path))
		return;
	slash = strrchr(path, '/');
	snprintf(name, sizeof name, "%s", slash + 1);
	n = strlen(name);
	if (is_story && n > 4)
		name[n - 4] = 0;
	if (!ask_name("New name:", name, sizeof name))
		return;
	*slash = 0;
	snprintf(to, sizeof to, "%s/%s%s", path, name, is_story ? slash + 1 + strlen(slash + 1) - 4 : "");
	*slash = '/';
	if (!move_path(path, to, is_story))
		refresh_after_change();
}

/* Names the place an item lands in the Trash, adding a number if it is taken. */
static void trash_name(char *out, size_t size, const char *name, int is_story)
{
	const char *ext = is_story ? name + strlen(name) - 4 : "";
	char base[64];
	size_t n;
	int i;

	snprintf(base, sizeof base, "%s", name);
	n = strlen(base);
	if (is_story && n > 4)
		base[n - 4] = 0;
	snprintf(out, size, "%s/trash/%s%s", ui_root(), base, ext);
	for (i = 2; !access(out, F_OK); i++)
		snprintf(out, size, "%s/trash/%s %d%s", ui_root(), base, i, ext);
}

static void trash_item(void)
{
	char path[300];
	char to[340];
	int is_story;

	if (!selected_path(path, sizeof path, &is_story) || blocked_by_open(path))
		return;
	if (current == TRASH) {
		snprintf(notice, sizeof notice, "That is already in the Trash");
		return;
	}
	trash_name(to, sizeof to, strrchr(path, '/') + 1, is_story);
	if (!move_path(path, to, is_story))
		refresh_after_change();
}

#define MAX_PLACES 12

/* Lists the folders something can move to. Returns how many there are. */
static int list_places(char paths[][300], char labels[][64])
{
	struct dirent *e;
	DIR *d;
	int n = 0;
	int i;

	snprintf(paths[n], 300, "%s/stories", ui_root());
	strcpy(labels[n++], "Stories");
	snprintf(paths[n], 300, "%s/code", ui_root());
	strcpy(labels[n++], "Code");
	for (i = FIXED_FOLDERS; i < folder_count && n < MAX_PLACES; i++) {
		snprintf(paths[n], 300, "%.200s/desktop/%.47s", ui_root(), folder_names[i]);
		snprintf(labels[n++], 64, "%s", folder_names[i]);
	}
	d = opendir(paths[0]);
	if (!d)
		return n;
	while ((e = readdir(d)) && n < MAX_PLACES) {
		char path[300];
		struct stat st;

		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof path, "%.250s/%.40s", paths[0], e->d_name);
		if (stat(path, &st) || !S_ISDIR(st.st_mode))
			continue;
		strcpy(paths[n], path);
		snprintf(labels[n++], 64, "Stories: %.40s", e->d_name);
	}
	closedir(d);
	return n;
}

static void move_item(void)
{
	static char paths[MAX_PLACES][300];
	static char labels[MAX_PLACES][64];
	const char *names[MAX_PLACES];
	char path[300];
	char to[340];
	const char *name;
	int is_story;
	int count;
	int pick;
	int i;
	size_t n;

	if (!selected_path(path, sizeof path, &is_story) || blocked_by_open(path))
		return;
	count = list_places(paths, labels);
	for (i = 0; i < count; i++)
		names[i] = labels[i];
	pick = ui_pick(170, 40, 300, names, count, paint);
	if (pick < 0)
		return;
	n = strlen(path);
	if (!strncmp(paths[pick], path, n) && (!paths[pick][n] || paths[pick][n] == '/')) {
		snprintf(notice, sizeof notice, "A folder cannot go inside itself");
		return;
	}
	name = strrchr(path, '/') + 1;
	snprintf(to, sizeof to, "%s/%s", paths[pick], name);
	if (!strcmp(to, path)) {
		snprintf(notice, sizeof notice, "It is already there");
		return;
	}
	if (!move_path(path, to, is_story))
		refresh_after_change();
}

static void remove_tree(const char *path)
{
	struct stat st;
	struct dirent *e;
	DIR *d;

	if (lstat(path, &st))
		return;
	if (!S_ISDIR(st.st_mode)) {
		unlink(path);
		return;
	}
	d = opendir(path);
	if (d) {
		while ((e = readdir(d))) {
			char inner[400];

			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
				continue;
			snprintf(inner, sizeof inner, "%.280s/%.100s", path, e->d_name);
			remove_tree(inner);
		}
		closedir(d);
	}
	rmdir(path);
}

/* Cancel comes first, so a stray Enter cannot delete anything. */
static void empty_trash(void)
{
	static const char *const choices[] = { "Cancel", "Delete everything in the Trash" };
	char path[300];
	struct dirent *e;
	DIR *d;

	if (ui_pick(150, 80, 320, choices, 2, paint) != 1)
		return;
	snprintf(path, sizeof path, "%s/trash", ui_root());
	d = opendir(path);
	if (!d)
		return;
	while ((e = readdir(d))) {
		char inner[400];

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		snprintf(inner, sizeof inner, "%.280s/%.100s", path, e->d_name);
		remove_tree(inner);
	}
	closedir(d);
	sync();
	load_folder();
}

static int run_menu(int start)
{
	int in_trash = window_open && current == TRASH;
	int pick;

	/* Away from Power Off, and only where it makes sense. */
	menus[ITEM_MENU].labels[ITEM_LAST] = in_trash ? "Empty Trash" : "Move to Trash";
	pick = ui_run_menus(menus, MENU_COUNT, start, paint);

	switch (pick) {
	case 0: open_chosen(); break;
	case 1:
		if (current == DISK || current == GAMES || current == TRASH)
			open_folder(STORIES);
		window_open = 1;
		new_story();
		break;
	case 2: new_folder(); break;
	case 3: close_folder(); break;
	case 256: calibrate(); break;
	case 257: ui_screen_dialog(paint); break;
	case 258: return 1;
	case 259: return 2;
	case 512: rename_item(); break;
	case 513: move_item(); break;
	case 514:
		if (in_trash)
			empty_trash();
		else
			trash_item();
		break;
	}
	return 0;
}

static int handle_tap(const struct ui_event *ev)
{
	int i;

	if (ev->x < 0)
		return 0;
	if (ev->y < 16) {
		int m = ui_menu_at(menus, MENU_COUNT, ev->x);

		if (ev->x >= SCREEN_W - 140)
			show_open_apps();
		return m >= 0 ? run_menu(m) : 0;
	}
	/* A generous target: the box is small for a stylus. */
	if (window_open && ev->x >= WIN_X - 6 && ev->x < WIN_X + 34 && ev->y >= WIN_Y - 6 &&
	    ev->y < WIN_Y + 18) {
		close_folder();
		return 0;
	}
	for (i = 0; i < DESKTOP_ICONS; i++) {
		int y = icon_y(i);

		if (ev->x >= ICON_X && ev->x < ICON_X + ICON_W && ev->y >= y && ev->y < y + 50) {
			open_folder(i);
			return 0;
		}
	}
	if (window_open && ev->x >= WIN_X && ev->x < WIN_X + WIN_W && ev->y >= LIST_Y) {
		int row = top + (ev->y - LIST_Y) / ROW_H;

		if (row >= item_count)
			return 0;
		if (row == chosen)
			open_chosen();
		else
			chosen = row;
	}
	return 0;
}

static void select_desktop_icon(void)
{
	current = current == DISK ? TRASH : DISK;
	chosen = 0;
	load_folder();
}

/* Moves between the desktop icons, or between the folders inside Pocket HD. */
static void switch_folder(int step)
{
	int places = folder_count - STORIES;

	if (current < STORIES)
		open_folder(current == DISK ? TRASH : DISK);
	else
		open_folder(STORIES + (current - STORIES + step + places) % places);
}

static int handle(const struct ui_event *ev)
{
	notice[0] = 0;
	switch (ev->key) {
	case K_BACKSPACE:
		close_folder();
		break;
	case K_DELETE:
		trash_item();
		break;
	case K_UP:
		if (!window_open)
			select_desktop_icon();
		else if (chosen > 0)
			chosen--;
		break;
	case K_DOWN:
		if (!window_open)
			select_desktop_icon();
		else if (chosen < item_count - 1)
			chosen++;
		break;
	case K_LEFT:
		switch_folder(-1);
		break;
	case K_RIGHT:
		switch_folder(1);
		break;
	case K_TAB:
		show_open_apps();
		break;
	case K_ENTER:
		open_chosen();
		break;
	case K_ESC:
		return run_menu(0);
	case K_TAP:
		return handle_tap(ev);
	default:
		break;
	}
	return 0;
}

int main(void)
{
	struct ui_event ev;
	int action = 0;

	if (ui_open()) {
		fprintf(stderr, "could not open the screen\n");
		return 1;
	}
	ui_own_console();
	ui_screen_load();
	ui_load_image("wallpaper", &wallpaper);
	ui_load_image("folder", &folder_big);
	ui_load_image("hard-disk", &disk_big);
	ui_load_image("trash", &trash_big);
	ui_load_image("document-16", &doc_small);
	ui_load_image("folder-16", &folder_small);
	ui_load_image("application-16", &app_small);
	load_desktop();
	open_folder(DISK);
	window_open = 0;
	if (!ui_has_calibration() && !getenv("POCKET_KEYS"))
		calibrate();

	while (!action) {
		int got;

		if (chosen < top)
			top = chosen;
		if (chosen >= top + VISIBLE)
			top = chosen - VISIBLE + 1;
		paint();
		ui_present();
		/* Only the battery reading moves, so other idle time draws nothing. */
		do {
			got = ui_wait(&ev, 1000);
		} while (!got && !ui_battery_changed());
		if (got < 0)
			break;
		if (got)
			action = handle(&ev);
	}
	close_everything();
	ui_give_console_back();
	ui_close();
	return action == 2 ? EXIT_POWER_OFF : 0;
}
