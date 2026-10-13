#define main pager_main
#include "../src/pager.c"
#undef main
#include <assert.h>

static void set_input(const char *text)
{
	clear_content();
	buffer_size = strlen(text);
	ensure_buffer_capacity(buffer_size);
	if (buffer_size > 0)
		memcpy(buffer, text, buffer_size);
	build_line_index();
	wrap_mode = true;
	word_wrap = true;
	wrap_marker_space = true;
	show_line_numbers = false;
	header_lines = header_cols = header_start = 0;
	gutter_width = 0;
	content_cols = 10;
	content_rows = 4;
}

static void expect_line(size_t idx, const char *expected)
{
	size_t len = 0;
	const char *line = get_line(idx, &len);
	assert(len == strlen(expected));
	assert(memcmp(line, expected, len) == 0);
}

static void test_line_endings(void)
{
	const char *inputs[] = {
		"first\n\nlast", "first\n\nlast\n",
		"first\r\n\r\nlast", "first\r\n\r\nlast\r\n",
		"first\r\n\nlast", "first\n\r\nlast\r\n",
	};
	for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
		set_input(inputs[i]);
		assert(line_count == 3);
		expect_line(0, "first");
		expect_line(1, "");
		expect_line(2, "last");
	}
	set_input("");
	assert(line_count == 0);
	set_input("\r\n");
	assert(line_count == 1);
	expect_line(0, "");

	/* A CRLF pair may arrive across separate follow-mode reads. */
	set_input("first\r");
	ensure_buffer_capacity(buffer_size + 6);
	memcpy(buffer + buffer_size, "\nlast\n", 6);
	buffer_size += 6;
	build_line_index();
	assert(line_count == 2);
	expect_line(0, "first");
	expect_line(1, "last");
}

static void capture_output(void (*write_output)(void), char *out, size_t capacity)
{
	FILE *capture = tmpfile();
	assert(capture);
	int saved_stderr = dup(STDERR_FILENO);
	assert(saved_stderr >= 0);
	assert(fflush(stderr) == 0);
	assert(dup2(fileno(capture), STDERR_FILENO) >= 0);
	write_output();
	assert(fflush(stderr) == 0);
	assert(dup2(saved_stderr, STDERR_FILENO) >= 0);
	close(saved_stderr);
	assert(fseek(capture, 0, SEEK_SET) == 0);
	size_t n = fread(out, 1, capacity - 1, capture);
	out[n] = '\0';
	assert(feof(capture));
	fclose(capture);
}

static void test_bare_cr_is_preserved(void)
{
	char out[128];
	set_input("working\rdone\n");
	assert(line_count == 1);
	expect_line(0, "working\rdone");
	capture_output(write_buffer_to_terminal, out, sizeof(out));
	assert(strcmp(out, "working\rdone\n") == 0);
	set_input("first\r\nlast\r\n");
	capture_output(write_buffer_to_terminal, out, sizeof(out));
	assert(strcmp(out, "first\r\nlast\r\n") == 0);
}

static void test_wrapped_bottom(void)
{
	set_input("1\n2\n3\n12345678901\n5\n6\n");
	assert(line_count == 6);
	assert(!view_line_screen_span(5, NULL, NULL));
	scroll_to_bottom();
	assert(view_line_screen_span(5, NULL, NULL));
	assert(scroll_line == 3);

	scroll_line = 0;
	while (handle_navigation_action(ACTION_DOWN_LINE)) {}
	assert(view_line_screen_span(5, NULL, NULL));
	assert(go_to_line(6));
	assert(view_line_screen_span(5, NULL, NULL));

	wrap_mode = false;
	clamp_scroll();
	assert(scroll_line == 2);
	wrap_mode = true;
	content_cols = 20;
	scroll_to_bottom();
	assert(scroll_line == 2);
	content_cols = 10;
	scroll_to_bottom();
	assert(scroll_line == 3);

	/* Short inputs can still overflow a page through wrapping. */
	set_input("12345678901\n12345678901\nlast\n");
	scroll_to_bottom();
	assert(scroll_line == 1);
	assert(view_line_screen_span(2, NULL, NULL));
	find_all_matches("last", false);
	scroll_line = 0;
	ensure_match_visible(0);
	assert(view_line_screen_span(2, NULL, NULL));

	set_input("keep1\nskip\nkeep12345678901\nkeep3\nkeep4\n");
	assert(apply_filter("keep", true));
	scroll_to_bottom();
	assert(view_line_screen_span(filtered_count - 1, NULL, NULL));
}

