#ifndef FLASSH_PREDICTOR_H
#define FLASSH_PREDICTOR_H

#include "flassh/ssh_connection.h"
#include <stddef.h>

// Predictive execution: speculatively runs commands the user is *likely* to
// type next and caches their output, so pressing Enter costs no round trip.
//
// Only commands proven to have no side effects are ever run speculatively —
// see is_predictable_command(). Anything that writes, deletes, pipes,
// redirects or substitutes is refused outright. Getting this wrong would
// mean running a destructive command the user never actually submitted, so
// the allowlist is deliberately narrow and the parser refuses anything it
// does not fully understand.
void predictor_start(ssh_session session);

// True if `command` is on the read-only allowlist and contains no shell
// metacharacters. Exposed for testing.
int is_predictable_command(const char *command);

// Serves a cached result for `command` in `cwd`. Returns 1 on a fresh hit
// (out/len/exit_status filled, caller must free *out), 0 otherwise.
int predictor_take_cached(const char *cwd, const char *command,
                          char **out, int *len, int *exit_status);

// Called after each real command so the predictor can prefetch what is
// likely to follow (a new directory's listing, its subdirectories, ...).
void predictor_note_command(const char *cwd, const char *command);

// Number of entries currently cached — used by tests and diagnostics.
int predictor_cache_count(void);

#endif
