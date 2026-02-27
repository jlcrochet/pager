#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <poll.h>
#include <regex.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "lib/esc.h"
#include "lib/keys.h"
#include "lib/stdio_helpers.h"

#define TAB_SIZE 8
#define MAX_QUERY 256
#define MAX_COMMAND 256
#define SEARCH_HISTORY_SIZE 16
#define STATUS_FLASH_MS 900
#define DEFAULT_COMMAND_POPUP_ROWS 5
#define MAX_COMMAND_POPUP_ROWS 32
#define WRAP_CONT_MARK "\xE2\x86\xAA" /* ↪ */
#define CONFIG_PATH_MAX 4096
#define MAX_BINDINGS_PER_ACTION 16
#define MAX_BINDING_TOKEN 64
#define MAX_SGR_PARAMS 96
#define MAX_SGR_SEQUENCE 128

#define ALT_SCREEN_ENABLE CSI "?1049h"
#define ALT_SCREEN_DISABLE CSI "?1049l"
#define ENABLE_SGR_MOUSE CSI "?1006h"
#define DISABLE_SGR_MOUSE CSI "?1006l"
#define OSC52_PREFIX "\x1b]52;c;"
#define OSC52_SUFFIX "\x1b\\"

enum key_code {
	KEY_EOF = -1,
	KEY_NONE = 0,
	KEY_ESC = 1000,
	KEY_UP,
	KEY_DOWN,
	KEY_LEFT,
	KEY_RIGHT,
	KEY_HOME,
	KEY_END,
	KEY_PGUP,
	KEY_PGDN,
	KEY_DELETE,
	KEY_CTRL_LEFT,
	KEY_CTRL_RIGHT,
	KEY_CTRL_DELETE,
	KEY_MOUSE_UP,
	KEY_MOUSE_DOWN,
};

enum config_section {
	CONFIG_SECTION_ROOT,
	CONFIG_SECTION_KEYBINDINGS,
	CONFIG_SECTION_OTHER,
};

enum search_case_mode {
	SEARCH_CASE_SMART = 0,
	SEARCH_CASE_INSENSITIVE,
	SEARCH_CASE_SENSITIVE,
};

enum pager_action {
	ACTION_NONE = 0,
	ACTION_QUIT,
	ACTION_DOWN_LINE,
	ACTION_UP_LINE,
	ACTION_PAGE_DOWN,
	ACTION_PAGE_UP,
	ACTION_HALF_PAGE_DOWN,
	ACTION_HALF_PAGE_UP,
	ACTION_TOP,
	ACTION_BOTTOM,
	ACTION_SCROLL_LEFT,
	ACTION_SCROLL_RIGHT,
	ACTION_MOUSE_SCROLL_UP,
	ACTION_MOUSE_SCROLL_DOWN,
	ACTION_TOGGLE_WRAP,
	ACTION_SEARCH_FORWARD,
	ACTION_SEARCH_BACKWARD,
	ACTION_NEXT_MATCH,
	ACTION_PREV_MATCH,
	ACTION_NEXT_FILE,
	ACTION_PREV_FILE,
	ACTION_COMMAND_PROMPT,
	ACTION_FOLLOW_MODE,
	ACTION_YANK_LINE,
	ACTION_SHOW_HELP,
	ACTION_COUNT,
};

static int read_key(void);
static bool str_eq_ci(const char *a, const char *b);
static void render_status_bar(void);

struct sgr_state {
	char bytes[1024];
	size_t len;
};

struct search_match {
	size_t line_idx;
	size_t start;
	size_t end;
};

struct command_desc {
	const char *name;
	const char *desc;
};

struct prompt_buffer {
	char text[MAX_COMMAND];
	size_t len;
	size_t cursor;
};

struct action_bindings {
	int keys[MAX_BINDINGS_PER_ACTION];
	size_t count;
};

static char *buffer = NULL;
static size_t buffer_size = 0;
static size_t buffer_capacity = 0;

static size_t *line_offsets = NULL;
static size_t line_count = 0;
static size_t line_capacity = 0;

static size_t *filtered_lines = NULL;
static size_t filtered_count = 0;
static bool filter_active = false;
static char filter_pattern[MAX_QUERY] = "";
static regex_t filter_regex;
static bool filter_regex_valid = false;

static struct search_match *matches = NULL;
static size_t match_count = 0;
static size_t match_capacity = 0;
static size_t current_match = 0;
static size_t *line_match_first = NULL;
static size_t *line_match_count = NULL;
static char search_query[MAX_QUERY] = "";
static regex_t search_regex;
static bool search_regex_valid = false;
static bool search_forward = true;

static char search_history[SEARCH_HISTORY_SIZE][MAX_QUERY];
static size_t search_history_len = 0;

static struct termios original_termios;
static struct termios raw_termios;
static bool termios_valid = false;
static bool raw_mode_enabled = false;
static bool alternate_screen_enabled = false;

static int term_rows = 24;
static int term_cols = 80;
static int content_rows = 23;
static int content_cols = 80;
static int gutter_width = 0;

static size_t scroll_line = 0;
static size_t scroll_col = 0;

static bool wrap_mode = true;
static bool show_line_numbers = false;
static bool follow_mode = false;
static bool search_use_regex = true;
static bool search_wrap = true;
static enum search_case_mode search_case = SEARCH_CASE_SMART;
static char search_current_match_sgr[MAX_SGR_SEQUENCE] = CSI "7;33m";
static char search_other_match_sgr[MAX_SGR_SEQUENCE] = CSI "7m";
static size_t command_popup_rows = DEFAULT_COMMAND_POPUP_ROWS;
static bool sync_output_enabled = true;
static bool quit_if_one_screen = false;

static bool running = true;

static volatile sig_atomic_t term_resized = 0;
static volatile sig_atomic_t got_sigcont = 0;

static char status_flash[128] = "";
static uint64_t status_flash_until = 0;

static char regex_error[256] = "";

static char **filenames = NULL;
static size_t file_count = 0;
static size_t current_file = 0;
static bool source_is_stdin = false;
static bool stdin_was_pipe = false;
static int follow_pipe_fd = -1;
static bool follow_pipe_nonblocking = false;
static off_t follow_file_offset = 0;
static struct action_bindings key_bindings[ACTION_COUNT];

static const struct command_desc commands[] = {
	{ "q", "Quit" },
	{ "wrap", "Enable line wrapping" },
	{ "nowrap", "Disable line wrapping" },
	{ "number", "Show line numbers" },
	{ "nonumber", "Hide line numbers" },
	{ "sync", "Enable sync rendering" },
	{ "nosync", "Disable sync rendering" },
	{ "filter", "Filter lines by regex" },
	{ "n", "Next file" },
	{ "p", "Previous file" },
	{ "follow", "Enter follow mode" },
	{ "help", "Show key bindings" },
};

static void die(const char *message)
{
	perror(message);
	exit(EXIT_FAILURE);
}

static uint64_t monotonic_ms(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void set_flash(const char *text)
{
	if (!text || text[0] == '\0') {
		status_flash[0] = '\0';
		status_flash_until = 0;
		return;
	}

	snprintf(status_flash, sizeof(status_flash), "%s", text);
	status_flash_until = monotonic_ms() + STATUS_FLASH_MS;
}

static void clear_flash_if_expired(void)
{
	if (status_flash_until == 0)
		return;
	if (monotonic_ms() >= status_flash_until) {
		status_flash[0] = '\0';
		status_flash_until = 0;
	}
}

static void sync_frame_begin(void)
{
	if (sync_output_enabled)
		PUTS_ERR(SYNC_BEGIN);
}

static void sync_frame_end(void)
{
	if (sync_output_enabled)
		PUTS_ERR(SYNC_END);
}

static size_t digits_u(size_t value)
{
	size_t d = 1;
	while (value >= 10) {
		value /= 10;
		d++;
	}
	return d;
}

static char *trim_left(char *s)
{
	while (*s != '\0' && isspace((unsigned char)*s))
		s++;
	return s;
}

static void trim_right(char *s)
{
	size_t len = strlen(s);
	while (len > 0 && isspace((unsigned char)s[len - 1])) {
		s[len - 1] = '\0';
		len--;
	}
}

static void strip_toml_comment(char *line)
{
	bool in_string = false;
	bool escape = false;

	for (size_t i = 0; line[i] != '\0'; i++) {
		char ch = line[i];
		if (in_string) {
			if (escape) {
				escape = false;
			} else if (ch == '\\') {
				escape = true;
			} else if (ch == '"') {
				in_string = false;
			}
			continue;
		}

		if (ch == '"') {
			in_string = true;
			continue;
		}
		if (ch == '#') {
			line[i] = '\0';
			return;
		}
	}
}

static bool parse_toml_bool(const char *value, bool *out)
{
	if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) {
		*out = true;
		return true;
	}
	if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0) {
		*out = false;
		return true;
	}
	return false;
}

static bool parse_toml_size(const char *value, size_t *out)
{
	if (!value || !out || value[0] == '\0')
		return false;
	if (value[0] == '+' || value[0] == '-')
		return false;

	errno = 0;
	char *end = NULL;
	unsigned long long v = strtoull(value, &end, 10);
	if (!end || end == value || *end != '\0' || v == 0 || errno == ERANGE)
		return false;
	if (v > (unsigned long long)SIZE_MAX)
		return false;
	*out = (size_t)v;
	return true;
}

static bool parse_toml_string(const char *value, char *dst, size_t dst_size)
{
	if (!value || !dst || dst_size == 0)
		return false;

	if (value[0] == '"') {
		size_t in = 1;
		size_t out = 0;
		bool escaped = false;
		while (value[in] != '\0') {
			char ch = value[in++];
			if (!escaped && ch == '"') {
				while (value[in] != '\0') {
					if (!isspace((unsigned char)value[in]))
						return false;
					in++;
				}
				dst[out] = '\0';
				return true;
			}

			if (escaped) {
				switch (ch) {
					case 'n': ch = '\n'; break;
					case 'r': ch = '\r'; break;
					case 't': ch = '\t'; break;
					case '\\': ch = '\\'; break;
					case '"': ch = '"'; break;
					default: break;
				}
				escaped = false;
			} else if (ch == '\\') {
				escaped = true;
				continue;
			}

			if (out + 1 >= dst_size)
				return false;
			dst[out++] = ch;
		}
		return false;
	}

	size_t len = strlen(value);
	if (len + 1 > dst_size)
		return false;
	memcpy(dst, value, len + 1);
	return true;
}

static bool parse_search_case(const char *value, enum search_case_mode *out_mode)
{
	if (!value || !out_mode)
		return false;

	bool b = false;
	if (parse_toml_bool(value, &b)) {
		*out_mode = b ? SEARCH_CASE_SENSITIVE : SEARCH_CASE_INSENSITIVE;
		return true;
	}

	char text[MAX_BINDING_TOKEN];
	if (!parse_toml_string(value, text, sizeof(text)))
		return false;

	size_t len = strlen(text);
	if (len >= 2 && text[0] == '\'' && text[len - 1] == '\'') {
		memmove(text, text + 1, len - 2);
		text[len - 2] = '\0';
	}

	if (str_eq_ci(text, "smart")) {
		*out_mode = SEARCH_CASE_SMART;
		return true;
	}
	if (str_eq_ci(text, "true")) {
		*out_mode = SEARCH_CASE_SENSITIVE;
		return true;
	}
	if (str_eq_ci(text, "false")) {
		*out_mode = SEARCH_CASE_INSENSITIVE;
		return true;
	}
	if (str_eq_ci(text, "sensitive")) {
		*out_mode = SEARCH_CASE_SENSITIVE;
		return true;
	}
	if (str_eq_ci(text, "insensitive")) {
		*out_mode = SEARCH_CASE_INSENSITIVE;
		return true;
	}

	return false;
}

struct sgr_alias {
	const char *name;
	const char *params;
};

static bool sgr_token_is_number(const char *token)
{
	if (!token || token[0] == '\0')
		return false;
	for (size_t i = 0; token[i] != '\0'; i++) {
		if (!isdigit((unsigned char)token[i]))
			return false;
	}
	return true;
}

static bool is_sgr_token_delim(int ch)
{
	return ch == ',' || ch == ';' || ch == '+' || ch == '|'
		|| isspace((unsigned char)ch);
}

static const char *lookup_sgr_alias(const char *token)
{
	static const struct sgr_alias aliases[] = {
		{ "reset", "0" },
		{ "bold", "1" },
		{ "dim", "2" },
		{ "faint", "2" },
		{ "half-bright", "2" },
		{ "half_bright", "2" },
		{ "italic", "3" },
		{ "underline", "4" },
		{ "blink", "5" },
		{ "reverse", "7" },
		{ "reversed", "7" },
		{ "inverse", "7" },
		{ "black", "30" },
		{ "red", "31" },
		{ "green", "32" },
		{ "yellow", "33" },
		{ "brown", "33" },
		{ "blue", "34" },
		{ "magenta", "35" },
		{ "cyan", "36" },
		{ "white", "37" },
		{ "default", "39" },
		{ "default-fg", "39" },
		{ "default_fg", "39" },
		{ "bright-black", "90" },
		{ "bright_black", "90" },
		{ "bright-red", "91" },
		{ "bright_red", "91" },
		{ "bright-green", "92" },
		{ "bright_green", "92" },
		{ "bright-yellow", "93" },
		{ "bright_yellow", "93" },
		{ "bright-blue", "94" },
		{ "bright_blue", "94" },
		{ "bright-magenta", "95" },
		{ "bright_magenta", "95" },
		{ "bright-cyan", "96" },
		{ "bright_cyan", "96" },
		{ "bright-white", "97" },
		{ "bright_white", "97" },
		{ "bg-black", "40" },
		{ "bg_black", "40" },
		{ "bg-red", "41" },
		{ "bg_red", "41" },
		{ "bg-green", "42" },
		{ "bg_green", "42" },
		{ "bg-yellow", "43" },
		{ "bg_yellow", "43" },
		{ "bg-blue", "44" },
		{ "bg_blue", "44" },
		{ "bg-magenta", "45" },
		{ "bg_magenta", "45" },
		{ "bg-cyan", "46" },
		{ "bg_cyan", "46" },
		{ "bg-white", "47" },
		{ "bg_white", "47" },
		{ "bg-default", "49" },
		{ "bg_default", "49" },
		{ "bg-bright-black", "100" },
		{ "bg_bright_black", "100" },
		{ "bg-bright-red", "101" },
		{ "bg_bright_red", "101" },
		{ "bg-bright-green", "102" },
		{ "bg_bright_green", "102" },
		{ "bg-bright-yellow", "103" },
		{ "bg_bright_yellow", "103" },
		{ "bg-bright-blue", "104" },
		{ "bg_bright_blue", "104" },
		{ "bg-bright-magenta", "105" },
		{ "bg_bright_magenta", "105" },
		{ "bg-bright-cyan", "106" },
		{ "bg_bright_cyan", "106" },
		{ "bg-bright-white", "107" },
		{ "bg_bright_white", "107" },
	};

	for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
		if (str_eq_ci(token, aliases[i].name))
			return aliases[i].params;
	}
	return NULL;
}

static bool append_sgr_params(char *dst, size_t dst_size, size_t *dst_len, const char *params)
{
	size_t len = strlen(params);
	if (*dst_len > 0) {
		if (*dst_len + 1 >= dst_size)
			return false;
		dst[(*dst_len)++] = ';';
	}

	if (*dst_len + len >= dst_size)
		return false;
	memcpy(dst + *dst_len, params, len);
	*dst_len += len;
	dst[*dst_len] = '\0';
	return true;
}

static bool parse_toml_sgr(const char *value, char *dst, size_t dst_size)
{
	char params[MAX_SGR_PARAMS];
	if (!parse_toml_string(value, params, sizeof(params)))
		return false;

	char *trimmed = trim_left(params);
	trim_right(trimmed);
	if (trimmed[0] == '\0')
		return false;

	char merged[MAX_SGR_PARAMS] = "";
	size_t merged_len = 0;

	const char *cursor = trimmed;
	while (*cursor != '\0') {
		while (*cursor != '\0' && is_sgr_token_delim((unsigned char)*cursor))
			cursor++;
		if (*cursor == '\0')
			break;

		char token[MAX_SGR_PARAMS];
		size_t token_len = 0;
		while (cursor[token_len] != '\0' && !is_sgr_token_delim((unsigned char)cursor[token_len])) {
			if (token_len + 1 >= sizeof(token))
				return false;
			token[token_len] = cursor[token_len];
			token_len++;
		}
		token[token_len] = '\0';
		cursor += token_len;

		const char *mapped = NULL;
		if (sgr_token_is_number(token))
			mapped = token;
		else
			mapped = lookup_sgr_alias(token);
		if (!mapped)
			return false;
		if (!append_sgr_params(merged, sizeof(merged), &merged_len, mapped))
			return false;
	}

	if (merged_len == 0)
		return false;

	int n = snprintf(dst, dst_size, CSI "%sm", merged);
	if (n < 0 || (size_t)n >= dst_size)
		return false;

	return true;
}

