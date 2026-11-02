/*
 * Pocket Write: the plain text editor for stories.
 *
 * Usage: write file.txt
 *
 * Saving: the text goes to a temporary file first, is flushed to the card, and
 * only then replaces the story, with the previous version kept as file.bak.
 * While there are unsaved changes, an autosave file is written after a few
 * seconds of quiet and picked up again on the next open.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "lock.h"
#include "monocypher.h"
#include "ui.h"

#define TEXT_LEFT 10
#define TEXT_TOP 35
#define TEXT_BOTTOM 224
#define TEXT_WIDTH (SCREEN_W - TEXT_LEFT * 2 - 14)
#define LINE_HEIGHT 14
#define VISIBLE_LINES ((TEXT_BOTTOM - TEXT_TOP) / LINE_HEIGHT)
#define AUTOSAVE_QUIET_MS 5000
#define BLINK_MS 500
#define MAX_UNDO 256
#define MAX_WORD 64

/* The story, as a gap buffer. */
static char *buf;
static int cap;
static int gap_start;
static int gap_end;

/* Line starts from the last word wrap. */
static int *lines;
static int line_cap;
static int line_count;
static int word_count;

static int cursor;
static int anchor = -1;		/* other end of the selection, or -1 */
static int top_line;
static int want_x = -1;
static int caret_on = 1;
static int text_changed = 1;	/* the line table needs rebuilding */

/* Where the caret sits, and the pixels under it, so blinking redraws nothing else. */
static int caret_shown;
static int caret_x;
static int caret_y;
static uint16_t under_caret[LINE_HEIGHT];

static int dirty;
static int autosave_due;
static struct lock story_lock;
static int code_mode;		/* a .lua file: no spelling, and it can be run */
static volatile sig_atomic_t running_pid;
static int scrub_copies;	/* a save replaces copies that must not linger */
static char path[512];
static char status[96];

static char *clip;
static int clip_len;

struct edit {
	int pos;
	char *removed;
	int removed_len;
	char *added;
	int added_len;
	int cursor_before;
};

static struct edit undo_log[MAX_UNDO];
static int undo_count;

static const char *dict;
static size_t dict_len;
static char **user_words;
static int user_count;
static char user_path[560];

static int searching;
static char search[64];
static int search_len;

static struct ui_menu menus[] = {
	{ "File", { "Save", "Save As...", "Password...", "Brightness & Contrast...", "Close", "Back to Finder" }, "sepbqw" },
	{ "Edit", { "Undo", "Cut", "Copy", "Paste", "Select All" }, "zxcva" },
	{ "Find", { "Find...", "Find Next" }, "fg" },
	{ "Spelling", { "Suggest Fixes", "Add to My Words" }, "ld" },
};

#define MENU_COUNT ((int)(sizeof menus / sizeof menus[0]))
#define LAST_MENU (MENU_COUNT - 1)
#define MAX_SUGGESTIONS 8

