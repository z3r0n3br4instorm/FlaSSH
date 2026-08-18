#include "stream.h"
#include "ssh_session.h"
#include "line_editor.h"
#include <libssh/libssh.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <time.h>

#define CTRL_Q 0x11
#define CTRL_RBRACKET 0x1d // telnet's classic escape char — many terminal
                           // emulators (Termux included) intercept Ctrl+Q
                           // themselves for flow control before it ever
                           // reaches us, so this is the reliable fallback.
#define STATUS_BAR_REPAINT_MS 300

// Predictive local echo: typed characters are painted immediately in grey,
// then the server's authoritative echo repaints them in the app's own
// colors. See the comment above paint_prediction() for the mechanism.
#define PREDICT_COLOR "\033[38;5;242m"
#define MAX_PREDICTIONS 32

// Whether prediction helps depends on the program, not on its name, so it's
// scored at runtime instead of guessed from a list: predict one character,
// see whether the server answers with a small chunk containing it, and
// widen or shut off accordingly. A program that echoes what you type
// (shell, editor, REPL, codex) confirms quickly; one that repaints its whole
// screen per keypress never does, and prediction switches itself off.
#define PREDICT_SMALL_ECHO_MAX 512   // bigger replies are redraws, not echoes
#define PREDICT_COOLDOWN_MS 2000     // doubles up to the cap on each re-disable
#define PREDICT_COOLDOWN_MAX_MS 30000

static const char *STREAMING_COMMANDS[] = {
    "btop", "htop", "top", "vim", "vi", "nvim", "nano", "less", "more",
    "man", "tmux", "screen", "watch", "mc", "irssi", "weechat",
    "sudo", "su", "ssh", "mysql", "psql", "sqlite3", "ftp", "sftp",
    "python3", "python", "node", "irb", "pry", NULL
};

// Full-screen dashboards where a keypress is a *command*, not text to be
// echoed (pressing 'f' in btop opens a filter, it doesn't type an "f").
// Predicting there would paint a character the server will never echo.
static const char *NO_PREDICT_COMMANDS[] = {
    "btop", "htop", "top", "watch", "mc", "irssi", "weechat", NULL
};

static void extract_first_word(const char *command, char *out, size_t out_size) {
    size_t i = 0;
    while (command[i] != '\0' && command[i] != ' ' && i < out_size - 1) {
        out[i] = command[i];
        i++;
    }
    out[i] = '\0';
}

static int word_in_list(const char *word, const char **list) {
    for (int j = 0; list[j] != NULL; j++) {
        if (strcmp(word, list[j]) == 0) return 1;
    }
    return 0;
}

int is_streaming_command(const char *command) {
    char first_word[64];
    extract_first_word(command, first_word, sizeof(first_word));
    return word_in_list(first_word, STREAMING_COMMANDS);
}

static int is_echo_predictable_command(const char *command) {
    char first_word[64];
    extract_first_word(command, first_word, sizeof(first_word));
    return !word_in_list(first_word, NO_PREDICT_COMMANDS);
}

static volatile sig_atomic_t got_sigwinch = 0;

static void on_sigwinch(int sig) {
    (void)sig;
    got_sigwinch = 1;
}

static void get_terminal_size(int *cols, int *rows) {
    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    } else {
        *cols = 80;
        *rows = 24;
    }
}

