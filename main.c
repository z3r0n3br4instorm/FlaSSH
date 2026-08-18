// #include <libssh/libssh.h>
#include <stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <signal.h>
#include "headers/ssh_session.h"
#include "headers/ssh_connection.h"
#include "headers/history.h"
#include "headers/line_editor.h"
#include "headers/dir_cache.h"
#include "headers/stream.h"

static char remote_home[256] = "";

static void fetch_remote_home(ssh_session session) {
    int nbytes = 0;
    char output[256];
    int rc = exec_(session, "echo $HOME", &nbytes, output, sizeof(output), NULL);
    if (rc != SSH_OK) return;

    while (nbytes > 0 && (output[nbytes - 1] == '\n' || output[nbytes - 1] == '\r')) nbytes--;
    output[nbytes] = '\0';
    snprintf(remote_home, sizeof(remote_home), "%s", output);
}

static void abbreviate_home(const char *cwd, char *out, size_t out_size) {
    size_t home_len = strlen(remote_home);
    if (home_len > 0 && strncmp(cwd, remote_home, home_len) == 0) {
        snprintf(out, out_size, "~%s", cwd + home_len);
    } else {
        snprintf(out, out_size, "%s", cwd);
    }
}

// Powerline-style two-line prompt: "[user@host]->[cwd]" segments on line
// one, a caret on line two colored green/red by the last exit status.
static void build_prompt(char *out, size_t out_size, const char *user, const char *host, const char *cwd) {
    char display_path[256];
    abbreviate_home(cwd, display_path, sizeof(display_path));

    char identity[256];
    if (user != NULL) {
        snprintf(identity, sizeof(identity), "%s@%s", user, host);
    } else {
        snprintf(identity, sizeof(identity), "%s", host);
    }

    const char *caret_color = (get_last_exit_status() == 0) ? "\033[38;5;42m" : "\033[38;5;196m";
    const char *caret_char = (user != NULL && strcmp(user, "root") == 0) ? "#" : "$";

    snprintf(out, out_size,
        "\033[48;5;25m\033[38;5;255m %s "
        "\033[38;5;25m\033[48;5;238m"
        "\033[38;5;255m %s "
        "\033[38;5;238m\033[49m"
        "\033[0m\r\n"
        "%s%s\033[0m ",
        identity, display_path, caret_color, caret_char);
}

int main(int argc, char *argv[]) {

    if (argc < 2) {
        fprintf(stderr, "One or more required arguments are missing !\n Eg: fssh <username>@<host>");
        return -2;
    }
    fprintf(stdout, "\033[3mFlashSSH\033[0m Version 0.0.1\n");
    signal(SIGINT, SIG_IGN); // Ctrl+C must never kill the client, only the remote process it's forwarded to
    ssh_session session;

    char *username = NULL;
    char *host = argv[1];
    char *at = strchr(argv[1], '@');
    if (at != NULL) {
        *at = '\0';
        username = argv[1];
        host = at + 1;
    }

    fprintf(stdout, "Establishing connection with %s\n", host);
    session = establish_connection(host, username);
    fetch_remote_home(session);
    get_workDir(session); // seed the tracked cwd once; later prompts read it locally instead of re-querying over SSH
    history_load(session);
    dir_cache_start(session);

    // Discard any keystrokes queued by the terminal while the above setup
    // (connecting, password prompt, history download) was still running,
    // so they don't replay as spurious empty commands once we start reading.
    tcflush(STDIN_FILENO, TCIFLUSH);

    for(int i = 0; i > -1; i++) {
        char cwd[256];
        get_current_dir(cwd, sizeof(cwd));

        char prompt[512];
        build_prompt(prompt, sizeof(prompt), username, host, cwd);

        char *command = read_line(prompt);
        if (command[0] != '\0') {
            if (is_streaming_command(command)) {
                if (run_streaming_session(session, command) != 0) {
                    fprintf(stderr, "Couldn't open a PTY for '%s', running it normally instead.\n", command);
                    exec_command(session, command, 0);
                }
            } else {
                exec_command(session, command, 0);
            }
        }
        free(command);
    }
    printf("Reached Here");

}