static void *must_alloc(void *p, size_t n)
{
	p = realloc(p, n ? n : 1);
	if (!p) {
		ui_close();
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	return p;
}

static int text_len(void)
{
	return cap - (gap_end - gap_start);
}

static char char_at(int i)
{
	return i < gap_start ? buf[i] : buf[i + (gap_end - gap_start)];
}

static void move_gap(int pos)
{
	if (pos < gap_start) {
		int n = gap_start - pos;

		memmove(buf + gap_end - n, buf + pos, n);
		gap_start -= n;
		gap_end -= n;
	} else if (pos > gap_start) {
		int n = pos - gap_start;

		memmove(buf + gap_start, buf + gap_end, n);
		gap_start += n;
		gap_end += n;
	}
}

static void raw_insert(int pos, const char *s, int n)
{
	if (gap_end - gap_start < n) {
		int grow = n + cap / 2 + 4096;
		int tail = cap - gap_end;

		buf = must_alloc(buf, cap + grow);
		memmove(buf + gap_end + grow, buf + gap_end, tail);
		gap_end += grow;
		cap += grow;
	}
	move_gap(pos);
	memcpy(buf + gap_start, s, n);
	gap_start += n;
}

static void raw_delete(int pos, int n)
{
	move_gap(pos);
	gap_end += n;
}

static char *copy_range(int pos, int n)
{
	char *out = must_alloc(NULL, n + 1);
	int i;

	for (i = 0; i < n; i++)
		out[i] = char_at(pos + i);
	out[n] = 0;
	return out;
}

static void drop_edit(struct edit *e)
{
	free(e->removed);
	free(e->added);
}

/* All text changes pass through here, so each one can be undone. */
static void change(int pos, int remove, const char *add, int add_len)
{
	struct edit *last = undo_count ? &undo_log[undo_count - 1] : NULL;

	/* A word undoes in one step, along with the space typed before it. */
	if (!remove && add_len == 1 && add[0] != ' ' && add[0] != '\n' && last &&
	    !last->removed_len && last->pos + last->added_len == pos) {
		last->added = must_alloc(last->added, last->added_len + 2);
		last->added[last->added_len++] = add[0];
		last->added[last->added_len] = 0;
	} else {
		struct edit *e;

		if (undo_count == MAX_UNDO) {
			drop_edit(&undo_log[0]);
			memmove(undo_log, undo_log + 1, sizeof undo_log[0] * (MAX_UNDO - 1));
			undo_count--;
		}
		e = &undo_log[undo_count++];
		e->pos = pos;
		e->removed = copy_range(pos, remove);
		e->removed_len = remove;
		e->added = must_alloc(NULL, add_len + 1);
		memcpy(e->added, add, add_len);
		e->added[add_len] = 0;
		e->added_len = add_len;
		e->cursor_before = cursor;
	}
	raw_delete(pos, remove);
	raw_insert(pos, add, add_len);
	cursor = pos + add_len;
	dirty = 1;
	autosave_due = 1;
	want_x = -1;
	text_changed = 1;
}

static void undo(void)
{
	struct edit *e;

	if (!undo_count) {
		strcpy(status, "Nothing to undo");
		return;
	}
	e = &undo_log[--undo_count];
	raw_delete(e->pos, e->added_len);
	raw_insert(e->pos, e->removed, e->removed_len);
	cursor = e->cursor_before;
	anchor = -1;
	drop_edit(e);
	dirty = 1;
	autosave_due = 1;
	want_x = -1;
	text_changed = 1;
}

static int is_word_char(char c)
{
	return isalpha((unsigned char)c) || c == '\'';
}

static void add_line(int start)
{
	if (line_count == line_cap) {
		line_cap = line_cap ? line_cap * 2 : 256;
		lines = must_alloc(lines, line_cap * sizeof *lines);
	}
	lines[line_count++] = start;
}

/* Rebuilds the line table and word count. Runs after every edit. */
static void wrap_text(void)
{
	int n = text_len();
	int pos = 0;
	int in_word = 0;
	int i;

	line_count = 0;
	word_count = 0;
	add_line(0);
	while (pos < n) {
		int width = 0;
		int space = -1;

		for (i = pos; i < n; i++) {
			char c = char_at(i);

			if (c == '\n') {
				pos = i + 1;
				add_line(pos);
				break;
			}
			if (c == ' ' || c == '\t')
				space = i;
			width += ui_char_width(c);
			if (width > TEXT_WIDTH) {
				/* Break after the last space, or mid-word if there was none. */
				pos = space >= pos ? space + 1 : (i > pos ? i : i + 1);
				add_line(pos);
				break;
			}
		}
		if (i == n)
			break;
	}
	for (i = 0; i < n; i++) {
		if (isspace((unsigned char)char_at(i))) {
			in_word = 0;
		} else if (!in_word) {
			in_word = 1;
			word_count++;
		}
	}
}

static int line_of(int pos)
{
	int lo = 0;
	int hi = line_count - 1;

	while (lo < hi) {
		int mid = (lo + hi + 1) / 2;

		if (lines[mid] <= pos)
			lo = mid;
		else
			hi = mid - 1;
	}
	return lo;
}

static int line_end(int line)
{
	int end = line + 1 < line_count ? lines[line + 1] : text_len();

	if (end > lines[line] && line + 1 < line_count) {
		char c = char_at(end - 1);

		if (c == '\n' || c == ' ')
			end--;
	}
	return end;
}

static int x_of(int pos)
{
	int line = line_of(pos);
	int x = 0;
	int i;

	for (i = lines[line]; i < pos; i++)
		x += ui_char_width(char_at(i));
	return x;
}

static int pos_at(int line, int x)
{
	int end = line_end(line);
	int i = lines[line];
	int w = 0;

	for (; i < end; i++) {
		int cw = ui_char_width(char_at(i));

		if (w + cw / 2 >= x)
			break;
		w += cw;
	}
	return i;
}

static int dict_has(const char *word)
{
	size_t lo = 0;
	size_t hi = dict_len;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		size_t start = mid;
		size_t end;
		const char *a;
		const char *b = word;

		while (start > lo && dict[start - 1] != '\n')
			start--;
		end = start;
		while (end < dict_len && dict[end] != '\n')
			end++;
		a = dict + start;
		while (a < dict + end && *b && *a == *b) {
			a++;
			b++;
		}
		if (a == dict + end && !*b)
			return 1;
		if (a < dict + end && (!*b || (unsigned char)*a > (unsigned char)*b))
			hi = start;
		else
			lo = end + 1;
	}
	return 0;
}

static int user_has(const char *word)
{
	int i;

	for (i = 0; i < user_count; i++) {
		if (!strcmp(user_words[i], word))
			return 1;
	}
	return 0;
}

static int word_ok(const char *word)
{
	char copy[MAX_WORD];
	size_t n = strlen(word);

	if (dict_has(word) || user_has(word))
		return 1;
	/* The dictionary leaves out possessives: dragon's checks as dragon. */
	if (n > 2 && n < sizeof copy && !strcmp(word + n - 2, "'s")) {
		memcpy(copy, word, n - 2);
		copy[n - 2] = 0;
		return dict_has(copy) || user_has(copy);
	}
	return 0;
}

/* Lower-cases the word at [start,end), trimming outer quotes. Returns its length or 0. */
static int clean_word(int *start, int *end, char *out)
{
	int s = *start;
	int e = *end;
	int n = 0;
	int i;

	while (s < e && char_at(s) == '\'')
		s++;
	while (e > s && char_at(e - 1) == '\'')
		e--;
	if (e - s < 2 || e - s >= MAX_WORD)
		return 0;
	for (i = s; i < e; i++)
		out[n++] = tolower((unsigned char)char_at(i));
	out[n] = 0;
	*start = s;
	*end = e;
	return n;
}

static void underline(int x, int y, int w)
{
	uint16_t *px = ui_pixels();
	int i;

	for (i = 0; i < w; i++) {
		int py = y + (i % 4 < 2 ? 0 : 1);

		if (x + i < SCREEN_W && py < SCREEN_H)
			px[py * SCREEN_W + x + i] = C_MISSPELT;
	}
}

static void check_line(int line, int y)
{
	int end = line + 1 < line_count ? lines[line + 1] : text_len();
	int i = lines[line];
	char word[MAX_WORD];

	if (!dict || code_mode)
		return;
	while (i < end) {
		int s;
		int e;
		int ws;
		int we;

		if (!is_word_char(char_at(i))) {
			i++;
			continue;
		}
		s = i;
		while (i < end && is_word_char(char_at(i)))
			i++;
		e = i;
		/* A word stays unmarked while it is still being typed. */
		if (cursor >= s && cursor <= e)
			continue;
		ws = s;
		we = e;
		if (clean_word(&ws, &we, word) && !word_ok(word)) {
			int x = TEXT_LEFT;
			int j;
			int w = 0;

			for (j = lines[line]; j < ws; j++)
				x += ui_char_width(char_at(j));
			for (j = ws; j < we; j++)
				w += ui_char_width(char_at(j));
			underline(x, y + LINE_HEIGHT - 2, w);
		}
	}
}

