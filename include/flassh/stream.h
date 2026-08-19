#ifndef STREAM_H
#define STREAM_H

#include "flassh/ssh_connection.h"

// Best-effort heuristic: does `command`'s first word look like a
// full-screen/interactive program (btop, vim, sudo, ...) that needs a real
// PTY rather than the normal capture-the-output exec model? Matched
// against a fixed allowlist — there's no general way to know without
// running it.
int is_streaming_command(const char *command);

// Runs `command` in a PTY-backed passthrough session: local keystrokes are
// forwarded raw to the remote process and its output is written straight
// to the terminal (colors, cursor movement, full-screen redraws and all).
// Ctrl+C and other control bytes are forwarded to the remote program as-is
// (so e.g. btop or a sudo password prompt behave normally and nothing can
// kill the local client); Ctrl+Q is reserved locally to detach back to the
// FlaSSH prompt without forwarding it and without touching the remote
// process. Returns 0 if the session ran (however it ended), -1 if the
// PTY/exec setup itself failed (caller should fall back to exec_command).
int run_streaming_session(ssh_session session, const char *command);

#endif
