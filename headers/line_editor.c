#include "line_editor.h"
#include "history.h"
#include "dir_cache.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>

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
        } else if (c == 127 || c == 8) { // Backspace
            if (pos > 0) {
                memmove(&buf[pos - 1], &buf[pos], len - pos);
                pos--;
                len--;
            }
        } else if (c == 9) { // Tab completion
            complete_word(buf, &len, &pos);
        } else if (c == 27) { // ESC — arrow keys arrive as ESC [ <letter>
            char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) != 1) continue;
            if (read(STDIN_FILENO, &seq[1], 1) != 1) continue;
            if (seq[0] != '[') continue;

            if (seq[1] == 'A') { // Up
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
            } else if (seq[1] == 'B') { // Down
                if (hist_index < history_count()) {
                    hist_index++;
                    const char *h = (hist_index == history_count()) ? saved_line : history_get_by_index(hist_index);
                    len = strlen(h);
                    memcpy(buf, h, len);
                    pos = len;
                }
            } else if (seq[1] == 'C') { // Right — accepts the ghost suggestion at end-of-line
                if (pos == len && suggestion[0] != '\0') {
                    int slen = strlen(suggestion);
                    if (len + slen < LINE_MAX_LEN) {
                        memcpy(&buf[len], suggestion, slen);
                        len += slen;
                        pos = len;
                    }
                } else if (pos < len) {
                    pos++;
                }
            } else if (seq[1] == 'D') { // Left
                if (pos > 0) {
                    pos--;
                }
            } else {
                continue; // unrecognized escape sequence, nothing changed
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