static void test_status_visible_range(void)
{
	char out[1024];
	set_input("12345678901\n12345678901\nlast\n");
	term_cols = 80;
	term_rows = 5;
	capture_output(render_status_bar, out, sizeof(out));
	assert(strstr(out, "1-2/3  WRAP VIEW"));
	scroll_to_bottom();
	capture_output(render_status_bar, out, sizeof(out));
	assert(strstr(out, "2-3/3  WRAP VIEW"));
}

static void draw_first_wrapped_line(void)
{
	redraw_visible_view_line(0);
}

static void expect_wrapping(const char *input, size_t cols, const char *expected)
{
	set_input(input);
	content_cols = (int)cols;
	content_rows = 100;
	char out[4096], rows[4096];
	capture_output(draw_first_wrapped_line, out, sizeof(out));
	size_t n = 0, row_count = 0;
	for (size_t pos = 0; out[pos];) {
		if (out[pos] == '\x1b') {
			size_t bytes = skip_ansi_sequence(out, pos, strlen(out));
			assert(bytes > 0);
			if (out[pos + bytes - 1] == 'H') {
				if (row_count++ > 0)
					rows[n++] = '\n';
			}
			pos += bytes;
		} else {
			rows[n++] = out[pos++];
		}
	}
	rows[n] = '\0';
	if (strcmp(rows, expected) != 0)
		fprintf(stderr, "Wrapping %s at %zu:\nexpected: [%s]\nactual:   [%s]\n", input, cols, expected, rows);
	assert(strcmp(rows, expected) == 0);
	assert(rendered_rows_for_line(input, strlen(input)) == row_count);
}

static void test_word_wrapping(void)
{
	const char *big_words[] = {
		"https://www.example.com/some-really-really-ridiculously-and-completely-unnecessary-route",
		"[{\"x\":1,\"y\":\"neoshoesnhtoenshnethosneht\"}]",
		"/tmp/some-long-filename.tar.gz", "123.456,", "})", "load-bearing",
	};
	for (size_t i = 0; i < sizeof(big_words) / sizeof(big_words[0]); i++) {
		char input[256], expected[256];
		snprintf(input, sizeof(input), "xx %s", big_words[i]);
		snprintf(expected, sizeof(expected), "xx \n↪ %s", big_words[i]);
		expect_wrapping(input, strlen(big_words[i]) + 2, expected);
	}
	expect_wrapping("load-bearing", 9, "load-\n↪ bearing");
	expect_wrapping("jlcrochet91@pm.me", 12, "jlcrochet91@\n↪ pm.me");
	expect_wrapping("blahblahblahblahblahblahyakyak", 22, "blahblahblahblahblahbl\n↪ ahyakyak");
	expect_wrapping("foo_bar-baz", 9, "foo_bar-\n↪ baz");
	expect_wrapping("foo-verylongword", 10, "foo-verylo\n↪ ngword");
	expect_wrapping("foo bar", 6, "foo \n↪ bar");
	expect_wrapping("foo   bar", 5, "foo  \n↪ bar");
	expect_wrapping("foo\t  bar", 6, "foo   \n↪ bar");
	expect_wrapping("foo   ", 3, "foo");
	expect_wrapping("load-bearing\t  next", 9, "load-\n↪ bearing\n↪ next");
	expect_wrapping("xx café", 6, "xx \n↪ café");
	expect_wrapping("préfixe-café", 9, "préfixe-\n↪ café");
	expect_wrapping("abc—defgh", 7, "abc—\n↪ defgh");
	expect_wrapping("foo\u2003bar", 6, "foo\u2003\n↪ bar");
	expect_wrapping("xx 界界", 6, "xx \n↪ 界界");
	expect_wrapping("界界界", 4, "界界\n↪ 界");
	expect_wrapping("foo\x1b[31m-\x1b[0mbarbaz", 8, "foo-\n↪ barbaz");
	expect_wrapping("foo\x1b[31m  \x1b[0m bar", 5, "foo  \n↪ bar");

	/* Hard wrapping and physical newlines keep their existing meaning. */
	set_input("load-bearing");
	word_wrap = false;
	content_cols = 9;
	struct wrap_slice slice = wrapped_slice(buffer, buffer_size, 0, 9);
	assert(slice.render_cols == 9 && slice.advance_cols == 9);
	set_input("foo\nbar\n");
	assert(line_count == 2);
	assert(rendered_rows_for_line("foo", 3) == 1);
	assert(rendered_rows_for_line("bar", 3) == 1);

	/* Even a tab or wide character wider than the available row must advance. */
	const char *narrow_inputs[] = { "\t\tX", "界界界", "foo\t  bar", "a-/long_long_word" };
	for (size_t i = 0; i < sizeof(narrow_inputs) / sizeof(narrow_inputs[0]); i++) {
		for (int cols = 1; cols <= 12; cols++) {
			for (int mode = 0; mode < 2; mode++) {
				set_input(narrow_inputs[i]);
				content_cols = cols;
				word_wrap = mode;
				size_t width = display_width(buffer, buffer_size), start = 0, rows = 0;
				while (start < width) {
					size_t prefix = rows > 0 ? wrap_prefix_cols() : 0;
					size_t capacity = (size_t)cols - prefix;
					slice = wrapped_slice(buffer, buffer_size, start, capacity);
					assert(slice.advance_cols > 0 && slice.advance_cols <= width - start);
					assert(slice.render_cols <= capacity);
					start += slice.advance_cols;
					assert(++rows <= width);
				}
				assert(rendered_rows_for_line(buffer, buffer_size) == rows);
			}
		}
	}
}