static bool str_eq_ci(const char *a, const char *b)
{
	if (!a || !b)
		return false;
	while (*a != '\0' && *b != '\0') {
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return false;
		a++;
		b++;
	}
	return *a == '\0' && *b == '\0';
}

static bool str_has_ci_prefix(const char *s, const char *prefix)
{
	if (!s || !prefix)
		return false;
	while (*prefix != '\0') {
		if (*s == '\0')
			return false;
		if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix))
			return false;
		s++;
		prefix++;
	}
	return true;
}

static enum config_section parse_config_section(const char *name)
{
	if (!name || name[0] == '\0')
		return CONFIG_SECTION_OTHER;
	if (strcmp(name, "keybindings") == 0)
		return CONFIG_SECTION_KEYBINDINGS;
	return CONFIG_SECTION_OTHER;
}

static bool append_action_binding(enum pager_action action, int key)
{
	if (action <= ACTION_NONE || action >= ACTION_COUNT)
		return false;

	struct action_bindings *bindings = &key_bindings[action];
	for (size_t i = 0; i < bindings->count; i++) {
		if (bindings->keys[i] == key)
			return true;
	}

	if (bindings->count >= MAX_BINDINGS_PER_ACTION)
		return false;

	bindings->keys[bindings->count++] = key;
	return true;
}

static void remove_action_binding(enum pager_action action, int key)
{
	if (action <= ACTION_NONE || action >= ACTION_COUNT)
		return;

	struct action_bindings *bindings = &key_bindings[action];
	for (size_t i = 0; i < bindings->count; i++) {
		if (bindings->keys[i] != key)
			continue;
		memmove(bindings->keys + i, bindings->keys + i + 1, (bindings->count - i - 1) * sizeof(bindings->keys[0]));
		bindings->count--;
		return;
	}
}

static bool bind_key_to_action(enum pager_action action, int key)
{
	if (action <= ACTION_NONE || action >= ACTION_COUNT)
		return false;

	for (int i = ACTION_NONE + 1; i < ACTION_COUNT; i++) {
		enum pager_action other = (enum pager_action)i;
		if (other == action)
			continue;
		remove_action_binding(other, key);
	}

	return append_action_binding(action, key);
}

static bool action_has_key(enum pager_action action, int key)
{
	if (action <= ACTION_NONE || action >= ACTION_COUNT)
		return false;

	const struct action_bindings *bindings = &key_bindings[action];
	for (size_t i = 0; i < bindings->count; i++) {
		if (bindings->keys[i] == key)
			return true;
	}
	return false;
}

static enum pager_action action_for_key(int key)
{
	for (int i = ACTION_NONE + 1; i < ACTION_COUNT; i++) {
		enum pager_action action = (enum pager_action)i;
		if (action_has_key(action, key))
			return action;
	}
	return ACTION_NONE;
}

static void init_default_key_bindings(void)
{
	memset(key_bindings, 0, sizeof(key_bindings));

	(void)bind_key_to_action(ACTION_QUIT, 'q');

	(void)bind_key_to_action(ACTION_DOWN_LINE, 'j');
	(void)bind_key_to_action(ACTION_DOWN_LINE, KEY_DOWN);
	(void)bind_key_to_action(ACTION_DOWN_LINE, '\n');
	(void)bind_key_to_action(ACTION_DOWN_LINE, '\r');

	(void)bind_key_to_action(ACTION_UP_LINE, 'k');
	(void)bind_key_to_action(ACTION_UP_LINE, KEY_UP);

	(void)bind_key_to_action(ACTION_PAGE_DOWN, ' ');
	(void)bind_key_to_action(ACTION_PAGE_DOWN, KEY_PGDN);

	(void)bind_key_to_action(ACTION_PAGE_UP, 'b');
	(void)bind_key_to_action(ACTION_PAGE_UP, KEY_PGUP);

	(void)bind_key_to_action(ACTION_HALF_PAGE_DOWN, 'd');
	(void)bind_key_to_action(ACTION_HALF_PAGE_UP, 'u');

	(void)bind_key_to_action(ACTION_TOP, 'g');
	(void)bind_key_to_action(ACTION_TOP, KEY_HOME);

	(void)bind_key_to_action(ACTION_BOTTOM, 'G');
	(void)bind_key_to_action(ACTION_BOTTOM, KEY_END);

	(void)bind_key_to_action(ACTION_SCROLL_LEFT, KEY_LEFT);
	(void)bind_key_to_action(ACTION_SCROLL_RIGHT, KEY_RIGHT);

	(void)bind_key_to_action(ACTION_MOUSE_SCROLL_UP, KEY_MOUSE_UP);
	(void)bind_key_to_action(ACTION_MOUSE_SCROLL_DOWN, KEY_MOUSE_DOWN);

	(void)bind_key_to_action(ACTION_TOGGLE_WRAP, 'w');
	(void)bind_key_to_action(ACTION_SEARCH_FORWARD, '/');
	(void)bind_key_to_action(ACTION_SEARCH_BACKWARD, '?');
	(void)bind_key_to_action(ACTION_NEXT_MATCH, 'n');
	(void)bind_key_to_action(ACTION_PREV_MATCH, 'N');
	(void)bind_key_to_action(ACTION_NEXT_FILE, ']');
	(void)bind_key_to_action(ACTION_PREV_FILE, '[');
	(void)bind_key_to_action(ACTION_COMMAND_PROMPT, ':');
	(void)bind_key_to_action(ACTION_FOLLOW_MODE, 'F');
	(void)bind_key_to_action(ACTION_YANK_LINE, 'y');
	(void)bind_key_to_action(ACTION_SHOW_HELP, 'h');
}

static bool parse_binding_key_name(const char *name, int *out_key)
{
	if (!name || !out_key || name[0] == '\0')
		return false;

	size_t len = strlen(name);
	if (len == 1) {
		*out_key = (unsigned char)name[0];
		return true;
	}

	if (str_eq_ci(name, "space")) {
		*out_key = ' ';
		return true;
	}
	if (str_eq_ci(name, "tab")) {
		*out_key = K_TAB;
		return true;
	}
	if (str_eq_ci(name, "enter") || str_eq_ci(name, "return")) {
		*out_key = K_ENTER;
		return true;
	}
	if (str_eq_ci(name, "esc") || str_eq_ci(name, "escape")) {
		*out_key = KEY_ESC;
		return true;
	}
	if (str_eq_ci(name, "up")) {
		*out_key = KEY_UP;
		return true;
	}
	if (str_eq_ci(name, "down")) {
		*out_key = KEY_DOWN;
		return true;
	}
	if (str_eq_ci(name, "left")) {
		*out_key = KEY_LEFT;
		return true;
	}
	if (str_eq_ci(name, "right")) {
		*out_key = KEY_RIGHT;
		return true;
	}
	if (str_eq_ci(name, "home")) {
		*out_key = KEY_HOME;
		return true;
	}
	if (str_eq_ci(name, "end")) {
		*out_key = KEY_END;
		return true;
	}
	if (str_eq_ci(name, "pgup") || str_eq_ci(name, "pageup") || str_eq_ci(name, "page_up")
		|| str_eq_ci(name, "page-up")) {
		*out_key = KEY_PGUP;
		return true;
	}
	if (str_eq_ci(name, "pgdn") || str_eq_ci(name, "pagedown") || str_eq_ci(name, "page_down")
		|| str_eq_ci(name, "page-down")) {
		*out_key = KEY_PGDN;
		return true;
	}
	if (str_eq_ci(name, "delete") || str_eq_ci(name, "del")) {
		*out_key = KEY_DELETE;
		return true;
	}
	if (str_eq_ci(name, "backspace")) {
		*out_key = K_DEL;
		return true;
	}
	if (str_eq_ci(name, "ctrl-left") || str_eq_ci(name, "ctrl_left") || str_eq_ci(name, "ctrl+left")) {
		*out_key = KEY_CTRL_LEFT;
		return true;
	}
	if (str_eq_ci(name, "ctrl-right") || str_eq_ci(name, "ctrl_right") || str_eq_ci(name, "ctrl+right")) {
		*out_key = KEY_CTRL_RIGHT;
		return true;
	}
	if (str_eq_ci(name, "ctrl-delete") || str_eq_ci(name, "ctrl_delete") || str_eq_ci(name, "ctrl+delete")) {
		*out_key = KEY_CTRL_DELETE;
		return true;
	}
	if (str_eq_ci(name, "mouse-up") || str_eq_ci(name, "mouse_up") || str_eq_ci(name, "mouseup")) {
		*out_key = KEY_MOUSE_UP;
		return true;
	}
	if (str_eq_ci(name, "mouse-down") || str_eq_ci(name, "mouse_down") || str_eq_ci(name, "mousedown")) {
		*out_key = KEY_MOUSE_DOWN;
		return true;
	}

	const char *suffix = NULL;
	if (str_has_ci_prefix(name, "ctrl-") || str_has_ci_prefix(name, "ctrl+"))
		suffix = name + 5;
	else if (str_has_ci_prefix(name, "c-") || str_has_ci_prefix(name, "c+"))
		suffix = name + 2;

	if (suffix && suffix[0] != '\0' && suffix[1] == '\0' && isalpha((unsigned char)suffix[0])) {
		char ch = (char)tolower((unsigned char)suffix[0]);
		*out_key = (ch - 'a') + 1;
		return true;
	}

	return false;
}

static bool parse_binding_action(const char *name, enum pager_action *out_action)
{
	if (!name || !out_action)
		return false;

	if (str_eq_ci(name, "quit")) {
		*out_action = ACTION_QUIT;
		return true;
	}
	if (str_eq_ci(name, "down")) {
		*out_action = ACTION_DOWN_LINE;
		return true;
	}
	if (str_eq_ci(name, "up")) {
		*out_action = ACTION_UP_LINE;
		return true;
	}
	if (str_eq_ci(name, "page_down")) {
		*out_action = ACTION_PAGE_DOWN;
		return true;
	}
	if (str_eq_ci(name, "page_up")) {
		*out_action = ACTION_PAGE_UP;
		return true;
	}
	if (str_eq_ci(name, "half_page_down")) {
		*out_action = ACTION_HALF_PAGE_DOWN;
		return true;
	}
	if (str_eq_ci(name, "half_page_up")) {
		*out_action = ACTION_HALF_PAGE_UP;
		return true;
	}
	if (str_eq_ci(name, "top")) {
		*out_action = ACTION_TOP;
		return true;
	}
	if (str_eq_ci(name, "bottom")) {
		*out_action = ACTION_BOTTOM;
		return true;
	}
	if (str_eq_ci(name, "left")) {
		*out_action = ACTION_SCROLL_LEFT;
		return true;
	}
	if (str_eq_ci(name, "right")) {
		*out_action = ACTION_SCROLL_RIGHT;
		return true;
	}
	if (str_eq_ci(name, "mouse_up")) {
		*out_action = ACTION_MOUSE_SCROLL_UP;
		return true;
	}
	if (str_eq_ci(name, "mouse_down")) {
		*out_action = ACTION_MOUSE_SCROLL_DOWN;
		return true;
	}
	if (str_eq_ci(name, "wrap")) {
		*out_action = ACTION_TOGGLE_WRAP;
		return true;
	}
	if (str_eq_ci(name, "search_forward")) {
		*out_action = ACTION_SEARCH_FORWARD;
		return true;
	}
	if (str_eq_ci(name, "search_backward")) {
		*out_action = ACTION_SEARCH_BACKWARD;
		return true;
	}
	if (str_eq_ci(name, "next_match")) {
		*out_action = ACTION_NEXT_MATCH;
		return true;
	}
	if (str_eq_ci(name, "prev_match")) {
		*out_action = ACTION_PREV_MATCH;
		return true;
	}
	if (str_eq_ci(name, "next_file")) {
		*out_action = ACTION_NEXT_FILE;
		return true;
	}
	if (str_eq_ci(name, "prev_file")) {
		*out_action = ACTION_PREV_FILE;
		return true;
	}
	if (str_eq_ci(name, "command")) {
		*out_action = ACTION_COMMAND_PROMPT;
		return true;
	}
	if (str_eq_ci(name, "follow")) {
		*out_action = ACTION_FOLLOW_MODE;
		return true;
	}
	if (str_eq_ci(name, "yank")) {
		*out_action = ACTION_YANK_LINE;
		return true;
	}
	if (str_eq_ci(name, "help")) {
		*out_action = ACTION_SHOW_HELP;
		return true;
	}

	return false;
}

static const char *skip_ascii_space(const char *s)
{
	while (*s != '\0' && isspace((unsigned char)*s))
		s++;
	return s;
}

static bool parse_binding_token(const char **cursor, char *token, size_t token_size)
{
	if (!cursor || !*cursor || !token || token_size == 0)
		return false;

	const char *p = skip_ascii_space(*cursor);
	if (*p == '\0')
		return false;

	if (*p == '"') {
		const char *q = p + 1;
		bool escaped = false;
		while (*q != '\0') {
			if (escaped) {
				escaped = false;
			} else if (*q == '\\') {
				escaped = true;
			} else if (*q == '"') {
				break;
			}
			q++;
		}
		if (*q != '"')
			return false;

		size_t span = (size_t)(q - p + 1);
		char quoted[MAX_COMMAND];
		if (span + 1 > sizeof(quoted))
			return false;
		memcpy(quoted, p, span);
		quoted[span] = '\0';
		if (!parse_toml_string(quoted, token, token_size))
			return false;
		*cursor = q + 1;
		return true;
	}

	const char *start = p;
	while (*p != '\0' && *p != ',' && *p != ']' && !isspace((unsigned char)*p))
		p++;

	size_t len = (size_t)(p - start);
	if (len == 0 || len + 1 > token_size)
		return false;
	memcpy(token, start, len);
	token[len] = '\0';
	*cursor = p;
	return true;
}

static bool parse_binding_key_value(const char *value, int *keys, size_t keys_cap, size_t *keys_len)
{
	if (!value || !keys || !keys_len)
		return false;

	*keys_len = 0;
	const char *p = skip_ascii_space(value);
	bool is_array = false;
	if (*p == '[') {
		is_array = true;
		p++;
	}

	for (;;) {
		p = skip_ascii_space(p);

		if (is_array && *p == ']') {
			p++;
			p = skip_ascii_space(p);
			return *p == '\0';
		}
		if (!is_array && *p == '\0')
			return false;

		char token[MAX_BINDING_TOKEN];
		if (!parse_binding_token(&p, token, sizeof(token)))
			return false;

		int key = 0;
		if (!parse_binding_key_name(token, &key))
			return false;
		if (*keys_len >= keys_cap)
			return false;
		keys[(*keys_len)++] = key;

		p = skip_ascii_space(p);
		if (!is_array)
			return *p == '\0';

		if (*p == ',') {
			p++;
			continue;
		}
		if (*p == ']') {
			p++;
			p = skip_ascii_space(p);
			return *p == '\0';
		}
		return false;
	}
}

static bool resolve_default_config_path(char *path, size_t path_size)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && xdg[0] != '\0') {
		int n = snprintf(path, path_size, "%s/pager.toml", xdg);
		if (n < 0 || (size_t)n >= path_size)
			return false;
		return true;
	}

	const char *home = getenv("HOME");
	if (!home || home[0] == '\0')
		return false;

	int n = snprintf(path, path_size, "%s/.config/pager.toml", home);

	if (n < 0 || (size_t)n >= path_size)
		return false;
	return true;
}

static bool resolve_generate_config_path(const char *custom_path, char *path, size_t path_size)
{
	if (custom_path && custom_path[0] != '\0') {
		int n = snprintf(path, path_size, "%s", custom_path);
		return n >= 0 && (size_t)n < path_size;
	}

	const char *explicit_path = getenv("PAGER_CONFIG");
	if (explicit_path && explicit_path[0] != '\0') {
		int n = snprintf(path, path_size, "%s", explicit_path);
		return n >= 0 && (size_t)n < path_size;
	}

	return resolve_default_config_path(path, path_size);
}

static bool ensure_dir_exists(const char *path)
{
	struct stat st;
	if (stat(path, &st) == 0)
		return S_ISDIR(st.st_mode);

	if (errno != ENOENT)
		return false;

	if (mkdir(path, 0755) == 0)
		return true;

	if (errno != EEXIST)
		return false;

	if (stat(path, &st) == 0)
		return S_ISDIR(st.st_mode);

	return false;
}

static bool ensure_config_parent_dirs(const char *config_path)
{
	if (!config_path || config_path[0] == '\0')
		return false;

	char tmp[CONFIG_PATH_MAX];
	int n = snprintf(tmp, sizeof(tmp), "%s", config_path);
	if (n < 0 || (size_t)n >= sizeof(tmp)) {
		errno = ENAMETOOLONG;
		return false;
	}

	char *slash = strrchr(tmp, '/');
	if (!slash)
		return true;

	if (slash == tmp) {
		tmp[1] = '\0';
		return true;
	}

	*slash = '\0';

	for (char *p = tmp + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;

		*p = '\0';
		if (!ensure_dir_exists(tmp))
			return false;
		*p = '/';
	}

	return ensure_dir_exists(tmp);
}

