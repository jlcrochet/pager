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
	wrap_marker_space = false;
	show_line_numbers = false;
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

int main(void)
{
	test_line_endings();
	test_bare_cr_is_preserved();
	test_wrapped_bottom();
	test_status_visible_range();
	clear_content();
	puts("pager regression tests passed");
	return 0;
}
