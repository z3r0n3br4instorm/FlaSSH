#include <stdio.h>
#include <stdlib.h>
#include <libssh/libssh.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include "flassh/ssh_connection.h"

int verify_knownhost(ssh_session session)
{
    enum ssh_known_hosts_e state;
    unsigned char *hash = NULL;
    ssh_key srv_pubkey = NULL;
    size_t hlen;
    char buf[10];
    char *p = NULL;
    int cmp;
    int rc;

    rc = ssh_get_server_publickey(session, &srv_pubkey);
    if (rc < 0) {
        return -1;
    }

    rc = ssh_get_publickey_hash(srv_pubkey,
                                SSH_PUBLICKEY_HASH_SHA256,
                                &hash,
                                &hlen);
    ssh_key_free(srv_pubkey);
    if (rc < 0) {
        return -1;
    }

    state = ssh_session_is_known_server(session);
    switch (state) {
        case SSH_KNOWN_HOSTS_OK:
            /* OK */

            break;
        case SSH_KNOWN_HOSTS_CHANGED:
            fprintf(stderr, "Host key for server changed: it is now:\n");
            ssh_print_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hlen);
            fprintf(stderr, "For security reasons, connection will be stopped\n");
            ssh_clean_pubkey_hash(&hash);

            return -1;
        case SSH_KNOWN_HOSTS_OTHER:
            fprintf(stderr, "The host key for this server was not found but an other"
                    "type of key exists.\n");
            fprintf(stderr, "An attacker might change the default server key to"
                    "confuse your client into thinking the key does not exist\n");
            ssh_clean_pubkey_hash(&hash);

            return -1;
        case SSH_KNOWN_HOSTS_NOT_FOUND:
            fprintf(stderr, "Could not find known host file.\n");
            fprintf(stderr, "If you accept the host key here, the file will be"
                    "automatically created.\n");

            /* FALL THROUGH to SSH_SERVER_NOT_KNOWN behavior */

        case SSH_KNOWN_HOSTS_UNKNOWN:
            fprintf(stderr,"The server is unknown. Do you trust the host key?\n");
            fprintf(stderr, "Public key hash: ");
            ssh_print_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hlen);
            ssh_clean_pubkey_hash(&hash);
            p = fgets(buf, sizeof(buf), stdin);
            if (p == NULL) {
                return -1;
            }

            cmp = strncasecmp(buf, "yes", 3);
            if (cmp != 0) {
                return -1;
            }

            rc = ssh_session_update_known_hosts(session);
            if (rc < 0) {
                fprintf(stderr, "Error %s\n", strerror(errno));
                return -1;
            }

            break;
        case SSH_KNOWN_HOSTS_ERROR:
            fprintf(stderr, "Error %s", ssh_get_error(session));
            ssh_clean_pubkey_hash(&hash);
            return -1;
    }

    ssh_clean_pubkey_hash(&hash);
    return 0;
}

// Tries public-key auth like the real `ssh` binary: with -i, that specific
// key; otherwise whatever ssh-agent/default ~/.ssh keys are available.
static int try_pubkey_auth(ssh_session session, char* username, char* identity_file)
{
  if (identity_file == NULL) {
    return ssh_userauth_publickey_auto(session, username, NULL);
  }

  ssh_key privkey = NULL;
  int rc = ssh_pki_import_privkey_file(identity_file, NULL, NULL, NULL, &privkey);
  if (rc == SSH_ERROR) {
    char *passphrase = getpass("Enter passphrase for key: ");
    rc = ssh_pki_import_privkey_file(identity_file, passphrase, NULL, NULL, &privkey);
  }
  if (rc != SSH_OK) {
    fprintf(stderr, "Could not load identity file %s\n", identity_file);
    return SSH_AUTH_ERROR;
  }

  int auth_rc = ssh_userauth_publickey(session, username, privkey);
  ssh_key_free(privkey);
  return auth_rc;
}

static void remember_connection(const char *host, const char *username, const char *identity_file);

