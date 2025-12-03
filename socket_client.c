#include "socket_client.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

int sockfd = -1;

int socket_connect_to(const char *server_ip, int port) {
    struct sockaddr_in serv;
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    serv.sin_family = AF_INET;
    serv.sin_port = htons(port);
    inet_pton(AF_INET, server_ip, &serv.sin_addr);
    return connect(sockfd, (struct sockaddr*)&serv, sizeof(serv));
}

void socket_send_cmd(const char *cmd) {
    if (sockfd < 0)
        return;

    char line[512];
    size_t len = strlen(cmd);
    if (len >= sizeof(line))
        len = sizeof(line) - 1;
    memcpy(line, cmd, len);
    line[len] = '\n';
    line[len + 1] = '\0';
    send(sockfd, line, len + 1, 0);
}

int socket_recv_response(char *outbuf, size_t size) {
    int n = recv(sockfd, outbuf, size - 1, 0);
    if (n > 0) outbuf[n] = 0;
    return n;
}

void socket_close(void) {
    if (sockfd >= 0) {
        close(sockfd);
        sockfd = -1;
    }
}