static bool resolve_config_path(char *path, size_t path_size)
{
	const char *explicit_path = getenv("PAGER_CONFIG");
	if (explicit_path && explicit_path[0] != '\0') {
		int n = snprintf(path, path_size, "%s", explicit_path);
		if (n < 0 || (size_t)n >= path_size)
			return false;
		return true;
	}

	if (!resolve_default_config_path(path, path_size))
		return false;

	if (access(path, R_OK) == 0)
		return true;

	return false;
}

static void apply_config_kv(
	const char *path,
	size_t line_no,
	enum config_section section,
	const char *key,
	const char *value,
	size_t *start_line,
	char *start_pattern,
	bool *has_start_pattern)
{
	if (section == CONFIG_SECTION_KEYBINDINGS) {
		enum pager_action action = ACTION_NONE;
		if (!parse_binding_action(key, &action)) {
			fprintf(stderr, "pager: %s:%zu unknown binding action %s\n", path, line_no, key);
			return;
		}

		int parsed_keys[MAX_BINDINGS_PER_ACTION];
		size_t parsed_len = 0;
		if (!parse_binding_key_value(value, parsed_keys, MAX_BINDINGS_PER_ACTION, &parsed_len)) {
			fprintf(stderr, "pager: %s:%zu invalid key list for %s\n", path, line_no, key);
			return;
		}

		for (size_t i = 0; i < parsed_len; i++) {
			if (!bind_key_to_action(action, parsed_keys[i])) {
				fprintf(stderr, "pager: %s:%zu too many bindings for %s (max %d)\n",
					path, line_no, key, MAX_BINDINGS_PER_ACTION);
				return;
			}
		}
		return;
	}

	if (section != CONFIG_SECTION_ROOT)
		return;

	if (strcmp(key, "number") == 0) {
		bool b = false;
		if (!parse_toml_bool(value, &b)) {
			fprintf(stderr, "pager: %s:%zu invalid boolean for %s\n", path, line_no, key);
			return;
		}
		show_line_numbers = b;
		return;
	}

	if (strcmp(key, "wrap") == 0) {
		bool b = false;
		if (!parse_toml_bool(value, &b)) {
			fprintf(stderr, "pager: %s:%zu invalid boolean for %s\n", path, line_no, key);
			return;
		}
		wrap_mode = b;
		return;
	}

	if (strcmp(key, "follow") == 0) {
		bool b = false;
		if (!parse_toml_bool(value, &b)) {
			fprintf(stderr, "pager: %s:%zu invalid boolean for %s\n", path, line_no, key);
			return;
		}
		follow_mode = b;
		return;
	}

	if (strcmp(key, "search_regex") == 0) {
		bool b = false;
		if (!parse_toml_bool(value, &b)) {
			fprintf(stderr, "pager: %s:%zu invalid boolean for %s\n", path, line_no, key);
			return;
		}
		search_use_regex = b;
		return;
	}

	if (strcmp(key, "search_wrap") == 0) {
		bool b = false;
		if (!parse_toml_bool(value, &b)) {
			fprintf(stderr, "pager: %s:%zu invalid boolean for %s\n", path, line_no, key);
			return;
		}
		search_wrap = b;
		return;
	}

	if (strcmp(key, "search_case") == 0) {
		enum search_case_mode mode = SEARCH_CASE_SMART;
		if (!parse_search_case(value, &mode)) {
			fprintf(stderr, "pager: %s:%zu invalid value for %s (use false, true, or \"smart\")\n",
				path, line_no, key);
			return;
		}
		search_case = mode;
		return;
	}

	if (strcmp(key, "search_current_match_sgr") == 0) {
		if (!parse_toml_sgr(value, search_current_match_sgr, sizeof(search_current_match_sgr))) {
			fprintf(stderr, "pager: %s:%zu invalid SGR params/aliases for %s (example: \"reversed yellow\")\n",
				path, line_no, key);
			return;
		}
		return;
	}

	if (strcmp(key, "search_other_match_sgr") == 0) {
		if (!parse_toml_sgr(value, search_other_match_sgr, sizeof(search_other_match_sgr))) {
			fprintf(stderr, "pager: %s:%zu invalid SGR params/aliases for %s (example: \"reversed\")\n",
				path, line_no, key);
			return;
		}
		return;
	}

	if (strcmp(key, "command_popup_rows") == 0) {
		size_t n = 0;
		if (!parse_toml_size(value, &n)) {
			fprintf(stderr, "pager: %s:%zu invalid positive integer for %s\n", path, line_no, key);
			return;
		}
		if (n > MAX_COMMAND_POPUP_ROWS) {
			fprintf(stderr, "pager: %s:%zu %s exceeds max %d\n",
				path, line_no, key, MAX_COMMAND_POPUP_ROWS);
			return;
		}
		command_popup_rows = n;
		return;
	}

	if (strcmp(key, "sync_output") == 0) {
		bool b = false;
		if (!parse_toml_bool(value, &b)) {
			fprintf(stderr, "pager: %s:%zu invalid boolean for %s\n", path, line_no, key);
			return;
		}
		sync_output_enabled = b;
		return;
	}

	if (strcmp(key, "quit_if_one_screen") == 0) {
		bool b = false;
		if (!parse_toml_bool(value, &b)) {
			fprintf(stderr, "pager: %s:%zu invalid boolean for %s\n", path, line_no, key);
			return;
		}
		quit_if_one_screen = b;
		return;
	}

	if (strcmp(key, "line") == 0) {
		size_t n = 0;
		if (!parse_toml_size(value, &n)) {
			fprintf(stderr, "pager: %s:%zu invalid positive integer for %s\n", path, line_no, key);
			return;
		}
		*start_line = n;
		return;
	}

	if (strcmp(key, "pattern") == 0) {
		if (!parse_toml_string(value, start_pattern, MAX_QUERY)) {
			fprintf(stderr, "pager: %s:%zu invalid string for %s\n", path, line_no, key);
			return;
		}
		*has_start_pattern = start_pattern[0] != '\0';
		return;
	}

	fprintf(stderr, "pager: %s:%zu unknown config key %s\n", path, line_no, key);
}

static void load_config_defaults(size_t *start_line, char *start_pattern, bool *has_start_pattern)
{
	char config_path[CONFIG_PATH_MAX];
	if (!resolve_config_path(config_path, sizeof(config_path)))
		return;

	FILE *fp = fopen(config_path, "r");
	if (!fp) {
		fprintf(stderr, "pager: failed to read config %s: %s\n", config_path, strerror(errno));
		return;
	}

	char *line = NULL;
	size_t cap = 0;
	ssize_t nread = 0;
	enum config_section section = CONFIG_SECTION_ROOT;
	size_t line_no = 0;

	while ((nread = getline(&line, &cap, fp)) != -1) {
		line_no++;
		(void)nread;
		strip_toml_comment(line);
		trim_right(line);
		char *text = trim_left(line);
		if (*text == '\0')
			continue;

		if (text[0] == '[') {
			char *end = strchr(text, ']');
			if (!end)
				continue;
			*end = '\0';
			char *section_name = trim_left(text + 1);
			trim_right(section_name);
			section = parse_config_section(section_name);
			continue;
		}

		char *eq = strchr(text, '=');
		if (!eq)
			continue;
		*eq = '\0';
		char *key = trim_left(text);
		trim_right(key);
		char *value = trim_left(eq + 1);
		trim_right(value);

		if (*key == '\0' || *value == '\0')
			continue;

		apply_config_kv(config_path, line_no, section, key, value, start_line, start_pattern, has_start_pattern);
	}

	free(line);
	fclose(fp);
}

static bool write_generated_config(FILE *fp)
{
	if (!fp)
		return false;

	if (fprintf(fp,
		"# pager config template\n"
		"#\n"
		"# File path:\n"
		"#   $PAGER_CONFIG, or $XDG_CONFIG_HOME/pager.toml\n"
		"#   (default: ~/.config/pager.toml)\n"
		"#\n"
		"# Root keys (global options). Uncomment to override defaults.\n"
		"# number = false\n"
		"# wrap = true\n"
		"# follow = false\n"
		"# line = 1\n"
		"# pattern = \"\"\n"
		"# search_regex = true\n"
		"# search_wrap = true\n"
		"# search_case = \"smart\"  # false=insensitive, true=sensitive, smart=smart-case\n"
		"# search_current_match_sgr = \"reversed yellow\"  # aliases also accepted\n"
		"# search_other_match_sgr = \"reversed\"\n"
		"# command_popup_rows = 5  # max 32\n"
		"# sync_output = true\n"
		"# quit_if_one_screen = false\n"
		"#\n"
		"# Keybindings: each key can map to one action.\n"
		"# Defining a key here moves it from any previous action.\n"
		"[keybindings]\n"
		"# quit = [\"q\"]\n"
		"# down = [\"j\", \"down\", \"enter\", \"ctrl-j\"]\n"
		"# up = [\"k\", \"up\"]\n"
		"# page_down = [\"space\", \"pgdn\"]\n"
		"# page_up = [\"b\", \"pgup\"]\n"
		"# half_page_down = [\"d\"]\n"
		"# half_page_up = [\"u\"]\n"
		"# top = [\"g\", \"home\"]\n"
		"# bottom = [\"G\", \"end\"]\n"
		"# left = [\"left\"]\n"
		"# right = [\"right\"]\n"
		"# mouse_up = [\"mouse-up\"]\n"
		"# mouse_down = [\"mouse-down\"]\n"
		"# wrap = [\"w\"]\n"
		"# search_forward = [\"/\"]\n"
		"# search_backward = [\"?\"]\n"
		"# next_match = [\"n\"]\n"
		"# prev_match = [\"N\"]\n"
		"# next_file = [\"]\"]\n"
		"# prev_file = [\"[\"]\n"
		"# command = [\":\"]\n"
		"# follow = [\"F\"]\n"
		"# yank = [\"y\"]\n"
		"# help = [\"h\"]\n") < 0)
		return false;

	return true;
}

static int generate_default_config_file(const char *custom_path)
{
	char path[CONFIG_PATH_MAX];
	if (!resolve_generate_config_path(custom_path, path, sizeof(path))) {
		fprintf(stderr, "pager: unable to determine config path\n");
		return EXIT_FAILURE;
	}

	if (!ensure_config_parent_dirs(path)) {
		fprintf(stderr, "pager: failed to create config directory for %s: %s\n", path, strerror(errno));
		return EXIT_FAILURE;
	}

	if (access(path, F_OK) == 0) {
		fprintf(stderr, "pager: config already exists at %s\n", path);
		fprintf(stderr, "pager: remove it first if you want to regenerate\n");
		return EXIT_FAILURE;
	}

	FILE *fp = fopen(path, "w");
	if (!fp) {
		fprintf(stderr, "pager: failed to write %s: %s\n", path, strerror(errno));
		return EXIT_FAILURE;
	}

	bool ok = write_generated_config(fp);
	if (fclose(fp) != 0)
		ok = false;

	if (!ok) {
		fprintf(stderr, "pager: failed to write %s: %s\n", path, strerror(errno));
		return EXIT_FAILURE;
	}

	fprintf(stdout, "Generated config: %s\n", path);
	return EXIT_SUCCESS;
}

static void sgr_state_clear(struct sgr_state *state)
{
	if (!state)
		return;
	state->len = 0;
}

static bool is_sgr_sequence(const char *seq, size_t len)
{
	return len >= 3 && seq[0] == '\x1b' && seq[1] == '[' && seq[len - 1] == 'm';
}

static bool sgr_sequence_contains_reset(const char *seq, size_t len, bool *only_reset)
{
	if (!is_sgr_sequence(seq, len)) {
		if (only_reset)
			*only_reset = false;
		return false;
	}

	if (len == 3) {
		if (only_reset)
			*only_reset = true;
		return true;
	}

	bool has_any = false;
	bool has_reset = false;
	bool has_non_reset = false;

	size_t i = 2;
	while (i + 1 < len) {
		if (seq[i] == ';') {
			has_any = true;
			has_reset = true;
			i++;
			continue;
		}

		if (!isdigit((unsigned char)seq[i])) {
			i++;
			continue;
		}

		int value = 0;
		while (i + 1 < len && isdigit((unsigned char)seq[i])) {
			value = value * 10 + (seq[i] - '0');
			i++;
		}

		has_any = true;
		if (value == 0)
			has_reset = true;
		else
			has_non_reset = true;

		if (i + 1 < len && seq[i] == ';')
			i++;
	}

	if (!has_any) {
		has_reset = true;
	}

	if (only_reset)
		*only_reset = has_reset && !has_non_reset;
	return has_reset;
}

static void sgr_state_append(struct sgr_state *state, const char *seq, size_t len)
{
	if (!state || !is_sgr_sequence(seq, len))
		return;

	bool only_reset = false;
	bool has_reset = sgr_sequence_contains_reset(seq, len, &only_reset);
	if (has_reset)
		state->len = 0;
	if (only_reset)
		return;

	if (len > sizeof(state->bytes)) {
		state->len = 0;
		return;
	}

	if (state->len + len > sizeof(state->bytes))
		state->len = 0;

	if (state->len + len <= sizeof(state->bytes)) {
		memcpy(state->bytes + state->len, seq, len);
		state->len += len;
	}
}

static void emit_sgr_state(const struct sgr_state *state)
{
	if (!state || state->len == 0)
		return;
	WRITE_ERR(state->bytes, state->len);
}

static size_t skip_ansi_sequence(const char *src, size_t pos, size_t end)
{
	if (pos >= end || src[pos] != '\x1b')
		return 0;

	if (pos + 1 >= end)
		return 1;

	unsigned char c1 = (unsigned char)src[pos + 1];

	if (c1 == '[') {
		size_t i = pos + 2;
		while (i < end) {
			unsigned char ch = (unsigned char)src[i];
			if (ch >= 0x40 && ch <= 0x7e)
				return i - pos + 1;
			i++;
		}
		return end - pos;
	}

	if (c1 == ']' || c1 == 'P' || c1 == 'X' || c1 == '^' || c1 == '_') {
		size_t i = pos + 2;
		while (i < end) {
			unsigned char ch = (unsigned char)src[i];
			if (ch == '\x07')
				return i - pos + 1;
			if (ch == '\x1b' && i + 1 < end && src[i + 1] == '\\')
				return i - pos + 2;
			i++;
		}
		return end - pos;
	}

	if (c1 == '(' || c1 == ')' || c1 == '*' || c1 == '+' || c1 == '-' || c1 == '.' || c1 == '/') {
		if (pos + 2 < end)
			return 3;
		return end - pos;
	}

	if (pos + 2 <= end)
		return 2;
	return end - pos;
}

static size_t utf8_char_len(const unsigned char *s, size_t max)
{
	if (max == 0)
		return 0;

	if (s[0] < 0x80)
		return 1;
	if ((s[0] & 0xe0) == 0xc0)
		return max >= 2 ? 2 : 1;
	if ((s[0] & 0xf0) == 0xe0)
		return max >= 3 ? 3 : 1;
	if ((s[0] & 0xf8) == 0xf0)
		return max >= 4 ? 4 : 1;
	return 1;
}

static uint32_t utf8_decode(const unsigned char *s, size_t len, size_t *used)
{
	if (len == 0) {
		if (used)
			*used = 0;
		return 0;
	}

	if (s[0] < 0x80) {
		if (used)
			*used = 1;
		return s[0];
	}

	size_t n = utf8_char_len(s, len);
	if (n == 1) {
		if (used)
			*used = 1;
		return s[0];
	}

	uint32_t cp = 0;
	if (n == 2) {
		cp = (uint32_t)(s[0] & 0x1f) << 6;
		cp |= (uint32_t)(s[1] & 0x3f);
	} else if (n == 3) {
		cp = (uint32_t)(s[0] & 0x0f) << 12;
		cp |= (uint32_t)(s[1] & 0x3f) << 6;
		cp |= (uint32_t)(s[2] & 0x3f);
	} else {
		cp = (uint32_t)(s[0] & 0x07) << 18;
		cp |= (uint32_t)(s[1] & 0x3f) << 12;
		cp |= (uint32_t)(s[2] & 0x3f) << 6;
		cp |= (uint32_t)(s[3] & 0x3f);
	}

	if (used)
		*used = n;
	return cp;
}