ssh_session establish_connection(char* host, char* username, char* identity_file)
{
  remember_connection(host, username, identity_file);
  ssh_session my_ssh_session = NULL;
  int rc;
  char *password = NULL;

  // Open session and set options
  my_ssh_session = ssh_new();
  if (my_ssh_session == NULL)
    exit(-1);
  ssh_options_set(my_ssh_session, SSH_OPTIONS_HOST, host);
  if (username != NULL)
    ssh_options_set(my_ssh_session, SSH_OPTIONS_USER, username);

  // Connect to server
  rc = ssh_connect(my_ssh_session);
  if (rc != SSH_OK)
  {
    fprintf(stderr, "Error connecting to %s: %s\n", host,
            ssh_get_error(my_ssh_session));
    ssh_free(my_ssh_session);
    exit(-1);
  }

  // Verify the server's identity
  // For the source code of verify_knownhost(), check previous example
  if (verify_knownhost(my_ssh_session) < 0)
  {
    ssh_disconnect(my_ssh_session);
    ssh_free(my_ssh_session);
    exit(-1);
  }

  // Authenticate ourselves: public key first (like ssh does), password as fallback
  int auth_rc = try_pubkey_auth(my_ssh_session, username, identity_file);
  if (auth_rc != SSH_AUTH_SUCCESS) {
    password = getpass("Enter Password: ");
    auth_rc = ssh_userauth_password(my_ssh_session, username, password);
  }

  if (auth_rc != SSH_AUTH_SUCCESS)
  {
    fprintf(stderr, "Error authenticating: %s\n",
            ssh_get_error(my_ssh_session));
    ssh_disconnect(my_ssh_session);
    ssh_free(my_ssh_session);
    exit(-1);
  }

  return my_ssh_session;
}

// ---------------------------------------------------------------------------
// Reconnection
// ---------------------------------------------------------------------------

// Remembered from the initial connect so a dropped session can be restored
// without asking the user for anything they already supplied.
static char saved_host[256] = "";
static char saved_username[256] = "";
static char saved_identity[512] = "";
static int saved_have_username = 0;
static int saved_have_identity = 0;

static void remember_connection(const char *host, const char *username, const char *identity_file) {
    snprintf(saved_host, sizeof(saved_host), "%s", host ? host : "");
    if (username != NULL) {
        snprintf(saved_username, sizeof(saved_username), "%s", username);
        saved_have_username = 1;
    }
    if (identity_file != NULL) {
        snprintf(saved_identity, sizeof(saved_identity), "%s", identity_file);
        saved_have_identity = 1;
    }
}

static void terminal_size(int *cols, int *rows) {
    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    } else {
        *cols = 80;
        *rows = 24;
    }
}

// Red counterpart to the streaming status bar: pinned to the bottom row,
// drawn between save/restore cursor so it never disturbs whatever is on
// screen above it.
static void draw_reconnect_bar(const char *message) {
    int cols, rows;
    terminal_size(&cols, &rows);

    char seq[64];
    write(STDOUT_FILENO, "\0337", 2);
    int n = snprintf(seq, sizeof(seq), "\033[%d;1H", rows);
    write(STDOUT_FILENO, seq, n);
    write(STDOUT_FILENO, "\033[41m\033[K", 8); // red background, blank the row

    char line[256];
    int len = snprintf(line, sizeof(line), " %s", message);
    if (len > cols) len = cols;
    write(STDOUT_FILENO, line, len);

    write(STDOUT_FILENO, "\033[0m", 4);
    write(STDOUT_FILENO, "\0338", 2);
}

static void clear_reconnect_bar(void) {
    int cols, rows;
    terminal_size(&cols, &rows);
    (void)cols;

    char seq[64];
    write(STDOUT_FILENO, "\0337", 2);
    int n = snprintf(seq, sizeof(seq), "\033[%d;1H", rows);
    write(STDOUT_FILENO, seq, n);
    write(STDOUT_FILENO, "\033[0m\033[K", 7);
    write(STDOUT_FILENO, "\0338", 2);
}

