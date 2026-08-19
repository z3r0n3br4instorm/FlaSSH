#include "flassh/ssh_session.h"
#include <libssh/libssh.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include "flassh/ssh_connection.h"

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


char* exec_command(ssh_session session, char *command, int visibility) {
    int nbytes;
    char output[4048];
    char full_cmd[1400];
    int exit_status = 0;

    if (! strcmp(command, "clear")) { // Placeholder until i figure this out
        system("clear");
        return SSH_OK;
    }

    // Without a PTY, remote `ls` sees a non-tty stdout and falls back to
    // one entry per line. Force column formatting like a real terminal
    // would get, inserted right after "ls" so any flags the user typed
    // still take effect afterward.
    char adjusted_command[600];
    if (command[0] == 'l' && command[1] == 's' && (command[2] == '\0' || command[2] == ' ')) {
        snprintf(adjusted_command, sizeof(adjusted_command), "ls -C%s", command + 2);
        command = adjusted_command;
    }

    // `;` instead of `&&` between every step so pwd (and the exit-code
    // marker) always run even if the command fails — previously a failing
    // command short-circuited past `&& pwd`, which silently reset the
    // tracked directory to empty (-> $HOME) on any error. The trailing
    // `2>&1` on the whole subshell merges stderr into the stream we
    // actually read back (exec_() only reads the channel's stdout stream,
    // so without this, error output was just discarded).
    static const char *EXIT_MARKER = "==FLASHSSH_EXIT==";
    pthread_mutex_lock(&workdir_mutex);
    snprintf(full_cmd, sizeof(full_cmd), "(cd %s; %s; echo \"%s$?\"; pwd) 2>&1",
             work_dir, command, EXIT_MARKER);
    pthread_mutex_unlock(&workdir_mutex);

    pthread_t spinner_tid;
    int show_spinner = (visibility != 1);
    if (show_spinner) {
        spinner_running = 1;
        pthread_create(&spinner_tid, NULL, spinner_thread_fn, NULL);
    }

    exec_(session, full_cmd, &nbytes, output, sizeof(output), &exit_status);

    if (show_spinner) {
        spinner_running = 0;
        pthread_join(spinner_tid, NULL);
    }

    // Find the marker to split "visible output" from the exit code + new pwd
    // that follow it. Falls back to showing everything if it's ever missing
    // (shouldn't happen, but better than swallowing output silently).
    int marker_len = (int)strlen(EXIT_MARKER);
    int marker_pos = -1;
    for (int i = 0; i + marker_len <= nbytes; i++) {
        if (memcmp(&output[i], EXIT_MARKER, marker_len) == 0) {
            marker_pos = i;
            break;
        }
    }

    int visible_len = (marker_pos >= 0) ? marker_pos : nbytes;

    if (marker_pos >= 0) {
        last_exit_status = atoi(&output[marker_pos + marker_len]);

        int p = marker_pos + marker_len;
        while (p < nbytes && output[p] != '\n') p++;
        p++; // skip the newline after the exit code

        int end = nbytes;
        while (end > p && (output[end - 1] == '\n' || output[end - 1] == '\r')) end--;

        char new_dir[256];
        memset(new_dir, '\0', sizeof(new_dir));
        int len = end - p;
        if (len > (int)sizeof(new_dir) - 1) len = (int)sizeof(new_dir) - 1;
        if (len > 0) memcpy(new_dir, &output[p], len);

        pthread_mutex_lock(&workdir_mutex);
        memcpy(work_dir, new_dir, sizeof(new_dir));
        pthread_mutex_unlock(&workdir_mutex);
    } else {
        last_exit_status = exit_status;
    }

    // A non-zero exit plus a "needs a terminal" message means this command
    // simply can't work through a plain exec channel. Flag it (and swallow
    // the error text) so the caller can re-run it under a real PTY instead.
    tty_required = (last_exit_status != 0) && output_demands_tty(output, visible_len);

    if (visibility != 1 && !tty_required) {
        fwrite(output, 1, visible_len, stdout);
    }

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
