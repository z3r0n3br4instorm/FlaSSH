#include "dir_cache.h"
#include "ssh_session.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DIR_CACHE_MAX_ENTRIES 1024
#define DIR_CACHE_POLL_SECONDS 1
#define DIR_CACHE_BUF_SIZE (64 * 1024)

static char *entries[DIR_CACHE_MAX_ENTRIES];
static int entry_count = 0;
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static int is_dot_or_dotdot(const char *s, int len) {
    return (len == 1 && s[0] == '.') || (len == 2 && s[0] == '.' && s[1] == '.');
}

static void replace_entries(const char *output, int nbytes) {
    char *new_entries[DIR_CACHE_MAX_ENTRIES];
    int new_count = 0;

    int line_start = 0;
    for (int i = 0; i <= nbytes && new_count < DIR_CACHE_MAX_ENTRIES; i++) {
        if (i == nbytes || output[i] == '\n') {
            int line_len = i - line_start;
            if (line_len > 0 && !is_dot_or_dotdot(&output[line_start], line_len)) {
                char *copy = malloc(line_len + 1);
                if (copy != NULL) {
                    memcpy(copy, &output[line_start], line_len);
                    copy[line_len] = '\0';
                    new_entries[new_count++] = copy;
                }
            }
            line_start = i + 1;
        }
    }

    pthread_mutex_lock(&cache_mutex);
    for (int i = 0; i < entry_count; i++) free(entries[i]);
    for (int i = 0; i < new_count; i++) entries[i] = new_entries[i];
    entry_count = new_count;
    pthread_mutex_unlock(&cache_mutex);
}

static void *poll_loop(void *arg) {
    ssh_session session = (ssh_session)arg;
    char *output = malloc(DIR_CACHE_BUF_SIZE);
    if (output == NULL) return NULL;

    while (1) {
        char dir[256];
        get_current_dir(dir, sizeof(dir));

        char cmd[320];
        snprintf(cmd, sizeof(cmd), "cd %s && ls -a", dir);

        int nbytes = 0;
        int rc = exec_(session, cmd, &nbytes, output, DIR_CACHE_BUF_SIZE, NULL);
        if (rc == SSH_OK) {
            replace_entries(output, nbytes);
        }

        sleep(DIR_CACHE_POLL_SECONDS);
    }

    free(output);
    return NULL;
}

void dir_cache_start(ssh_session session) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, poll_loop, (void *)session) == 0) {
        pthread_detach(thread);
    }
}

int dir_cache_find_prefix_matches(const char *prefix, char matches[][DIR_CACHE_NAME_MAX], int max_matches) {
    size_t plen = strlen(prefix);
    if (plen == 0) return 0;

    pthread_mutex_lock(&cache_mutex);
    int n = 0;
    for (int i = 0; i < entry_count && n < max_matches; i++) {
        if (strncmp(entries[i], prefix, plen) == 0) {
            snprintf(matches[n], DIR_CACHE_NAME_MAX, "%s", entries[i]);
            n++;
        }
    }
    pthread_mutex_unlock(&cache_mutex);
    return n;
}