static bool is_wide_cjk(uint32_t cp)
{
	return
		(cp >= 0x1100 && cp <= 0x115f) ||
		(cp >= 0x2329 && cp <= 0x232a) ||
		(cp >= 0x2e80 && cp <= 0xa4cf) ||
		(cp >= 0xac00 && cp <= 0xd7a3) ||
		(cp >= 0xf900 && cp <= 0xfaff) ||
		(cp >= 0xfe10 && cp <= 0xfe19) ||
		(cp >= 0xfe30 && cp <= 0xfe6f) ||
		(cp >= 0xff00 && cp <= 0xff60) ||
		(cp >= 0xffe0 && cp <= 0xffe6) ||
		(cp >= 0x20000 && cp <= 0x2fffd) ||
		(cp >= 0x30000 && cp <= 0x3fffd);
}

struct display_token {
	size_t bytes;
	size_t plain_bytes;
	size_t width;
	bool ansi;
	bool tab;
};

static struct display_token next_display_token(const char *line, size_t len, size_t pos, size_t col)
{
	struct display_token token = {
		.bytes = 0,
		.plain_bytes = 0,
		.width = 0,
		.ansi = false,
		.tab = false,
	};

	if (pos >= len)
		return token;

	unsigned char ch = (unsigned char)line[pos];
	if (ch == '\x1b') {
		size_t skip = skip_ansi_sequence(line, pos, len);
		if (skip > 0) {
			token.bytes = skip;
			token.ansi = true;
			return token;
		}
	}

	if (line[pos] == '\t') {
		token.bytes = 1;
		token.plain_bytes = 1;
		token.width = TAB_SIZE - (col % TAB_SIZE);
		token.tab = true;
		return token;
	}

	if (ch < 0x20 || ch == 0x7f) {
		token.bytes = 1;
		token.plain_bytes = 1;
		token.width = 0;
		return token;
	}

	if (ch < 0x80) {
		token.bytes = 1;
		token.plain_bytes = 1;
		token.width = 1;
		return token;
	}

	size_t used = 1;
	uint32_t cp = utf8_decode((const unsigned char *)line + pos, len - pos, &used);
	token.bytes = used;
	token.plain_bytes = used;
	token.width = is_wide_cjk(cp) ? 2 : 1;
	return token;
}

static size_t display_width(const char *line, size_t len)
{
	size_t pos = 0;
	size_t col = 0;

	while (pos < len) {
		struct display_token token = next_display_token(line, len, pos, col);
		if (token.bytes == 0)
			break;
		if (!token.ansi)
			col += token.width;
		pos += token.bytes;
	}

	return col;
}

static size_t byte_offset_at_display_col(
	const char *line,
	size_t len,
	size_t target_col,
	struct sgr_state *state,
	size_t *plain_pos_out)
{
	size_t pos = 0;
	size_t col = 0;
	size_t plain_pos = 0;

	if (state)
		sgr_state_clear(state);

	while (pos < len) {
		struct display_token token = next_display_token(line, len, pos, col);
		if (token.bytes == 0)
			break;

		if (token.ansi) {
			if (state)
				sgr_state_append(state, line + pos, token.bytes);
			pos += token.bytes;
			continue;
		}

		if (col + token.width > target_col)
			break;

		col += token.width;
		plain_pos += token.plain_bytes;
		pos += token.bytes;
	}

	if (plain_pos_out)
		*plain_pos_out = plain_pos;

	return pos;
}

static size_t strip_ansi(const char *src, size_t src_len, char *dst, size_t dst_cap)
{
	size_t in = 0;
	size_t out = 0;

	if (dst_cap == 0)
		return 0;

	while (in < src_len) {
		if (src[in] == '\x1b') {
			size_t skip = skip_ansi_sequence(src, in, src_len);
			if (skip > 0) {
				in += skip;
				continue;
			}
		}

		if (out + 1 < dst_cap)
			dst[out] = src[in];
		out++;
		in++;
	}

	if (out >= dst_cap)
		out = dst_cap - 1;
	dst[out] = '\0';
	return out;
}

static bool is_probably_binary(const char *data, size_t len)
{
	size_t sample_len = len < 4096 ? len : 4096;
	size_t odd = 0;

	for (size_t i = 0; i < sample_len; i++) {
		unsigned char ch = (unsigned char)data[i];
		if (ch == '\0')
			return true;

		if (ch < 32) {
			if (ch == '\n' || ch == '\r' || ch == '\t' || ch == '\b' || ch == '\f' || ch == '\x1b')
				continue;
			odd++;
		} else if (ch == 127) {
			odd++;
		}
	}

	/* More than 20% unusual control bytes indicates likely binary data. */
	return sample_len > 0 && (odd * 5 > sample_len);
}

static bool prompt_binary_open(const char *label)
{
	if (raw_mode_enabled) {
		char msg[1024];
		snprintf(msg, sizeof(msg), "%s may be binary. Open anyway? (y/n)", label ? label : "Input");

		sync_frame_begin();
		PUTS_ERR(HIDE_CURSOR);
		PRINTF_ERR(CUP(%d, 1), term_rows);
		PUTS_ERR(SGR_REVERSE_VIDEO_ON);

		size_t len = strlen(msg);
		size_t cols = (size_t)term_cols;
		if (len > cols)
			len = cols;
		WRITE_ERR(msg, len);
		for (size_t i = len; i < cols; i++)
			PUTC_ERR(' ');

		PUTS_ERR(SGR_RESET);
		sync_frame_end();
		FLUSH_FILE(stderr);

		for (;;) {
			int key = read_key();
			if (key == KEY_NONE)
				continue;
			if (key == 'y' || key == 'Y')
				return true;
			if (key == 'n' || key == 'N' || key == 'q' || key == 'Q' || key == KEY_ESC)
				return false;
		}
	}

	struct termios old_termios;
	struct termios prompt_termios;
	bool term_ok = tcgetattr(STDIN_FILENO, &old_termios) == 0;
	if (term_ok) {
		prompt_termios = old_termios;
		prompt_termios.c_lflag &= (tcflag_t)~(ICANON | ECHO);
		prompt_termios.c_cc[VMIN] = 1;
		prompt_termios.c_cc[VTIME] = 0;
		(void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &prompt_termios);
	}

	PRINTF_ERR("%s may be binary. Open anyway? (y/n) ", label ? label : "Input");
	FLUSH_FILE(stderr);

	bool allow = false;
	for (;;) {
		char ch = 0;
		ssize_t n = read(STDIN_FILENO, &ch, 1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0)
			break;

		if (ch == 'y' || ch == 'Y') {
			allow = true;
			break;
		}
		if (ch == 'n' || ch == 'N' || ch == 'q' || ch == 'Q' || ch == '\x1b') {
			allow = false;
			break;
		}
	}

	if (term_ok)
		(void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_termios);

	PUTS_ERR("\n");
	FLUSH_FILE(stderr);
	return allow;
}

static void ensure_buffer_capacity(size_t want)
{
	if (want <= buffer_capacity)
		return;

	size_t new_capacity = buffer_capacity > 0 ? buffer_capacity : 4096;
	while (new_capacity < want) {
		if (new_capacity > SIZE_MAX / 4) {
			PUTS_ERR("Error: input too large\n");
			exit(EXIT_FAILURE);
		}
		new_capacity *= 4;
	}

	char *new_buffer = realloc(buffer, new_capacity);
	if (!new_buffer)
		die("realloc");

	buffer = new_buffer;
	buffer_capacity = new_capacity;
}

static void clear_content(void)
{
	free(buffer);
	buffer = NULL;
	buffer_size = 0;
	buffer_capacity = 0;

	free(line_offsets);
	line_offsets = NULL;
	line_count = 0;
	line_capacity = 0;

	free(filtered_lines);
	filtered_lines = NULL;
	filtered_count = 0;
	filter_active = false;
	filter_pattern[0] = '\0';
	if (filter_regex_valid) {
		regfree(&filter_regex);
		filter_regex_valid = false;
	}

	free(matches);
	matches = NULL;
	match_count = 0;
	match_capacity = 0;
	current_match = 0;

	free(line_match_first);
	line_match_first = NULL;
	free(line_match_count);
	line_match_count = NULL;

	search_query[0] = '\0';
	if (search_regex_valid) {
		regfree(&search_regex);
		search_regex_valid = false;
	}

	scroll_line = 0;
	scroll_col = 0;
}

static void load_fd(int fd)
{
	buffer_size = 0;
	ensure_buffer_capacity(4096);

	for (;;) {
		if (buffer_capacity == buffer_size)
			ensure_buffer_capacity(buffer_capacity + 1);

		ssize_t n = read(fd, buffer + buffer_size, buffer_capacity - buffer_size);
		if (n > 0) {
			buffer_size += (size_t)n;
			continue;
		}
		if (n == 0)
			break;
		if (errno == EINTR)
			continue;
		die("read");
	}
}

static void build_line_index(void)
{
	size_t count = 1;
	for (size_t i = 0; i < buffer_size; i++) {
		if (buffer[i] == '\n')
			count++;
	}

	if (count > line_capacity) {
		size_t *new_offsets = realloc(line_offsets, count * sizeof(size_t));
		if (!new_offsets)
			die("realloc");
		line_offsets = new_offsets;
		line_capacity = count;
	}

	line_offsets[0] = 0;
	line_count = 1;
	for (size_t i = 0; i < buffer_size; i++) {
		if (buffer[i] == '\n')
			line_offsets[line_count++] = i + 1;
	}

	free(line_match_first);
	free(line_match_count);
	line_match_first = calloc(line_count, sizeof(size_t));
	line_match_count = calloc(line_count, sizeof(size_t));
	if (!line_match_first || !line_match_count)
		die("calloc");
}

static const char *get_line(size_t line_idx, size_t *out_len)
{
	static const char empty[] = "";

	if (line_count == 0 || line_idx >= line_count) {
		if (out_len)
			*out_len = 0;
		return empty;
	}

	size_t start = line_offsets[line_idx];
	size_t end = (line_idx + 1 < line_count) ? line_offsets[line_idx + 1] : buffer_size;

	if (end > start && buffer[end - 1] == '\n')
		end--;
	if (end > start && buffer[end - 1] == '\r')
		end--;

	if (out_len)
		*out_len = end - start;
	return buffer + start;
}

static bool has_uppercase(const char *text)
{
	for (size_t i = 0; text[i] != '\0'; i++) {
		if (isupper((unsigned char)text[i]))
			return true;
	}
	return false;
}

static bool search_ignore_case_for_pattern(const char *pattern)
{
	if (search_case == SEARCH_CASE_INSENSITIVE)
		return true;
	if (search_case == SEARCH_CASE_SENSITIVE)
		return false;
	return !has_uppercase(pattern);
}

static bool chars_equal(char a, char b, bool ignore_case)
{
	if (!ignore_case)
		return a == b;
	return tolower((unsigned char)a) == tolower((unsigned char)b);
}

static bool find_plain_substring(
	const char *haystack,
	size_t hay_len,
	const char *needle,
	size_t needle_len,
	bool ignore_case,
	size_t start,
	size_t *match_start)
{
	if (!haystack || !needle || !match_start)
		return false;
	if (needle_len == 0 || hay_len < needle_len || start > hay_len - needle_len)
		return false;

	for (size_t i = start; i + needle_len <= hay_len; i++) {
		bool match = true;
		for (size_t j = 0; j < needle_len; j++) {
			if (!chars_equal(haystack[i + j], needle[j], ignore_case)) {
				match = false;
				break;
			}
		}
		if (match) {
			*match_start = i;
			return true;
		}
	}

	return false;
}

static bool compile_regex_with_case(
	regex_t *out,
	const char *pattern,
	bool ignore_case,
	char *errbuf,
	size_t errbuf_size)
{
	int flags = REG_EXTENDED;
	if (ignore_case)
		flags |= REG_ICASE;

	int rc = regcomp(out, pattern, flags);
	if (rc != 0) {
		if (errbuf && errbuf_size > 0)
			regerror(rc, out, errbuf, errbuf_size);
		return false;
	}

	return true;
}

static size_t view_line_count(void)
{
	return filter_active ? filtered_count : line_count;
}

static size_t view_to_raw_line(size_t view_idx)
{
	if (filter_active)
		return filtered_lines[view_idx];
	return view_idx;
}

static bool raw_to_view_line(size_t raw_line, size_t *view_idx)
{
	if (!filter_active) {
		if (raw_line >= line_count)
			return false;
		if (view_idx)
			*view_idx = raw_line;
		return true;
	}

	size_t lo = 0;
	size_t hi = filtered_count;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (filtered_lines[mid] == raw_line) {
			if (view_idx)
				*view_idx = mid;
			return true;
		}
		if (filtered_lines[mid] < raw_line)
			lo = mid + 1;
		else
			hi = mid;
	}
	return false;
}

static void clamp_scroll(void)
{
	size_t count = view_line_count();
	if (count == 0) {
		scroll_line = 0;
		scroll_col = 0;
		return;
	}

	size_t max_scroll = 0;
	if (count > (size_t)content_rows)
		max_scroll = count - (size_t)content_rows;

	if (scroll_line > max_scroll)
		scroll_line = max_scroll;

	if (wrap_mode)
		scroll_col = 0;
}

static void update_term_size(void)
{
	struct winsize ws;
	if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
		term_rows = ws.ws_row;
		term_cols = ws.ws_col;
	} else {
		term_rows = 24;
		term_cols = 80;
	}

	content_rows = term_rows > 1 ? term_rows - 1 : 1;

	if (show_line_numbers) {
		size_t max_line = line_count > 0 ? line_count : 1;
		gutter_width = (int)digits_u(max_line) + 1;
	} else {
		gutter_width = 0;
	}

	content_cols = term_cols - gutter_width;
	if (content_cols < 1)
		content_cols = 1;

	clamp_scroll();
}

static void clear_matches(void)
{
	free(matches);
	matches = NULL;
	match_count = 0;
	match_capacity = 0;
	current_match = 0;

	if (search_regex_valid) {
		regfree(&search_regex);
		search_regex_valid = false;
	}

	if (line_match_first)
		memset(line_match_first, 0, line_count * sizeof(size_t));
	if (line_match_count)
		memset(line_match_count, 0, line_count * sizeof(size_t));
}

static void append_match(size_t line_idx, size_t start, size_t end)
{
	if (match_count == match_capacity) {
		size_t new_capacity = match_capacity > 0 ? match_capacity * 2 : 128;
		struct search_match *new_matches = realloc(matches, new_capacity * sizeof(struct search_match));
		if (!new_matches)
			die("realloc");
		matches = new_matches;
		match_capacity = new_capacity;
	}

	matches[match_count++] = (struct search_match){
		.line_idx = line_idx,
		.start = start,
		.end = end,
	};
}

static void build_match_line_index(void)
{
	if (!line_match_first || !line_match_count)
		return;

	memset(line_match_first, 0, line_count * sizeof(size_t));
	memset(line_match_count, 0, line_count * sizeof(size_t));

	for (size_t i = 0; i < match_count; i++) {
		size_t line_idx = matches[i].line_idx;
		if (line_idx >= line_count)
			continue;
		if (line_match_count[line_idx] == 0)
			line_match_first[line_idx] = i;
		line_match_count[line_idx]++;
	}
}

static void ensure_match_visible(size_t match_idx)
{
	if (match_idx >= match_count)
		return;

	size_t view_idx = 0;
	if (!raw_to_view_line(matches[match_idx].line_idx, &view_idx))
		return;

	if (view_idx < scroll_line)
		scroll_line = view_idx;
	else if (view_idx >= scroll_line + (size_t)content_rows)
		scroll_line = view_idx;

	clamp_scroll();
}

static void find_all_matches(const char *pattern, bool preserve_current_line)
{
	size_t old_line = 0;
	if (preserve_current_line && match_count > 0)
		old_line = matches[current_match].line_idx;

	clear_matches();

	if (!pattern || pattern[0] == '\0') {
		search_query[0] = '\0';
		return;
	}

	snprintf(search_query, sizeof(search_query), "%s", pattern);

	size_t needle_len = strlen(pattern);
	bool ignore_case = search_ignore_case_for_pattern(pattern);

	if (search_use_regex) {
		if (!compile_regex_with_case(&search_regex, pattern, ignore_case, regex_error, sizeof(regex_error))) {
			set_flash(regex_error[0] ? regex_error : "Invalid regex");
			return;
		}
		search_regex_valid = true;
	}

	size_t view_count = view_line_count();
	char *plain = NULL;
	size_t plain_cap = 0;

	for (size_t i = 0; i < view_count; i++) {
		size_t raw_line = view_to_raw_line(i);
		size_t len = 0;
		const char *line = get_line(raw_line, &len);

		if (len + 1 > plain_cap) {
			plain_cap = len + 1;
			char *new_plain = realloc(plain, plain_cap);
			if (!new_plain)
				die("realloc");
			plain = new_plain;
		}

		size_t plain_len = strip_ansi(line, len, plain, plain_cap);

		if (search_use_regex) {
			size_t offset = 0;
			while (offset <= plain_len) {
				regmatch_t rm;
				int rc = regexec(&search_regex, plain + offset, 1, &rm, 0);
				if (rc != 0)
					break;

				if (rm.rm_so < 0 || rm.rm_eo < 0)
					break;

				size_t start = offset + (size_t)rm.rm_so;
				size_t end = offset + (size_t)rm.rm_eo;
				append_match(raw_line, start, end);

				if (end == start) {
					if (end >= plain_len)
						break;
					offset = end + 1;
				} else {
					offset = end;
				}
			}
		} else {
			size_t offset = 0;
			while (offset + needle_len <= plain_len) {
				size_t start = 0;
				if (!find_plain_substring(plain, plain_len, pattern, needle_len, ignore_case, offset, &start))
					break;
				append_match(raw_line, start, start + needle_len);
				offset = start + needle_len;
			}
		}
	}

	free(plain);

	build_match_line_index();

	if (match_count == 0) {
		set_flash("No matches");
		current_match = 0;
		return;
	}

	if (preserve_current_line) {
		for (size_t i = 0; i < match_count; i++) {
			if (matches[i].line_idx >= old_line) {
				current_match = i;
				ensure_match_visible(current_match);
				return;
			}
		}
	}

	current_match = 0;
	ensure_match_visible(current_match);
}

