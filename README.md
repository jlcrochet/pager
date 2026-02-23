# pager

Terminal pager for viewing files and command output, similar to `less`.

## Features

- Line wrapping (on by default, togglable)
- Line numbers
- Regex search with match highlighting (forward and backward)
- Incremental search while typing (`/` and `?`)
- Configurable search case mode (`false` insensitive, `true` sensitive, `"smart"` smart-case)
- Search history
- Line filtering by regex
- Follow mode (like `tail -f`) for files and pipes
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
| `-l`, `--line N` | Start at line N |
| `-p`, `--pattern P` | Start with search pattern |
| `--sync-output` | Enable synchronized output rendering |
| `--no-sync-output` | Disable synchronized output rendering |
| `--generate-config[=PATH]` | Write a default config template |
| `-h`, `--help` | Show help |

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
