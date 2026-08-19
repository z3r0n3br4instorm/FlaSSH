#include <stdio.h>
#include <stdlib.h>
#include <libssh/libssh.h>
#include <errno.h>
#include <string.h>

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

ssh_session establish_connection(char* host, char* username, char* identity_file)
{
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