static void format_bytes(long bytes, char *out, size_t out_size) {
    if (bytes < 1024) {
        snprintf(out, out_size, "%ldB", bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(out, out_size, "%ld.%ldKB", bytes / 1024, (bytes % 1024) * 10 / 1024);
    } else {
        snprintf(out, out_size, "%ld.%ldMB", bytes / (1024 * 1024),
                 (bytes % (1024 * 1024)) * 10 / (1024 * 1024));
    }
}

// The remote process is only ever given (rows - 1) rows of PTY, so it never
// tries to draw into the real last row — that row is reserved for this bar.
// Drawing saves/restores the cursor so it doesn't disturb whatever the
// streamed program is doing.
//
// Layout: "Streaming... | <live status>" on the left, "<bytes> | FlashSSH" on
// the right. `status` is what the session is currently doing (local password
// entry, predictive echo state, resizes, ...) so the bar reports what's going
// on instead of just sitting there. Everything is plain ASCII so the column
// arithmetic for right-alignment and truncation stays correct.
static void draw_status_bar(int total_rows, int cols, const char *status, long bytes) {
    char seq[64];
    write(STDOUT_FILENO, "\0337", 2); // save cursor position + attributes

    int n = snprintf(seq, sizeof(seq), "\033[%d;1H", total_rows);
    write(STDOUT_FILENO, seq, n);
    write(STDOUT_FILENO, "\033[44m\033[K", 8); // blue background, blank the row

    static const char brand[] = "FlashSSH";
    char bytes_str[32];
    format_bytes(bytes, bytes_str, sizeof(bytes_str));

    // Right block first, so we know how much room the left block has.
    char right[64];
    int right_len = snprintf(right, sizeof(right), "%s | ", bytes_str);
    int right_total = right_len + (int)(sizeof(brand) - 1) + 1; // +1 trailing pad

    char left[256];
    int left_len;
    if (status != NULL && status[0] != '\0') {
        left_len = snprintf(left, sizeof(left), " Streaming... | %s", status);
    } else {
        left_len = snprintf(left, sizeof(left), " Streaming...");
    }

    int left_room = cols - right_total - 1;
    if (left_room < 0) left_room = 0;
    if (left_len > left_room) left_len = left_room; // truncate rather than wrap
    if (left_len > 0) write(STDOUT_FILENO, left, left_len);

    int right_col = cols - right_total + 1;
    if (right_col > left_len + 1) {
        n = snprintf(seq, sizeof(seq), "\033[%d;%dH", total_rows, right_col);
        write(STDOUT_FILENO, seq, n);
        write(STDOUT_FILENO, right, right_len);
        write(STDOUT_FILENO, "\033[3m", 4); // italic
        write(STDOUT_FILENO, brand, sizeof(brand) - 1);
    }

    write(STDOUT_FILENO, "\033[0m", 4);
    write(STDOUT_FILENO, "\0338", 2); // restore cursor position + attributes
}

static long elapsed_ms(const struct timespec *since) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - since->tv_sec) * 1000L + (now.tv_nsec - since->tv_nsec) / 1000000L;
}

// Paints one predicted character in grey at the cursor, then advances the
// cursor by one cell — the same place the server's echo would land.
//
// DECSC/DECRC (\0337 / \0338) save and restore the cursor *and* the current
// SGR attributes, so the app's own colors survive our write; the explicit
// \033[1C afterwards is what actually advances past the character we just
// painted (DECRC alone would put the cursor back on top of it).
static void paint_prediction(char ch) {
    char seq[64];
    int n = snprintf(seq, sizeof(seq), "\0337" PREDICT_COLOR "%c\0338\033[1C", ch);
    write(STDOUT_FILENO, seq, n);
}

// Un-paints the last `k` predicted characters and leaves the cursor exactly
// where the first of them started, so whatever the server sends next
// overwrites them with its authoritative rendering. Erasing with spaces is
// only correct for the append-at-end-of-line case, which is the dominant
// one — mid-line inserts rely on the app redrawing the rest of the line.
static void rollback_predictions(int k) {
    if (k <= 0) return;

    char seq[64];
    int n = snprintf(seq, sizeof(seq), "\033[%dD\0337\033[0m", k);
    write(STDOUT_FILENO, seq, n);
    for (int i = 0; i < k; i++) write(STDOUT_FILENO, " ", 1);
    write(STDOUT_FILENO, "\0338", 2);
}

