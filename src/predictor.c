#include "flassh/predictor.h"
#include "flassh/ssh_session.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CACHE_MAX 24
#define CACHE_TTL_MS 6000        // a listing older than this is re-fetched
#define PREFETCH_BUF (48 * 1024)
#define MAX_SUBDIR_PREFETCH 6
#define POLL_INTERVAL_US 300000

// Commands that only read state. Anything not on this list is never run
// speculatively — the cost of being wrong (executing something destructive
// the user never submitted) is far higher than a missed cache hit.
static const char *SAFE_COMMANDS[] = {
    "ls", "pwd", "whoami", "id", "date", "uptime", "hostname", "uname",
    "df", "free", "ps", "env", "printenv", "which", "type", "groups",
    "lsblk", "lscpu", "arch", "stat", "file", "wc", NULL
};

// `git` is only safe for its read-only subcommands.
static const char *SAFE_GIT_SUBCOMMANDS[] = {
    "status", "log", "branch", "diff", "show", "remote", "config", NULL
};

// Shell syntax that could redirect, chain or substitute — refuse outright
// rather than try to reason about what it would do.
static int has_shell_metachars(const char *s) {
    for (const char *p = s; *p; p++) {
        if (strchr(">|<;&`$(){}*?!\\", *p) != NULL) return 1;
    }
    return 0;
}

static void first_token(const char *s, char *out, size_t out_size) {
    while (*s == ' ' || *s == '\t') s++;
    size_t i = 0;
    while (s[i] && s[i] != ' ' && s[i] != '\t' && i < out_size - 1) {
        out[i] = s[i];
        i++;
    }
    out[i] = '\0';
}

int is_predictable_command(const char *command) {
    if (command == NULL || command[0] == '\0') return 0;
    if (has_shell_metachars(command)) return 0;

    char word[64];
    first_token(command, word, sizeof(word));
    if (word[0] == '\0') return 0;

    for (int i = 0; SAFE_COMMANDS[i]; i++) {
        if (strcmp(word, SAFE_COMMANDS[i]) == 0) return 1;
    }

    if (strcmp(word, "git") == 0) {
        const char *rest = strstr(command, "git");
        rest += 3;
        char sub[64];
        first_token(rest, sub, sizeof(sub));
        for (int i = 0; SAFE_GIT_SUBCOMMANDS[i]; i++) {
            if (strcmp(sub, SAFE_GIT_SUBCOMMANDS[i]) == 0) return 1;
        }
    }
    return 0;
}

typedef struct {
    char cwd[256];
    char cmd[256];
    char *out;
    int len;
    int exit_status;
    struct timespec at;
    int valid;
} CacheEntry;

static CacheEntry cache[CACHE_MAX];
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

// Work queued for the background thread by predictor_note_command().
static char pending_cwd[256];
static int pending_dirty = 0;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;

static long age_ms(const struct timespec *t0) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - t0->tv_sec) * 1000L + (now.tv_nsec - t0->tv_nsec) / 1000000L;
}

static void cache_store(const char *cwd, const char *cmd, const char *out, int len, int exit_status) {
    char *copy = malloc(len > 0 ? len : 1);
    if (copy == NULL) return;
    if (len > 0) memcpy(copy, out, len);

    pthread_mutex_lock(&cache_mutex);

    int slot = -1;
    for (int i = 0; i < CACHE_MAX; i++) { // replace same key first
        if (cache[i].valid && strcmp(cache[i].cwd, cwd) == 0 && strcmp(cache[i].cmd, cmd) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < CACHE_MAX; i++) if (!cache[i].valid) { slot = i; break; }
    }
    if (slot < 0) { // evict the oldest
        long oldest = -1;
        slot = 0;
        for (int i = 0; i < CACHE_MAX; i++) {
            long a = age_ms(&cache[i].at);
            if (a > oldest) { oldest = a; slot = i; }
        }
    }

    free(cache[slot].out);
    snprintf(cache[slot].cwd, sizeof(cache[slot].cwd), "%s", cwd);
    snprintf(cache[slot].cmd, sizeof(cache[slot].cmd), "%s", cmd);
    cache[slot].out = copy;
    cache[slot].len = len;
    cache[slot].exit_status = exit_status;
    clock_gettime(CLOCK_MONOTONIC, &cache[slot].at);
    cache[slot].valid = 1;

    pthread_mutex_unlock(&cache_mutex);
}

