#include "flassh/ssh_session.h"
#include <libssh/libssh.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include "flassh/ssh_connection.h"
#include "flassh/predictor.h"

// Session VARs
char work_dir[256];
static pthread_mutex_t workdir_mutex = PTHREAD_MUTEX_INITIALIZER;

// libssh sessions aren't safe to drive from multiple threads at once; the
// background directory-cache thread and the interactive loop both funnel
// through exec_(), so serialize them here.
static pthread_mutex_t session_mutex = PTHREAD_MUTEX_INITIALIZER;

static int last_exit_status = 0;
static int tty_required = 0;

// Programs that insist on a real terminal say so in a handful of recognisable
// ways. Matching the message is far more general than trying to keep a list
// of every such program up to date.
static const char *TTY_ERROR_PHRASES[] = {
    "not a terminal",
    "not a tty",
    "no tty",
    "requires a terminal",
    "must be run in a terminal",
    "must be run from a terminal",
    "terminal required",
    "inappropriate ioctl for device",
    "device not configured",
    NULL
};

static int output_demands_tty(const char *buf, int len) {
    // Lowercase a bounded copy so the match is case-insensitive without
    // touching the output we're about to hand back to the caller.
    char lower[1024];
    int n = (len < (int)sizeof(lower) - 1) ? len : (int)sizeof(lower) - 1;
    for (int i = 0; i < n; i++) {
        char c = buf[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
    }
    lower[n] = '\0';

    for (int i = 0; TTY_ERROR_PHRASES[i] != NULL; i++) {
        if (strstr(lower, TTY_ERROR_PHRASES[i]) != NULL) return 1;
    }
    return 0;
}

// Spinner shown on stderr while exec_() blocks on the network round trip.
static volatile int spinner_running = 0;

static void *spinner_thread_fn(void *arg) {
    (void)arg;
    static const char *frames[] = {"⣾", "⣽", "⣻", "⢿", "⡿", "⣟", "⣯", "⣷"};
    int i = 0;
    while (spinner_running) {
        fprintf(stderr, "\r%s", frames[i % 8]);
        fflush(stderr);
        i++;
        usleep(80000);
    }
    fprintf(stderr, "\r\033[K");
    fflush(stderr);
    return NULL;
}

int exec_(ssh_session session, char* command, int *nbytes, char *output, size_t output_size, int *exit_status)
{
  ssh_channel channel = NULL;
  int rc;
  char buffer[256];

  pthread_mutex_lock(&session_mutex);

  channel = ssh_channel_new(session);
  if (channel == NULL) {
    pthread_mutex_unlock(&session_mutex);
    return SSH_ERROR;
  }

  rc = ssh_channel_open_session(channel);
  if (rc != SSH_OK)
  {
    ssh_channel_free(channel);
    pthread_mutex_unlock(&session_mutex);
    return rc;
  }

  rc = ssh_channel_request_exec(channel, command);
  if (rc != SSH_OK)
  {
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    pthread_mutex_unlock(&session_mutex);
    return rc;
  }

  int total = 0;
  int n = ssh_channel_read(channel, buffer, sizeof(buffer), 0);

  while (n > 0)
  {
      if ((size_t)(total + n) > output_size - 1) {
          ssh_channel_close(channel);
          ssh_channel_free(channel);
          pthread_mutex_unlock(&session_mutex);
          return SSH_ERROR;
      }
      for (int i = 0; i < n; i++) {
          output[total + i] = buffer[i];
      }
      total += n;
      n = ssh_channel_read(channel, buffer, sizeof(buffer), 0);
  }
  output[total] = '\0';
  *nbytes = total;

  if (n < 0)
  {
      ssh_channel_close(channel);
      ssh_channel_free(channel);
      pthread_mutex_unlock(&session_mutex);
      return SSH_ERROR;
  }

  ssh_channel_send_eof(channel);
  if (exit_status != NULL) {
      // ssh_channel_get_exit_state() would avoid the deprecation warning
      // here, but it only exists in libssh >= 0.10 — this older call works
      // on every libssh we've actually needed to build against.
      *exit_status = ssh_channel_get_exit_status(channel);
  }
  ssh_channel_close(channel);
  ssh_channel_free(channel);

  pthread_mutex_unlock(&session_mutex);
  return SSH_OK;
}


// Puts the terminal into a mode where Ctrl+C arrives as a byte we can act on
// instead of a signal (SIGINT is ignored process-wide so it can never kill
// the client). OPOST is deliberately left enabled, unlike the line editor's
// raw mode, so the remote's bare "\n" still expands to CRLF and streamed
// output doesn't staircase down the screen.
static struct termios exec_saved_termios;
static int exec_termios_active = 0;

static void exec_enter_interruptible_mode(void) {
    if (tcgetattr(STDIN_FILENO, &exec_saved_termios) != 0) return;
    struct termios raw = exec_saved_termios;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    exec_termios_active = 1;
}

static void exec_leave_interruptible_mode(void) {
    if (!exec_termios_active) return;
    tcsetattr(STDIN_FILENO, TCSANOW, &exec_saved_termios);
    exec_termios_active = 0;
}

// GNU coreutils and BSD/macOS spell "colorize even though stdout is not a
// terminal" differently, so ask the remote once which one it is.
static const char *ls_color_flag = NULL;

static void detect_ls_color_flag(ssh_session session) {
    if (ls_color_flag != NULL) return;
    ls_color_flag = "--color=always"; // GNU default if the probe fails

    int nbytes = 0;
    char out[128];
    if (exec_(session, "uname -s", &nbytes, out, sizeof(out), NULL) == SSH_OK) {
        if (strstr(out, "Darwin") != NULL || strstr(out, "BSD") != NULL) {
            ls_color_flag = "-G";
        }
    }
}

// Without a PTY the remote `ls` sees a non-tty stdout: it drops to one entry
// per line and turns colour off. -C restores columns.
//
// Colour needs more than --color=always. A non-interactive SSH exec never
// sources the shell rc files that define LS_COLORS, and GNU ls with an empty
// LS_COLORS emits no colour at all even when told "always" (measured: 249
// bytes uncoloured vs 548 coloured for the same listing). So the palette is
// supplied inline. BSD/macOS ls uses -G plus CLICOLOR_FORCE instead.
#define FLASSH_LS_COLORS \
    "di=01;34:ln=01;36:so=01;35:pi=33:ex=01;32:bd=33;01:cd=33;01:or=31;01:" \
    "*.tar=01;31:*.tgz=01;31:*.zip=01;31:*.gz=01;31:*.bz2=01;31:*.xz=01;31:" \
    "*.jpg=01;35:*.jpeg=01;35:*.png=01;35:*.gif=01;35:*.svg=01;35:*.mp4=01;35"

void expand_command_for_exec(ssh_session session, const char *in, char *out, size_t out_size) {
    if (in[0] == 'l' && in[1] == 's' && (in[2] == '\0' || in[2] == ' ')) {
        detect_ls_color_flag(session);
        if (strcmp(ls_color_flag, "-G") == 0) {
            snprintf(out, out_size, "CLICOLOR_FORCE=1 ls -C -G%s", in + 2);
        } else {
            snprintf(out, out_size, "LS_COLORS='%s' ls -C --color=always%s",
                     FLASSH_LS_COLORS, in + 2);
        }
    } else {
        snprintf(out, out_size, "%s", in);
    }
}

static long ms_since(const struct timespec *t0) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - t0->tv_sec) * 1000L + (now.tv_nsec - t0->tv_nsec) / 1000000L;
}

