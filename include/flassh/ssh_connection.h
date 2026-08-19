#ifndef SSH_CONNECTION_H
#define SSH_CONNECTION_H
#include <libssh/libssh.h>

int verify_knownhost(ssh_session session);
ssh_session establish_connection(char* host, char* username, char* identity_file);

// Verifies the session is still up and, if the server dropped us, reconnects
// in place: a red "Reconnecting" bar is shown along the bottom row, the
// original credentials are retried, and if the server now wants a password a
// TUI prompt is drawn for it. The same ssh_session handle is reused (libssh
// keeps its options across a disconnect), so every module already holding the
// pointer — background threads included — stays valid across the reconnect.
// Returns 1 if the session is usable on return, 0 if it could not be
// restored.
int session_ensure_connected(ssh_session session);

#endif
