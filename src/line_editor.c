#include "flassh/line_editor.h"
#include "flassh/history.h"
#include "flassh/dir_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <poll.h>

#define LINE_MAX_LEN 1024
#define MAX_COMPLETION_MATCHES 64
#define SUGGESTION_COLOR "\033[38;5;242m"
#define RESET_COLOR "\033[0m"

static struct termios orig_termios;

void terminal_disable_raw_mode(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void terminal_enable_raw_mode(void) {
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

// Redraws the whole prompt block: since prompt may itself span multiple
// terminal rows (e.g. a two-line powerline-style prompt), a plain
// erase-current-line isn't enough — we move back up to the top of the
// block, wipe everything below, then redraw prompt + typed text, then
// (if the cursor is at the end of the line and one is available) a dimmed
// "ghost" suggestion, then reposition the cursor back to `pos`.
static void refresh_line(const char *prompt, const char *buf, int len, int pos, const char *suggestion) {
    char seq[32];

    int prompt_lines = 0;
    for (const char *p = prompt; *p != '\0'; p++) {
        if (*p == '\n') prompt_lines++;
    }
    if (prompt_lines > 0) {
        int n = snprintf(seq, sizeof(seq), "\033[%dA", prompt_lines);
        write(STDOUT_FILENO, seq, n);
    }
    write(STDOUT_FILENO, "\r\033[J", 4);

    write(STDOUT_FILENO, prompt, strlen(prompt));
    write(STDOUT_FILENO, buf, len);

    int suggestion_len = 0;
    if (pos == len && suggestion != NULL && suggestion[0] != '\0') {
        suggestion_len = strlen(suggestion);
        write(STDOUT_FILENO, SUGGESTION_COLOR, strlen(SUGGESTION_COLOR));
        write(STDOUT_FILENO, suggestion, suggestion_len);
        write(STDOUT_FILENO, RESET_COLOR, strlen(RESET_COLOR));
    }

    int move_left = (len - pos) + suggestion_len;
    if (move_left > 0) {
        int n = snprintf(seq, sizeof(seq), "\033[%dD", move_left);
        write(STDOUT_FILENO, seq, n);
    }
}

// Fish-style ambient suggestion: the most recent history line that starts
// with the whole buffer, minus the part already typed.
static void compute_suggestion(const char *buf, int len, char *out, size_t out_size) {
    out[0] = '\0';
    if (len == 0) return;

    const char *matches[MAX_COMPLETION_MATCHES];
    int match_count = history_find_prefix_matches(buf, matches, MAX_COMPLETION_MATCHES);
    if (match_count == 0) return;

    const char *best = matches[0];
    size_t mlen = strlen(best);
    if (mlen <= (size_t)len) return;

    size_t suffix_len = mlen - len;
    if (suffix_len >= out_size) suffix_len = out_size - 1;
    memcpy(out, best + len, suffix_len);
    out[suffix_len] = '\0';
}

// Tab completion: completes the word under the cursor. The first word of
// the line completes against history (whole commands); later words
// complete against the live directory-listing cache (filenames).
static void complete_word(char *buf, int *len, int *pos) {
    int word_start = *pos;
    while (word_start > 0 && buf[word_start - 1] != ' ') word_start--;
    int word_len = *pos - word_start;

    char word[LINE_MAX_LEN];
    memcpy(word, &buf[word_start], word_len);
    word[word_len] = '\0';

    const char *matches[MAX_COMPLETION_MATCHES];
    int match_count;

    if (word_start == 0) {
        match_count = history_find_prefix_matches(word, matches, MAX_COMPLETION_MATCHES);
    } else {
        char dir_matches[MAX_COMPLETION_MATCHES][DIR_CACHE_NAME_MAX];
        match_count = dir_cache_find_prefix_matches(word, dir_matches, MAX_COMPLETION_MATCHES);
        for (int i = 0; i < match_count; i++) matches[i] = dir_matches[i];
    }
    if (match_count == 0) return;

    char replacement[LINE_MAX_LEN];
    int replacement_len;

    if (match_count == 1) {
        replacement_len = strlen(matches[0]);
        memcpy(replacement, matches[0], replacement_len);
    } else {
        replacement_len = strlen(matches[0]);
        for (int i = 1; i < match_count; i++) {
            int j = 0;
            while (j < replacement_len && matches[i][j] == matches[0][j]) j++;
            replacement_len = j;
        }
        if (replacement_len <= word_len) return; // nothing new to add
        memcpy(replacement, matches[0], replacement_len);
    }

    int tail_len = *len - *pos;
    int new_len = word_start + replacement_len + tail_len;
    if (new_len >= LINE_MAX_LEN) return;

    memmove(&buf[word_start + replacement_len], &buf[*pos], tail_len);
    memcpy(&buf[word_start], replacement, replacement_len);
    *len = new_len;
    *pos = word_start + replacement_len;
}

// Reads one more byte of a multi-byte key sequence, giving up after `ms` so a
// bare Esc keypress doesn't block forever waiting for bytes that never come.
static int read_byte_timeout(int ms) {
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
    if (poll(&pfd, 1, ms) <= 0) return -1;
    unsigned char b;
    if (read(STDIN_FILENO, &b, 1) != 1) return -1;
    return b;
}

static int is_word_char(char c) {
    return !(c == ' ' || c == '\t');
}

// Start of the word at/before `pos` (skips any run of spaces first), so
// Alt+Left and Ctrl+Backspace agree on where a word begins.
static int word_start_before(const char *buf, int pos) {
    while (pos > 0 && !is_word_char(buf[pos - 1])) pos--;
    while (pos > 0 && is_word_char(buf[pos - 1])) pos--;
    return pos;
}

static int word_end_after(const char *buf, int len, int pos) {
    while (pos < len && !is_word_char(buf[pos])) pos++;
    while (pos < len && is_word_char(buf[pos])) pos++;
    return pos;
}

static void delete_range(char *buf, int *len, int *pos, int from, int to) {
    if (from >= to) return;
    memmove(&buf[from], &buf[to], *len - to);
    *len -= (to - from);
    *pos = from;
}

char* read_line(const char *prompt) {
    static char buf[LINE_MAX_LEN];
    static char saved_line[LINE_MAX_LEN];
    char suggestion[LINE_MAX_LEN];
    int len = 0;
    int pos = 0;
    int hist_index = history_count();

    suggestion[0] = '\0';
    terminal_enable_raw_mode();
    write(STDOUT_FILENO, prompt, strlen(prompt));

    while (1) {
        char c;
        if (read(STDIN_FILENO, &c, 1) != 1) continue;

        if (c == '\r' || c == '\n') {
            refresh_line(prompt, buf, len, len, ""); // drop any ghost suggestion before committing
            write(STDOUT_FILENO, "\r\n", 2);
            break;
        } else if (c == 4) { // Ctrl+D — quit on an empty line, delete forward otherwise
            if (len == 0) {
                write(STDOUT_FILENO, "\r\n", 2);
                terminal_disable_raw_mode();
                return NULL;
            }
            if (pos < len) delete_range(buf, &len, &pos, pos, pos + 1);
        } else if (c == 127) { // Backspace
            if (pos > 0) {
                memmove(&buf[pos - 1], &buf[pos], len - pos);
                pos--;
                len--;
            }
        } else if (c == 8 || c == 23) { // Ctrl+Backspace (^H) / Ctrl+W — delete word back
            delete_range(buf, &len, &pos, word_start_before(buf, pos), pos);
        } else if (c == 1) {  // Ctrl+A — start of line
            pos = 0;
        } else if (c == 5) {  // Ctrl+E — end of line
            pos = len;
        } else if (c == 21) { // Ctrl+U — kill to start of line
            delete_range(buf, &len, &pos, 0, pos);
        } else if (c == 11) { // Ctrl+K — kill to end of line
            len = pos;
        } else if (c == 9) { // Tab completion
            complete_word(buf, &len, &pos);
        } else if (c == 27) { // Esc — start of an escape/meta key sequence
            int b = read_byte_timeout(60);
            if (b < 0) continue; // a bare Esc press

            if (b == '[' || b == 'O') {
                // CSI: parameter bytes until a final byte in 0x40..0x7e.
                // Alt/Ctrl arrows arrive as e.g. ESC [ 1 ; 3 D, which the old
                // fixed 2-byte read mangled into literal text.
                char params[16];
                int plen = 0, final = -1;
                while (plen < (int)sizeof(params) - 1) {
                    int d = read_byte_timeout(60);
                    if (d < 0) break;
                    if (b == 'O' || (d >= 0x40 && d <= 0x7e)) { final = d; break; }
                    params[plen++] = (char)d;
                }
                params[plen] = '\0';
                if (final < 0) continue;

                // ";2" shift, ";3" alt, ";5" ctrl, ";7" ctrl+alt — any of the
                // word-wise modifiers turn arrows into word movement.
                const char *semi = strchr(params, ';');
                int mod = semi ? atoi(semi + 1) : 0;
                int wordwise = (mod == 3 || mod == 4 || mod == 5 || mod == 7);

                if (final == 'A') { // Up
                    if (hist_index > 0) {
                        if (hist_index == history_count()) {
                            memcpy(saved_line, buf, len);
                            saved_line[len] = '\0';
                        }
                        hist_index--;
                        const char *h = history_get_by_index(hist_index);
                        len = strlen(h);
                        memcpy(buf, h, len);
                        pos = len;
                    }
                } else if (final == 'B') { // Down
                    if (hist_index < history_count()) {
                        hist_index++;
                        const char *h = (hist_index == history_count()) ? saved_line : history_get_by_index(hist_index);
                        len = strlen(h);
                        memcpy(buf, h, len);
                        pos = len;
                    }
                } else if (final == 'C') { // Right / Alt+Right
                    if (wordwise) {
                        pos = word_end_after(buf, len, pos);
                    } else if (pos == len && suggestion[0] != '\0') {
                        int slen = strlen(suggestion);
                        if (len + slen < LINE_MAX_LEN) {
                            memcpy(&buf[len], suggestion, slen);
                            len += slen;
                            pos = len;
                        }
                    } else if (pos < len) {
                        pos++;
                    }
                } else if (final == 'D') { // Left / Alt+Left
                    if (wordwise) {
                        pos = word_start_before(buf, pos);
                    } else if (pos > 0) {
                        pos--;
                    }
                } else if (final == 'H') { // Home
                    pos = 0;
                } else if (final == 'F') { // End
                    pos = len;
                } else if (final == '~') {
                    int n = atoi(params);
                    if (n == 1 || n == 7) pos = 0;             // Home
                    else if (n == 4 || n == 8) pos = len;      // End
                    else if (n == 3) {                          // Delete / Ctrl+Delete
                        if (wordwise) delete_range(buf, &len, &pos, pos, word_end_after(buf, len, pos));
                        else if (pos < len) delete_range(buf, &len, &pos, pos, pos + 1);
                    } else continue;
                } else {
                    continue; // unrecognized sequence, nothing changed
                }
            } else if (b == 127 || b == 8) { // Alt+Backspace — delete word back
                delete_range(buf, &len, &pos, word_start_before(buf, pos), pos);
            } else if (b == 'b') { // Alt+B — word left (readline)
                pos = word_start_before(buf, pos);
            } else if (b == 'f') { // Alt+F — word right (readline)
                pos = word_end_after(buf, len, pos);
            } else if (b == 'd') { // Alt+D — delete word forward
                delete_range(buf, &len, &pos, pos, word_end_after(buf, len, pos));
            } else {
                continue;
            }
        } else if (c >= 32 && c < 127) { // Printable
            if (len < LINE_MAX_LEN - 1) {
                memmove(&buf[pos + 1], &buf[pos], len - pos);
                buf[pos] = c;
                len++;
                pos++;
            }
        } else {
            continue; // unhandled control character, nothing changed
        }

        buf[len] = '\0';
        compute_suggestion(buf, len, suggestion, sizeof(suggestion));
        refresh_line(prompt, buf, len, pos, suggestion);
    }

    terminal_disable_raw_mode();
    buf[len] = '\0';

    char *result = strdup(buf);
    history_add(result);
    return result;
}