static void selection(int *from, int *to)
{
	*from = anchor < cursor ? anchor : cursor;
	*to = anchor < cursor ? cursor : anchor;
}

static void draw_line(int line, int y)
{
	char row[300];
	int start = lines[line];
	int end = line_end(line);
	int n = end - start;
	int i;

	if (n > (int)sizeof row)
		n = sizeof row;
	for (i = 0; i < n; i++)
		row[i] = char_at(start + i);

	if (anchor >= 0 && anchor != cursor) {
		int from;
		int to;
		int a;
		int b;

		selection(&from, &to);
		a = from > start ? from : start;
		b = to < start + n ? to : start + n;
		if (a < b) {
			int ax = TEXT_LEFT + ui_text_width(row, a - start);
			int bw = ui_text_width(row + (a - start), b - a);

			ui_text(TEXT_LEFT, y - 3, row, a - start, C_INK);
			ui_fill(ax, y, bw, LINE_HEIGHT, C_SELECT);
			ui_text(ax, y - 3, row + (a - start), b - a, C_WHITE);
			ui_text(ax + bw, y - 3, row + (b - start), n - (b - start), C_INK);
			check_line(line, y);
			return;
		}
	}
	ui_text(TEXT_LEFT, y - 3, row, n, C_INK);
	check_line(line, y);
}

static void draw_scrollbar(void)
{
	int x = SCREEN_W - 14;
	int h = TEXT_BOTTOM - TEXT_TOP;
	int thumb_h = line_count > VISIBLE_LINES ? h * VISIBLE_LINES / line_count : h;
	int thumb_y;

	ui_fill(x, TEXT_TOP, 14, h, C_TITLE);
	ui_fill(x, TEXT_TOP, 1, h, C_INK);
	if (thumb_h >= h)
		return;
	if (thumb_h < 10)
		thumb_h = 10;
	thumb_y = TEXT_TOP + (h - thumb_h) * top_line / (line_count - VISIBLE_LINES);
	ui_fill(x + 2, thumb_y, 11, thumb_h, C_WINDOW);
	ui_fill(x + 2, thumb_y, 11, 1, C_WHITE);
	ui_fill(x + 2, thumb_y + thumb_h - 1, 11, 1, C_MID);
}

static void scroll_to_cursor(void)
{
	int line = line_of(cursor);

	if (line < top_line)
		top_line = line;
	if (line >= top_line + VISIBLE_LINES)
		top_line = line - VISIBLE_LINES + 1;
}

static void paint(void)
{
	char info[96];
	char *name = strrchr(path, '/');
	const char *state;
	int i;

	ui_menu_bar(menus, MENU_COUNT, code_mode ? "Pocket Code" : "Pocket Write");
	snprintf(info, sizeof info, "%s%.60s", dirty ? "* " : "", name ? name + 1 : path);
	ui_title_bar(0, 17, SCREEN_W, info);
	ui_fill(0, TEXT_TOP, SCREEN_W, TEXT_BOTTOM - TEXT_TOP, C_WHITE);
	for (i = 0; i < VISIBLE_LINES && top_line + i < line_count; i++)
		draw_line(top_line + i, TEXT_TOP + i * LINE_HEIGHT);
	draw_scrollbar();
	caret_shown = 0;
	if (!searching && (anchor < 0 || anchor == cursor)) {
		int line = line_of(cursor);

		if (line >= top_line && line < top_line + VISIBLE_LINES) {
			caret_shown = 1;
			caret_x = TEXT_LEFT + x_of(cursor);
			caret_y = TEXT_TOP + (line - top_line) * LINE_HEIGHT;
			for (i = 0; i < LINE_HEIGHT - 1; i++)
				under_caret[i] = ui_pixels()[(caret_y + i) * SCREEN_W + caret_x];
			if (caret_on)
				ui_fill(caret_x, caret_y, 1, LINE_HEIGHT - 1, C_INK);
		}
	}
	ui_fill(0, TEXT_BOTTOM, SCREEN_W, SCREEN_H - TEXT_BOTTOM, C_WINDOW);
	ui_fill(0, TEXT_BOTTOM, SCREEN_W, 1, C_INK);
	if (searching) {
		snprintf(info, sizeof info, "Find: %.*s_", search_len, search);
		ui_text(8, TEXT_BOTTOM - 2, info, strlen(info), C_INK);
	} else {
		if (code_mode)
			snprintf(info, sizeof info, "Line %d of %d", line_of(cursor) + 1, line_count);
		else
			snprintf(info, sizeof info, "%d words   Line %d of %d%s", word_count,
				 line_of(cursor) + 1, line_count,
				 story_lock.on ? "   Password on" : "");
		ui_text(8, TEXT_BOTTOM - 2, info, strlen(info), C_INK);
		/* Always says where the work stands, unless there is news to show. */
		state = status[0] ? status : !dirty ? "Saved" :
			autosave_due ? "Not saved yet" : "Autosaved";
		ui_text(SCREEN_W - 8 - ui_text_width(state, strlen(state)), TEXT_BOTTOM - 2,
			state, strlen(state), C_SHADE);
	}
}

static void draw(void)
{
	paint();
	ui_present();
}

static void show_caret_state(void)
{
	uint16_t *px = ui_pixels();
	int i;

	if (!caret_shown)
		return;
	for (i = 0; i < LINE_HEIGHT - 1; i++)
		px[(caret_y + i) * SCREEN_W + caret_x] = caret_on ? C_INK : under_caret[i];
	ui_present();
}

static int write_file(const char *name)
{
	int n = text_len();
	int size = n;
	uint8_t *sealed = NULL;
	const void *data = buf;
	int fd;
	int ok;

	move_gap(n);
	if (story_lock.on) {
		size = n + LOCK_HEADER;
		sealed = must_alloc(NULL, size);
		if (lock_seal(&story_lock, buf, n, sealed)) {
			free(sealed);
			errno = EIO;
			return -1;
		}
		data = sealed;
	}
	fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		free(sealed);
		return -1;
	}
	ok = write(fd, data, size) == size && !fsync(fd);
	if (close(fd))
		ok = 0;
	free(sealed);
	return ok ? 0 : -1;
}

