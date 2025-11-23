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
    if (sockfd < 0) return -1;

    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &serv.sin_addr) <= 0) {
        close(sockfd);
        sockfd = -1;
        return -1;
    }
    if (connect(sockfd, (struct sockaddr*)&serv, sizeof(serv)) < 0) {
        close(sockfd);
        sockfd = -1;
        return -1;
    }
    return 0;
}

void socket_send_cmd(const char *cmd) {
    if (sockfd >= 0) {
        send(sockfd, cmd, strlen(cmd), 0);
        // 개행 없으면 하나 추가해줘도 됨 (서버가 \n 기준으로 처리할 때)
        if (strchr(cmd, '\n') == NULL) {
            send(sockfd, "\n", 1, 0);
        }
    }
}

int socket_recv_response(char *outbuf, size_t size) {
    if (sockfd < 0) return -1;
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
