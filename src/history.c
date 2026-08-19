#include "flassh/history.h"
#include "flassh/ssh_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HISTORY_MAX 512
#define HISTORY_DOWNLOAD_BUF (64 * 1024)

static char *entries[HISTORY_MAX];
static int entry_count = 0;

static void history_push(const char *line, int line_len) {
    if (line_len <= 0) return;

    char *copy = malloc(line_len + 1);
    if (copy == NULL) return;
    memcpy(copy, line, line_len);
    copy[line_len] = '\0';

    if (entry_count == HISTORY_MAX) {
        free(entries[0]);
        memmove(&entries[0], &entries[1], sizeof(char*) * (HISTORY_MAX - 1));
        entry_count--;
    }
    entries[entry_count++] = copy;
}

// zsh's EXTENDED_HISTORY option prefixes each line with ": <epoch>:<elapsed>;"
// before the actual command. Strips that prefix if present; plain
// bash-history lines (and non-extended zsh history) are left untouched.
static void strip_zsh_extended_prefix(const char **line, int *line_len) {
    const char *p = *line;
    int len = *line_len;
    if (len < 2 || p[0] != ':' || p[1] != ' ') return;

    int i = 2;
    while (i < len && p[i] >= '0' && p[i] <= '9') i++;
    if (i == 2 || i >= len || p[i] != ':') return;
    i++;
    int j = i;
    while (j < len && p[j] >= '0' && p[j] <= '9') j++;
    if (j == i || j >= len || p[j] != ';') return;
    j++;

    *line = p + j;
    *line_len = len - j;
}

static void load_history_file(ssh_session session, char *command) {
    int nbytes = 0;
    char *output = malloc(HISTORY_DOWNLOAD_BUF);
    if (output == NULL) return;

    int rc = exec_(session, command, &nbytes, output, HISTORY_DOWNLOAD_BUF, NULL);
    if (rc == SSH_OK) {
        int line_start = 0;
        for (int i = 0; i <= nbytes; i++) {
            if (i == nbytes || output[i] == '\n') {
                const char *line = &output[line_start];
                int line_len = i - line_start;
                strip_zsh_extended_prefix(&line, &line_len);
                history_push(line, line_len);
                line_start = i + 1;
            }
        }
    }

    free(output);
}

void history_load(ssh_session session) {
    load_history_file(session, "cat ~/.bash_history 2>/dev/null");
    load_history_file(session, "cat ~/.zsh_history 2>/dev/null");
}

void history_add(const char *command) {
    if (command == NULL || command[0] == '\0') return;
    if (entry_count > 0 && strcmp(entries[entry_count - 1], command) == 0) return;
    history_push(command, strlen(command));
}

int history_count(void) {
    return entry_count;
}

const char* history_get_by_index(int index) {
    if (index < 0 || index >= entry_count) return "";
    return entries[index];
}

int history_find_prefix_matches(const char *prefix, const char **matches, int max_matches) {
    size_t plen = strlen(prefix);
    if (plen == 0) return 0;

    int n = 0;
    for (int i = entry_count - 1; i >= 0 && n < max_matches; i--) {
        if (strncmp(entries[i], prefix, plen) == 0) {
            matches[n++] = entries[i];
        }
    }
    return n;
}