static void jump_to_next_match(void)
{
	if (match_count == 0)
		return;

	if (search_forward) {
		if (current_match + 1 < match_count)
			current_match++;
		else if (search_wrap)
			current_match = 0;
		else {
			set_flash("Search hit bottom");
			return;
		}
	} else {
		if (current_match > 0)
			current_match--;
		else if (search_wrap)
			current_match = match_count - 1;
		else {
			set_flash("Search hit top");
			return;
		}
	}

	ensure_match_visible(current_match);
}

static void jump_to_prev_match(void)
{
	if (match_count == 0)
		return;

	if (search_forward) {
		if (current_match > 0)
			current_match--;
		else if (search_wrap)
			current_match = match_count - 1;
		else {
			set_flash("Search hit top");
			return;
		}
	} else {
		if (current_match + 1 < match_count)
			current_match++;
		else if (search_wrap)
			current_match = 0;
		else {
			set_flash("Search hit bottom");
			return;
		}
	}

	ensure_match_visible(current_match);
}

static void apply_search_query_from_anchor(const char *query, bool backward, size_t anchor_raw_line)
{
	if (!query || query[0] == '\0') {
		clear_matches();
		search_query[0] = '\0';
		return;
	}

	search_forward = !backward;
	find_all_matches(query, false);
	if (match_count == 0)
		return;

	if (backward) {
		size_t idx = match_count - 1;
		for (size_t i = match_count; i > 0; i--) {
			if (matches[i - 1].line_idx <= anchor_raw_line) {
				idx = i - 1;
				break;
			}
		}
		current_match = idx;
	} else {
		size_t idx = 0;
		while (idx < match_count && matches[idx].line_idx < anchor_raw_line)
			idx++;
		if (idx >= match_count)
			idx = 0;
		current_match = idx;
	}

	ensure_match_visible(current_match);
}

static void restore_search_state_after_cancel(
	const char *saved_query,
	bool saved_search_forward,
	bool saved_match_valid,
	struct search_match saved_match,
	size_t saved_scroll_line,
	size_t saved_scroll_col,
	const char *saved_flash,
	uint64_t saved_flash_until)
{
	search_forward = saved_search_forward;

	if (!saved_query || saved_query[0] == '\0') {
		clear_matches();
		search_query[0] = '\0';
	} else {
		find_all_matches(saved_query, false);
		if (saved_match_valid && match_count > 0) {
			for (size_t i = 0; i < match_count; i++) {
				if (matches[i].line_idx != saved_match.line_idx)
					continue;
				if (matches[i].start != saved_match.start)
					continue;
				if (matches[i].end != saved_match.end)
					continue;
				current_match = i;
				break;
			}
			ensure_match_visible(current_match);
		}
	}

	scroll_line = saved_scroll_line;
	scroll_col = saved_scroll_col;
	clamp_scroll();

	snprintf(status_flash, sizeof(status_flash), "%s", saved_flash ? saved_flash : "");
	status_flash_until = saved_flash_until;
}

static void clear_filter(void)
{
	filter_active = false;
	filtered_count = 0;
	filter_pattern[0] = '\0';
	if (filter_regex_valid) {
		regfree(&filter_regex);
		filter_regex_valid = false;
	}
	clamp_scroll();
}

static bool apply_filter(const char *pattern, bool quiet)
{
	if (!pattern || pattern[0] == '\0') {
		clear_filter();
		if (search_query[0] != '\0')
			find_all_matches(search_query, true);
		return true;
	}

	regex_t re;
	bool ignore_case = !has_uppercase(pattern);
	if (!compile_regex_with_case(&re, pattern, ignore_case, regex_error, sizeof(regex_error))) {
		if (!quiet)
			set_flash(regex_error[0] ? regex_error : "Invalid filter regex");
		return false;
	}

	size_t *new_filtered = realloc(filtered_lines, line_count * sizeof(size_t));
	if (!new_filtered) {
		regfree(&re);
		die("realloc");
	}

	filtered_lines = new_filtered;
	filtered_count = 0;

	char *plain = NULL;
	size_t plain_cap = 0;

	for (size_t line_idx = 0; line_idx < line_count; line_idx++) {
		size_t len = 0;
		const char *line = get_line(line_idx, &len);

		if (len + 1 > plain_cap) {
			plain_cap = len + 1;
			char *new_plain = realloc(plain, plain_cap);
			if (!new_plain)
				die("realloc");
			plain = new_plain;
		}

		strip_ansi(line, len, plain, plain_cap);
		if (regexec(&re, plain, 0, NULL, 0) == 0)
			filtered_lines[filtered_count++] = line_idx;
	}

	free(plain);

	if (filter_regex_valid)
		regfree(&filter_regex);
	filter_regex = re;
	filter_regex_valid = true;
	filter_active = true;
	snprintf(filter_pattern, sizeof(filter_pattern), "%s", pattern);

	scroll_line = 0;
	scroll_col = 0;
	clamp_scroll();

	if (search_query[0] != '\0')
		find_all_matches(search_query, true);

	return true;
}

static void draw_gutter(size_t raw_line, bool continuation)
{
	if (!show_line_numbers)
		return;

	if (continuation) {
		for (int i = 0; i < gutter_width; i++)
			PUTC_ERR(' ');
		return;
	}

	char number[64];
	int n = snprintf(number, sizeof(number), "%*zu ", gutter_width - 1, raw_line + 1);
	if (n < 0)
		return;

	PUTS_ERR(SGR_FG_BRIGHT_YELLOW);
	WRITE_ERR(number, (size_t)n);
	PUTS_ERR(SGR_RESET);
}

static void render_line_slice(const char *line, size_t len, size_t start_col, size_t max_cols, size_t raw_line)
{
	struct sgr_state sgr;
	size_t plain_pos = 0;
	size_t pos = byte_offset_at_display_col(line, len, start_col, &sgr, &plain_pos);

	emit_sgr_state(&sgr);

	size_t line_first = 0;
	size_t line_matches = 0;
	if (line_match_count && raw_line < line_count) {
		line_matches = line_match_count[raw_line];
		if (line_matches > 0)
			line_first = line_match_first[raw_line];
	}

	size_t match_idx = 0;
	while (match_idx < line_matches && matches[line_first + match_idx].end <= plain_pos)
		match_idx++;

	size_t logical_col = start_col;
	size_t out_cols = 0;
	bool highlight_on = false;
	bool current_highlight = false;

	while (pos < len && out_cols < max_cols) {
		struct display_token token = next_display_token(line, len, pos, logical_col);
		if (token.bytes == 0)
			break;

		if (token.ansi) {
			WRITE_ERR(line + pos, token.bytes);
			sgr_state_append(&sgr, line + pos, token.bytes);
			pos += token.bytes;
			continue;
		}

		if (token.width > max_cols - out_cols)
			break;

		while (match_idx < line_matches && matches[line_first + match_idx].end <= plain_pos)
			match_idx++;

		bool should_highlight = false;
		bool should_current = false;
		if (match_idx < line_matches) {
			struct search_match *m = &matches[line_first + match_idx];
			size_t span_start = plain_pos;
			size_t span_end = plain_pos + token.plain_bytes;
			if (m->start < span_end && m->end > span_start) {
				should_highlight = true;
				should_current = (line_first + match_idx == current_match);
			}
		}

		if (highlight_on && (!should_highlight || should_current != current_highlight)) {
			PUTS_ERR(SGR_RESET);
			emit_sgr_state(&sgr);
			highlight_on = false;
		}

		if (!highlight_on && should_highlight) {
			const char *style = should_current ? search_current_match_sgr : search_other_match_sgr;
			WRITE_ERR(style, strlen(style));
			highlight_on = true;
			current_highlight = should_current;
		}

		if (token.tab) {
			for (size_t i = 0; i < token.width; i++)
				PUTC_ERR(' ');
		} else {
			WRITE_ERR(line + pos, token.bytes);
		}

		pos += token.bytes;
		plain_pos += token.plain_bytes;
		logical_col += token.width;
		out_cols += token.width;
	}

	if (highlight_on) {
		PUTS_ERR(SGR_RESET);
		emit_sgr_state(&sgr);
	}

	PUTS_ERR(SGR_RESET);
}

static size_t rendered_rows_for_line(const char *line, size_t len)
{
	if (!wrap_mode)
		return 1;

	size_t width = display_width(line, len);
	size_t first_cols = (size_t)content_cols;
	if (width <= first_cols)
		return 1;

	size_t mark_width = display_width(WRAP_CONT_MARK, strlen(WRAP_CONT_MARK));
	size_t cont_cols = first_cols > mark_width ? first_cols - mark_width : 1;
	size_t remaining = width - first_cols;
	return 1 + (remaining + cont_cols - 1) / cont_cols;
}

static bool view_line_screen_span(size_t target_view_idx, size_t *out_row_start, size_t *out_rows_visible)
{
	if (target_view_idx < scroll_line)
		return false;

	size_t row = 0;
	size_t total = view_line_count();
	for (size_t view_idx = scroll_line; view_idx < total && row < (size_t)content_rows; view_idx++) {
		size_t raw_line = view_to_raw_line(view_idx);
		size_t len = 0;
		const char *line = get_line(raw_line, &len);
		size_t line_rows = rendered_rows_for_line(line, len);

		if (view_idx == target_view_idx) {
			size_t visible = line_rows;
			if (row + visible > (size_t)content_rows)
				visible = (size_t)content_rows - row;
			if (visible == 0)
				return false;
			if (out_row_start)
				*out_row_start = row;
			if (out_rows_visible)
				*out_rows_visible = visible;
			return true;
		}

		row += line_rows;
	}

	return false;
}

static void redraw_visible_view_line(size_t view_idx)
{
	size_t row_start = 0;
	size_t rows_visible = 0;
	if (!view_line_screen_span(view_idx, &row_start, &rows_visible))
		return;

	size_t raw_line = view_to_raw_line(view_idx);
	size_t line_len = 0;
	const char *line = get_line(raw_line, &line_len);

	if (!wrap_mode) {
		PRINTF_ERR(CUP(%zu, 1), row_start + 1);
		draw_gutter(raw_line, false);
		render_line_slice(line, line_len, scroll_col, (size_t)content_cols, raw_line);
		PUTS_ERR(SGR_RESET ERASE_TO_EOL);
		return;
	}

	size_t mark_width = display_width(WRAP_CONT_MARK, strlen(WRAP_CONT_MARK));
	size_t start_col = 0;
	size_t wrap_idx = 0;

	for (size_t r = 0; r < rows_visible; r++) {
		PRINTF_ERR(CUP(%zu, 1), row_start + r + 1);
		draw_gutter(raw_line, wrap_idx > 0);

		size_t line_cols = (size_t)content_cols;
		if (wrap_idx > 0) {
			PUTS_ERR(SGR_HALF_BRIGHT_ON WRAP_CONT_MARK);
			PUTS_ERR(SGR_RESET);
			line_cols = (size_t)content_cols > mark_width ? (size_t)content_cols - mark_width : 1;
		}

		render_line_slice(line, line_len, start_col, line_cols, raw_line);
		PUTS_ERR(SGR_RESET ERASE_TO_EOL);

		start_col += line_cols;
		wrap_idx++;
	}
}

static bool redraw_search_transition_if_possible(size_t old_match_idx)
{
	if (match_count == 0 || old_match_idx >= match_count || current_match >= match_count)
		return false;
	if (old_match_idx == current_match)
		return false;

	size_t old_raw = matches[old_match_idx].line_idx;
	size_t new_raw = matches[current_match].line_idx;
	size_t old_view = 0;
	size_t new_view = 0;
	bool old_visible = raw_to_view_line(old_raw, &old_view);
	bool new_visible = raw_to_view_line(new_raw, &new_view);

	if ((!old_visible || old_view < scroll_line || old_view >= scroll_line + (size_t)content_rows)
		&& (!new_visible || new_view < scroll_line || new_view >= scroll_line + (size_t)content_rows))
		return false;

	sync_frame_begin();
	PUTS_ERR(HIDE_CURSOR);
	if (old_visible)
		redraw_visible_view_line(old_view);
	if (new_visible && (!old_visible || new_view != old_view))
		redraw_visible_view_line(new_view);
	render_status_bar();
	sync_frame_end();
	FLUSH_FILE(stderr);
	return true;
}

static void render_status_bar(void)
{
	size_t total = view_line_count();
	size_t top = total > 0 ? scroll_line + 1 : 0;
	size_t bottom = total > 0 ? scroll_line + (size_t)content_rows : 0;
	if (bottom > total)
		bottom = total;

	char source[512];
	if (source_is_stdin) {
		snprintf(source, sizeof(source), "[stdin]");
	} else if (file_count > 1) {
		snprintf(source, sizeof(source), "%s (%zu/%zu)", filenames[current_file], current_file + 1, file_count);
	} else if (file_count == 1) {
		snprintf(source, sizeof(source), "%s", filenames[current_file]);
	} else {
		snprintf(source, sizeof(source), "[buffer]");
	}

	char flags[128];
	snprintf(flags, sizeof(flags), "%s %s%s",
		wrap_mode ? "WRAP" : "NOWRAP",
		follow_mode ? "FOLLOW" : "VIEW",
		filter_active ? " FILTER" : "");

	char right[256];
	snprintf(right, sizeof(right), "%zu-%zu/%zu  %s", top, bottom, total, flags);

	char left[1024];
	if (follow_mode) {
		snprintf(left, sizeof(left), "%s | Waiting for data...", source);
	} else if (status_flash[0] != '\0') {
		snprintf(left, sizeof(left), "%s | %s", source, status_flash);
	} else if (search_query[0] != '\0') {
		if (match_count > 0 && current_match < match_count)
			snprintf(left, sizeof(left), "%s | /%s (%zu/%zu matches)",
				source, search_query, current_match + 1, match_count);
		else
			snprintf(left, sizeof(left), "%s | /%s (0 matches)", source, search_query);
	} else {
		snprintf(left, sizeof(left), "%s", source);
	}

	PUTS_ERR(CUP(1, 1));
	if (term_rows > 1)
		PRINTF_ERR(CUP(%d, 1), term_rows);
	PUTS_ERR(SGR_REVERSE_VIDEO_ON);

	size_t cols = (size_t)term_cols;
	size_t right_len = strlen(right);

	if (right_len >= cols) {
		WRITE_ERR(right + (right_len - cols), cols);
	} else {
		size_t left_space = cols - right_len;
		size_t left_max = left_space > 0 ? left_space - 1 : 0;
		size_t left_len = strlen(left);
		if (left_len > left_max)
			left_len = left_max;

		WRITE_ERR(left, left_len);
		for (size_t i = left_len; i < left_space; i++)
			PUTC_ERR(' ');
		WRITE_ERR(right, right_len);
	}

	PUTS_ERR(SGR_RESET);
}

