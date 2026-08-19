// Includes the unit under test directly so the static helpers are reachable
// without widening the public header.
#include "history.c"

#include <assert.h>
#include <stdio.h>

static void test_plain_bash_line_untouched(void) {
    const char *line = "ls -la";
    int len = (int)strlen(line);
    strip_zsh_extended_prefix(&line, &len);
    assert(len == 6);
    assert(strncmp(line, "ls -la", 6) == 0);
    printf("  plain bash line untouched: ok\n");
}

static void test_zsh_extended_prefix_stripped(void) {
    const char *line = ": 1690000000:0;sudo pacman -Syu";
    int len = (int)strlen(line);
    strip_zsh_extended_prefix(&line, &len);

    char got[64];
    memcpy(got, line, len);
    got[len] = '\0';
    assert(strcmp(got, "sudo pacman -Syu") == 0);
    printf("  zsh EXTENDED_HISTORY prefix stripped: ok\n");
}

static void test_lookalike_prefix_not_stripped(void) {
    // Starts with ": " but has no "<digits>:<digits>;" run, so it is a real
    // command, not an extended-history timestamp.
    const char *line = ": not a real prefix";
    int len = (int)strlen(line), before = len;
    strip_zsh_extended_prefix(&line, &len);
    assert(len == before);
    printf("  lookalike prefix left alone: ok\n");
}

static void test_add_skips_empty_and_dupes(void) {
    entry_count = 0;
    history_add("echo one");
    history_add("echo one"); // immediate repeat
    history_add("");         // empty
    history_add(NULL);
    history_add("echo two");
    assert(history_count() == 2);
    assert(strcmp(history_get_by_index(0), "echo one") == 0);
    assert(strcmp(history_get_by_index(1), "echo two") == 0);
    printf("  add skips empties and immediate repeats: ok\n");
}

static void test_prefix_matches_are_newest_first(void) {
    entry_count = 0;
    history_add("git status");
    history_add("git commit");

    const char *matches[8];
    int n = history_find_prefix_matches("git ", matches, 8);
    assert(n == 2);
    assert(strcmp(matches[0], "git commit") == 0); // most recent first
    assert(history_find_prefix_matches("zzz", matches, 8) == 0);
    assert(history_find_prefix_matches("", matches, 8) == 0);
    printf("  prefix matches newest-first: ok\n");
}

int main(void) {
    printf("test_history:\n");
    test_plain_bash_line_untouched();
    test_zsh_extended_prefix_stripped();
    test_lookalike_prefix_not_stripped();
    test_add_skips_empty_and_dupes();
    test_prefix_matches_are_newest_first();
    printf("test_history: all passed\n");
    return 0;
}
