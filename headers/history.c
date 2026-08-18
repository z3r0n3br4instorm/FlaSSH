#include "history.h"
#include "ssh_session.h"
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

void history_load(ssh_session session) {
    int nbytes = 0;
    char *output = malloc(HISTORY_DOWNLOAD_BUF);
    if (output == NULL) return;

    int rc = exec_(session, "cat ~/.bash_history 2>/dev/null", &nbytes, output, HISTORY_DOWNLOAD_BUF, NULL);
    if (rc == SSH_OK) {
        int line_start = 0;
        for (int i = 0; i < nbytes; i++) {
            if (output[i] == '\n') {
                history_push(&output[line_start], i - line_start);
                line_start = i + 1;
            }
        }
        if (line_start < nbytes) {
            history_push(&output[line_start], nbytes - line_start);
        }
    }

    free(output);
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
