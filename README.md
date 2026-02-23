# pager

Terminal pager for viewing files and command output, similar to `less`.

## Features

- Line wrapping (on by default, togglable)
- Line numbers
- Regex search with match highlighting (forward and backward)
- Incremental search while typing (`/` and `?`)
- Smart case sensitivity (case-insensitive unless the query contains uppercase)
- Search history
- Line filtering by regex
- Follow mode (like `tail -f`) for files and pipes
- Yank current line to clipboard via OSC 52
- Command mode with autocompletion popup
- Multi-file support with `:n` / `:p` navigation
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
search_current_match_sgr = "7;93"
search_other_match_sgr = "7"
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
follow = ["F"]
yank = ["y"]
command = [":"]
help = ["h"]
```

`search_current_match_sgr` and `search_other_match_sgr` accept SGR parameter lists
(for example `7;93` for reverse + bright yellow, or `7` for plain reverse).
