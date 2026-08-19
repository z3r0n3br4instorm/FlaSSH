#ifndef HISTORY_H
#define HISTORY_H

#include "flassh/ssh_connection.h"

// Downloads the remote user's bash history over `session` and loads it
// into this process's in-memory history.
void history_load(ssh_session session);

// Appends a newly run command to the in-memory history (skips empty lines
// and immediate repeats).
void history_add(const char *command);

int history_count(void);

// 0-based, oldest first. Returns "" if index is out of range.
const char* history_get_by_index(int index);

// Fills `matches` (most-recent-first) with history entries starting with
// `prefix`. Returns the number of matches written (capped at max_matches).
int history_find_prefix_matches(const char *prefix, const char **matches, int max_matches);

#endif