static void render_frame(bool include_status_bar)
{
	clear_flash_if_expired();
	update_term_size();

	sync_frame_begin();
	PUTS_ERR(HIDE_CURSOR HOME);

	size_t view_idx = scroll_line;
	int row = 1;

	while (row <= content_rows) {
		if (view_idx >= view_line_count()) {
			if (show_line_numbers) {
				for (int i = 0; i < gutter_width; i++)
					PUTC_ERR(' ');
			}
			PUTS_ERR(ERASE_TO_EOL);
			if (row < content_rows)
				PUTC_ERR('\n');
			row++;
			continue;
		}

		size_t raw_line = view_to_raw_line(view_idx);
		size_t len = 0;
		const char *line = get_line(raw_line, &len);

		if (wrap_mode) {
			size_t width = display_width(line, len);
			if (width == 0) {
				draw_gutter(raw_line, false);
				PUTS_ERR(SGR_RESET ERASE_TO_EOL);
				if (row < content_rows)
					PUTC_ERR('\n');
				row++;
			} else {
				size_t start_col = 0;
				size_t wrap_idx = 0;

				while (row <= content_rows && start_col < width) {
					draw_gutter(raw_line, wrap_idx > 0);

					size_t prefix_cols = 0;
					if (wrap_idx > 0 && content_cols > 1) {
						PUTS_ERR(SGR_HALF_BRIGHT_ON WRAP_CONT_MARK);
						prefix_cols = 1;
						if (content_cols > 2) {
							PUTC_ERR(' ');
							prefix_cols = 2;
						}
						PUTS_ERR(SGR_RESET);
					}

					size_t line_cols = (size_t)content_cols;
					if (line_cols > prefix_cols)
						line_cols -= prefix_cols;
					else
						line_cols = 1;

					render_line_slice(line, len, start_col, line_cols, raw_line);

					PUTS_ERR(SGR_RESET ERASE_TO_EOL);
					if (row < content_rows)
						PUTC_ERR('\n');
					row++;

					start_col += line_cols;
					wrap_idx++;
				}
			}
		} else {
			draw_gutter(raw_line, false);
			render_line_slice(line, len, scroll_col, (size_t)content_cols, raw_line);
			PUTS_ERR(SGR_RESET ERASE_TO_EOL);
			if (row < content_rows)
				PUTC_ERR('\n');
			row++;
		}

		view_idx++;
	}

	if (include_status_bar)
		render_status_bar();
	sync_frame_end();
	FLUSH_FILE(stderr);
}

static void render(void)
{
	render_frame(true);
}

static void render_without_status_bar(void)
{
	render_frame(false);
}

static void search_history_add(const char *query)
{
	if (!query || query[0] == '\0')
		return;

	if (search_history_len > 0 && strcmp(search_history[search_history_len - 1], query) == 0)
		return;

	if (search_history_len < SEARCH_HISTORY_SIZE) {
		snprintf(search_history[search_history_len], MAX_QUERY, "%s", query);
		search_history_len++;
		return;
	}

	for (size_t i = 1; i < SEARCH_HISTORY_SIZE; i++)
		memcpy(search_history[i - 1], search_history[i], MAX_QUERY);
	snprintf(search_history[SEARCH_HISTORY_SIZE - 1], MAX_QUERY, "%s", query);
}

static void prompt_delete_before(struct prompt_buffer *prompt)
{
	if (prompt->cursor == 0)
		return;

	memmove(prompt->text + prompt->cursor - 1, prompt->text + prompt->cursor, prompt->len - prompt->cursor + 1);
	prompt->cursor--;
	prompt->len--;
}

static void prompt_delete_at(struct prompt_buffer *prompt)
{
	if (prompt->cursor >= prompt->len)
		return;

	memmove(prompt->text + prompt->cursor, prompt->text + prompt->cursor + 1, prompt->len - prompt->cursor);
	prompt->len--;
}

static void prompt_delete_word_before(struct prompt_buffer *prompt)
{
	size_t start = prompt->cursor;
	while (start > 0 && isspace((unsigned char)prompt->text[start - 1]))
		start--;
	while (start > 0 && !isspace((unsigned char)prompt->text[start - 1]))
		start--;

	if (start == prompt->cursor)
		return;

	memmove(prompt->text + start, prompt->text + prompt->cursor, prompt->len - prompt->cursor + 1);
	prompt->len -= prompt->cursor - start;
	prompt->cursor = start;
}

static void prompt_delete_to_start(struct prompt_buffer *prompt)
{
	if (prompt->cursor == 0)
		return;

	memmove(prompt->text, prompt->text + prompt->cursor, prompt->len - prompt->cursor + 1);
	prompt->len -= prompt->cursor;
	prompt->cursor = 0;
}

static void prompt_insert(struct prompt_buffer *prompt, int ch)
{
	if (ch < 32 || ch > 126)
		return;
	if (prompt->len + 1 >= sizeof(prompt->text))
		return;

	memmove(prompt->text + prompt->cursor + 1, prompt->text + prompt->cursor, prompt->len - prompt->cursor + 1);
	prompt->text[prompt->cursor] = (char)ch;
	prompt->cursor++;
	prompt->len++;
}

static enum key_code parse_csi_sequence(int first)
{
	if (first == 'A')
		return KEY_UP;
	if (first == 'B')
		return KEY_DOWN;
	if (first == 'C')
		return KEY_RIGHT;
	if (first == 'D')
		return KEY_LEFT;
	if (first == 'H')
		return KEY_HOME;
	if (first == 'F')
		return KEY_END;

	if (first == 'M') {
		int b = GETC();
		int x = GETC();
		int y = GETC();
		(void)x;
		(void)y;
		if (b == EOF || x == EOF || y == EOF)
			return KEY_ESC;
		int button = b - 32;
		if (button == 64)
			return KEY_MOUSE_UP;
		if (button == 65)
			return KEY_MOUSE_DOWN;
		return KEY_NONE;
	}

	if (first == '<') {
		char seq[64];
		size_t n = 0;
		seq[n++] = '<';
		while (n + 1 < sizeof(seq)) {
			int ch = GETC();
			if (ch == EOF)
				break;
			seq[n++] = (char)ch;
			if (ch == 'm' || ch == 'M')
				break;
		}
		seq[n] = '\0';

		int button = 0;
		int x = 0;
		int y = 0;
		char suffix = 0;
		if (sscanf(seq, "<%d;%d;%d%c", &button, &x, &y, &suffix) == 4) {
			(void)x;
			(void)y;
			if (button == 64)
				return KEY_MOUSE_UP;
			if (button == 65)
				return KEY_MOUSE_DOWN;
		}
		return KEY_NONE;
	}

	char seq[64];
	size_t n = 0;
	seq[n++] = (char)first;

	while (n + 1 < sizeof(seq)) {
		int ch = GETC();
		if (ch == EOF)
			break;
		seq[n++] = (char)ch;
		if ((ch >= '@' && ch <= '~') || ch == '~')
			break;
	}
	seq[n] = '\0';

	if (strcmp(seq, "1~") == 0 || strcmp(seq, "7~") == 0)
		return KEY_HOME;
	if (strcmp(seq, "4~") == 0 || strcmp(seq, "8~") == 0)
		return KEY_END;
	if (strcmp(seq, "5~") == 0)
		return KEY_PGUP;
	if (strcmp(seq, "6~") == 0)
		return KEY_PGDN;
	if (strcmp(seq, "3~") == 0)
		return KEY_DELETE;
	if (strcmp(seq, "1;5C") == 0)
		return KEY_CTRL_RIGHT;
	if (strcmp(seq, "1;5D") == 0)
		return KEY_CTRL_LEFT;
	if (strcmp(seq, "3;5~") == 0)
		return KEY_CTRL_DELETE;

	return KEY_ESC;
}

static int read_next_key_byte(int timeout_ms)
{
	struct pollfd pfd = {
		.fd = STDIN_FILENO,
		.events = POLLIN,
		.revents = 0,
	};

	for (;;) {
		int rc = poll(&pfd, 1, timeout_ms);
		if (rc > 0)
			break;
		if (rc == 0)
			return -1;
		if (errno == EINTR)
			continue;
		return -1;
	}

	int c = GETC();
	if (c == EOF) {
		if (errno == EINTR) {
			clearerr(stdin);
			return -1;
		}
		return -1;
	}

	return c;
}

static int read_key(void)
{
	int c = GETC();
	if (c == EOF) {
		if (errno == EINTR) {
			clearerr(stdin);
			return KEY_NONE;
		}
		return KEY_EOF;
	}

	if (c != K_ESC)
		return c;

	/*
	 * Treat lone ESC as an immediate key press; only parse escape
	 * sequences if another byte follows within a short interval.
	 */
	int next = read_next_key_byte(25);
	if (next < 0)
		return KEY_ESC;

	if (next == K_ESC)
		return KEY_ESC;

	if (next == '[') {
		int first = GETC();
		if (first == EOF)
			return KEY_ESC;
		return parse_csi_sequence(first);
	}

	if (next == 'O') {
		int code = GETC();
		if (code == EOF)
			return KEY_ESC;
		if (code == 'H')
			return KEY_HOME;
		if (code == 'F')
			return KEY_END;
		if (code == 'A')
			return KEY_UP;
		if (code == 'B')
			return KEY_DOWN;
		if (code == 'C')
			return KEY_RIGHT;
		if (code == 'D')
			return KEY_LEFT;
	}

	return KEY_ESC;
}

static bool string_is_number(const char *s)
{
	if (!s || s[0] == '\0')
		return false;
	for (size_t i = 0; s[i] != '\0'; i++) {
		if (!isdigit((unsigned char)s[i]))
			return false;
	}
	return true;
}

static bool go_to_line(size_t one_based)
{
	if (one_based == 0)
		return false;

	if (line_count == 0) {
		scroll_line = 0;
		return true;
	}

	size_t raw_line = one_based - 1;
	if (raw_line >= line_count)
		raw_line = line_count - 1;

	size_t view_idx = 0;
	if (!raw_to_view_line(raw_line, &view_idx)) {
		set_flash("Line hidden by filter");
		return false;
	}

	scroll_line = view_idx;
	clamp_scroll();
	return true;
}

static void scroll_to_bottom(void)
{
	size_t count = view_line_count();
	if (count == 0) {
		scroll_line = 0;
		return;
	}

	if (count <= (size_t)content_rows)
		scroll_line = 0;
	else
		scroll_line = count - (size_t)content_rows;
}

static void show_help_screen(void)
{
	sync_frame_begin();
	PUTS_ERR(HOME HIDE_CURSOR CLS);
	PUTS_ERR(
		"pager key bindings\n"
		"\n"
		"  q                 quit\n"
		"  j / Down / Enter  down one line\n"
		"  k / Up            up one line\n"
		"  Space / PgDn      page down\n"
		"  b / PgUp          page up\n"
		"  d / u             half page down/up\n"
		"  g / Home          top\n"
		"  G / End           bottom\n"
		"  Left / Right      horizontal scroll in nowrap\n"
		"  w                 toggle wrap\n"
		"  / ?               incremental search forward/backward\n"
		"  n / N             next/previous match\n"
		"  ] / [             next/previous file\n"
		"  Esc               clear active search\n"
		"  :                 command line\n"
		"  F                 enter follow mode\n"
		"  y                 yank current line to clipboard\n"
		"\n"
		"Commands: :q :123 :wrap :nowrap :number :nonumber :sync :nosync\n"
		"          :filter <pattern> :n :p :follow :help\n"
		"Config:   [keybindings]\n"
		"\n"
		"Press any key to return..."
	);
	sync_frame_end();
	FLUSH_FILE(stderr);
	(void)read_key();
}

static void draw_search_prompt(char prefix, const struct prompt_buffer *prompt, bool full_redraw)
{
	if (full_redraw)
		render_without_status_bar();

	sync_frame_begin();
	PUTS_ERR(HIDE_CURSOR);
	PRINTF_ERR(CUP(%d, 1), term_rows);
	PUTS_ERR(SGR_REVERSE_VIDEO_ON);
	PUTC_ERR(prefix);
	WRITE_ERR(prompt->text, prompt->len);
	for (size_t i = prompt->len + 1; i < (size_t)term_cols; i++)
		PUTC_ERR(' ');
	PUTS_ERR(SGR_RESET);
	PRINTF_ERR(CUP(%d, %zu) SHOW_CURSOR, term_rows, prompt->cursor + 2);
	sync_frame_end();
	FLUSH_FILE(stderr);
}

static size_t command_popup_row_limit(void)
{
	size_t limit = command_popup_rows;
	size_t available = term_rows > 1 ? (size_t)(term_rows - 1) : 1;
	if (limit > available)
		limit = available;
	if (limit > MAX_COMMAND_POPUP_ROWS)
		limit = MAX_COMMAND_POPUP_ROWS;
	return limit;
}

static size_t command_popup_count(const struct prompt_buffer *prompt)
{
	char prefix[MAX_COMMAND];
	size_t prefix_len = 0;
	size_t row_limit = command_popup_row_limit();
	if (row_limit == 0)
		return 0;

	while (prefix_len < prompt->len && prefix_len + 1 < sizeof(prefix)) {
		char ch = prompt->text[prefix_len];
		if (isspace((unsigned char)ch))
			break;
		prefix[prefix_len++] = ch;
	}
	prefix[prefix_len] = '\0';

	if (string_is_number(prefix))
		return 0;

	size_t count = 0;

	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
		if (prefix_len > 0 && strncmp(commands[i].name, prefix, prefix_len) != 0)
			continue;
		count++;
		if (count == row_limit)
			break;
	}

	return count;
}

static bool draw_command_popup(const struct prompt_buffer *prompt)
{
	size_t shown_count = command_popup_count(prompt);
	if (shown_count == 0)
		return false;

	const struct command_desc *shown[MAX_COMMAND_POPUP_ROWS] = {0};
	size_t prefix_len = 0;
	char prefix[MAX_COMMAND];

	while (prefix_len < prompt->len && prefix_len + 1 < sizeof(prefix)) {
		char ch = prompt->text[prefix_len];
		if (isspace((unsigned char)ch))
			break;
		prefix[prefix_len++] = ch;
	}
	prefix[prefix_len] = '\0';

	size_t shown_idx = 0;
	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
		if (prefix_len > 0 && strncmp(commands[i].name, prefix, prefix_len) != 0)
			continue;
		shown[shown_idx++] = &commands[i];
		if (shown_idx == shown_count)
			break;
	}

	int popup_top = term_rows - (int)command_popup_row_limit();
	if (popup_top < 1)
		popup_top = 1;
	int popup_bottom = term_rows - 1;

	for (int row = popup_top; row <= popup_bottom; row++) {
		PRINTF_ERR(CUP(%d, 1), row);
		PUTS_ERR(SGR_RESET);
		for (int c = 0; c < term_cols; c++)
			PUTC_ERR(' ');
	}

	int start_row = term_rows - (int)shown_count;
	if (start_row < 1)
		start_row = 1;

	for (size_t i = 0; i < shown_count; i++) {
		int row = start_row + (int)i;
		PRINTF_ERR(CUP(%d, 1), row);
		PUTS_ERR(SGR_REVERSE_VIDEO_ON);
		char line[512];
		snprintf(line, sizeof(line), ":%-10s %s", shown[i]->name, shown[i]->desc);
		size_t len = strlen(line);
		if (len > (size_t)term_cols)
			len = (size_t)term_cols;
		WRITE_ERR(line, len);
		for (size_t c = len; c < (size_t)term_cols; c++)
			PUTC_ERR(' ');
		PUTS_ERR(SGR_RESET);
	}

	return true;
}

static bool draw_command_prompt(const struct prompt_buffer *prompt, bool full_redraw)
{
	if (full_redraw)
		render();
	sync_frame_begin();
	PUTS_ERR(HIDE_CURSOR);
	bool popup_visible = draw_command_popup(prompt);

	PRINTF_ERR(CUP(%d, 1), term_rows);
	PUTS_ERR(SGR_REVERSE_VIDEO_ON ":");
	WRITE_ERR(prompt->text, prompt->len);
	for (size_t i = prompt->len + 1; i < (size_t)term_cols; i++)
		PUTC_ERR(' ');
	PUTS_ERR(SGR_RESET);
	PRINTF_ERR(CUP(%d, %zu) SHOW_CURSOR, term_rows, prompt->cursor + 2);
	sync_frame_end();
	FLUSH_FILE(stderr);

	return popup_visible;
}

static bool switch_file(size_t idx, bool check_binary)
{
	if (idx >= file_count)
		return false;

	int fd = open(filenames[idx], O_RDONLY);
	if (fd < 0) {
		set_flash("Failed to open file");
		return false;
	}

	if (check_binary) {
		char sample[4096];
		ssize_t n = 0;
		for (;;) {
			n = read(fd, sample, sizeof(sample));
			if (n < 0 && errno == EINTR)
				continue;
			break;
		}

		if (n < 0 || lseek(fd, 0, SEEK_SET) < 0) {
			close(fd);
			set_flash("Failed to inspect file");
			return false;
		}

		if (is_probably_binary(sample, (size_t)n)) {
			if (!prompt_binary_open(filenames[idx])) {
				close(fd);
				set_flash("Skipped binary file");
				return false;
			}
		}
	}

	clear_content();
	load_fd(fd);
	close(fd);
	build_line_index();

	current_file = idx;
	source_is_stdin = false;
	follow_file_offset = (off_t)buffer_size;

	set_flash("File loaded");
	return true;
}

static bool switch_to_next_file(void)
{
	if (file_count <= 1) {
		set_flash("No other file");
		return true;
	}

	size_t next = current_file + 1;
	if (next >= file_count)
		next = 0;
	(void)switch_file(next, true);
	return true;
}

static bool switch_to_prev_file(void)
{
	if (file_count <= 1) {
		set_flash("No other file");
		return true;
	}

	size_t prev = current_file == 0 ? file_count - 1 : current_file - 1;
	(void)switch_file(prev, true);
	return true;
}

