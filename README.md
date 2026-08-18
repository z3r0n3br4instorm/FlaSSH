<p align="center">
  <img src="Assets/logo.png" alt="FlashSSH logo">
</p>

<p align="center"><strong>A lag-free SSH experience.</strong></p>

## Install

```sh
curl -fsSL https://raw.githubusercontent.com/z3r0n3br4instorm/FlashSSH/main/install.sh | sh
```

Downloads the latest release binary for your architecture (`x86_64` or
`arm64`) and installs it as `fssh` in `/usr/local/bin` (prompting for sudo
only if that directory needs it). To install somewhere on your own PATH
without sudo:

```sh
curl -fsSL https://raw.githubusercontent.com/z3r0n3br4instorm/FlashSSH/main/install.sh | INSTALL_DIR="$HOME/.local/bin" sh
```

The binary links against `libssh` at runtime; the installer tells you if it's
missing. Prefer to read before you pipe to a shell? The script is
[`install.sh`](install.sh), or grab a binary straight from the
[releases page](../../releases). Building from source is covered
[below](#building).

A normal SSH session ties every keystroke to a full network round trip —
type a character, wait for the remote to echo it back, see it appear. On
anything but a fast, low-latency link, that's exactly where the familiar
laggy, rubber-banding feeling of typing over SSH comes from. FlashSSH exists
because most of that round-tripping is unnecessary: typing, cursor movement,
history recall, and tab completion all happen instantly against a client-side
line editor, with a single network round trip only when you actually press
Enter — instead of one per keystroke.

FlashSSH is a custom SSH client built on top of [libssh](https://www.libssh.org/),
with a p10k-style prompt, fish-style history suggestions, and a best-effort
PTY passthrough mode for full-screen and interactive programs (`btop`, `vim`,
`sudo`, `tmux`, ...) that genuinely need a real terminal and can't be made
lag-free this way.

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
- **PTY streaming mode** — commands that need a real terminal get a full PTY
  instead of a plain exec: local keystrokes are forwarded raw, remote output
  (including cursor movement, colors, and full-screen redraws) is written
  straight to your terminal. Three things route a command here:
  1. the allowlist below (known TTY programs, no round trip wasted);
  2. **automatic fallback** — if a plain exec fails with a "needs a terminal"
     style error (`stdin is not a terminal`, `inappropriate ioctl for device`,
     ...), FlashSSH swallows that error and transparently re-runs the command
     under a PTY. This is what makes things the allowlist has never heard of
     (`codex`, and anything else) just work;
  3. **`!` prefix** — `!somecommand` forces streaming mode outright, for
     programs that need a TTY but don't say so in a recognisable way.
- **Live status bar** — a blue bar pinned to the bottom row reports what the
  streaming session is doing rather than just sitting there: the running
  command, bytes received, terminal resizes, whether predictive echo is on or
  off (and why), and when input is being captured locally for a password
  prompt. Format: `Streaming... | <status>` on the left, `<bytes> | FlashSSH`
  on the right.
  - **Predictive local echo** — even inside a PTY session, typed characters
    are painted immediately in **grey** at the cursor instead of waiting for
    the round trip. When the server's echo arrives it repaints them in the
    app's own colors (i.e. they "turn white"), so grey text is exactly the
    text the server hasn't acknowledged yet. Backspacing over a still-grey
    character un-paints it locally right away. This is the same idea as
    [mosh](https://mosh.org/)'s predictive echo, minus the full terminal
    emulator: rather than diffing screen state, FlashSSH simply erases its
    guesses the instant authoritative output arrives and lets the app
    repaint. Whether prediction helps is **scored at runtime**, not guessed
    from the program's name: FlashSSH predicts one character, checks whether
    the server answers with a small chunk containing it, and either widens the
    window (a shell, editor, REPL or `codex` confirms almost immediately) or
    switches prediction off with an exponential backoff before re-probing.
    That's why it works in sessions the allowlist has never heard of. Known
    full-screen dashboards (`btop`, `htop`, `top`, `watch`, `mc`) start
    switched off so their first keystrokes don't flicker.
  - **Ctrl+C** and other control keys are forwarded to the remote program as
    normal — nothing local intercepts them, so a stuck remote process can be
    killed the normal way without touching the FlashSSH client itself.
  - **Ctrl+Q** (or **Ctrl+]** if your terminal eats Ctrl+Q for its own flow
    control — Termux and several desktop terminal emulators do) detaches
    back to the FlashSSH prompt. For most streamed programs this also ends
    the remote process (same as closing an SSH session would). The one
    exception is `tmux`: since a tmux session lives in a persistent server
    process, detaching (either with Ctrl+Q/Ctrl+] or tmux's own `Ctrl+B d`)
    leaves the session running on the remote host.
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

The list only exists to avoid wasting a failed round trip on programs we
already know about — it is no longer the only route into streaming mode. The
automatic "needs a terminal" fallback and the `!` prefix cover everything
else, so an unlisted program like `codex` works without editing this list.

## Known limitations

- **No general "text box focus" detection.** There's no protocol-level
  signal for "a text field is focused" over raw SSH/PTY. The password-prompt
  override above works because the prompt text itself is a reliable
  give-away; full-screen programs that draw their own input widgets (e.g.
  `btop`'s process filter) can't be detected this way and are just passed
  through raw.
- **Predictive echo can't help inside `btop`-style dashboards.** This is a
  hard limit of not embedding a terminal emulator, not a missing switch.
  Measured against real `btop`: it repaints its *entire* screen on a timer
  (~19KB/s), never emits a small incremental echo for a keystroke, and never
  toggles the terminal's cursor visibility — so there is nothing to confirm a
  prediction against, and no way to know where (or whether) typed text will
  land. Painting a grey character there would just be overwritten by the next
  full redraw within ~250ms: flicker, not latency hiding. Closing this
  properly needs client-side VT screen-state emulation with server-state
  diffing (the full mosh architecture), which is a much larger project than
  the heuristics here.
- **Predictive echo guesses, and sometimes guesses wrong.** Predictions are
  erased by overwriting them with spaces, which is only strictly correct when
  you're appending at the end of a line — the dominant case. Inserting
  mid-line relies on the app redrawing the rest of the line (editors do).
  A program that's in the predict-enabled set but doesn't echo a particular
  key can leave a grey character visible until its next redraw. Only
  single-byte printable ASCII is predicted; UTF-8 input, arrows, and control
  keys are forwarded without prediction.
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

Prebuilt `linux-x86_64` and `linux-arm64` binaries are attached to each
[GitHub release](../../releases), built automatically by
`.github/workflows/build-and-release.yml` on every push to `main`.

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

### At the prompt

| Input             | Meaning                                                    |
|-------------------|------------------------------------------------------------|
| `exit` / `quit`   | Close the connection and leave FlashSSH                    |
| `!<command>`      | Force streaming (PTY) mode for that command                 |

### Keybindings

| Key            | Action                                                        |
|----------------|----------------------------------------------------------------|
| Left / Right   | Move the cursor (Right at end-of-line accepts a suggestion)   |
| Up / Down      | Walk command history                                          |
| Tab            | Complete the current word (history for the first word, live directory listing for the rest) |
| Ctrl+D         | Quit on an empty line                                          |
| Ctrl+C         | (streaming mode) forwarded to the remote program               |
| Ctrl+Q or Ctrl+] | (streaming mode) detach back to the FlashSSH prompt (use Ctrl+] if your terminal intercepts Ctrl+Q) |
| Esc            | (streaming mode) cancel a local password entry without sending |

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
