// Includes the unit under test directly so the static helpers are reachable
// without widening the public header.
#include "predictor.c"

#include <assert.h>
#include <stdio.h>

// The safety property that matters: nothing with a side effect may ever be
// run speculatively, because the user has not pressed Enter on it.
static void test_readonly_commands_allowed(void) {
    assert(is_predictable_command("ls"));
    assert(is_predictable_command("ls -la"));
    assert(is_predictable_command("pwd"));
    assert(is_predictable_command("whoami"));
    assert(is_predictable_command("df -h"));
    assert(is_predictable_command("git status"));
    assert(is_predictable_command("git log --oneline"));
    printf("  read-only commands allowed: ok\n");
}

static void test_mutating_commands_refused(void) {
    assert(!is_predictable_command("rm -rf /"));
    assert(!is_predictable_command("mv a b"));
    assert(!is_predictable_command("cp a b"));
    assert(!is_predictable_command("dd if=/dev/zero of=/dev/sda"));
    assert(!is_predictable_command("shutdown now"));
    assert(!is_predictable_command("apt install foo"));
    assert(!is_predictable_command("git push"));       // git, but not read-only
    assert(!is_predictable_command("git commit -am x"));
    assert(!is_predictable_command("git clean -fd"));
    printf("  mutating commands refused: ok\n");
}

// An allowlisted first word is not enough: shell syntax could still redirect,
// chain or substitute something destructive onto the end of it.
static void test_shell_metacharacters_refused(void) {
    assert(!is_predictable_command("ls > /etc/passwd"));
    assert(!is_predictable_command("ls; rm -rf ~"));
    assert(!is_predictable_command("ls && reboot"));
    assert(!is_predictable_command("ls | xargs rm"));
    assert(!is_predictable_command("ls $(reboot)"));
    assert(!is_predictable_command("ls `reboot`"));
    assert(!is_predictable_command("ls *"));
    printf("  shell metacharacters refused: ok\n");
}

static void test_empty_and_unknown(void) {
    assert(!is_predictable_command(""));
    assert(!is_predictable_command(NULL));
    assert(!is_predictable_command("   "));
    assert(!is_predictable_command("some-unknown-binary"));
    printf("  empty and unknown refused: ok\n");
}

static void test_cache_roundtrip_and_ttl(void) {
    cache_store("/tmp", "ls", "a\nb\n", 4, 0);
    assert(predictor_cache_count() == 1);

    char *out = NULL; int len = 0, status = -1;
    assert(predictor_take_cached("/tmp", "ls", &out, &len, &status) == 1);
    assert(len == 4 && memcmp(out, "a\nb\n", 4) == 0 && status == 0);
    free(out);

    // Wrong directory must never serve another directory's listing.
    assert(predictor_take_cached("/var", "ls", &out, &len, &status) == 0);
    assert(predictor_take_cached("/tmp", "pwd", &out, &len, &status) == 0);

    // Anything past the TTL is refused rather than shown stale.
    cache[0].at.tv_sec -= (CACHE_TTL_MS / 1000) + 5;
    assert(predictor_take_cached("/tmp", "ls", &out, &len, &status) == 0);
    printf("  cache round-trip, isolation and TTL: ok\n");
}

int main(void) {
    printf("test_predictor:\n");
    test_readonly_commands_allowed();
    test_mutating_commands_refused();
    test_shell_metacharacters_refused();
    test_empty_and_unknown();
    test_cache_roundtrip_and_ttl();
    printf("test_predictor: all passed\n");
    return 0;
}