static void refresh_after_content_update(void)
{
	build_line_index();

	if (filter_active && filter_pattern[0] != '\0')
		apply_filter(filter_pattern, true);
	if (search_query[0] != '\0')
		find_all_matches(search_query, true);

	update_term_size();
	clamp_scroll();
}

static void append_line_index_from(size_t start_offset)
{
	if (start_offset > buffer_size)
		start_offset = buffer_size;

	if (!line_offsets || line_count == 0 || !line_match_first || !line_match_count) {
		build_line_index();
		return;
	}

	size_t extra_newlines = 0;
	for (size_t i = start_offset; i < buffer_size; i++) {
		if (buffer[i] == '\n')
			extra_newlines++;
	}
	if (extra_newlines == 0)
		return;

	size_t needed = line_count + extra_newlines;
	if (needed > line_capacity) {
		size_t *new_offsets = realloc(line_offsets, needed * sizeof(size_t));
		if (!new_offsets)
			die("realloc");
		line_offsets = new_offsets;
		line_capacity = needed;
	}

	size_t old_line_count = line_count;
	for (size_t i = start_offset; i < buffer_size; i++) {
		if (buffer[i] == '\n')
			line_offsets[line_count++] = i + 1;
	}

	size_t *new_line_match_first = realloc(line_match_first, line_count * sizeof(size_t));
	size_t *new_line_match_count = realloc(line_match_count, line_count * sizeof(size_t));
	if (!new_line_match_first || !new_line_match_count)
		die("realloc");

	line_match_first = new_line_match_first;
	line_match_count = new_line_match_count;
	memset(line_match_first + old_line_count, 0, (line_count - old_line_count) * sizeof(size_t));
	memset(line_match_count + old_line_count, 0, (line_count - old_line_count) * sizeof(size_t));
}

static void refresh_after_append(size_t old_buffer_size)
{
	if (filter_active || search_query[0] != '\0') {
		refresh_after_content_update();
		return;
	}

	append_line_index_from(old_buffer_size);
	update_term_size();
	clamp_scroll();
}

static bool read_new_data_pipe(void)
{
	if (follow_pipe_fd < 0)
		return false;

	if (!follow_pipe_nonblocking) {
		int flags = fcntl(follow_pipe_fd, F_GETFL, 0);
		if (flags >= 0)
			(void)fcntl(follow_pipe_fd, F_SETFL, flags | O_NONBLOCK);
		follow_pipe_nonblocking = true;
	}

	bool changed = false;
	size_t old_buffer_size = buffer_size;
	char tmp[4096];

	for (;;) {
		ssize_t n = read(follow_pipe_fd, tmp, sizeof(tmp));
		if (n > 0) {
			ensure_buffer_capacity(buffer_size + (size_t)n + 1);
			memcpy(buffer + buffer_size, tmp, (size_t)n);
			buffer_size += (size_t)n;
			changed = true;
			continue;
		}

		if (n == 0)
			break;

		if (errno == EINTR)
			continue;

		if (errno == EAGAIN || errno == EWOULDBLOCK)
			break;

		break;
	}

	if (changed)
		refresh_after_append(old_buffer_size);

	return changed;
}

static bool read_new_data_file(void)
{
	if (file_count == 0)
		return false;

	struct stat st;
	if (stat(filenames[current_file], &st) != 0)
		return false;

	if (st.st_size < follow_file_offset) {
		if (!switch_file(current_file, false))
			return false;
		return true;
	}

	if (st.st_size == follow_file_offset)
		return false;

	int fd = open(filenames[current_file], O_RDONLY);
	if (fd < 0)
		return false;

	if (lseek(fd, follow_file_offset, SEEK_SET) < 0) {
		close(fd);
		return false;
	}

	bool changed = false;
	size_t old_buffer_size = buffer_size;
	char tmp[4096];

	for (;;) {
		ssize_t n = read(fd, tmp, sizeof(tmp));
		if (n > 0) {
			ensure_buffer_capacity(buffer_size + (size_t)n + 1);
			memcpy(buffer + buffer_size, tmp, (size_t)n);
			buffer_size += (size_t)n;
			changed = true;
			continue;
		}
		if (n == 0)
			break;
		if (errno == EINTR)
			continue;
		break;
	}

	close(fd);

	if (changed) {
		follow_file_offset = (off_t)buffer_size;
		refresh_after_append(old_buffer_size);
	}

	return changed;
}

static char *base64_encode(const unsigned char *src, size_t len)
{
	static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	size_t out_len = ((len + 2) / 3) * 4;
	char *out = malloc(out_len + 1);
	if (!out)
		return NULL;

	size_t i = 0;
	size_t j = 0;
	while (i + 2 < len) {
		uint32_t v = (uint32_t)src[i] << 16 | (uint32_t)src[i + 1] << 8 | (uint32_t)src[i + 2];
		out[j++] = table[(v >> 18) & 63];
		out[j++] = table[(v >> 12) & 63];
		out[j++] = table[(v >> 6) & 63];
		out[j++] = table[v & 63];
		i += 3;
	}

	if (i < len) {
		uint32_t v = (uint32_t)src[i] << 16;
		out[j++] = table[(v >> 18) & 63];
		if (i + 1 < len) {
			v |= (uint32_t)src[i + 1] << 8;
			out[j++] = table[(v >> 12) & 63];
			out[j++] = table[(v >> 6) & 63];
			out[j++] = '=';
		} else {
			out[j++] = table[(v >> 12) & 63];
			out[j++] = '=';
			out[j++] = '=';
		}
	}

	out[j] = '\0';
	return out;
}

static void yank_current_line(void)
{
	if (view_line_count() == 0)
		return;

	size_t raw_line = view_to_raw_line(scroll_line);
	size_t len = 0;
	const char *line = get_line(raw_line, &len);

	char *plain = malloc(len + 1);
	if (!plain)
		return;

	size_t plain_len = strip_ansi(line, len, plain, len + 1);
	char *b64 = base64_encode((const unsigned char *)plain, plain_len);
	free(plain);
	if (!b64)
		return;

	WRITE_ERR(OSC52_PREFIX, strlen(OSC52_PREFIX));
	WRITE_ERR(b64, strlen(b64));
	WRITE_ERR(OSC52_SUFFIX, strlen(OSC52_SUFFIX));
	FLUSH_FILE(stderr);
	free(b64);

	set_flash("Yanked");
}

static void handle_sigwinch(int sig)
{
	(void)sig;
	term_resized = 1;
}

static void restore_terminal_state(void)
{
	if (raw_mode_enabled && termios_valid) {
		tcsetattr(STDERR_FILENO, TCSAFLUSH, &original_termios);
		raw_mode_enabled = false;
	}

	sync_frame_end();
	PUTS_ERR(SHOW_CURSOR DISABLE_X11_MOUSE DISABLE_SGR_MOUSE SGR_RESET);
	if (alternate_screen_enabled) {
		PUTS_ERR(ALT_SCREEN_DISABLE);
		alternate_screen_enabled = false;
	}
	FLUSH_FILE(stderr);
}

static void cleanup(void)
{
	restore_terminal_state();

	if (follow_pipe_fd >= 0) {
		close(follow_pipe_fd);
		follow_pipe_fd = -1;
	}

	clear_content();
}

static void handle_sigtstp(int sig)
{
	(void)sig;
	restore_terminal_state();
	signal(SIGTSTP, SIG_DFL);
	raise(SIGTSTP);
}

static void handle_sigcont(int sig)
{
	(void)sig;
	got_sigcont = 1;
}

static void enable_raw_mode(void)
{
	if (tcgetattr(STDERR_FILENO, &original_termios) != 0)
		die("tcgetattr");

	raw_termios = original_termios;
	raw_termios.c_lflag &= (tcflag_t)~(ECHO | ICANON | ISIG);
	raw_termios.c_cc[VMIN] = 1;
	raw_termios.c_cc[VTIME] = 0;

	if (tcsetattr(STDERR_FILENO, TCSAFLUSH, &raw_termios) != 0)
		die("tcsetattr");

	termios_valid = true;
	raw_mode_enabled = true;

	setvbuf(stdin, NULL, _IONBF, 0);

	PUTS_ERR(ALT_SCREEN_ENABLE ENABLE_X11_MOUSE ENABLE_SGR_MOUSE HIDE_CURSOR HOME CLS);
	alternate_screen_enabled = true;
}

static void resume_after_suspend(void)
{
	if (!termios_valid)
		return;

	if (tcsetattr(STDERR_FILENO, TCSAFLUSH, &raw_termios) != 0)
		die("tcsetattr");
	raw_mode_enabled = true;

	PUTS_ERR(ALT_SCREEN_ENABLE ENABLE_X11_MOUSE ENABLE_SGR_MOUSE HIDE_CURSOR HOME CLS);
	alternate_screen_enabled = true;

	signal(SIGTSTP, handle_sigtstp);
	term_resized = 1;
}

static bool execute_command_line(const char *input)
{
	if (!input)
		return false;

	char command[MAX_COMMAND];
	snprintf(command, sizeof(command), "%s", input);

	size_t start = 0;
	while (isspace((unsigned char)command[start]))
		start++;

	char *cmd = command + start;
	size_t end = strlen(cmd);
	while (end > 0 && isspace((unsigned char)cmd[end - 1])) {
		cmd[end - 1] = '\0';
		end--;
	}

	if (cmd[0] == '\0')
		return false;

	if (string_is_number(cmd)) {
		size_t line_num = 0;
		if (!parse_toml_size(cmd, &line_num)) {
			set_flash("Invalid line number");
			return true;
		}
		go_to_line(line_num);
		return true;
	}

	if (strcmp(cmd, "q") == 0) {
		running = false;
		return true;
	}

	if (strcmp(cmd, "wrap") == 0) {
		wrap_mode = true;
		scroll_col = 0;
		return true;
	}

	if (strcmp(cmd, "nowrap") == 0) {
		wrap_mode = false;
		return true;
	}

	if (strcmp(cmd, "number") == 0) {
		show_line_numbers = true;
		update_term_size();
		return true;
	}

	if (strcmp(cmd, "nonumber") == 0) {
		show_line_numbers = false;
		update_term_size();
		return true;
	}

	if (strcmp(cmd, "n") == 0) {
		(void)switch_to_next_file();
		return true;
	}

	if (strcmp(cmd, "p") == 0) {
		(void)switch_to_prev_file();
		return true;
	}

	if (strcmp(cmd, "follow") == 0) {
		follow_mode = true;
		scroll_to_bottom();
		return true;
	}

	if (strcmp(cmd, "sync") == 0) {
		sync_output_enabled = true;
		set_flash("Sync rendering on");
		return true;
	}

	if (strcmp(cmd, "nosync") == 0) {
		sync_output_enabled = false;
		set_flash("Sync rendering off");
		return true;
	}

	if (strcmp(cmd, "help") == 0) {
		show_help_screen();
		return true;
	}

	if (strncmp(cmd, "filter", 6) == 0 && (cmd[6] == '\0' || isspace((unsigned char)cmd[6]))) {
		char *pattern = cmd + 6;
		while (*pattern != '\0' && isspace((unsigned char)*pattern))
			pattern++;

		if (*pattern == '\0') {
			clear_filter();
			if (search_query[0] != '\0')
				find_all_matches(search_query, true);
		} else {
			apply_filter(pattern, false);
		}
		return true;
	}

	set_flash("Unknown command");
	return true;
}

static void open_command_prompt(void)
{
	struct prompt_buffer prompt = {
		.text = {0},
		.len = 0,
		.cursor = 0,
	};
	bool full_redraw = true;
	bool had_popup = false;

	for (;;) {
		if (term_resized) {
			term_resized = 0;
			update_term_size();
			full_redraw = true;
		}

		bool popup_now = command_popup_count(&prompt) > 0;
		if (!full_redraw && had_popup && !popup_now)
			full_redraw = true;

		had_popup = draw_command_prompt(&prompt, full_redraw);
		full_redraw = false;

		int key = read_key();
		if (key == KEY_EOF)
			break;
		if (key == KEY_NONE)
			continue;

		if (key == '\n' || key == '\r' || key == K_ENTER) {
			execute_command_line(prompt.text);
			break;
		}

		if (key == KEY_ESC) {
			break;
		}

		if (key == K_DEL) {
			prompt_delete_before(&prompt);
			continue;
		}

		if (key == KEY_DELETE) {
			prompt_delete_at(&prompt);
			continue;
		}

		if (key == K_CTRL_U) {
			prompt_delete_to_start(&prompt);
			continue;
		}

		if (key == K_CTRL_W || key == K_CTRL_H) {
			prompt_delete_word_before(&prompt);
			continue;
		}

		if (key == KEY_LEFT || key == K_CTRL_B) {
			if (prompt.cursor > 0)
				prompt.cursor--;
			continue;
		}

		if (key == KEY_RIGHT || key == K_CTRL_F) {
			if (prompt.cursor < prompt.len)
				prompt.cursor++;
			continue;
		}

		if (key == KEY_HOME || key == K_CTRL_A) {
			prompt.cursor = 0;
			continue;
		}

		if (key == KEY_END || key == K_CTRL_E) {
			prompt.cursor = prompt.len;
			continue;
		}

		if (key >= 32 && key <= 126)
			prompt_insert(&prompt, key);
	}

	PUTS_ERR(HIDE_CURSOR);
}

static void open_search_prompt(bool backward)
{
	struct prompt_buffer prompt = {
		.text = {0},
		.len = 0,
		.cursor = 0,
	};

	size_t anchor_raw_line = 0;
	if (view_line_count() > 0)
		anchor_raw_line = view_to_raw_line(scroll_line);

	char original_query[MAX_QUERY] = "";
	snprintf(original_query, sizeof(original_query), "%s", search_query);
	bool original_search_forward = search_forward;
	size_t original_scroll_line = scroll_line;
	size_t original_scroll_col = scroll_col;
	char original_flash[sizeof(status_flash)] = "";
	snprintf(original_flash, sizeof(original_flash), "%s", status_flash);
	uint64_t original_flash_until = status_flash_until;
	bool original_match_valid = match_count > 0 && current_match < match_count;
	struct search_match original_match = {0};
	if (original_match_valid)
		original_match = matches[current_match];

	/* Opening a new search starts from a cleared query/match state. */
	clear_matches();
	search_query[0] = '\0';

	int history_pos = -1;
	char saved_input[MAX_QUERY] = "";
	bool accepted = false;

	bool full_redraw = true;

	for (;;) {
		if (term_resized) {
			term_resized = 0;
			update_term_size();
			full_redraw = true;
		}

		draw_search_prompt(backward ? '?' : '/', &prompt, full_redraw);
		full_redraw = false;

		int key = read_key();
		if (key == KEY_EOF)
			break;
		if (key == KEY_NONE)
			continue;

		if (key == '\n' || key == '\r' || key == K_ENTER) {
			apply_search_query_from_anchor(prompt.text, backward, anchor_raw_line);
			if (prompt.len > 0)
				search_history_add(prompt.text);
			accepted = true;
			break;
		}

		if (key == KEY_ESC)
			break;

		if (key == KEY_UP && search_history_len > 0) {
			if (history_pos < 0) {
				snprintf(saved_input, sizeof(saved_input), "%s", prompt.text);
				history_pos = (int)search_history_len - 1;
			} else if (history_pos > 0) {
				history_pos--;
			}
			snprintf(prompt.text, sizeof(prompt.text), "%s", search_history[history_pos]);
			prompt.len = strlen(prompt.text);
			prompt.cursor = prompt.len;
			apply_search_query_from_anchor(prompt.text, backward, anchor_raw_line);
			full_redraw = true;
			continue;
		}

		if (key == KEY_DOWN && search_history_len > 0) {
			if (history_pos >= 0 && history_pos + 1 < (int)search_history_len) {
				history_pos++;
				snprintf(prompt.text, sizeof(prompt.text), "%s", search_history[history_pos]);
			} else if (history_pos >= 0) {
				history_pos = -1;
				snprintf(prompt.text, sizeof(prompt.text), "%s", saved_input);
			}
			prompt.len = strlen(prompt.text);
			prompt.cursor = prompt.len;
			apply_search_query_from_anchor(prompt.text, backward, anchor_raw_line);
			full_redraw = true;
			continue;
		}

		if (key == K_DEL) {
			prompt_delete_before(&prompt);
			history_pos = -1;
			apply_search_query_from_anchor(prompt.text, backward, anchor_raw_line);
			full_redraw = true;
			continue;
		}

		if (key == KEY_DELETE) {
			prompt_delete_at(&prompt);
			history_pos = -1;
			apply_search_query_from_anchor(prompt.text, backward, anchor_raw_line);
			full_redraw = true;
			continue;
		}

		if (key == K_CTRL_U) {
			prompt_delete_to_start(&prompt);
			history_pos = -1;
			apply_search_query_from_anchor(prompt.text, backward, anchor_raw_line);
			full_redraw = true;
			continue;
		}

		if (key == K_CTRL_W || key == K_CTRL_H) {
			prompt_delete_word_before(&prompt);
			history_pos = -1;
			apply_search_query_from_anchor(prompt.text, backward, anchor_raw_line);
			full_redraw = true;
			continue;
		}

		if (key == KEY_LEFT || key == K_CTRL_B) {
			if (prompt.cursor > 0)
				prompt.cursor--;
			continue;
		}

		if (key == KEY_RIGHT || key == K_CTRL_F) {
			if (prompt.cursor < prompt.len)
				prompt.cursor++;
			continue;
		}

		if (key == KEY_HOME || key == K_CTRL_A) {
			prompt.cursor = 0;
			continue;
		}

		if (key == KEY_END || key == K_CTRL_E) {
			prompt.cursor = prompt.len;
			continue;
		}

		if (key >= 32 && key <= 126) {
			prompt_insert(&prompt, key);
			history_pos = -1;
			apply_search_query_from_anchor(prompt.text, backward, anchor_raw_line);
			full_redraw = true;
		}
	}

	if (!accepted) {
		restore_search_state_after_cancel(
			original_query,
			original_search_forward,
			original_match_valid,
			original_match,
			original_scroll_line,
			original_scroll_col,
			original_flash,
			original_flash_until);
	}

	PUTS_ERR(HIDE_CURSOR);
}

