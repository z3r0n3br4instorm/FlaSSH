#include "ssh_session.h"
#include <libssh/libssh.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include "ssh_connection.h"

// Session VARs
char work_dir[256];
static pthread_mutex_t workdir_mutex = PTHREAD_MUTEX_INITIALIZER;

// libssh sessions aren't safe to drive from multiple threads at once; the
// background directory-cache thread and the interactive loop both funnel
// through exec_(), so serialize them here.
static pthread_mutex_t session_mutex = PTHREAD_MUTEX_INITIALIZER;

static int last_exit_status = 0;

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
      uint32_t exit_code = 0;
      ssh_channel_get_exit_state(channel, &exit_code, NULL, NULL);
      *exit_status = (int)exit_code;
  }
  ssh_channel_close(channel);
  ssh_channel_free(channel);

  pthread_mutex_unlock(&session_mutex);
  return SSH_OK;
}


char* exec_command(ssh_session session, char *command, int visibility) {
    int nbytes;
    char output[4048];
    char full_cmd[512];
    int previous_line_break = -1;
    int line_breaks_position = -1;
    int exit_status = 0;

    if (! strcmp(command, "clear")) { // Placeholder until i figure this out
        system("clear");
        return SSH_OK;
    }

    pthread_mutex_lock(&workdir_mutex);
    snprintf(full_cmd, sizeof(full_cmd), "(cd %s && %s && pwd)", work_dir, command);
    pthread_mutex_unlock(&workdir_mutex);

    exec_(session, full_cmd, &nbytes, output, sizeof(output), &exit_status);
    last_exit_status = exit_status;

    for (int i = 0; i < nbytes; i++) {
        if (output[i] == '\n') {
            previous_line_break = line_breaks_position;
            line_breaks_position = i;
        }
    }

    // The final pwd's output is the line between the second-to-last and last newline.
    char new_dir[256];
    memset(new_dir, '\0', sizeof(new_dir));
    int j = 0;
    for (int i = previous_line_break + 1; i < line_breaks_position && j < (int)sizeof(new_dir) - 1; i++) {
        new_dir[j++] = output[i];
        output[i] = '\0';
    }

    pthread_mutex_lock(&workdir_mutex);
    memcpy(work_dir, new_dir, sizeof(new_dir));
    pthread_mutex_unlock(&workdir_mutex);

    if (visibility != 1){
        fprintf(stdout, "%s", output);
    }

    return SSH_OK;
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
