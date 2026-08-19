#ifndef SSH_SESSION_H
#define SSH_SESSION_H

#include "flassh/ssh_connection.h"
#include <stddef.h>

// Runs `command` over `session` and collects its output into `output`
// (bounded by output_size). Safe to call from multiple threads against the
// same session — internally serialized. `exit_status` may be NULL if the
// caller doesn't need it.
int exec_(ssh_session session, char* command, int *nbytes, char *output, size_t output_size, int *exit_status);

char* exec_command(ssh_session session, char* command, int visibility);

// Applies the same rewrites the interactive path uses before running a
// command (forcing `ls` into coloured column output, etc.). Shared with the
// predictor so speculatively cached output matches what a live run prints.
void expand_command_for_exec(ssh_session session, const char *in, char *out, size_t out_size);
char* get_workDir(ssh_session session);

// Thread-safe snapshot of the client-tracked current directory (used by the
// background directory-cache thread).
void get_current_dir(char *out, size_t out_size);

// Exit status of the last command run through exec_command (0 = success).
int get_last_exit_status(void);

// True if the last exec_command failed specifically because the remote
// program demanded a real terminal ("stdin is not a terminal" and friends).
// The allowlist in stream.h can never cover every such program, so the
// caller uses this to transparently re-run the command under a PTY. When
// this is set, exec_command deliberately suppresses the error text, since
// the retry is about to produce the real output.
int last_command_needed_tty(void);

// Held internally by exec_() around a whole channel lifecycle. A streaming
// PTY session (see stream.h) opens its own channel directly rather than
// through exec_(), so it takes this lock itself for the session's whole
// duration — otherwise the background directory-cache thread could try to
// open a second channel on the same libssh session concurrently, which
// libssh does not support.
void ssh_session_lock(void);
void ssh_session_unlock(void);

#endif
