# FlashSSH

A custom SSH client built on top of [libssh](https://www.libssh.org/), with a
p10k-style prompt, fish-style history suggestions, and a best-effort PTY
passthrough mode for full-screen and interactive programs (`btop`, `vim`,
`sudo`, `tmux`, ...).

FlashSSH normally runs each command as a one-shot remote exec and prints the
result (fast, but not a real terminal for the remote program). When it
detects a command that needs a real TTY, it transparently switches to a raw
PTY passthrough session instead.

## Features

- **Powerline-style prompt** — two-line prompt with `user@host` and the
  current directory as colored segments, and a `$`/`#` caret that turns red
  when the last command exited non-zero.
- **Fish-style autosuggestions** — as you type, the most recent matching
  history entry is shown dimmed after the cursor. Press **Right** at the end
  of the line to accept it.
- **Tab completion** — completes the first word of the line against command
  history, and any later word against a live directory listing of the
  current remote working directory (kept fresh by a background thread, so
  completion doesn't block on a network round trip).
- **Remote history** — downloads `~/.bash_history` from the remote host on
  connect and merges it with commands you run this session, so **Up/Down**
  recall works from the first command.
- **PTY streaming mode** — commands that look like they need a real
  terminal (see the allowlist below) get a full PTY instead of a plain exec:
  local keystrokes are forwarded raw, remote output (including cursor
  movement, colors, and full-screen redraws) is written straight to your
  terminal. A blue status bar (`Streaming...` / `FlashSSH`) is pinned to the
  bottom row for the duration.
  - **Ctrl+C** and other control keys are forwarded to the remote program as
    normal — nothing local intercepts them, so a stuck remote process can be
    killed the normal way without touching the FlashSSH client itself.
  - **Ctrl+Q** detaches back to the FlashSSH prompt. For most streamed
    programs this also ends the remote process (same as closing an SSH
    session would). The one exception is `tmux`: since a tmux session lives
    in a persistent server process, detaching (either with Ctrl+Q or tmux's
    own `Ctrl+B d`) leaves the session running on the remote host.
  - **Password prompt override** — if the remote output looks like a
    password prompt, input switches to a local, blind (unechoed) buffer and
    is sent to the remote in one shot when you press Enter, instead of being
    forwarded keystroke-by-keystroke. Press **Escape** to bail out without
    sending anything if this triggers by mistake.
- **Public-key authentication** — same `-i <identity_file>` flag as the real
  `ssh` binary. Without `-i`, it tries ssh-agent/default `~/.ssh` keys
  first (`ssh_userauth_publickey_auto`), then falls back to a password
  prompt, matching `ssh`'s own default behavior.

### What gets a PTY

The following are treated as needing a real terminal (matched against the
first word you type):

```
btop htop top vim vi nvim nano less more man tmux screen watch mc
irssi weechat sudo su ssh mysql psql sqlite3 ftp sftp
python3 python node irb pry
```

This is a fixed, best-effort list — there's no general way to know a program
needs a TTY without running it.

## Known limitations

- **No general "text box focus" detection.** There's no protocol-level
  signal for "a text field is focused" over raw SSH/PTY. The password-prompt
  override above works because the prompt text itself is a reliable
  give-away; full-screen programs that draw their own input widgets (e.g.
  `btop`'s process filter) can't be detected this way and are just passed
  through raw.
- **Status bar repaints on a timer, not on demand.** `clear`, alternate
  screen buffers (`btop`, `tmux`, `vim`, ...) and similar can wipe the
  reserved bottom row using escape sequences that ignore the reduced PTY
  height we report to the remote process. Rather than trying to catch every
  such sequence, the bar just repaints every 300ms, so there's a brief
  (usually imperceptible) window where it can be missing.
- **Directory tracking is client-side.** Since a plain SSH exec channel
  doesn't persist state between commands, FlashSSH tracks your current
  directory itself and re-`cd`s into it before every command. Running an
  interactive shell through streaming mode (e.g. `sudo -i`) and `cd`-ing
  around inside it won't be reflected back once you return to the normal
  prompt.
- Tab completion is word-based (bash-style), not a fully general shell
  parser — it won't understand quoting, `$VAR` expansion, etc. when deciding
  what "the current word" is.

## Building

Dependencies: `gcc`, `make`, `libssh` (with headers), and `pthread` (part of
glibc on Linux).

```sh
# Debian/Ubuntu
sudo apt-get install libssh-dev

make
```

This produces a `main` binary in the project root.

## Usage

```sh
./main [-i identity_file] <username>@<host>
```

Examples:

```sh
./main zerone@192.168.1.10
./main -i ~/.ssh/id_ed25519 zerone@192.168.1.10
```

### Keybindings

| Key            | Action                                                        |
|----------------|----------------------------------------------------------------|
| Left / Right   | Move the cursor (Right at end-of-line accepts a suggestion)   |
| Up / Down      | Walk command history                                          |
| Tab            | Complete the current word (history for the first word, live directory listing for the rest) |
| Ctrl+C         | (streaming mode) forwarded to the remote program               |
| Ctrl+Q         | (streaming mode) detach back to the FlashSSH prompt             |

## Project layout

```
main.c                  entry point, prompt rendering, CLI arg parsing
headers/ssh_connection.c/.h   connection setup, host-key verification, auth
headers/ssh_session.c/.h      remote exec, client-tracked cwd, exit status
headers/line_editor.c/.h      raw-mode line editor: history nav, completion, ghost suggestions
headers/history.c/.h          remote bash history download + in-memory log
headers/dir_cache.c/.h        background thread caching a live directory listing
headers/stream.c/.h           PTY passthrough / streaming mode, status bar
```
