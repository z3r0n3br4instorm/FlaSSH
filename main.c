// #include <libssh/libssh.h>
#include <stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "headers/ssh_session.h"
#include "headers/ssh_connection.h"


int main(int argc, char *argv[]) {

    if (argc < 2) {
        fprintf(stderr, "One or more required arguments are missing !\n Eg: fssh <username>@<host>");
        return -2;
    }
    fprintf(stdout, "\033[3mFlashSSH\033[0m Version 0.0.1\n");
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

    char command[256];

    for(int i = 0; i > -1; i++) {
        // printf("$ ");
        //
        fprintf(stdout, "FlashSSH [\033[3m%s\033[0m] $ ", get_workDir(session));
        scanf(" %255[^\n]", command);
        exec_command(session, command, 0);
        printf("\n");
    }
    printf("Reached Here");

}
