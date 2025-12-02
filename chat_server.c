// chat_server.c — TalkShell ChatOps Server
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
#include <stdbool.h>
#include <ctype.h>

#include "auth.h"

// #define PORT 5050
#define MAX_CLIENTS 20

typedef struct
{
    int sock;
    bool authenticated;
    char username[64];
    int permission_level;
} ClientSlot;

void error_handling(char *message);

static ClientSlot clients[MAX_CLIENTS];
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void broadcast(const char *msg, int sender_sock)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].sock > 0 && clients[i].authenticated && clients[i].sock != sender_sock)
            send(clients[i].sock, msg, strlen(msg), 0);
    }
    pthread_mutex_unlock(&lock);
}

static void trim_whitespace(char *s)
{
    if (!s)
        return;

    // 왼쪽 공백 제거
    while (*s && isspace((unsigned char)*s))
        memmove(s, s + 1, strlen(s));

    // 오른쪽 공백 제거
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
    {
        s[len - 1] = '\0';
        len--;
    }
}

static void handle_upload_plan(ClientSlot *slot, const char *buf)
{
    char kind[8] = {0};
    char name[256] = {0};
    int matched = sscanf(buf, "UPLOAD PLAN %7s %255s", kind, name);
    if (matched != 2)
    {
        const char *err = "ERR: invalid upload plan\n";
        send(slot->sock, err, strlen(err), 0);
        return;
    }

    bool is_dir = strcasecmp(kind, "DIR") == 0;
    printf("[server/upload] PLAN %s %s from %s\n", is_dir ? "DIR" : "FILE", name, slot->username);

    char resp[256];
    snprintf(resp, sizeof(resp), "OK: upload plan (%s: %s)\n", is_dir ? "dir" : "file", name);
    send(slot->sock, resp, strlen(resp), 0);
}

static void handle_command(ClientSlot *slot, const char *buf, const char *client_ip, int client_port)
{
    char msg[1100];

    if (!slot->authenticated)
    {
        if (buf[0] == '\0')
            return;

        char cmd[16] = {0};
        char user[64] = {0};
        char pw_hash[80] = {0};
        int perm = 0;
        int remaining = -1;
        AuthResult res = AUTH_INVALID;

        int fields = sscanf(buf, "%15s %63s %79s", cmd, user, pw_hash);
        if (fields == 3 && strcasecmp(cmd, "LOGIN") == 0)
            res = verify_credentials(user, pw_hash, &perm, &remaining);

        if (res == AUTH_OK)
        {
            slot->authenticated = true;
            snprintf(slot->username, sizeof(slot->username), "%s", user);
            slot->permission_level = perm;
            printf("👤 User logged in: %s (%s:%d)\n", user, client_ip, client_port);
            send(slot->sock, "OK: login successful\n", strlen("OK: login successful\n"), 0);
        }
        else if (res == AUTH_LOCKED)
        {
            send(slot->sock, "ERR: account locked\n", strlen("ERR: account locked\n"), 0);
        }
        else if (fields == 3)
        {
            if (remaining >= 0)
            {
                char err[80];
                snprintf(err, sizeof(err), "ERR: invalid credentials (%d tries left)\n", remaining);
                send(slot->sock, err, strlen(err), 0);
            }
            else
            {
                send(slot->sock, "ERR: invalid credentials\n", strlen("ERR: invalid credentials\n"), 0);
            }
        }
        else
        {
            send(slot->sock, "ERR: please login first\n", strlen("ERR: please login first\n"), 0);
        }
        return;
    }

    // ========== 명령어 처리 ==========
    if (strncmp(buf, "cd ", 3) == 0)
    {
        if (chdir(buf + 3) == 0)
            send(slot->sock, "OK: changed directory\n", strlen("OK: changed directory\n"), 0);
        else
            send(slot->sock, "ERR: invalid path\n", strlen("ERR: invalid path\n"), 0);
    }
    else if (strncmp(buf, "mkdir ", 6) == 0)
    {
        if (mkdir(buf + 6, 0755) == 0)
            send(slot->sock, "OK: dir created\n", strlen("OK: dir created\n"), 0);
        else
            send(slot->sock, "ERR: mkdir failed\n", strlen("ERR: mkdir failed\n"), 0);
    }
    else if (strncmp(buf, "ls", 2) == 0)
    {
        char tmpbuf[1024];
        FILE *fp = popen("ls -al", "r");
        while (fgets(tmpbuf, sizeof(tmpbuf), fp))
            send(slot->sock, tmpbuf, strlen(tmpbuf), 0);
        pclose(fp);
        send(slot->sock, "EOF\n", strlen("EOF\n"), 0);
    }
    else if (strncasecmp(buf, "UPLOAD PLAN", 11) == 0)
    {
        handle_upload_plan(slot, buf);
    }
    else
    {
        // 일반 메시지: 서버 콘솔 출력 + 다른 클라이언트에게 브로드캐스트
        printf("[%s:%d][%s] %s\n", client_ip, client_port, slot->username[0] ? slot->username : "?", buf);
        snprintf(msg, sizeof(msg), "%s: %s\n", slot->username[0] ? slot->username : "client", buf);
        broadcast(msg, slot->sock);
        send(slot->sock, "ACK: message received\n", strlen("ACK: message received\n"), 0);
    }
}