char* exec_command(ssh_session session, char *command, int visibility) {
    char full_cmd[1400];

    if (! strcmp(command, "clear")) { // Placeholder until i figure this out
        system("clear");
        return SSH_OK;
    }

    // A speculatively-run, still-fresh result means the answer is already
    // here and no round trip is needed at all.
    if (visibility != 1) {
        char cwd_now[256];
        get_current_dir(cwd_now, sizeof(cwd_now));

        char *cached = NULL;
        int cached_len = 0, cached_status = 0;
        if (predictor_take_cached(cwd_now, command, &cached, &cached_len, &cached_status)) {
            fwrite(cached, 1, cached_len, stdout);
            fflush(stdout);
            free(cached);
            last_exit_status = cached_status;
            tty_required = 0;
            predictor_note_command(cwd_now, command);
            return SSH_OK;
        }
    }

    char adjusted_command[600];
    expand_command_for_exec(session, command, adjusted_command, sizeof(adjusted_command));
    command = adjusted_command;

    // `;` instead of `&&` between every step so pwd (and the exit-code
    // marker) always run even if the command fails — previously a failing
    // command short-circuited past `&& pwd`, which silently reset the
    // tracked directory to empty (-> $HOME) on any error. The trailing
    // `2>&1` on the whole subshell merges stderr into the stream we
    // actually read back (exec_() only reads the channel's stdout stream,
    // so without this, error output was just discarded).
    static const char *EXIT_MARKER = "==FLASSH_EXIT==";
    const int marker_len = (int)strlen(EXIT_MARKER);

    pthread_mutex_lock(&workdir_mutex);
    snprintf(full_cmd, sizeof(full_cmd), "(cd %s; %s; echo \"%s$?\"; pwd) 2>&1",
             work_dir, command, EXIT_MARKER);
    pthread_mutex_unlock(&workdir_mutex);

    // If the link dropped since the last command, restore it (red bar +
    // TUI password prompt if needed) before doing anything else.
    if (!ssh_is_connected(session)) {
        ssh_session_lock();
        int ok = session_ensure_connected(session);
        ssh_session_unlock();
        if (!ok) { last_exit_status = 255; return NULL; }
    }

    ssh_session_lock();

    ssh_channel channel = ssh_channel_new(session);
    if (channel == NULL) { ssh_session_unlock(); return NULL; }
    if (ssh_channel_open_session(channel) != SSH_OK) {
        ssh_channel_free(channel);
        ssh_session_unlock();
        return NULL;
    }
    if (ssh_channel_request_exec(channel, full_cmd) != SSH_OK) {
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        ssh_session_unlock();
        return NULL;
    }

    int visible = (visibility != 1);
    if (visible) exec_enter_interruptible_mode();

    // Output is streamed as it arrives rather than collected first, so
    // never-ending commands (journalctl -f, tail -f, ping) print live
    // instead of hanging behind a spinner that never stops.
    char chunk[4096];
    char pending[8192];  int pending_len = 0;   // holds a possibly-split marker
    char head[1024];     int head_len = 0;      // withheld briefly for TTY sniffing
    char tail[1024];     int tail_len = 0;      // exit code + pwd, after the marker
    int marker_seen = 0, head_flushed = 0, interrupted = 0, got_any = 0;

    struct timespec started;
    clock_gettime(CLOCK_MONOTONIC, &started);

    pthread_t spinner_tid;
    int spinner_on = 0;
    if (visible) { spinner_running = 1; spinner_on = 1; pthread_create(&spinner_tid, NULL, spinner_thread_fn, NULL); }

    #define FLUSH_VISIBLE(ptr, n) do { \
        if (visible && (n) > 0) { fwrite((ptr), 1, (n), stdout); fflush(stdout); } \
    } while (0)

    while (1) {
        if (visible) {
            char key;
            if (read(STDIN_FILENO, &key, 1) == 1 && key == 3) { // Ctrl+C
                ssh_channel_request_send_signal(channel, "INT");
                interrupted = 1;
                break;
            }
        }

        // Negative covers both SSH_ERROR (-1) and SSH_EOF (-127); treating
        // SSH_EOF as a byte count spins forever and memcpys a negative size.
        int n = ssh_channel_read_nonblocking(channel, chunk, sizeof(chunk), 0);
        if (n < 0) break;
        if (n == 0) {
            if (ssh_channel_is_eof(channel)) break;
            // Nothing yet: release the head once it is clear no more is coming
            // right away, so a slow first line isn't held hostage by the sniff.
            if (!head_flushed && head_len > 0 && ms_since(&started) > 200) {
                if (spinner_on) { spinner_running = 0; pthread_join(spinner_tid, NULL); spinner_on = 0; }
                FLUSH_VISIBLE(head, head_len);
                head_len = 0; head_flushed = 1;
            }
            usleep(15000);
            continue;
        }

        if (!got_any) {
            got_any = 1;
            if (spinner_on) { spinner_running = 0; pthread_join(spinner_tid, NULL); spinner_on = 0; }
        }

        int off = 0;
        // The first few hundred bytes are withheld so a "needs a terminal"
        // failure can be swallowed before it reaches the screen (main.c
        // retries those under a PTY). Anything longer or slower streams.
        if (!head_flushed) {
            int room = (int)sizeof(head) - head_len;
            int take = (n < room) ? n : room;
            memcpy(head + head_len, chunk, take);
            head_len += take;
            off = take;
            if (head_len >= 512 || ms_since(&started) > 200) {
                FLUSH_VISIBLE(head, head_len);
                // Keep the head bytes in `pending` so a marker straddling the
                // head/stream boundary is still found.
                if (head_len <= (int)sizeof(pending)) { memcpy(pending, head, head_len); pending_len = head_len; }
                head_len = 0; head_flushed = 1;
            }
            if (off >= n) continue;
        }

        if (marker_seen) {
            int room = (int)sizeof(tail) - tail_len;
            int take = (n - off < room) ? n - off : room;
            memcpy(tail + tail_len, chunk + off, take);
            tail_len += take;
            continue;
        }

        int room = (int)sizeof(pending) - pending_len;
        int take = (n - off < room) ? n - off : room;
        memcpy(pending + pending_len, chunk + off, take);
        pending_len += take;

        int found = -1;
        for (int i = 0; i + marker_len <= pending_len; i++) {
            if (memcmp(pending + i, EXIT_MARKER, marker_len) == 0) { found = i; break; }
        }

        if (found >= 0) {
            if (head_flushed) FLUSH_VISIBLE(pending, found);
            marker_seen = 1;
            int rest = pending_len - (found + marker_len);
            if (rest > (int)sizeof(tail)) rest = (int)sizeof(tail);
            memcpy(tail, pending + found + marker_len, rest);
            tail_len = rest;
            pending_len = 0;
        } else {
            // Hold back only the trailing bytes that could still turn out to
            // be the start of the marker — usually none, so output streams
            // live rather than lagging a fixed number of bytes behind.
            int keep = 0;
            int maxk = (marker_len - 1 < pending_len) ? marker_len - 1 : pending_len;
            for (int k = maxk; k > 0; k--) {
                if (memcmp(pending + pending_len - k, EXIT_MARKER, k) == 0) { keep = k; break; }
            }
            int flush = pending_len - keep;
            if (head_flushed) FLUSH_VISIBLE(pending, flush);
            memmove(pending, pending + flush, keep);
            pending_len = keep;
        }
    }

    if (spinner_on) { spinner_running = 0; pthread_join(spinner_tid, NULL); }

    // Whatever never made it out (short command, or no marker at all).
    if (!head_flushed && head_len > 0) {
        tty_required = output_demands_tty(head, head_len);
        // A marker inside the withheld head still has to be split off.
        int found = -1;
        for (int i = 0; i + marker_len <= head_len; i++) {
            if (memcmp(head + i, EXIT_MARKER, marker_len) == 0) { found = i; break; }
        }
        if (found >= 0) {
            if (!tty_required) FLUSH_VISIBLE(head, found);
            marker_seen = 1;
            int rest = head_len - (found + marker_len);
            if (rest > (int)sizeof(tail)) rest = (int)sizeof(tail);
            memcpy(tail, head + found + marker_len, rest);
            tail_len = rest;
        } else if (!tty_required) {
            FLUSH_VISIBLE(head, head_len);
        }
    } else {
        tty_required = 0;
        if (pending_len > 0 && !marker_seen) FLUSH_VISIBLE(pending, pending_len);
    }

    if (interrupted) {
        if (visible) { fputs("^C\n", stdout); fflush(stdout); }
        last_exit_status = 130; // 128 + SIGINT, as a shell would report
    } else if (marker_seen && tail_len > 0) {
        last_exit_status = atoi(tail);

        int p = 0;
        while (p < tail_len && tail[p] != '\n') p++;
        p++; // step over the newline after the exit code
        int end = tail_len;
        while (end > p && (tail[end - 1] == '\n' || tail[end - 1] == '\r')) end--;

        if (end > p) {
            char new_dir[256];
            memset(new_dir, '\0', sizeof(new_dir));
            int len = end - p;
            if (len > (int)sizeof(new_dir) - 1) len = (int)sizeof(new_dir) - 1;
            memcpy(new_dir, tail + p, len);

            pthread_mutex_lock(&workdir_mutex);
            memcpy(work_dir, new_dir, sizeof(new_dir));
            pthread_mutex_unlock(&workdir_mutex);
        }
    }

    tty_required = tty_required && (last_exit_status != 0);

    #undef FLUSH_VISIBLE

    if (visible) exec_leave_interruptible_mode();

    ssh_channel_send_eof(channel);
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    ssh_session_unlock();

    return SSH_OK;
}

int last_command_needed_tty(void) {
    return tty_required;
}


char* get_workDir(ssh_session session) {
    exec_command(session, "pwd", 1);
    return work_dir;
}

void get_current_dir(char *out, size_t out_size) {
    pthread_mutex_lock(&workdir_mutex);
    snprintf(out, out_size, "%s", work_dir);
    pthread_mutex_unlock(&workdir_mutex);
}

int get_last_exit_status(void) {
    return last_exit_status;
}

void ssh_session_lock(void) {
    pthread_mutex_lock(&session_mutex);
}

void ssh_session_unlock(void) {
    pthread_mutex_unlock(&session_mutex);
}
