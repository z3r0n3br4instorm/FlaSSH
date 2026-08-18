#ifndef LINE_EDITOR_H
#define LINE_EDITOR_H

// Reads one line of input with a readline-like editor: left/right arrows
// move the cursor, up/down arrows walk command history, and Tab completes
// against history entries. Returns a malloc'd string (caller frees it).
char* read_line(const char *prompt);

// Shared terminal raw-mode toggles (byte-at-a-time input, no local echo).
// Used by read_line() itself and reused by the PTY streaming session
// (see stream.h) so both go through identical termios setup/teardown.
void terminal_enable_raw_mode(void);
void terminal_disable_raw_mode(void);

#endif