static void sync_folder(void)
{
	char copy[sizeof path];
	int fd;

	strcpy(copy, path);
	fd = open(dirname(copy), O_RDONLY);
	if (fd >= 0) {
		fsync(fd);
		close(fd);
	}
}

static void side_path(char *out, size_t size, const char *suffix)
{
	snprintf(out, size, "%s%s", path, suffix);
}

/* Blanks a copy first if it held the words in the open, or the old password. */
static void remove_copy(const char *name)
{
	struct stat st;
	int fd;

	if (scrub_copies && !stat(name, &st) && (fd = open(name, O_WRONLY)) >= 0) {
		char zero[4096];
		off_t left = st.st_size;

		memset(zero, 0, sizeof zero);
		while (left > 0) {
			int n = left < (off_t)sizeof zero ? (int)left : (int)sizeof zero;

			if (write(fd, zero, n) != n)
				break;
			left -= n;
		}
		fsync(fd);
		close(fd);
	}
	unlink(name);
}

static void save(void)
{
	char tmp[sizeof path + 8];
	char bak[sizeof path + 8];
	char autosave[sizeof path + 16];

	side_path(tmp, sizeof tmp, ".tmp");
	side_path(bak, sizeof bak, ".bak");
	side_path(autosave, sizeof autosave, ".autosave");
	if (write_file(tmp)) {
		snprintf(status, sizeof status, "SAVE FAILED: %s", strerror(errno));
		return;
	}
	if (scrub_copies)
		remove_copy(bak);
	if (!access(path, F_OK) && rename(path, bak)) {
		snprintf(status, sizeof status, "SAVE FAILED: %s", strerror(errno));
		return;
	}
	if (rename(tmp, path)) {
		snprintf(status, sizeof status, "SAVE FAILED: %s", strerror(errno));
		return;
	}
	remove_copy(autosave);
	if (scrub_copies)
		remove_copy(bak);
	sync_folder();
	dirty = 0;
	autosave_due = 0;
	strcpy(status, "Saved");
}

static void autosave(void)
{
	char name[sizeof path + 16];

	side_path(name, sizeof name, ".autosave");
	if (write_file(name)) {
		strcpy(status, "AUTOSAVE FAILED");
		return;
	}
	sync_folder();
	autosave_due = 0;
	strcpy(status, "Autosaved");
}

enum { LOAD_OK, LOAD_MISSING, LOAD_NEED_PASSWORD, LOAD_WRONG, LOAD_DAMAGED };

/* password is NULL until the person has typed one. */
static int load_file(const char *name, const char *password)
{
	struct stat st;
	char *data;
	int fd = open(name, O_RDONLY);
	int len = 0;
	int n;
	int i;

	if (fd < 0)
		return LOAD_MISSING;
	fstat(fd, &st);
	data = must_alloc(NULL, st.st_size + 1);
	while ((n = read(fd, data + len, st.st_size - len)) > 0)
		len += n;
	close(fd);
	if (lock_is_locked(data, len)) {
		char *plain;
		int tried;

		if (!password) {
			free(data);
			return LOAD_NEED_PASSWORD;
		}
		tried = lock_try(&story_lock, password, (uint8_t *)data, len);
		if (tried != LOCK_OK) {
			free(data);
			return tried == LOCK_WRONG ? LOAD_WRONG : LOAD_DAMAGED;
		}
		plain = must_alloc(NULL, len - LOCK_HEADER + 1);
		if (lock_open(&story_lock, (uint8_t *)data, len, (uint8_t *)plain) != LOCK_OK) {
			free(plain);
			free(data);
			return LOAD_DAMAGED;
		}
		free(data);
		data = plain;
		len -= LOCK_HEADER;
		story_lock.on = 1;
	} else if (password) {
		/* A wrong password must not fall through to an old unlocked copy. */
		free(data);
		return LOAD_MISSING;
	}
	/* Stories from Windows end lines with CR LF. */
	n = 0;
	for (i = 0; i < len; i++) {
		if (data[i] != '\r')
			data[n++] = data[i];
	}
	raw_insert(0, data, n);
	if (story_lock.on)
		crypto_wipe(data, len);
	free(data);
	return LOAD_OK;
}

static int ask_password(const char *question, char *password);
static int story_damaged;

/*
 * Prefers the newest copy that can only exist if a save or session was cut short.
 * One damaged copy never hides a good one. Returns 0 if the story must stay shut,
 * and nothing may be written over it then.
 */
static int open_story(void)
{
	static const struct { const char *suffix; const char *news; } copies[] = {
		{ ".autosave", "Recovered unsaved work" },
		{ "", NULL },
		{ ".tmp", "Recovered an interrupted save" },
		{ ".bak", "Recovered the backup copy" },
	};
	char name[sizeof path + 16];
	char password[LOCK_MAX_PASSWORD] = "";
	const char *question = "Password:";
	int typed = 0;
	unsigned i;

	for (;;) {
		int need = 0, wrong = 0, damaged = 0;

		for (i = 0; i < sizeof copies / sizeof copies[0]; i++) {
			int got;

			side_path(name, sizeof name, copies[i].suffix);
			got = load_file(name, typed ? password : NULL);
			if (got == LOAD_OK) {
				crypto_wipe(password, sizeof password);
				if (copies[i].news) {
					dirty = 1;
					strcpy(status, copies[i].news);
				} else if (damaged) {
					strcpy(status, "A newer copy was damaged and skipped");
				}
				return 1;
			}
			if (got == LOAD_NEED_PASSWORD) {
				need = 1;
				break;
			}
			wrong |= got == LOAD_WRONG;
			damaged |= got == LOAD_DAMAGED;
		}
		if (!need && !wrong) {
			if (damaged) {
				story_damaged = 1;
				return 0;
			}
			strcpy(status, "New story");
			return 1;
		}
		if (wrong)
			question = "Wrong password, try again:";
		lock_retry(&story_lock);
		if (!ask_password(question, password))
			return 0;
		typed = 1;
		strcpy(status, "Unlocking...");
		paint();
		ui_present();
		status[0] = 0;
	}
}

