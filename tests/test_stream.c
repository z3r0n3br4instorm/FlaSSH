// Includes the unit under test directly so the static helpers are reachable
// without widening the public header.
#include "stream.c"

#include <assert.h>
#include <stdio.h>

static void test_streaming_allowlist(void) {
    assert(is_streaming_command("btop"));
    assert(is_streaming_command("sudo apt update")); // matches on first word
    assert(is_streaming_command("vim src/main.c"));
    assert(!is_streaming_command("ls -la"));
    assert(!is_streaming_command("codex")); // reaches PTY via the TTY-error retry
    printf("  streaming allowlist matches first word: ok\n");
}

static void test_dashboards_start_with_prediction_off(void) {
    assert(!is_echo_predictable_command("btop"));
    assert(!is_echo_predictable_command("htop -d 5"));
    assert(is_echo_predictable_command("nano notes.txt"));
    assert(is_echo_predictable_command("codex"));
    printf("  dashboards seeded prediction-off: ok\n");
}

static void test_password_prompt_sniffing(void) {
    char recent[128];
    int len = 0;
    const char *prompt = "\033[1m[sudo] Password: \033[0m";
    feed_recent_text(recent, &len, sizeof(recent), prompt, (int)strlen(prompt));
    assert(recent_looks_like_password(recent, len));

    len = 0;
    const char *search = "Search: ";
    feed_recent_text(recent, &len, sizeof(recent), search, (int)strlen(search));
    assert(!recent_looks_like_password(recent, len));
    printf("  password prompt sniffing: ok\n");
}

static void test_recent_text_window_keeps_tail(void) {
    char recent[8];
    int len = 0;
    const char *data = "abcdefghijkl"; // longer than the window
    feed_recent_text(recent, &len, (int)sizeof(recent), data, (int)strlen(data));
    assert(len == (int)sizeof(recent));
    assert(memcmp(recent, "efghijkl", 8) == 0); // oldest bytes dropped
    printf("  rolling text window keeps the tail: ok\n");
}

static void test_byte_formatting(void) {
    char out[32];
    format_bytes(512, out, sizeof(out));
    assert(strcmp(out, "512B") == 0);
    format_bytes(2048, out, sizeof(out));
    assert(strcmp(out, "2.0KB") == 0);
    format_bytes(3 * 1024 * 1024, out, sizeof(out));
    assert(strcmp(out, "3.0MB") == 0);
    printf("  byte formatting: ok\n");
}

int main(void) {
    printf("test_stream:\n");
    test_streaming_allowlist();
    test_dashboards_start_with_prediction_off();
    test_password_prompt_sniffing();
    test_recent_text_window_keeps_tail();
    test_byte_formatting();
    printf("test_stream: all passed\n");
    return 0;
}