static void test_wrap_prefix(void)
{
	set_input("text");
	char out[128], plain[128];
	assert(wrap_prefix_cols() == 2);
	capture_output(draw_wrap_prefix, out, sizeof(out));
	strip_ansi(out, strlen(out), plain, sizeof(plain));
	assert(strcmp(plain, "↪ ") == 0);
	wrap_marker_space = false;
	assert(wrap_prefix_cols() == 1);
	capture_output(draw_wrap_prefix, out, sizeof(out));
	strip_ansi(out, strlen(out), plain, sizeof(plain));
	assert(strcmp(plain, "↪") == 0);
	wrap_marker_space = true;
	content_cols = 2;
	assert(wrap_prefix_cols() == 1);
	content_cols = 1;
	assert(wrap_prefix_cols() == 0);
	capture_output(draw_wrap_prefix, out, sizeof(out));
	assert(out[0] == '\0');
}

static void test_word_wrapped_bottom(void)
{
	set_input("1\n2\nload-bearing-file\nlast\n");
	assert(rendered_rows_for_line("load-bearing-file", 17) == 3);
	scroll_to_bottom();
	assert(scroll_line == 2);
	size_t row = 0;
	assert(view_line_screen_span(3, &row, NULL) && row == 3);
	find_all_matches("last", false);
	scroll_line = 0;
	ensure_match_visible(0);
	assert(scroll_line == 2 && view_line_screen_span(3, NULL, NULL));
	content_cols = 14;
	scroll_to_bottom();
	assert(scroll_line == 1 && view_line_screen_span(3, NULL, NULL));
	content_cols = 10;
	word_wrap = false;
	scroll_to_bottom();
	assert(scroll_line == 1 && view_line_screen_span(3, NULL, NULL));
}

static void test_header_options(void)
{
	set_input("title\nbody\n");
	assert(set_header("1", 0));
	assert(header_lines == 1 && header_cols == 0 && header_start == 0);
	assert(!lines_wrap());
	assert(set_header(",3,2", 0));
	assert(header_lines == 1 && header_cols == 3 && header_start == 1);
	assert(set_header("2,", 4));
	assert(header_lines == 2 && header_cols == 3 && header_start == 4);
	assert(set_header("", 0));
	assert(header_lines == 2 && header_cols == 3 && header_start == 0);

	const char *invalid[] = { "-1", "1,-2", "1,2,0", "1,2,3,4", "1,2,3,",
		"x", "1x", "1,2x", "1,2,3x", "1 2", "+1",
		"999999999999999999999999999999999999999999" };
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		assert(!set_header(invalid[i], 0));
		assert(header_lines == 2 && header_cols == 3 && header_start == 0);
	}
	assert(set_header("-", 0));
	assert(!header_active() && lines_wrap());
	assert(set_header("0,0", 0));
	assert(!header_active());
}