void *client_handler(void *arg)
{
    ClientSlot *slot = (ClientSlot *)arg;
    int sock = slot->sock;

    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    getpeername(sock, (struct sockaddr *)&addr, &len);

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, client_ip, sizeof(client_ip));
    int client_port = ntohs(addr.sin_port);

    printf("🟢 Client connected: %s:%d\n", client_ip, client_port);

    char buf[1024];
    const char *banner = "INFO: login required\n";
    send(sock, banner, strlen(banner), 0);

    while (1)
    {
        ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0)
            break; // 클라이언트 종료 또는 오류
        buf[n] = '\0';
        trim_whitespace(buf);
        handle_command(slot, buf, client_ip, client_port);
    }

    // 연결 종료 로그
    printf("🔴 Client disconnected: %s:%d\n", client_ip, client_port);

    close(sock);
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].sock == sock)
        {
            clients[i].sock = 0;
            clients[i].authenticated = false;
            clients[i].username[0] = '\0';
            clients[i].permission_level = 0;
        }
    pthread_mutex_unlock(&lock);

    return NULL;
}

int main(int argc, char *argv[])
{
    // 호스트는 로컬 루프백으로, 포트는 5050으로 기본경로를 설정
    char host[256] = "127.0.0.1";
    int port = 5050;

    if (!auth_init())
    {
        fprintf(stderr, "[WARN] Failed to initialize authentication state.\n");
    }

    // 그 외에 다른 호스트 주소랑 포트를 사용자가 입력했다면, 그 주소:포트로 기본경로 덮어쓰기
    if (argc >= 3)
    { // 인자가 3개 이하(예 make run-client 127.0.0.1 9190) -> 형식: host port
        strncpy(host, argv[1], sizeof(host) - 1);
        host[sizeof(host) - 1] = '\0';
        int p = atoi(argv[2]);
        if (p > 0)
            port = p;
    }
    else if (argc >= 2)
    { // 인자가 2개 이하 -> 즉, host[:port] 처럼 호스트, 포트 붙여 보내거나 호스트 ip만 보낼 때
        strncpy(host, argv[1], sizeof(host) - 1);
        host[sizeof(host) - 1] = '\0';
        char *colon = strrchr(host, ':');
        if (colon)
        {
            *colon = '\0';
            int p = atoi(colon + 1);
            if (p > 0)
                port = p;
        }
        else
        {
            char *endptr = NULL;
            long p = strtol(host, &endptr, 10);
            if (endptr && *endptr == '\0' && p > 0)
            {
                port = (int)p; // 단일 숫자 인자가 들어오면 포트로 간주
                strcpy(host, "127.0.0.1");
            }
        }
    }

    int serv_sock;
    int clnt_sock;

    struct sockaddr_in serv_addr;
    struct sockaddr_in clnt_addr;
    socklen_t clnt_addr_size;

    // ✅ 서버 시작 시 자동으로 /home 디렉토리로 이동
    (void)chdir("/home"); // 반환값 무시
    printf("📁 Server base directory: /home\n");

    /* 서버 소켓(리스닝 소켓) 생성 */
    serv_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (serv_sock == -1)
        error_handling("socket() error");
    int opt = 1;

    setsockopt(serv_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* 주소 정보 초기화 */
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    // serv_addr.sin_port = htons(atoi(argv[1]));
    serv_addr.sin_port = htons(port);

    /* 주소 정보 할당 */
    if (bind(serv_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == -1)
        error_handling("bind() error");

    if (listen(serv_sock, 5) == -1)
        error_handling("listen() error");

    printf("🚀 ChatOps server listening on port %d...\n", port);

    while (1)
    {
        clnt_addr_size = sizeof(clnt_addr);
        clnt_sock = accept(serv_sock, (struct sockaddr *)&clnt_addr, &clnt_addr_size);
        if (clnt_sock == -1)
            error_handling("accept() error");

        // 🔗 클라이언트 접속 로그
        printf("🔗 New client connected from %s:%d\n",
               inet_ntoa(clnt_addr.sin_addr),
               ntohs(clnt_addr.sin_port));

        pthread_mutex_lock(&lock);
        ClientSlot *target_slot = NULL;
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (clients[i].sock == 0)
            {
                clients[i].sock = clnt_sock;
                clients[i].authenticated = false;
                clients[i].username[0] = '\0';
                target_slot = &clients[i];
                break;
            }
        pthread_mutex_unlock(&lock);

        if (!target_slot)
        {
            const char *msg = "ERR: server busy\n";
            send(clnt_sock, msg, strlen(msg), 0);
            close(clnt_sock);
            continue;
        }

        pthread_t tid;
        pthread_create(&tid, NULL, client_handler, target_slot);
        pthread_detach(tid);
    }

    close(clnt_sock);
    close(serv_sock);
    return 0;
}

void error_handling(char *message)
{
    fputs(message, stderr);
    fputc('\n', stderr);
    exit(1);
}
