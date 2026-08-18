#ifndef SSH_SESSION_H
#define SSH_SESSION_H

#include "ssh_connection.h"

char* exec_command(ssh_session session, char* command, int visibility);
char* get_workDir(ssh_session session);

#endif
