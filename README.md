# pager

Terminal pager for viewing files and command output, similar to `less`.

## Features

- Line wrapping (on by default, togglable)
- Line numbers
- Fixed header lines and columns (`--header`, like `less`)
- Regex search with match highlighting (forward and backward)
- Incremental search while typing (`/` and `?`)
- Configurable search case mode (`false` insensitive, `true` sensitive, `"smart"` smart-case)
- Search history
- Line filtering by regex
- Follow mode (like `tail -f`) for files and pipes
- Live terminal resizing via `SIGWINCH`
- Yank current line to clipboard via OSC 52
- Command mode with autocompletion popup
- Multi-file support with `:n` / `:p` and `]` / `[` navigation
- Mouse scroll support (SGR mouse protocol)
- ANSI color passthrough
- Binary file detection with confirmation prompt
- Configurable keybindings via TOML config file
- No external dependencies

## Build

```
make
make install
```

Installs to `~/.local/bin/`.

Run regression tests with `make test`.

## Usage

```
pager [OPTIONS] [FILE...]
command | pager
```

### Options

| Flag | Description |
|------|-------------|
| `-N`, `--number` | Show line numbers |
| `-S`, `--nowrap` | Disable line wrapping |
| `-F`, `--follow` | Start in follow mode |
| `--wordwrap` | Prefer whole big words, then little-word boundaries (default) |
| `--no-wordwrap` | Wrap lines at any character |
| `--wrap-marker-space` | Add a space after the wrapped-line marker (default) |
| `--no-wrap-marker-space` | Omit the space after the wrapped-line marker |
| `--quit-if-one-screen` | Quit after printing if one known-size input fits one terminal page |
| `--header L[,C[,N]]` | Keep L lines and C columns fixed, starting at line N (default 1) |
| `-l`, `--line N` | Start at line N |
| `-p`, `--pattern P` | Start with search pattern |
| `--sync-output` | Enable synchronized output rendering |
| `--no-sync-output` | Disable synchronized output rendering |
| `--generate-config[=PATH]` | Write a default config template |
| `-h`, `--help` | Show help |

Wrapping is enabled by default (`wrap = true` in the config). Word wrapping first
tries to keep each run of non-whitespace characters together, including URLs,
paths, and punctuation. If a run cannot fit on a continuation row, it prefers
breaks after punctuation to keep runs of letters, digits, and `_` together. If
those runs are also too long, it fills the row before splitting them. Unicode
letters and digits count as word characters.

Trailing whitespace stays with the preceding text. Whitespace extending past the
edge is consumed without starting a continuation with extra spaces or tabs;
the input itself is unchanged. Physical newlines remain line breaks. Continuation
rows start with `↪ `, with the space omitted in very narrow terminals. Use
`--no-wrap-marker-space` for the compact `↪` prefix, or `--no-wordwrap` for wrapping
at any character. These flags control wrapping style; `-S` disables wrapping.

For tabular output, `pager --header=1 table.txt` keeps the first line visible while
scrolling. `pager --header=1,8 table.txt` also keeps the first eight display columns
fixed when scrolling horizontally. `--header=2,0,3` uses lines 3 and 4 as the header;
lines before the header are hidden. Header columns do not include the line-number
gutter.
When scrolling separates the header from the following input line, its last row
is underlined across the text area to show that it is floating. The underline
disappears when the body returns to the line immediately after the header.

Like `less`, active headers truncate long lines and allow horizontal scrolling,
regardless of the wrap setting. Use `--header=-` or `:header -` to disable headers
and restore the wrap setting. Empty or omitted L/C fields keep their previous
values, so `--header=,8` sets only the fixed columns. Header rows are limited to
the available height, leaving at least one row for the body.

## Keybindings

| Key | Action |
|-----|--------|
| `q` | Quit |
| `j` / Down / Enter | Down one line |
| `k` / Up | Up one line |
| Space / PgDn | Page down |
| `b` / PgUp | Page up |
| `d` / `u` | Half page down / up |
| `g` / Home | Go to top |
| `G` / End | Go to bottom |
| Left / Right | Horizontal scroll (when wrapping is off) |
| `w` | Toggle line wrapping |
| `/` | Search forward |
| `?` | Search backward |
| `n` / `N` | Next / previous match |
| `]` / `[` | Next / previous file |
| Esc | Clear active search |
| `F` | Enter follow mode |
| `y` | Yank current line to clipboard |
| `:` | Open command prompt |
| `h` | Show help |

## Commands

| Command | Description |
|---------|-------------|
| `:q` | Quit |
| `:123` | Go to line 123 |
| `:wrap` | Enable line wrapping |
| `:nowrap` | Disable line wrapping |
| `:number` | Show line numbers |
| `:nonumber` | Hide line numbers |
| `:sync` | Enable synchronized output rendering |
| `:nosync` | Disable synchronized output rendering |
| `:filter <pattern>` | Filter lines by regex (empty pattern clears filter) |
| `:header L[,C[,N]]` | Set fixed header lines/columns (N defaults to the top displayed line) |
| `:header -` | Disable fixed headers |
| `:n` | Next file |
| `:p` | Previous file |
| `:follow` | Enter follow mode |
| `:help` | Show key bindings |

## Configuration

Config file location: `$PAGER_CONFIG`, or `$XDG_CONFIG_HOME/pager.toml`, or `~/.config/pager.toml`.

Generate a default config template:

```
pager --generate-config
```

Example config:

```toml
wrap = true
number = false
search_regex = true
search_wrap = true
search_case = "smart"
search_current_match_sgr = "reversed yellow"
search_other_match_sgr = "reversed"
command_popup_rows = 5
sync_output = true
quit_if_one_screen = false

[keybindings]
quit = ["q"]
down = ["j", "down", "enter"]
up = ["k", "up"]
page_down = ["space", "pgdn"]
page_up = ["b", "pgup"]
half_page_down = ["d"]
half_page_up = ["u"]
top = ["g", "home"]
bottom = ["G", "end"]
search_forward = ["/"]
search_backward = ["?"]
next_match = ["n"]
prev_match = ["N"]
next_file = ["]"]
prev_file = ["["]
follow = ["F"]
yank = ["y"]
command = [":"]
help = ["h"]
```

`search_current_match_sgr` and `search_other_match_sgr` accept either numeric SGR
codes (for example `7;33`) or common aliases (for example `reversed yellow`,
`reversed`, `bright-yellow`, `bg-blue`).

`search_case` accepts `false` (always case-insensitive), `true` (always case-sensitive),
or `"smart"` (case-insensitive unless the query has uppercase). Default is `"smart"`.

`command_popup_rows` controls the maximum number of command suggestions shown in
the `:` popup (default `5`, max `32`).

`quit_if_one_screen` enables less-style auto-quit behavior for a single input that
fits the current terminal page. Piped input is checked after it has been read to EOF.