static void load_dictionary(void)
{
	const char *name = getenv("POCKET_DICT");
	struct stat st;
	FILE *f;
	char line[MAX_WORD + 2];
	char folder[sizeof path];
	int fd;

	if (!name)
		name = "/usr/share/pocket/words.txt";
	fd = open(name, O_RDONLY);
	if (fd >= 0 && !fstat(fd, &st)) {
		dict = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
		if (dict == MAP_FAILED)
			dict = NULL;
		else
			dict_len = st.st_size;
		close(fd);
	}
	/* One list for every story on the card, whichever folder it sits in. */
	snprintf(folder, sizeof folder, "%s/", ui_root());
	if (!strncmp(path, folder, strlen(folder))) {
		snprintf(user_path, sizeof user_path, "%sstories/my-words.txt", folder);
	} else {
		strcpy(folder, path);
		snprintf(user_path, sizeof user_path, "%s/my-words.txt", dirname(folder));
	}
	f = fopen(user_path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!line[0])
			continue;
		user_words = must_alloc(user_words, (user_count + 1) * sizeof *user_words);
		user_words[user_count++] = strdup(line);
	}
	fclose(f);
}

/* Finds the word touching the cursor. Returns its length or 0. */
static int word_at_cursor(char *word)
{
	int n = text_len();
	int s = cursor;
	int e = cursor;

	while (s > 0 && is_word_char(char_at(s - 1)))
		s--;
	while (e < n && is_word_char(char_at(e)))
		e++;
	return clean_word(&s, &e, word);
}

static void add_to_dictionary(void)
{
	char word[MAX_WORD];
	FILE *f;

	if (!word_at_cursor(word)) {
		strcpy(status, "Put the cursor on a word first");
		return;
	}
	f = fopen(user_path, "a");
	if (!f || fprintf(f, "%s\n", word) < 0 || fflush(f) || fsync(fileno(f))) {
		strcpy(status, "Could not save the word");
		if (f)
			fclose(f);
		return;
	}
	fclose(f);
	user_words = must_alloc(user_words, (user_count + 1) * sizeof *user_words);
	user_words[user_count++] = strdup(word);
	snprintf(status, sizeof status, "Added \"%s\"", word);
}

static void delete_selection(void)
{
	int from;
	int to;

	selection(&from, &to);
	cursor = to;
	change(from, to - from, "", 0);
	anchor = -1;
}

static int has_selection(void)
{
	return anchor >= 0 && anchor != cursor;
}

static void insert_text(const char *s, int n)
{
	if (has_selection())
		delete_selection();
	anchor = -1;
	change(cursor, 0, s, n);
}

static void copy_selection(void)
{
	int from;
	int to;

	if (!has_selection())
		return;
	selection(&from, &to);
	free(clip);
	clip = copy_range(from, to - from);
	clip_len = to - from;
	strcpy(status, "Copied");
}

static void find_next(void)
{
	int n = text_len();
	int i;
	int tries;

	if (!search_len)
		return;
	i = cursor;
	for (tries = 0; tries <= n; tries++, i = (i + 1 > n - search_len) ? 0 : i + 1) {
		int j;

		for (j = 0; j < search_len && i + j < n; j++) {
			if (tolower((unsigned char)char_at(i + j)) !=
			    tolower((unsigned char)search[j]))
				break;
		}
		if (j == search_len) {
			anchor = i;
			cursor = i + search_len;
			strcpy(status, "Found");
			want_x = -1;
			return;
		}
	}
	strcpy(status, "Not found");
}

static void move_cursor(int to, int mods)
{
	if (mods & MOD_SHIFT) {
		if (anchor < 0)
			anchor = cursor;
	} else {
		anchor = -1;
	}
	cursor = to;
}

static int word_left(void)
{
	int i = cursor;

	while (i > 0 && !is_word_char(char_at(i - 1)))
		i--;
	while (i > 0 && is_word_char(char_at(i - 1)))
		i--;
	return i;
}

static int word_right(void)
{
	int n = text_len();
	int i = cursor;

	while (i < n && is_word_char(char_at(i)))
		i++;
	while (i < n && !is_word_char(char_at(i)))
		i++;
	return i;
}

static void move_lines(int count, int mods)
{
	int line = line_of(cursor) + count;

	if (want_x < 0)
		want_x = x_of(cursor);
	if (line < 0)
		line = 0;
	if (line >= line_count)
		line = line_count - 1;
	move_cursor(pos_at(line, want_x), mods);
}

static void paint(void);
static void suggest_fixes(void);

/* The Finder asks open programs to finish when the Jornada powers off. */
static volatile sig_atomic_t finish_up;

static void on_terminate(int sig)
{
	(void)sig;
	finish_up = 1;
	if (running_pid > 0)
		kill(running_pid, SIGTERM);
}

/* Characters FAT cannot store in a name. */
static int name_char_ok(int c)
{
	return c >= 32 && c < 127 && !strchr("\\/:*?\"<>|", c);
}

/* Asks for text in the status bar. Passwords show as stars and take any character. */
static int ask_text(const char *question, char *name, int size, int hidden)
{
	struct ui_event ev;
	int len = strlen(name);

	for (;;) {
		char shown[160];
		char stars[LOCK_MAX_PASSWORD];
		int got;

		paint();
		if (hidden) {
			memset(stars, '*', len);
			stars[len] = 0;
		}
		snprintf(shown, sizeof shown, "%s %s_", question, hidden ? stars : name);
		ui_fill(0, TEXT_BOTTOM + 1, SCREEN_W, SCREEN_H - TEXT_BOTTOM - 1, C_WINDOW);
		ui_text(8, TEXT_BOTTOM - 2, shown, strlen(shown), C_INK);
		ui_present();
		got = ui_wait(&ev, 1000);
		if (got < 0 || (got && ev.key == K_ESC))
			return 0;
		if (!got)
			continue;
		if (ev.key == K_ENTER && len)
			break;
		if (ev.key == K_BACKSPACE && len)
			name[--len] = 0;
		if (ev.key == K_CHAR && !(ev.mods & MOD_CTRL) && len < size - 1 &&
		    (hidden ? ev.ch >= 32 && ev.ch < 127 : name_char_ok(ev.ch))) {
			name[len++] = ev.ch;
			name[len] = 0;
		}
	}
	return len > 0;
}