static void test_header_scrolling_and_search(void)
{
	set_input("preface\nTITLE\none\ntwo\nthree\nfour\nfive\n");
	assert(set_header("1,0,2", 0));
	assert(view_line_count() == 5);
	assert(header_screen_rows() == 1 && body_screen_rows() == 3);
	assert(view_to_raw_line(0) == 2);
	size_t idx = SIZE_MAX;
	assert(!raw_to_view_line(0, &idx));
	assert(!raw_to_view_line(1, &idx));
	assert(raw_to_view_line(2, &idx) && idx == 0);
	assert(rendered_rows_for_line("long line that would wrap", 25) == 1);
	scroll_to_bottom();
	assert(scroll_line == 2);
	char status[1024];
	term_cols = 80;
	term_rows = 5;
	capture_output(render_status_bar, status, sizeof(status));
	assert(strstr(status, "5-7/7  NOWRAP VIEW HEADER"));
	size_t row = 0;
	assert(view_line_screen_span(4, &row, NULL) && row == 3);
	assert(go_to_line(2) && scroll_line == 2);
	assert(handle_navigation_action(ACTION_TOP) && scroll_line == 0);
	assert(handle_navigation_action(ACTION_PAGE_DOWN) && scroll_line == 2);
	assert(handle_navigation_action(ACTION_SCROLL_RIGHT) && scroll_col == 1);
	clamp_scroll();
	assert(scroll_col == 1);

	find_all_matches("TITLE|five", false);
	assert(match_count == 2 && matches[0].line_idx == 1);
	assert(scroll_line == 2); /* A match in the pinned header does not move the body. */
	scroll_line = 0;
	ensure_match_visible(1);
	assert(scroll_line == 2);
	assert(view_line_screen_span(4, NULL, NULL));
	assert(apply_filter("one|three|five", true));
	assert(view_line_count() == 3 && view_to_raw_line(2) == 6);
	assert(header_screen_rows() == 1);
	assert(raw_to_view_line(4, &idx) && idx == 1);
	find_all_matches("TITLE", false);
	assert(match_count == 1 && matches[0].line_idx == 1);
	clear_filter();
	assert(view_line_count() == 5);
	assert(execute_command_line("header -"));
	assert(view_line_count() == 7 && lines_wrap() && scroll_col == 0);
	assert(execute_command_line("header=1,2,2"));
	assert(header_lines == 1 && header_cols == 2 && header_start == 1);

	set_input("title\n");
	assert(set_header("10", 0));
	assert(view_line_count() == 0 && header_screen_rows() == 1);
	assert(handle_navigation_action(ACTION_SCROLL_RIGHT) && scroll_col == 1);
	clamp_scroll();
	assert(scroll_col == 1);
	find_all_matches("title", false);
	assert(match_count == 1);
	set_input("");
	assert(set_header("10", 0));
	assert(view_line_count() == 0 && header_screen_rows() == 0);
	clamp_scroll();
	set_input("1\n2\n3\n4\n5\n6\n");
	header_lines = SIZE_MAX;
	assert(header_end() == 6 && header_screen_rows() == 3);
	content_rows = 2;
	assert(header_screen_rows() == 1 && body_screen_rows() == 1);
	content_rows = 1;
	assert(header_screen_rows() == 0 && body_screen_rows() == 1);
}

static void draw_first_unwrapped_line(void)
{
	draw_unwrapped_line(0);
}

static void expect_unwrapped_line(const char *expected)
{
	char out[2048], plain[2048];
	capture_output(draw_first_unwrapped_line, out, sizeof(out));
	strip_ansi(out, strlen(out), plain, sizeof(plain));
	assert(strcmp(plain, expected) == 0);
}

static void test_header_columns(void)
{
	set_input("IDabcdefghijklmn\n");
	assert(set_header("0,2", 0));
	scroll_col = 3;
	expect_unwrapped_line("IDdefghijk");
	show_line_numbers = true;
	gutter_width = 2;
	expect_unwrapped_line("1 IDdefghijk");
	show_line_numbers = false;
	assert(set_header("0,100", 0));
	expect_unwrapped_line("IDabcdefgh");

	set_input("ID\tabcdefghij\n");
	assert(set_header("0,3", 0));
	scroll_col = 2;
	expect_unwrapped_line("ID    abcd");
	set_input("\x1b[31mID\x1b[0mabcdefghijklmn\n");
	assert(set_header("0,2", 0));
	scroll_col = 3;
	expect_unwrapped_line("IDdefghijk");
	set_input("A界BCDEFGH\n");
	assert(set_header("0,2", 0));
	expect_unwrapped_line("A  BCDEFGH");
	scroll_col = 1;
	expect_unwrapped_line("A BCDEFGH");
	set_input("ID\n");
	assert(set_header("0,4", 0));
	scroll_col = 100;
	expect_unwrapped_line("ID  ");
}

