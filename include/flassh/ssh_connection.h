#ifndef SSH_CONNECTION_H
#define SSH_CONNECTION_H
#include <libssh/libssh.h>

int verify_knownhost(ssh_session session);
ssh_session establish_connection(char* host, char* username, char* identity_file);

#endif