/* Asks for a name in the status bar. Returns 1 if one was entered. */
static int ask_name(const char *question, char *name, int size)
{
	int len;

	if (!ask_text(question, name, size, 0))
		return 0;
	len = strlen(name);
	while (len && (name[len - 1] == ' ' || name[len - 1] == '.'))
		name[--len] = 0;
	return len > 0;
}

static int ask_password(const char *question, char *password)
{
	password[0] = 0;
	return ask_text(question, password, LOCK_MAX_PASSWORD, 1);
}

static void show_progress(const char *text)
{
	strcpy(status, text);
	paint();
	ui_present();
}

/* Replaces the lock, then rewrites every copy so the old one is gone. */
static void relock_and_save(const struct lock *next)
{
	struct lock old = story_lock;

	story_lock = *next;
	scrub_copies = 1;
	save();
	scrub_copies = 0;
	if (dirty)
		story_lock = old;
	else
		strcpy(status, story_lock.on ? "Password set" : "Password removed");
	lock_forget(&old);
}

static void choose_new_password(void)
{
	char first[LOCK_MAX_PASSWORD];
	char second[LOCK_MAX_PASSWORD];
	struct lock next;

	if (!ask_password("New password (if forgotten, the story is lost):", first))
		return;
	if (!ask_password("Type it again:", second)) {
		crypto_wipe(first, sizeof first);
		return;
	}
	if (strcmp(first, second)) {
		strcpy(status, "Those did not match, nothing changed");
	} else {
		show_progress("Locking...");
		if (lock_new(&next, first)) {
			strcpy(status, "Could not set the password, nothing changed");
		} else {
			/* A save that fails keeps dirty set, so count this as a change. */
			dirty = 1;
			relock_and_save(&next);
			lock_forget(&next);
		}
	}
	crypto_wipe(first, sizeof first);
	crypto_wipe(second, sizeof second);
}

static void password_menu(void)
{
	static const char *const choices[] = { "Change Password", "Remove Password", "Cancel" };
	char password[LOCK_MAX_PASSWORD];
	struct lock none;
	int matches;
	int pick;

	if (!story_lock.on) {
		choose_new_password();
		return;
	}
	pick = ui_pick(220, 90, 180, choices, 3, paint);
	if (pick != 0 && pick != 1)
		return;
	if (!ask_password("Current password:", password))
		return;
	show_progress("Checking...");
	matches = lock_matches(&story_lock, password);
	crypto_wipe(password, sizeof password);
	status[0] = 0;
	if (!matches) {
		strcpy(status, "Wrong password, nothing changed");
		return;
	}
	if (pick == 0) {
		choose_new_password();
		return;
	}
	memset(&none, 0, sizeof none);
	dirty = 1;
	relock_and_save(&none);
}

/* The Finder knows open stories by path, so it must not open the new name twice. */
static void tell_finder_new_path(void)
{
	const char *dir = getenv("POCKET_TMP");
	char note[64];
	FILE *file;

	if (!getenv("POCKET_SWITCHER"))
		return;
	snprintf(note, sizeof note, "%.40s/pocket-open.%d", dir ? dir : "/tmp", (int)getpid());
	file = fopen(note, "w");
	if (!file)
		return;
	fputs(path, file);
	fclose(file);
}

static void save_as(void)
{
	char old_autosave[sizeof path + 16];
	char name[48];
	char folder[sizeof path];
	char target[sizeof path + 64];
	char *base = strrchr(path, '/');
	const char *ext = code_mode ? ".lua" : ".txt";
	size_t n;

	snprintf(name, sizeof name, "%.47s", base ? base + 1 : path);
	n = strlen(name);
	if (n > 4 && !strcmp(name + n - 4, ext))
		name[n - 4] = 0;
	if (!ask_name("Save as:", name, sizeof name))
		return;
	strcpy(folder, path);
	snprintf(target, sizeof target, "%s/%s%s", dirname(folder), name, ext);
	if (strlen(target) >= sizeof path) {
		strcpy(status, "That name is too long");
		return;
	}
	if (strcmp(target, path) && !access(target, F_OK)) {
		strcpy(status, "That name is already in use");
		return;
	}
	side_path(old_autosave, sizeof old_autosave, ".autosave");
	strcpy(path, target);
	save();
	if (!dirty)
		unlink(old_autosave);
	tell_finder_new_path();
}

/* Returns 1 if the story may close. Unsaved work always gets a question. */
static int close_story(void)
{
	static const char *const choices[] = { "Save", "Don't Save", "Cancel" };
	char autosave_copy[sizeof path + 16];
	char tmp[sizeof path + 8];
	int pick;

	if (!dirty)
		return 1;
	pick = ui_pick(240, 90, 160, choices, 3, paint);
	if (pick == 0) {
		save();
		return !dirty;
	}
	if (pick != 1)
		return 0;
	side_path(autosave_copy, sizeof autosave_copy, ".autosave");
	side_path(tmp, sizeof tmp, ".tmp");
	unlink(autosave_copy);
	unlink(tmp);
	sync_folder();
	return 1;
}

static void back_to_finder(void)
{
	if (autosave_due)
		autosave();
	if (!ui_switch_away())
		strcpy(status, "The Finder is not running");
}

static int handle_search(const struct ui_event *ev)
{
	if (ev->key == K_ESC) {
		searching = 0;
	} else if (ev->key == K_ENTER) {
		searching = 0;
		find_next();
	} else if (ev->key == K_BACKSPACE) {
		if (search_len)
			search_len--;
	} else if (ev->key == K_CHAR && search_len < (int)sizeof search - 1) {
		search[search_len++] = ev->ch;
	}
	return 0;
}