// Centred bordered box asking for the password, drawn directly rather than
// via getpass() so it works while the terminal is in raw mode and so it
// matches the rest of the UI. Input is never echoed.
static int tui_password_prompt(const char *title, char *out, size_t out_size) {
    int cols, rows;
    terminal_size(&cols, &rows);

    int box_w = 54;
    if (box_w > cols - 4) box_w = cols - 4;
    if (box_w < 20) box_w = 20;
    int box_h = 5;
    int top = (rows - box_h) / 2;
    int left = (cols - box_w) / 2;
    if (top < 1) top = 1;
    if (left < 1) left = 1;

    struct termios saved, raw;
    int have_termios = (tcgetattr(STDIN_FILENO, &saved) == 0);
    if (have_termios) {
        raw = saved;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    char seq[64];
    int n;

    // Frame
    for (int r = 0; r < box_h; r++) {
        n = snprintf(seq, sizeof(seq), "\033[%d;%dH", top + r, left);
        write(STDOUT_FILENO, seq, n);
        write(STDOUT_FILENO, "\033[48;5;236m\033[38;5;255m", 22);
        for (int c = 0; c < box_w; c++) write(STDOUT_FILENO, " ", 1);
    }

    n = snprintf(seq, sizeof(seq), "\033[%d;%dH", top + 1, left + 2);
    write(STDOUT_FILENO, seq, n);
    char header[128];
    int hlen = snprintf(header, sizeof(header), "\033[1m%s\033[22m", title);
    write(STDOUT_FILENO, header, hlen);

    n = snprintf(seq, sizeof(seq), "\033[%d;%dH", top + 3, left + 2);
    write(STDOUT_FILENO, seq, n);
    write(STDOUT_FILENO, "Password: ", 10);

    size_t len = 0;
    int ok = 1;
    while (1) {
        char c;
        if (read(STDIN_FILENO, &c, 1) != 1) { ok = 0; break; }
        if (c == '\r' || c == '\n') break;
        if (c == 27) { ok = 0; break; }               // Esc cancels
        if (c == 127 || c == 8) {                      // Backspace
            if (len > 0) {
                len--;
                write(STDOUT_FILENO, "\b \b", 3);
            }
            continue;
        }
        if (c >= 32 && c < 127 && len < out_size - 1) {
            out[len++] = c;
            write(STDOUT_FILENO, "*", 1);
        }
    }
    out[len] = '\0';

    if (have_termios) tcsetattr(STDIN_FILENO, TCSANOW, &saved);

    // Wipe the box so the screen returns to what it was.
    for (int r = 0; r < box_h; r++) {
        n = snprintf(seq, sizeof(seq), "\033[%d;%dH\033[0m", top + r, left);
        write(STDOUT_FILENO, seq, n);
        for (int c = 0; c < box_w; c++) write(STDOUT_FILENO, " ", 1);
    }
    write(STDOUT_FILENO, "\033[0m", 4);
    return ok;
}

static int authenticate(ssh_session session, int allow_prompt) {
    int rc = try_pubkey_auth(session,
                             saved_have_username ? saved_username : NULL,
                             saved_have_identity ? saved_identity : NULL);
    if (rc == SSH_AUTH_SUCCESS) return 1;
    if (!allow_prompt) return 0;

    for (int attempt = 0; attempt < 3; attempt++) {
        char password[256];
        if (!tui_password_prompt("FlaSSH — session needs re-authentication", password, sizeof(password))) {
            return 0;
        }
        rc = ssh_userauth_password(session,
                                   saved_have_username ? saved_username : NULL,
                                   password);
        memset(password, 0, sizeof(password));
        if (rc == SSH_AUTH_SUCCESS) return 1;
        draw_reconnect_bar("Authentication failed — try again (Esc to give up)");
    }
    return 0;
}

int session_ensure_connected(ssh_session session) {
    if (session == NULL) return 0;
    if (ssh_is_connected(session)) return 1;

    // libssh keeps the session's options across a disconnect, so the same
    // handle can be reconnected — which matters because background threads
    // are still holding this exact pointer.
    for (int attempt = 1; attempt <= 10; attempt++) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Reconnecting to %s… (attempt %d/10)", saved_host, attempt);
        draw_reconnect_bar(msg);

        ssh_disconnect(session);
        if (ssh_connect(session) == SSH_OK) {
            if (authenticate(session, 1)) {
                clear_reconnect_bar();
                return 1;
            }
            draw_reconnect_bar("Could not re-authenticate.");
            clear_reconnect_bar();
            return 0;
        }

        int backoff = attempt < 5 ? attempt : 5; // 1s,2s,3s,4s then every 5s
        sleep(backoff);
    }

    draw_reconnect_bar("Reconnect failed — connection lost.");
    return 0;
}