static bool handle_navigation_action(enum pager_action action)
{
	size_t count = view_line_count();
	if (count == 0)
		return false;

	size_t half_page = (size_t)content_rows / 2;
	if (half_page == 0)
		half_page = 1;

	size_t prev_line = scroll_line;
	size_t prev_col = scroll_col;

	switch (action) {
		case ACTION_DOWN_LINE:
			if (scroll_line + 1 < count)
				scroll_line++;
			break;

		case ACTION_UP_LINE:
			if (scroll_line > 0)
				scroll_line--;
			break;

		case ACTION_PAGE_DOWN:
			scroll_line += (size_t)content_rows;
			break;

		case ACTION_PAGE_UP:
			if (scroll_line > (size_t)content_rows)
				scroll_line -= (size_t)content_rows;
			else
				scroll_line = 0;
			break;

		case ACTION_HALF_PAGE_DOWN:
			scroll_line += half_page;
			break;

		case ACTION_HALF_PAGE_UP:
			if (scroll_line > half_page)
				scroll_line -= half_page;
			else
				scroll_line = 0;
			break;

		case ACTION_TOP:
			scroll_line = 0;
			break;

		case ACTION_BOTTOM:
			scroll_line = count > 0 ? count - 1 : 0;
			break;

		case ACTION_SCROLL_LEFT:
			if (!wrap_mode && scroll_col > 0)
				scroll_col--;
			break;

		case ACTION_SCROLL_RIGHT:
			if (!wrap_mode)
				scroll_col++;
			break;

		case ACTION_MOUSE_SCROLL_UP:
			if (scroll_line >= 3)
				scroll_line -= 3;
			else
				scroll_line = 0;
			break;

		case ACTION_MOUSE_SCROLL_DOWN:
			scroll_line += 3;
			break;

		default:
			return false;
	}

	clamp_scroll();
	return scroll_line != prev_line || scroll_col != prev_col;
}

static bool handle_key(int key)
{
	if (key == KEY_NONE)
		return false;
	if (key == KEY_EOF) {
		running = false;
		return false;
	}

	if (key == K_CTRL_C && follow_mode) {
		follow_mode = false;
		set_flash("Follow off");
		return true;
	}

	if (key == K_CTRL_Z) {
		handle_sigtstp(0);
		return true;
	}

	enum pager_action action = action_for_key(key);
	if (action == ACTION_NONE && key == KEY_ESC && search_query[0] != '\0') {
		clear_matches();
		search_query[0] = '\0';
		return true;
	}

	switch (action) {
		case ACTION_NONE:
			return false;

		case ACTION_QUIT:
			if (follow_mode) {
				follow_mode = false;
				set_flash("Follow off");
				return true;
			}
			running = false;
			return false;

		case ACTION_TOGGLE_WRAP:
			wrap_mode = !wrap_mode;
			if (wrap_mode)
				scroll_col = 0;
			clamp_scroll();
			return true;

		case ACTION_SEARCH_FORWARD:
			open_search_prompt(false);
			return true;

		case ACTION_SEARCH_BACKWARD:
			open_search_prompt(true);
			return true;

		case ACTION_NEXT_MATCH: {
			size_t prev_match = current_match;
			size_t prev_scroll_line = scroll_line;
			size_t prev_scroll_col = scroll_col;
			jump_to_next_match();
			if (scroll_line == prev_scroll_line && scroll_col == prev_scroll_col
				&& redraw_search_transition_if_possible(prev_match))
				return false;
			return true;
		}

		case ACTION_PREV_MATCH: {
			size_t prev_match = current_match;
			size_t prev_scroll_line = scroll_line;
			size_t prev_scroll_col = scroll_col;
			jump_to_prev_match();
			if (scroll_line == prev_scroll_line && scroll_col == prev_scroll_col
				&& redraw_search_transition_if_possible(prev_match))
				return false;
			return true;
		}

		case ACTION_NEXT_FILE:
			return switch_to_next_file();

		case ACTION_PREV_FILE:
			return switch_to_prev_file();

		case ACTION_COMMAND_PROMPT:
			open_command_prompt();
			return true;

		case ACTION_FOLLOW_MODE:
			follow_mode = true;
			scroll_to_bottom();
			return true;

		case ACTION_YANK_LINE:
			yank_current_line();
			return true;

		case ACTION_SHOW_HELP:
			show_help_screen();
			return true;

		case ACTION_DOWN_LINE:
		case ACTION_UP_LINE:
		case ACTION_PAGE_DOWN:
		case ACTION_PAGE_UP:
		case ACTION_HALF_PAGE_DOWN:
		case ACTION_HALF_PAGE_UP:
		case ACTION_TOP:
		case ACTION_BOTTOM:
		case ACTION_SCROLL_LEFT:
		case ACTION_SCROLL_RIGHT:
		case ACTION_MOUSE_SCROLL_UP:
		case ACTION_MOUSE_SCROLL_DOWN:
			return handle_navigation_action(action);

		case ACTION_COUNT:
			return false;
	}

	return false;
}

static bool follow_tick(int *key_out, bool *has_key)
{
	struct pollfd fds[2];
	nfds_t count = 0;

	fds[count].fd = STDIN_FILENO;
	fds[count].events = POLLIN;
	count++;

	int pipe_idx = -1;
	if (follow_pipe_fd >= 0) {
		pipe_idx = (int)count;
		fds[count].fd = follow_pipe_fd;
		fds[count].events = POLLIN;
		count++;
	}

	int rc = poll(fds, count, 250);
	if (rc < 0) {
		if (errno == EINTR)
			return false;
		die("poll");
	}

	bool changed = false;

	if (rc > 0) {
		if (fds[0].revents & POLLIN) {
			*key_out = read_key();
			*has_key = true;
		}

		if (pipe_idx >= 0 && (fds[pipe_idx].revents & (POLLIN | POLLHUP))) {
			if (read_new_data_pipe())
				changed = true;
		}
	}

	if (file_count > 0 && read_new_data_file())
		changed = true;

	return changed;
}

static void run(void)
{
	render();

	while (running) {
		if (got_sigcont) {
			got_sigcont = 0;
			resume_after_suspend();
			render();
			continue;
		}

		if (term_resized) {
			term_resized = 0;
			update_term_size();
			render();
			continue;
		}

		int key = KEY_NONE;
		bool has_key = false;
		bool changed = false;

		if (follow_mode) {
			changed = follow_tick(&key, &has_key);
			if (changed) {
				scroll_to_bottom();
				render();
			}
			if (has_key) {
				if (handle_key(key))
					render();
			}
			continue;
		}

		key = read_key();
		has_key = true;
		if (has_key && handle_key(key))
			render();
	}
}

static bool input_fits_terminal_page(void)
{
	struct winsize ws;
	size_t max_rows = 24;
	size_t cols = 80;
	if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
		max_rows = (size_t)ws.ws_row;
		cols = (size_t)ws.ws_col;
	}

	size_t used_rows = 0;

	for (size_t line_idx = 0; line_idx < line_count; line_idx++) {
		size_t len = 0;
		const char *line = get_line(line_idx, &len);
		size_t width = display_width(line, len);
		size_t line_rows = width == 0 ? 1 : (width + cols - 1) / cols;

		if (line_rows > max_rows || used_rows > max_rows - line_rows)
			return false;

		used_rows += line_rows;
	}

	return true;
}

static void write_buffer_to_terminal(void)
{
	if (buffer_size == 0)
		return;

	int out_fd = STDERR_FILENO;
	if (!isatty(out_fd) && isatty(STDOUT_FILENO))
		out_fd = STDOUT_FILENO;

	const char *p = buffer;
	size_t remaining = buffer_size;
	while (remaining > 0) {
		ssize_t n = write(out_fd, p, remaining);
		if (n > 0) {
			p += (size_t)n;
			remaining -= (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		die("write");
	}
}

static void print_usage(FILE *stream)
{
	fprintf(stream,
		"Usage: pager [OPTIONS] [FILE...]\n"
		"\n"
		"Config File:\n"
		"  Path: $PAGER_CONFIG, or $XDG_CONFIG_HOME/pager.toml\n"
		"        (default: ~/.config/pager.toml)\n"
		"\n"
		"Config Schema:\n"
		"  Root keys (global options):\n"
		"    number = true|false\n"
		"    wrap = true|false\n"
		"    follow = true|false\n"
		"    line = <positive integer>\n"
		"    pattern = \"search text\"\n"
		"    search_regex = true|false\n"
		"    search_wrap = true|false\n"
		"    search_case = false|true|\"smart\"\n"
		"    search_current_match_sgr = \"reversed yellow\" | \"7;33\"\n"
		"    search_other_match_sgr = \"reversed\" | \"7\"\n"
		"    command_popup_rows = <positive integer, max 32>\n"
		"    sync_output = true|false\n"
		"    quit_if_one_screen = true|false\n"
		"\n"
		"  Sections:\n"
		"    [keybindings]\n"
		"\n"
		"  Keybinding actions:\n"
		"    quit down up page_down page_up half_page_down half_page_up\n"
		"    top bottom left right wrap search_forward search_backward\n"
		"    next_match prev_match next_file prev_file command follow yank help\n"
		"\n"
		"  Example:\n"
		"    number = true\n"
		"    search_regex = false\n"
		"    search_wrap = false\n"
		"    search_case = \"smart\"\n"
		"    command_popup_rows = 8\n"
		"    sync_output = true\n"
		"    [keybindings]\n"
		"    down = [\"ctrl-n\"]\n"
		"    quit = [\"x\"]\n"
		"\n"
		"Options:\n"
		"  -N, --number      Show line numbers\n"
		"  -S, --nowrap      Start in horizontal scroll mode\n"
		"  -F, --follow      Start in follow mode\n"
		"      --quit-if-one-screen\n"
		"                    Quit after printing if one known-size input fits one terminal page\n"
		"  -l, --line N      Start at line N\n"
		"  -p, --pattern P   Start with search pattern\n"
		"      --sync-output Enable synchronized output rendering\n"
		"      --no-sync-output\n"
		"                    Disable synchronized output rendering\n"
		"      --generate-config[=PATH]\n"
		"                    Write a commented default config template\n"
		"                    to PATH, or to $PAGER_CONFIG, or to\n"
		"                    $XDG_CONFIG_HOME/pager.toml (fallback ~/.config/pager.toml)\n"
		"  -h, --help        Show this help message\n");
}

static bool args_request_generate_config(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		const char *arg = argv[i];
		if (!arg)
			continue;
		if (strcmp(arg, "--") == 0)
			break;
		if (strncmp(arg, "--generate-config", 17) == 0
			&& (arg[17] == '\0' || arg[17] == '='))
			return true;
	}
	return false;
}

int main(int argc, char **argv)
{
	enum {
		OPT_GENERATE_CONFIG = 1000,
		OPT_SYNC_OUTPUT,
		OPT_NO_SYNC_OUTPUT,
		OPT_QUIT_IF_ONE_SCREEN,
	};

	struct option options[] = {
		{ "number", no_argument, 0, 'N' },
		{ "nowrap", no_argument, 0, 'S' },
		{ "follow", no_argument, 0, 'F' },
		{ "quit-if-one-screen", no_argument, 0, OPT_QUIT_IF_ONE_SCREEN },
		{ "line", required_argument, 0, 'l' },
		{ "pattern", required_argument, 0, 'p' },
		{ "sync-output", no_argument, 0, OPT_SYNC_OUTPUT },
		{ "no-sync-output", no_argument, 0, OPT_NO_SYNC_OUTPUT },
		{ "generate-config", optional_argument, 0, OPT_GENERATE_CONFIG },
		{ "help", no_argument, 0, 'h' },
		{ 0 }
	};

	size_t start_line = 0;
	char start_pattern[MAX_QUERY] = "";
	bool has_start_pattern = false;
	bool input_size_known = false;

	init_default_key_bindings();
	if (!args_request_generate_config(argc, argv))
		load_config_defaults(&start_line, start_pattern, &has_start_pattern);

	int opt;
	while ((opt = getopt_long(argc, argv, "NSFl:p:h", options, NULL)) != -1) {
		switch (opt) {
			case 'N':
				show_line_numbers = true;
				break;
			case 'S':
				wrap_mode = false;
				break;
			case 'F':
				follow_mode = true;
				break;
			case OPT_QUIT_IF_ONE_SCREEN:
				quit_if_one_screen = true;
				break;
			case 'l': {
				if (!parse_toml_size(optarg, &start_line)) {
					fprintf(stderr, "Invalid line number: %s\n", optarg);
					return EXIT_FAILURE;
				}
				break;
			}
			case 'p':
				snprintf(start_pattern, sizeof(start_pattern), "%s", optarg);
				has_start_pattern = start_pattern[0] != '\0';
				break;
			case OPT_SYNC_OUTPUT:
				sync_output_enabled = true;
				break;
			case OPT_NO_SYNC_OUTPUT:
				sync_output_enabled = false;
				break;
			case OPT_GENERATE_CONFIG: {
				const char *output_path = optarg;
				if (!output_path && optind < argc && argv[optind][0] != '-')
					output_path = argv[optind++];
				return generate_default_config_file(output_path);
			}
			case 'h':
				print_usage(stdout);
				return EXIT_SUCCESS;
			default:
				print_usage(stderr);
				return EXIT_FAILURE;
		}
	}

	file_count = (size_t)(argc - optind);
	filenames = argv + optind;

	if (file_count == 0) {
		if (isatty(STDIN_FILENO)) {
			print_usage(stderr);
			return EXIT_FAILURE;
		}

		struct stat st;
		if (fstat(STDIN_FILENO, &st) == 0 && S_ISREG(st.st_mode))
			input_size_known = true;

		source_is_stdin = true;
		stdin_was_pipe = true;
		follow_pipe_fd = dup(STDIN_FILENO);
		if (follow_pipe_fd < 0)
			die("dup");

		if (follow_mode) {
			/*
			 * In follow mode, avoid blocking until EOF on endless streams.
			 * Read what is currently available and continue in follow_tick().
			 */
			if (!read_new_data_pipe())
				build_line_index();
		} else {
			load_fd(STDIN_FILENO);
			build_line_index();
		}

		if (quit_if_one_screen
			&& input_size_known
			&& !follow_mode
			&& input_fits_terminal_page()) {
			write_buffer_to_terminal();
			return EXIT_SUCCESS;
		}

		if (!freopen("/dev/tty", "r", stdin))
			die("freopen /dev/tty");

		if (buffer_size > 0 && is_probably_binary(buffer, buffer_size) && !prompt_binary_open("[stdin]"))
			return EXIT_FAILURE;
	} else {
		if (file_count == 1) {
			struct stat st;
			if (stat(filenames[0], &st) == 0 && S_ISREG(st.st_mode))
				input_size_known = true;
		}

		if (!switch_file(0, true))
			return EXIT_FAILURE;
	}

	if (quit_if_one_screen
		&& input_size_known
		&& file_count == 1
		&& !follow_mode
		&& input_fits_terminal_page()) {
		write_buffer_to_terminal();
		return EXIT_SUCCESS;
	}

	if (start_line > 0)
		go_to_line(start_line);

	if (has_start_pattern)
		find_all_matches(start_pattern, false);

	if (follow_mode)
		scroll_to_bottom();

	if (signal(SIGINT, SIG_IGN) == SIG_ERR)
		die("signal");
	if (signal(SIGWINCH, handle_sigwinch) == SIG_ERR)
		die("signal");
	if (signal(SIGTSTP, handle_sigtstp) == SIG_ERR)
		die("signal");
	if (signal(SIGCONT, handle_sigcont) == SIG_ERR)
		die("signal");

	enable_raw_mode();
	atexit(cleanup);

	run();

	return EXIT_SUCCESS;
}