static void go_to_line(int line)
{
	int n = text_len();
	int i;

	cursor = 0;
	for (i = 0; i < n && line > 1; i++) {
		if (char_at(i) == '\n') {
			line--;
			cursor = i + 1;
		}
	}
	anchor = -1;
	want_x = -1;
	wrap_text();
}

/*
 * Saves, then runs the program in its own process with the screen handed over.
 * It reads the saved file, which a password would keep from it.
 */
static void run_program(void)
{
	const char *bin = getenv("POCKET_BIN");
	char runner[300];
	char fd_name[16];
	char answer[16] = "";
	int line_pipe[2];
	pid_t pid;
	int status_code;
	ssize_t got;

	if (story_lock.on) {
		strcpy(status, "Take the password off to run this");
		return;
	}
	if (dirty)
		save();
	if (dirty || pipe(line_pipe)) {
		strcpy(status, "Could not save, so it did not run");
		return;
	}
	snprintf(runner, sizeof runner, "%s/pocketlua", bin ? bin : "/usr/bin");
	snprintf(fd_name, sizeof fd_name, "%d", line_pipe[1]);
	ui_suspend();
	pid = fork();
	if (pid == 0) {
		close(line_pipe[0]);
		setenv("POCKET_LINE_FD", fd_name, 1);
		unsetenv("POCKET_SWITCHER");
		execl(runner, "pocketlua", path, (char *)NULL);
		_exit(127);
	}
	close(line_pipe[1]);
	running_pid = pid;
	ui_wake_target(pid);
	while (pid > 0 && waitpid(pid, &status_code, 0) < 0 && errno == EINTR)
		;
	ui_wake_target(0);
	running_pid = 0;
	got = read(line_pipe[0], answer, sizeof answer - 1);
	close(line_pipe[0]);
	ui_resume();
	if (got > 0) {
		int line = atoi(answer);

		go_to_line(line);
		snprintf(status, sizeof status, "Problem on line %d", line);
	} else if (pid < 0 || (WIFEXITED(status_code) && WEXITSTATUS(status_code) == 127)) {
		strcpy(status, "Could not start the program");
	}
}

static int handle_ctrl(int ch)
{
	switch (tolower(ch)) {
	case 's': save(); break;
	case 'e': save_as(); break;
	case 'p': password_menu(); break;
	case 'b': ui_screen_dialog(paint); break;
	case 'q': return close_story();
	case 'z': undo(); break;
	case 'c': copy_selection(); break;
	case 'x':
		copy_selection();
		if (has_selection())
			delete_selection();
		break;
	case 'v':
		if (clip)
			insert_text(clip, clip_len);
		break;
	case 'a':
		anchor = 0;
		cursor = text_len();
		break;
	case 'f':
		searching = 1;
		search_len = 0;
		break;
	case 'g': find_next(); break;
	case 'd':
		if (!code_mode)
			add_to_dictionary();
		break;
	case 'l':
		if (!code_mode)
			suggest_fixes();
		break;
	case 'r':
		if (code_mode)
			run_program();
		break;
	case 'w': back_to_finder(); break;
	}
	return 0;
}

static void try_suggestion(const char *candidate, char list[][MAX_WORD], int *count)
{
	int i;

	if (*count >= MAX_SUGGESTIONS || !word_ok(candidate))
		return;
	for (i = 0; i < *count; i++) {
		if (!strcmp(list[i], candidate))
			return;
	}
	strcpy(list[(*count)++], candidate);
}

/* Words one typing slip away, most likely slips first. */
static int find_suggestions(const char *word, char list[][MAX_WORD])
{
	char try[MAX_WORD + 1];
	int n = strlen(word);
	int count = 0;
	int i;
	char c;

	if (n >= MAX_WORD - 1)
		return 0;
	for (i = 0; i + 1 < n; i++) {
		strcpy(try, word);
		try[i] = word[i + 1];
		try[i + 1] = word[i];
		try_suggestion(try, list, &count);
	}
	for (i = 0; i < n; i++) {
		memcpy(try, word, i);
		strcpy(try + i, word + i + 1);
		try_suggestion(try, list, &count);
	}
	for (i = 0; i < n; i++) {
		strcpy(try, word);
		for (c = 'a'; c <= 'z'; c++) {
			try[i] = c;
			if (c != word[i])
				try_suggestion(try, list, &count);
		}
	}
	for (i = 0; i <= n; i++) {
		memcpy(try, word, i);
		strcpy(try + i + 1, word + i);
		for (c = 'a'; c <= 'z'; c++) {
			try[i] = c;
			try_suggestion(try, list, &count);
		}
	}
	return count;
}

static void suggest_fixes(void)
{
	char word[MAX_WORD];
	char list[MAX_SUGGESTIONS][MAX_WORD];
	const char *items[MAX_SUGGESTIONS];
	int n = text_len();
	int s = cursor;
	int e = cursor;
	int line;
	int x;
	int y;
	int w = 0;
	int count;
	int i;
	int pick;

	while (s > 0 && is_word_char(char_at(s - 1)))
		s--;
	while (e < n && is_word_char(char_at(e)))
		e++;
	if (!clean_word(&s, &e, word)) {
		strcpy(status, "Put the cursor on a word first");
		return;
	}
	if (word_ok(word)) {
		strcpy(status, "That word looks fine");
		return;
	}
	count = find_suggestions(word, list);
	if (!count) {
		strcpy(status, "No suggestions");
		return;
	}
	for (i = 0; i < count; i++) {
		int tw = ui_text_width(list[i], strlen(list[i]));

		if (isupper((unsigned char)char_at(s)))
			list[i][0] = toupper((unsigned char)list[i][0]);
		items[i] = list[i];
		if (tw > w)
			w = tw;
	}
	w += 24;
	line = line_of(s);
	x = TEXT_LEFT + x_of(s);
	y = TEXT_TOP + (line - top_line + 1) * LINE_HEIGHT;
	if (x + w > SCREEN_W - 4)
		x = SCREEN_W - 4 - w;
	if (y + count * 15 + 6 > SCREEN_H)
		y = SCREEN_H - count * 15 - 6;
	pick = ui_pick(x, y, w, items, count, paint);
	if (pick < 0)
		return;
	cursor = e;
	anchor = -1;
	change(s, e - s, list[pick], strlen(list[pick]));
	snprintf(status, sizeof status, "Changed to \"%s\"", list[pick]);
}

