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
#define STATUS_BAR_REPAINT_MS 300

static const char *STREAMING_COMMANDS[] = {
    "btop", "htop", "top", "vim", "vi", "nvim", "nano", "less", "more",
    "man", "tmux", "screen", "watch", "mc", "irssi", "weechat",
    "sudo", "su", "ssh", "mysql", "psql", "sqlite3", "ftp", "sftp",
    "python3", "python", "node", "irb", "pry", NULL
};

int is_streaming_command(const char *command) {
    char first_word[64];
    size_t i = 0;
    while (command[i] != '\0' && command[i] != ' ' && i < sizeof(first_word) - 1) {
        first_word[i] = command[i];
        i++;
    }
    first_word[i] = '\0';

    for (int j = 0; STREAMING_COMMANDS[j] != NULL; j++) {
        if (strcmp(first_word, STREAMING_COMMANDS[j]) == 0) return 1;
    }
    return 0;
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

// The remote process is only ever given (rows - 1) rows of PTY, so it never
// tries to draw into the real last row — that row is reserved for this bar.
// Drawing saves/restores the cursor so it doesn't disturb whatever the
// streamed program is doing.
static void draw_status_bar(int total_rows, int cols) {
    char seq[32];
    write(STDOUT_FILENO, "\0337", 2); // save cursor position + attributes

    int n = snprintf(seq, sizeof(seq), "\033[%d;1H", total_rows);
    write(STDOUT_FILENO, seq, n);
    write(STDOUT_FILENO, "\033[44m\033[K", 8); // blue background, blank the row

    static const char left[] = "Streaming...";
    static const char right[] = "FlashSSH";
    write(STDOUT_FILENO, left, sizeof(left) - 1);

    int right_col = cols - (int)(sizeof(right) - 1) + 1;
    if (right_col > (int)(sizeof(left) - 1) + 2) {
        n = snprintf(seq, sizeof(seq), "\033[%d;%dH", total_rows, right_col);
        write(STDOUT_FILENO, seq, n);
        write(STDOUT_FILENO, "\033[3m", 4); // italic
        write(STDOUT_FILENO, right, sizeof(right) - 1);
    }

    write(STDOUT_FILENO, "\033[0m", 4);
    write(STDOUT_FILENO, "\0338", 2); // restore cursor position + attributes
}

static long elapsed_ms(const struct timespec *since) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - since->tv_sec) * 1000L + (now.tv_nsec - since->tv_nsec) / 1000000L;
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

    fprintf(stderr, "\033[3mClient-side processing has been overridden by streaming mode. Press Ctrl+Q to return.\033[0m\r\n");

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigwinch;
    sigaction(SIGWINCH, &sa, NULL);
    got_sigwinch = 0;

    terminal_enable_raw_mode();
    draw_status_bar(rows, cols);
    struct timespec last_bar_draw;
    clock_gettime(CLOCK_MONOTONIC, &last_bar_draw);

    char iobuf[4096];
    int detached = 0;

    char recent_text[128];
    int recent_len = 0;

    int textbox_active = 0;
    char textbox_buf[512];
    int textbox_len = 0;

    while (ssh_channel_is_open(channel) && !ssh_channel_is_eof(channel)) {
        if (got_sigwinch) {
            got_sigwinch = 0;
            get_terminal_size(&cols, &rows);
            remote_rows = (rows > 1) ? rows - 1 : rows;
            ssh_channel_change_pty_size(channel, cols, remote_rows);
            draw_status_bar(rows, cols);
            clock_gettime(CLOCK_MONOTONIC, &last_bar_draw);
        }

        // The remote can wipe our reserved row via a raw "clear whole
        // screen" or by switching to its own alternate screen buffer —
        // neither respects the reduced PTY height we gave it, since both
        // operate on the real terminal geometry. There's no reliable way to
        // catch every such escape sequence, so just repaint continuously.
        if (elapsed_ms(&last_bar_draw) >= STATUS_BAR_REPAINT_MS) {
            draw_status_bar(rows, cols);
            clock_gettime(CLOCK_MONOTONIC, &last_bar_draw);
        }

        struct pollfd pfd;
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        if (poll(&pfd, 1, 10) > 0 && (pfd.revents & POLLIN)) {
            char inbuf[256];
            ssize_t n = read(STDIN_FILENO, inbuf, sizeof(inbuf));
            for (ssize_t i = 0; i < n; i++) {
                char ch = inbuf[i];

                if (ch == CTRL_Q) {
                    detached = 1;
                    break;
                }

                if (!textbox_active) {
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
                    textbox_active = 0;
                    textbox_len = 0;
                } else if (ch == 27) { // Escape — bail out without sending anything
                    textbox_active = 0;
                    textbox_len = 0;
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
            write(STDOUT_FILENO, iobuf, n);
            feed_recent_text(recent_text, &recent_len, sizeof(recent_text), iobuf, n);

            if (!textbox_active && recent_looks_like_password(recent_text, recent_len)) {
                textbox_active = 1;
                textbox_len = 0;
                recent_len = 0; // don't let this same prompt text re-trigger after we submit
            }
        }

        int en = ssh_channel_read_nonblocking(channel, iobuf, sizeof(iobuf), 1);
        if (en > 0) write(STDOUT_FILENO, iobuf, en);
    }

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
