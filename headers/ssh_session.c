#include "ssh_session.h"
#include <libssh/libssh.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include "ssh_connection.h"

// Session VARs
char work_dir[256];

int exec_(ssh_session session, char* command, int *nbytes, char *output, size_t output_size)
{
  ssh_channel channel = NULL;
  int rc;
  char buffer[256];
  // int nbytes;

  channel = ssh_channel_new(session);
  if (channel == NULL)
    return SSH_ERROR;

  rc = ssh_channel_open_session(channel);
  if (rc != SSH_OK)
  {
    ssh_channel_free(channel);
    return rc;
  }


  rc = ssh_channel_request_exec(channel, command);

  if (rc != SSH_OK)
  {
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    return rc;
  }

  // *nbytes = ssh_channel_read(channel, buffer, sizeof(buffer), 0);
  int total = 0;
  int n = ssh_channel_read(channel, buffer, sizeof(buffer), 0);

  while (n > 0)
  {
      if ((size_t)(total + n) > output_size - 1) {
          ssh_channel_close(channel);
          ssh_channel_free(channel);
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
      return SSH_ERROR;
  }

  ssh_channel_send_eof(channel);
  ssh_channel_close(channel);
  ssh_channel_free(channel);

  return SSH_OK;
}


char* exec_command(ssh_session session, char *command, int visibility) {
    // Check user's current directory
    int nbytes;
    char output[4048];
    char full_cmd[512];
    // int line_breaks[256];
    int previous_line_break;
    int line_breaks_position;
    line_breaks_position = -1;
    previous_line_break = -1;

    if (! strcmp(command, "clear")) { // Placeholder until i figure this out
        system("clear");
        return SSH_OK;
    }

    snprintf(full_cmd, sizeof(full_cmd), "(cd %s && %s && pwd)", work_dir, command);
    // fprintf(stdout, "Full Command %s\n", full_cmd);
    exec_(session, full_cmd, &nbytes, output, sizeof(output));

    for (int i = 0; i < nbytes; i++) {
        // printf("%c", output[i]);
        if (output[i] == '\n') {
            // line_breaks[i] = i;
            previous_line_break = line_breaks_position;
            line_breaks_position = i;
            // printf("Detected a new line at position [%d]", line_breaks);
        }
    }

    // Get last pwd
    //
    // printf("The last linebreak is at [%d]\n", line_breaks_position);
    memset(work_dir, '\0', sizeof(work_dir));
    int j = 0;
    for (int i = previous_line_break + 1; i < line_breaks_position; i++) {
        work_dir[j++] = output[i];
        output[i] = '\0';
    }
    // printf("Reached Here ! %d", 3);

    // snprintf(full_cmd, sizeof(full_cmd), "pwd", work_dir, command);
    // exec_(session, full_cmd, &nbytes, output);

    // for (int i = 0; i < nbytes; i++) {
    //     printf("%c", output[i]);
    // }
    //

    if (visibility != 1){
        fprintf(stdout, "%s", output);
    }

    return SSH_OK;
}


char* get_workDir(ssh_session session) {
    exec_command(session, "pwd", 1);
    return work_dir;
}
