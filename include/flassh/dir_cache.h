#ifndef DIR_CACHE_H
#define DIR_CACHE_H

#include "flassh/ssh_connection.h"

#define DIR_CACHE_NAME_MAX 256

// Spawns a detached background thread that periodically lists the
// client-tracked current directory (see get_current_dir() in
// ssh_session.h) so Tab-completion has fresh filenames without blocking on
// a live SSH round trip.
void dir_cache_start(ssh_session session);

// Copies entries starting with `prefix` into `matches` (caller-owned rows,
// most-recent-listing order). Returns the number written, capped at
// max_matches. Copies rather than returning pointers because the cache is
// refreshed from a different thread.
int dir_cache_find_prefix_matches(const char *prefix, char matches[][DIR_CACHE_NAME_MAX], int max_matches);

#endif
