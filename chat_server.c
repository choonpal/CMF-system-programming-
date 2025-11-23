// chat_server.c — TalkShell ChatOps Server (fixed)
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h> // mkdir
#include <dirent.h>   // opendir/readdir for optional checks
#include <errno.h>

#define PORT 5050
#define MAX_CLIENTS 20

static int clients[MAX_CLIENTS];
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void broadcast(const char *msg, int sender_sock)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i] > 0 && clients[i] != sender_sock)
            send(clients[i], msg, strlen(msg), 0);
    }
    pthread_mutex_unlock(&lock);
}

void *client_handler(void *arg)
{
    int sock = *(int *)arg;
    free(arg);

    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    getpeername(sock, (struct sockaddr *)&addr, &len);

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, client_ip, sizeof(client_ip));
    int client_port = ntohs(addr.sin_port);

    printf("🟢 Client connected: %s:%d\n", client_ip, client_port);

    char buf[1024];
    char msg[1100];

    while (1)
    {
        ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0)
            break; // 클라이언트 종료 또는 오류
        buf[n] = 0;

        // 개행 제거
        buf[strcspn(buf, "\r\n")] = '\0';

        // ========== 명령어 처리 ==========
        if (strncmp(buf, "cd ", 3) == 0)
        {
            if (chdir(buf + 3) == 0)
                send(sock, "OK: changed directory\n", 23, 0);
            else
                send(sock, "ERR: invalid path\n", 19, 0);
        }
        else if (strncmp(buf, "mkdir ", 6) == 0)
        {
            if (mkdir(buf + 6, 0755) == 0)
                send(sock, "OK: dir created\n", 17, 0);
            else
                send(sock, "ERR: mkdir failed\n", 19, 0);
        }
        else if (strncmp(buf, "ls", 2) == 0)
        {
            FILE *fp = popen("ls -al", "r");
            if (!fp)
            {
                send(sock, "ERR: ls failed\n", 16, 0);
            }
            else
            {
                while (fgets(buf, sizeof(buf), fp))
                    send(sock, buf, strlen(buf), 0);
                pclose(fp);
            }
            const char *end = "ENDLS\n";
            send(sock, end, strlen(end), 0);
        }
        else
        {
            // 일반 메시지: 서버 콘솔 출력 + 다른 클라이언트에게 브로드캐스트
            printf("[%s:%d] %s\n", client_ip, client_port, buf);
            snprintf(msg, sizeof(msg), "client: %s\n", buf);
            broadcast(msg, sock);
            send(sock, "ACK: message received\n", 23, 0);
        }
    }

    // 연결 종료 로그
    printf("🔴 Client disconnected: %s:%d\n", client_ip, client_port);

    close(sock);
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i] == sock)
            clients[i] = 0;
    pthread_mutex_unlock(&lock);

    return NULL;
}

int main(void)
{
    // ✅ 서버 시작 시 사용자 HOME 디렉토리로 이동
    const char *home = getenv("HOME");
    if (home && *home)
        chdir(home);
    else
        chdir("/home");
    printf("📁 Server base directory: %s\n", home ? home : "/home");

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        exit(1);
    }

    listen(srv, 5);
    printf("🚀 ChatOps server listening on port %d...\n", PORT);

    while (1)
    {
        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        int *cli = malloc(sizeof(int));
        *cli = accept(srv, (struct sockaddr *)&cliaddr, &clilen);

        if (*cli < 0)
        {
            perror("accept");
            free(cli);
            continue;
        }

        // 🔗 클라이언트 접속 로그
        printf("🔗 New client connected from %s:%d\n",
               inet_ntoa(cliaddr.sin_addr),
               ntohs(cliaddr.sin_port));

        pthread_mutex_lock(&lock);
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (clients[i] == 0)
            {
                clients[i] = *cli;
                break;
            }
        pthread_mutex_unlock(&lock);

        pthread_t tid;
        pthread_create(&tid, NULL, client_handler, cli);
        pthread_detach(tid);
    }

    close(srv);
    return 0;
}
