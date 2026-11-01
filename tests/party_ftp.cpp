// SPDX-License-Identifier: MIT
// The party app's FTP server (party/ftp.cpp) on the host, for the tests:
// party-ftp <root> <address> <port> [allowed address]. /Hdd1 is root.
#include "../party/ftp.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if(argc<4) { fprintf(stderr,"party-ftp <root> <address> <port> [allowed address]\n"); return 2; }
    if(!party::FtpStartAt(argv[1],inet_addr(argv[2]),atoi(argv[3]),argc>4 ? inet_addr(argv[4]) : 0)) { perror("listen"); return 1; }
    printf("ready\n"); fflush(stdout);
    for(;;) pause();
}