static void clear_status_bar(int total_rows) {
    char seq[32];
    write(STDOUT_FILENO, "\0337", 2);
    int n = snprintf(seq, sizeof(seq), "\033[%d;1H", total_rows);
    write(STDOUT_FILENO, seq, n);
    write(STDOUT_FILENO, "\033[0m\033[K", 7);
    write(STDOUT_FILENO, "\0338", 2);
}

// There's no real "a text field is focused" signal over raw SSH/PTY — it's
// just bytes, and testing against real btop and real sudo confirmed neither
// touches the terminal's actual cursor visibility (btop draws its own
// synthetic cursor for its filter box; sudo's password prompt just disables
// pty echo, which isn't observable as bytes at all). The one thing that IS
// reliably observable is the literal prompt text itself, so detection is
// keyed off that instead: a rolling window of recent printable output, used
// to sniff whether we're about to type into a password prompt.
static void feed_recent_text(char *recent, int *recent_len, int cap, const char *data, int len) {
    for (int i = 0; i < len; i++) {
        char c = data[i];
        if (c < 0x20 || c > 0x7e) continue;
        if (*recent_len == cap) {
            memmove(recent, recent + 1, cap - 1);
            recent[cap - 1] = c;
        } else {
            recent[(*recent_len)++] = c;
        }
    }
}