static void expect_line_underline(void (*draw_line)(void), const char *expected, size_t underline_from)
{
	char out[2048];
	capture_output(draw_line, out, sizeof(out));
	bool underline = false;
	size_t col = 0;
	for (size_t pos = 0; out[pos];) {
		if (out[pos] == '\x1b') {
			size_t bytes = skip_ansi_sequence(out, pos, strlen(out));
			assert(bytes > 0);
			if (strncmp(out + pos, SGR_UNDERSCORE_ON, bytes) == 0)
				underline = true;
			else if (strncmp(out + pos, SGR_RESET, bytes) == 0
				|| strncmp(out + pos, SGR_UNDERLINE_OFF, bytes) == 0)
				underline = false;
			pos += bytes;
			continue;
		}
		assert(out[pos++] == expected[col]);
		assert(underline == (col >= underline_from));
		col++;
	}
	assert(col == strlen(expected));
	assert(!underline); /* The boundary style must not leak into subsequent rows. */
}

static void draw_second_unwrapped_line(void)
{
	draw_unwrapped_line(1);
}

static void test_floating_header_underline(void)
{
	set_input("\x1b[31mH\x1b[0mE\x1b[24mAD\none\ntwo\nthree\nfour\nfive\n");
	assert(set_header("1", 0));
	expect_line_underline(draw_first_unwrapped_line, "HEAD", SIZE_MAX);
	assert(handle_navigation_action(ACTION_DOWN_LINE));
	expect_line_underline(draw_first_unwrapped_line, "HEAD      ", 0);
	find_all_matches("EA", false);
	expect_line_underline(draw_first_unwrapped_line, "HEAD      ", 0);
	show_line_numbers = true;
	gutter_width = 2;
	expect_line_underline(draw_first_unwrapped_line, "1 HEAD      ", 2);
	show_line_numbers = false;
	assert(handle_navigation_action(ACTION_TOP));
	expect_line_underline(draw_first_unwrapped_line, "HEAD", SIZE_MAX);
	assert(apply_filter("three|four|five", true));
	assert(scroll_line == 0);
	expect_line_underline(draw_first_unwrapped_line, "HEAD      ", 0);
	clear_filter();
	expect_line_underline(draw_first_unwrapped_line, "HEAD", SIZE_MAX);

	assert(set_header("2", 0));
	assert(handle_navigation_action(ACTION_DOWN_LINE));
	expect_line_underline(draw_first_unwrapped_line, "HEAD", SIZE_MAX);
	expect_line_underline(draw_second_unwrapped_line, "one       ", 0);
	assert(set_header(",2", 0));
	scroll_col = 1;
	expect_line_underline(draw_second_unwrapped_line, "on        ", 0);
	assert(set_header(",100", 0));
	expect_line_underline(draw_second_unwrapped_line, "one       ", 0);

	set_input("\n1\n2\n3\n4\n");
	assert(set_header("1", 0));
	assert(handle_navigation_action(ACTION_DOWN_LINE));
	expect_line_underline(draw_first_unwrapped_line, "          ", 0);
	assert(set_header("-", 0));
	expect_line_underline(draw_first_unwrapped_line, "", SIZE_MAX);
	set_input("HEAD\n");
	assert(set_header("1", 0));
	expect_line_underline(draw_first_unwrapped_line, "HEAD", SIZE_MAX);
}

int main(void)
{
	test_line_endings();
	test_bare_cr_is_preserved();
	test_wrapped_bottom();
	test_status_visible_range();
	test_word_wrapping();
	test_wrap_prefix();
	test_word_wrapped_bottom();
	test_header_options();
	test_header_scrolling_and_search();
	test_header_columns();
	test_floating_header_underline();
	clear_content();
	puts("pager regression tests passed");
	return 0;
}