/* Opens the menus. Returns what handle_ctrl returns for the chosen item. */
static int run_menus(int start)
{
	int pick = ui_run_menus(menus, MENU_COUNT, start, paint);

	if (pick < 0)
		return 0;
	return handle_ctrl(menus[pick / 256].keys[pick % 256]);
}

static int handle_tap(const struct ui_event *ev)
{
	int line;

	if (ev->x < 0)
		return 0;
	if (ev->y < 16) {
		int m = ui_menu_at(menus, MENU_COUNT, ev->x);

		if (ev->x >= SCREEN_W - 110)
			back_to_finder();
		return m >= 0 ? run_menus(m) : 0;
	}
	if (ev->y < TEXT_TOP && ev->x < 30)
		return close_story();
	if (ev->y < TEXT_TOP || ev->y >= TEXT_BOTTOM || ev->x >= SCREEN_W - 14)
		return 0;
	line = top_line + (ev->y - TEXT_TOP) / LINE_HEIGHT;
	if (line >= line_count)
		line = line_count - 1;
	move_cursor(pos_at(line, ev->x - TEXT_LEFT), 0);
	want_x = -1;
	return 0;
}

/* In code, a new line starts where the one above it did. */
static void copy_indent(void)
{
	char indent[64];
	int i = cursor - 2;
	int n = 0;

	while (i >= 0 && char_at(i) != '\n')
		i--;
	for (i++; n < (int)sizeof indent && i < cursor - 1 && char_at(i) == ' '; i++)
		indent[n++] = ' ';
	if (n)
		insert_text(indent, n);
}

static int handle(const struct ui_event *ev)
{
	char c;
	int n = text_len();

	status[0] = 0;
	if (searching)
		return handle_search(ev);
	if (ev->key == K_CHAR && (ev->mods & MOD_CTRL))
		return handle_ctrl(ev->ch);
	if (ev->key == K_ESC)
		return run_menus(0);
	if (ev->key == K_TAP)
		return handle_tap(ev);

	switch (ev->key) {
	case K_CHAR:
		c = ev->ch;
		insert_text(&c, 1);
		break;
	case K_ENTER:
		insert_text("\n", 1);
		if (code_mode)
			copy_indent();
		break;
	case K_TAB:
		if (ev->mods & MOD_CTRL)
			back_to_finder();
		else
			insert_text("    ", 4);
		break;
	case K_BACKSPACE:
		if (has_selection())
			delete_selection();
		else if (cursor > 0)
			change(cursor - 1, 1, "", 0);
		break;
	case K_DELETE:
		if (has_selection())
			delete_selection();
		else if (cursor < n)
			change(cursor, 1, "", 0);
		break;
	case K_LEFT:
		if (ev->mods & MOD_CTRL)
			move_cursor(word_left(), ev->mods);
		else if (cursor > 0)
			move_cursor(cursor - 1, ev->mods);
		want_x = -1;
		break;
	case K_RIGHT:
		if (ev->mods & MOD_CTRL)
			move_cursor(word_right(), ev->mods);
		else if (cursor < n)
			move_cursor(cursor + 1, ev->mods);
		want_x = -1;
		break;
	case K_UP: move_lines(-1, ev->mods); break;
	case K_DOWN: move_lines(1, ev->mods); break;
	case K_PAGEUP: move_lines(-VISIBLE_LINES, ev->mods); break;
	case K_PAGEDOWN: move_lines(VISIBLE_LINES, ev->mods); break;
	case K_HOME:
		move_cursor(lines[line_of(cursor)], ev->mods);
		want_x = -1;
		break;
	case K_END:
		move_cursor(line_end(line_of(cursor)), ev->mods);
		want_x = -1;
		break;
	default:
		break;
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct ui_event ev;
	int quiet_ms = 0;

	if (argc < 2 || strlen(argv[1]) >= sizeof path) {
		fprintf(stderr, "usage: write file.txt\n");
		return 1;
	}
	strcpy(path, argv[1]);
	code_mode = strlen(path) > 4 && !strcasecmp(path + strlen(path) - 4, ".lua");
	if (code_mode) {
		menus[LAST_MENU].title = "Run";
		menus[LAST_MENU].labels[0] = "Run Program";
		menus[LAST_MENU].labels[1] = NULL;
		menus[LAST_MENU].keys = "r";
	}
	cap = 65536;
	buf = must_alloc(NULL, cap);
	gap_end = cap;
	if (!code_mode)
		load_dictionary();
	if (ui_open()) {
		fprintf(stderr, "could not open the screen\n");
		return 1;
	}
	signal(SIGTERM, on_terminate);
	wrap_text();
	if (!open_story()) {
		struct ui_event ev;

		if (story_damaged) {
			strcpy(status, "Every copy of this story is damaged. Press a key.");
			paint();
			ui_present();
			while (!ui_wait(&ev, 1000))
				;
		}
		lock_forget(&story_lock);
		ui_close();
		return 0;
	}
	wrap_text();
	text_changed = 0;
	draw();

	for (;;) {
		int got = ui_wait(&ev, caret_on ? BLINK_MS : BLINK_MS / 2);

		if (finish_up) {
			if (dirty)
				save();
			break;
		}
		if (got < 0)
			break;
		if (got) {
			quiet_ms = 0;
			caret_on = 1;
			if (handle(&ev))
				break;
			if (text_changed) {
				wrap_text();
				text_changed = 0;
			}
			scroll_to_cursor();
			draw();
		} else {
			quiet_ms += BLINK_MS;
			caret_on = !caret_on;
			if (autosave_due && quiet_ms >= AUTOSAVE_QUIET_MS) {
				autosave();
				draw();
			} else if (ui_battery_changed()) {
				draw();
			} else {
				show_caret_state();
			}
		}
	}
	lock_forget(&story_lock);
	ui_close();
	return 0;
}