static int recent_looks_like_password(const char *buf, int len) {
    static const char needle[] = "assword";
    const int nlen = 7;
    for (int i = 0; i + nlen <= len; i++) {
        int match = 1;
        for (int j = 0; j < nlen; j++) {
            char c = buf[i + j];
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c != needle[j]) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

int run_streaming_session(ssh_session session, const char *command) {
    char cwd[256];
    get_current_dir(cwd, sizeof(cwd));

    char full_cmd[768];
    snprintf(full_cmd, sizeof(full_cmd), "cd %s && %s", cwd, command);

    ssh_session_lock();

    ssh_channel channel = ssh_channel_new(session);
    if (channel == NULL) {
        ssh_session_unlock();
        return -1;
    }

    if (ssh_channel_open_session(channel) != SSH_OK) {
        ssh_channel_free(channel);
        ssh_session_unlock();
        return -1;
    }

    int cols, rows;
    get_terminal_size(&cols, &rows);
    int remote_rows = (rows > 1) ? rows - 1 : rows; // reserve the bottom row for the status bar
    if (ssh_channel_request_pty_size(channel, "xterm-256color", cols, remote_rows) != SSH_OK) {
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        ssh_session_unlock();
        return -1;
    }

    if (ssh_channel_request_exec(channel, full_cmd) != SSH_OK) {
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        ssh_session_unlock();
        return -1;
    }

    fprintf(stderr, "\033[3mClient-side processing has been overridden by streaming mode. Press Ctrl+Q (or Ctrl+]) to return.\033[0m\r\n");

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigwinch;
    sigaction(SIGWINCH, &sa, NULL);
    got_sigwinch = 0;

    terminal_enable_raw_mode();

    char status[192];
    long bytes_in = 0;
    int status_dirty = 0; // set whenever `status` changes, so the bar repaints
                          // on the next loop cycle instead of waiting out the
                          // 300ms tick (which a short session may never reach)
    snprintf(status, sizeof(status), "running '%s'", command);

    draw_status_bar(rows, cols, status, bytes_in);
    struct timespec last_bar_draw;
    clock_gettime(CLOCK_MONOTONIC, &last_bar_draw);

    char iobuf[4096];
    int detached = 0;

    char recent_text[128];
    int recent_len = 0;

    int textbox_active = 0;
    char textbox_buf[512];
    int textbox_len = 0;

    // Known full-screen dashboards start switched off (their first keystrokes
    // would flicker before scoring caught up); everything else starts by
    // probing with a single character and earns a wider window.
    int predict_confidence = is_echo_predictable_command(command) ? 1 : 0;
    int predict_confirms = 0;
    int predict_misses = 0;
    int predict_in_cooldown = 0;
    long predict_cooldown_ms = PREDICT_COOLDOWN_MS;
    struct timespec predict_cooldown_start;

    char pred_chars[MAX_PREDICTIONS];
    int predicted = 0; // characters painted locally but not yet confirmed

    while (ssh_channel_is_open(channel) && !ssh_channel_is_eof(channel)) {
        if (got_sigwinch) {
            got_sigwinch = 0;
            get_terminal_size(&cols, &rows);
            remote_rows = (rows > 1) ? rows - 1 : rows;
            ssh_channel_change_pty_size(channel, cols, remote_rows);
            snprintf(status, sizeof(status), "resized to %dx%d", cols, remote_rows);
            draw_status_bar(rows, cols, status, bytes_in);
            clock_gettime(CLOCK_MONOTONIC, &last_bar_draw);
        }

        // The remote can wipe our reserved row via a raw "clear whole
        // screen" or by switching to its own alternate screen buffer —
        // neither respects the reduced PTY height we gave it, since both
        // operate on the real terminal geometry. There's no reliable way to
        // catch every such escape sequence, so just repaint continuously.
        if (status_dirty || elapsed_ms(&last_bar_draw) >= STATUS_BAR_REPAINT_MS) {
            draw_status_bar(rows, cols, status, bytes_in);
            clock_gettime(CLOCK_MONOTONIC, &last_bar_draw);
            status_dirty = 0;
        }

        // Prediction shut itself off after repeated wrong guesses; give it
        // another single-character probe once the cooldown expires, backing
        // off further each time so a never-echoing program settles down
        // instead of flickering forever.
        if (predict_in_cooldown && elapsed_ms(&predict_cooldown_start) >= predict_cooldown_ms) {
            predict_in_cooldown = 0;
            predict_confidence = 1;
            predict_confirms = 0;
            predict_misses = 0;
            if (predict_cooldown_ms < PREDICT_COOLDOWN_MAX_MS) predict_cooldown_ms *= 2;
        }

        struct pollfd pfd;
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        if (poll(&pfd, 1, 10) > 0 && (pfd.revents & POLLIN)) {
            char inbuf[256];
            ssize_t n = read(STDIN_FILENO, inbuf, sizeof(inbuf));
            for (ssize_t i = 0; i < n; i++) {
                char ch = inbuf[i];

                if (ch == CTRL_Q || ch == CTRL_RBRACKET) {
                    detached = 1;
                    break;
                }

                if (!textbox_active) {
                    if (ch >= 32 && ch < 127 && predicted < predict_confidence) {
                        // Plain printable char: paint it now, confirm later.
                        pred_chars[predicted] = ch;
                        paint_prediction(ch);
                        predicted++;
                    } else if ((ch == 127 || ch == 8) && predicted > 0) {
                        // Backspace over a still-unconfirmed char: just
                        // un-paint it locally (the byte still goes to the
                        // server, which will delete the real one).
                        rollback_predictions(1);
                        predicted--;
                    } else if (predicted > 0) {
                        // Enter, arrows, control keys: the resulting screen
                        // change is unpredictable, so give up on the
                        // outstanding predictions rather than guess.
                        rollback_predictions(predicted);
                        predicted = 0;
                    }
                    ssh_channel_write(channel, &ch, 1);
                    continue;
                }

                // Local text-box mode: buffer the password here instead of
                // forwarding raw, since the remote isn't echoing it anyway.
                // Nothing is echoed locally either — blind entry, same as
                // real sudo/ssh password prompts.
                if (ch == '\r' || ch == '\n') {
                    ssh_channel_write(channel, textbox_buf, textbox_len);
                    ssh_channel_write(channel, "\r", 1);
                    snprintf(status, sizeof(status), "password sent (%d chars)", textbox_len);
                    status_dirty = 1;
                    textbox_active = 0;
                    textbox_len = 0;
                } else if (ch == 27) { // Escape — bail out without sending anything
                    textbox_active = 0;
                    textbox_len = 0;
                    snprintf(status, sizeof(status), "password entry cancelled");
                    status_dirty = 1;
                } else if (ch == 127 || ch == 8) { // Backspace
                    if (textbox_len > 0) textbox_len--;
                } else if (ch >= 32 && ch < 127) {
                    if (textbox_len < (int)sizeof(textbox_buf) - 1) {
                        textbox_buf[textbox_len++] = ch;
                    }
                }
            }
            if (detached) break;
        }

        int n = ssh_channel_read_nonblocking(channel, iobuf, sizeof(iobuf), 0);
        if (n > 0) {
            // The server has spoken: drop our grey guesses and let its
            // authoritative output paint over them in the app's own colors.
            if (predicted > 0) {
                // Score the guess first. A small reply carrying the character
                // we predicted is an echo; a large one is a screen redraw,
                // which means this program doesn't echo keystrokes as text.
                int looks_like_echo = (n <= PREDICT_SMALL_ECHO_MAX) &&
                                      (memchr(iobuf, pred_chars[0], n) != NULL);
                if (looks_like_echo) {
                    predict_misses = 0;
                    if (++predict_confirms >= 2 && predict_confidence != MAX_PREDICTIONS) {
                        predict_confidence = MAX_PREDICTIONS;
                        predict_cooldown_ms = PREDICT_COOLDOWN_MS; // earned trust resets backoff
                        snprintf(status, sizeof(status), "predictive echo on (server echoes input)");
                        status_dirty = 1;
                    }
                } else {
                    predict_confirms = 0;
                    if (++predict_misses >= 2) {
                        if (predict_confidence != 0) {
                            snprintf(status, sizeof(status),
                                     "predictive echo off (no echo detected)");
                            status_dirty = 1;
                        }
                        predict_confidence = 0;
                        predict_in_cooldown = 1;
                        clock_gettime(CLOCK_MONOTONIC, &predict_cooldown_start);
                    } else if (predict_confidence > 1) {
                        predict_confidence = 1; // demote to probing
                    }
                }

                rollback_predictions(predicted);
                predicted = 0;
            }
            write(STDOUT_FILENO, iobuf, n);
            bytes_in += n;
            feed_recent_text(recent_text, &recent_len, sizeof(recent_text), iobuf, n);

            if (!textbox_active && recent_looks_like_password(recent_text, recent_len)) {
                textbox_active = 1;
                textbox_len = 0;
                recent_len = 0; // don't let this same prompt text re-trigger after we submit
                snprintf(status, sizeof(status),
                         "password prompt - typed locally, Enter sends, Esc cancels");
                draw_status_bar(rows, cols, status, bytes_in); // don't wait for the next tick
            }
        }

        int en = ssh_channel_read_nonblocking(channel, iobuf, sizeof(iobuf), 1);
        if (en > 0) {
            if (predicted > 0) {
                rollback_predictions(predicted);
                predicted = 0;
            }
            write(STDOUT_FILENO, iobuf, en);
            bytes_in += en;
        }
    }

    if (predicted > 0) rollback_predictions(predicted); // no grey leftovers on detach

    signal(SIGWINCH, SIG_DFL);
    clear_status_bar(rows);
    terminal_disable_raw_mode();

    // Closing the channel here ends the remote process too (same as
    // closing a plain SSH session kills what was running in it) — Ctrl+Q
    // returns you to the FlashSSH prompt, but it doesn't leave btop/sudo
    // running detached the way tmux would.
    ssh_channel_send_eof(channel);
    ssh_channel_close(channel);
    ssh_channel_free(channel);

    ssh_session_unlock();

    write(STDOUT_FILENO, "\r\n", 2);
    return 0;
}