int predictor_take_cached(const char *cwd, const char *command,
                          char **out, int *len, int *exit_status) {
    int hit = 0;
    pthread_mutex_lock(&cache_mutex);
    for (int i = 0; i < CACHE_MAX; i++) {
        if (!cache[i].valid) continue;
        if (strcmp(cache[i].cwd, cwd) != 0 || strcmp(cache[i].cmd, command) != 0) continue;
        if (age_ms(&cache[i].at) > CACHE_TTL_MS) break; // too old to trust

        char *copy = malloc(cache[i].len > 0 ? cache[i].len : 1);
        if (copy != NULL) {
            if (cache[i].len > 0) memcpy(copy, cache[i].out, cache[i].len);
            *out = copy;
            *len = cache[i].len;
            *exit_status = cache[i].exit_status;
            hit = 1;
        }
        break;
    }
    pthread_mutex_unlock(&cache_mutex);
    return hit;
}

int predictor_cache_count(void) {
    int n = 0;
    pthread_mutex_lock(&cache_mutex);
    for (int i = 0; i < CACHE_MAX; i++) if (cache[i].valid) n++;
    pthread_mutex_unlock(&cache_mutex);
    return n;
}

void predictor_note_command(const char *cwd, const char *command) {
    (void)command;
    pthread_mutex_lock(&queue_mutex);
    snprintf(pending_cwd, sizeof(pending_cwd), "%s", cwd);
    pending_dirty = 1;
    pthread_mutex_unlock(&queue_mutex);
}

// Runs `command` in `dir` and caches the result. Uses the same expansion the
// interactive path uses, so a cache hit is byte-identical to what the user
// would have seen had they waited for the round trip.
static void prefetch(ssh_session session, const char *dir, const char *command, char *scratch) {
    if (!is_predictable_command(command)) return;

    char expanded[600];
    expand_command_for_exec(session, command, expanded, sizeof(expanded));

    char wrapped[1200];
    snprintf(wrapped, sizeof(wrapped), "(cd %s 2>/dev/null || exit 1; %s) 2>&1", dir, expanded);

    int nbytes = 0, status = 0;
    if (exec_(session, wrapped, &nbytes, scratch, PREFETCH_BUF, &status) == SSH_OK) {
        cache_store(dir, command, scratch, nbytes, status);
    }
}

// Immediate subdirectories of `dir`, so that `cd <sub>` followed by `ls` is
// already answered before the user types it.
static int list_subdirs(ssh_session session, const char *dir, char subs[][256], int max, char *scratch) {
    char cmd[600];
    snprintf(cmd, sizeof(cmd),
             "cd %s 2>/dev/null && ls -d */ 2>/dev/null | head -n %d", dir, max);

    int nbytes = 0;
    if (exec_(session, cmd, &nbytes, scratch, PREFETCH_BUF, NULL) != SSH_OK) return 0;

    int count = 0, start = 0;
    for (int i = 0; i <= nbytes && count < max; i++) {
        if (i == nbytes || scratch[i] == '\n') {
            int len = i - start;
            while (len > 0 && scratch[start + len - 1] == '/') len--; // trailing slash
            if (len > 0 && len < 255) {
                memcpy(subs[count], scratch + start, len);
                subs[count][len] = '\0';
                count++;
            }
            start = i + 1;
        }
    }
    return count;
}

static void *predictor_thread(void *arg) {
    ssh_session session = (ssh_session)arg;
    char *scratch = malloc(PREFETCH_BUF);
    if (scratch == NULL) return NULL;

    char last_cwd[256] = "";

    while (1) {
        char cwd[256];
        get_current_dir(cwd, sizeof(cwd));

        int changed = (strcmp(cwd, last_cwd) != 0);

        int dirty = 0;
        pthread_mutex_lock(&queue_mutex);
        dirty = pending_dirty;
        pending_dirty = 0;
        pthread_mutex_unlock(&queue_mutex);

        if (cwd[0] != '\0' && (changed || dirty)) {
            snprintf(last_cwd, sizeof(last_cwd), "%s", cwd);

            // The listing of where we are is by far the most likely next
            // command, so refresh it first.
            prefetch(session, cwd, "ls", scratch);

            // Then pre-warm the directories the user could cd into next, so
            // "cd sub" + "ls" is already answered.
            char subs[MAX_SUBDIR_PREFETCH][256];
            int n = list_subdirs(session, cwd, subs, MAX_SUBDIR_PREFETCH, scratch);
            for (int i = 0; i < n; i++) {
                char child[512];
                if (cwd[strlen(cwd) - 1] == '/') snprintf(child, sizeof(child), "%s%s", cwd, subs[i]);
                else snprintf(child, sizeof(child), "%s/%s", cwd, subs[i]);
                prefetch(session, child, "ls", scratch);
            }
        }

        usleep(POLL_INTERVAL_US);
    }

    free(scratch);
    return NULL;
}

void predictor_start(ssh_session session) {
    pthread_t tid;
    if (pthread_create(&tid, NULL, predictor_thread, (void *)session) == 0) {
        pthread_detach(tid);
    }
}
