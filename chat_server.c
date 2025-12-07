// chat_server.c — TalkShell ChatOps Server (Refactored)
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <ctype.h>

#include "auth.h"

#define MAX_CLIENTS 20
#define BUFFER_SIZE 1024
#define FILE_BUFFER_SIZE 4096
#define DEFAULT_PORT 5050

typedef struct
{
    int sock;
    bool authenticated;
    char username[64];
    int permission_level;
    char pending_upload_file[256];
} ClientSlot;

static ClientSlot clients[MAX_CLIENTS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

// --- 유틸리티 함수 ---

void error_handling(const char *message)
{
    fputs(message, stderr);
    fputc('\n', stderr);
    exit(1);
}

static void trim_whitespace(char *s)
{
    if (!s) return;
    while (*s && isspace((unsigned char)*s)) memmove(s, s + 1, strlen(s));
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) { s[len - 1] = '\0'; len--; }
}

void broadcast(const char *msg, int sender_sock)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].sock > 0 && clients[i].authenticated && clients[i].sock != sender_sock)
        {
            send(clients[i].sock, msg, strlen(msg), 0);
        }
    }
    pthread_mutex_unlock(&lock);
}

// --- 명령 처리 함수들 ---

static void handle_login(ClientSlot *slot, const char *buf, const char *client_ip, int client_port)
{
    char cmd[16] = {0}, user[64] = {0}, pw_hash[80] = {0};
    int perm = 0, remaining = -1;
    AuthResult res = AUTH_INVALID;

    int fields = sscanf(buf, "%15s %63s %79s", cmd, user, pw_hash);
    if (fields == 3 && strcasecmp(cmd, "LOGIN") == 0)
    {
        res = verify_credentials(user, pw_hash, &perm, &remaining);
    }

    if (res == AUTH_OK)
    {
        slot->authenticated = true;
        snprintf(slot->username, sizeof(slot->username), "%s", user);
        slot->permission_level = perm;
        printf("👤 User logged in: %s (%s:%d)\n", user, client_ip, client_port);
        send(slot->sock, "OK: login successful\n", 21, 0);
    }
    else if (res == AUTH_LOCKED)
    {
        send(slot->sock, "ERR: account locked\n", 20, 0);
    }
    else
    {
        if (remaining >= 0) {
            char err[64];
            snprintf(err, sizeof(err), "ERR: invalid credentials (%d tries left)\n", remaining);
            send(slot->sock, err, strlen(err), 0);
        } else {
            send(slot->sock, "ERR: invalid credentials\n", 25, 0);
        }
    }
}

static void handle_upload_plan(ClientSlot *slot, const char *buf)
{
    char kind[8] = {0};
    char name[256] = {0};

    if (sscanf(buf, "UPLOAD PLAN %7s %255s", kind, name) != 2)
    {
        const char *err = "ERR: invalid upload plan\n";
        send(slot->sock, err, strlen(err), 0);
        return;
    }

    bool is_dir = (strcasecmp(kind, "DIR") == 0);
    snprintf(slot->pending_upload_file, sizeof(slot->pending_upload_file), "%s", name);

    printf("[server/upload] PLAN %s %s from %s\n", is_dir ? "DIR" : "FILE", name, slot->username);

    char resp[512];
    snprintf(resp, sizeof(resp), "OK: upload plan (%s: %s)\n", is_dir ? "dir" : "file", name);
    send(slot->sock, resp, strlen(resp), 0);
}

static void handle_upload_start(ClientSlot *slot, const char *buf)
{
    long filesize = atol(buf + 13);
    char filename[300];

    if (strlen(slot->pending_upload_file) > 0)
        snprintf(filename, sizeof(filename), "%s", slot->pending_upload_file);
    else
        snprintf(filename, sizeof(filename), "uploaded_file.bin");

    printf("[server/upload] Receiving %s (%ld bytes)...\n", filename, filesize);

    FILE *fp = fopen(filename, "wb");
    if (!fp)
    {
        const char *err = "ERR: cannot create file\n";
        send(slot->sock, err, strlen(err), 0);
        return;
    }

    // Handshake: 준비 완료 신호 전송
    send(slot->sock, "ACK: READY\n", 11, 0);

    long total_received = 0;
    char filebuf[FILE_BUFFER_SIZE];

    while (total_received < filesize)
    {
        size_t to_read = sizeof(filebuf);
        if (filesize - total_received < (long)to_read)
            to_read = filesize - total_received;

        ssize_t n = recv(slot->sock, filebuf, to_read, 0);
        if (n <= 0) break; // 연결 끊김 또는 에러

        fwrite(filebuf, 1, n, fp);
        total_received += n;
    }
    fclose(fp);

    // 초기화
    slot->pending_upload_file[0] = '\0';

    printf("[server/upload] Completed: %s\n", filename);
    const char *success_msg = "OK: Upload Complete\n";
    send(slot->sock, success_msg, strlen(success_msg), 0);
}

static void handle_command(ClientSlot *slot, const char *buf, const char *client_ip, int client_port)
{
    // 1. 인증되지 않은 사용자 처리
    if (!slot->authenticated)
    {
        if (buf[0] == '\0') return;
        handle_login(slot, buf, client_ip, client_port);
        return;
    }

    // 2. 명령어 처리
    if (strncmp(buf, "cd ", 3) == 0)
    {
        if (chdir(buf + 3) == 0)
            send(slot->sock, "OK: changed directory\n", 22, 0);
        else
            send(slot->sock, "ERR: invalid path\n", 18, 0);
    }
    else if (strncmp(buf, "mkdir ", 6) == 0)
    {
        if (mkdir(buf + 6, 0755) == 0)
            send(slot->sock, "OK: dir created\n", 16, 0);
        else
            send(slot->sock, "ERR: mkdir failed\n", 18, 0);
    }
    else if (strncmp(buf, "ls", 2) == 0)
    {
        char tmpbuf[1024];
        FILE *fp = popen("ls -al", "r");
        if (fp) {
            while (fgets(tmpbuf, sizeof(tmpbuf), fp))
                send(slot->sock, tmpbuf, strlen(tmpbuf), 0);
            pclose(fp);
        }
        send(slot->sock, "EOF\n", 4, 0);
    }
    else if (strncasecmp(buf, "UPLOAD PLAN", 11) == 0)
    {
        handle_upload_plan(slot, buf);
    }
    else if (strncasecmp(buf, "UPLOAD START", 12) == 0)
    {
        handle_upload_start(slot, buf);
    }
    else
    {
        // 3. 일반 채팅 메시지 처리
        char msg[1100];
        printf("[%s:%d][%s] %s\n", client_ip, client_port, slot->username, buf);
        snprintf(msg, sizeof(msg), "%s: %s\n", slot->username, buf);
        broadcast(msg, slot->sock);
        send(slot->sock, "ACK: message received\n", 22, 0);
    }
}

// --- 클라이언트 핸들러 스레드 ---

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
    send(sock, "INFO: login required\n", 21, 0);

    char buf[BUFFER_SIZE];
    while (1)
    {
        ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break; // 연결 종료 또는 에러

        buf[n] = '\0';
        trim_whitespace(buf);
        if (strlen(buf) > 0) {
            handle_command(slot, buf, client_ip, client_port);
        }
    }

    printf("🔴 Client disconnected: %s:%d\n", client_ip, client_port);
    close(sock);

    // 슬롯 초기화
    pthread_mutex_lock(&lock);
    slot->sock = 0;
    slot->authenticated = false;
    slot->username[0] = '\0';
    slot->permission_level = 0;
    slot->pending_upload_file[0] = '\0';
    pthread_mutex_unlock(&lock);

    return NULL;
}

// --- 메인 함수 ---

int main(int argc, char *argv[])
{
    char host[256] = "127.0.0.1";
    int port = DEFAULT_PORT;

    // 인증 모듈 초기화
    if (!auth_init()) {
        fprintf(stderr, "[WARN] Failed to initialize authentication state.\n");
    }

    // 인자 파싱 (IP:PORT 또는 PORT)
    if (argc >= 3) {
        strncpy(host, argv[1], sizeof(host) - 1);
        port = atoi(argv[2]);
    } else if (argc >= 2) {
        strncpy(host, argv[1], sizeof(host) - 1);
        char *colon = strrchr(host, ':');
        if (colon) {
            *colon = '\0';
            port = atoi(colon + 1);
        } else {
            // 숫자로만 된 인자는 포트로 간주
            char *endptr = NULL;
            long p = strtol(host, &endptr, 10);
            if (endptr && *endptr == '\0') {
                port = (int)p;
                strcpy(host, "127.0.0.1");
            }
        }
    }

    // 서버 시작 디렉토리 설정
    if (chdir("/home") != 0) {
        perror("chdir failed");
    }
    printf("📁 Server base directory: /home\n");

    int serv_sock, clnt_sock;
    struct sockaddr_in serv_addr, clnt_addr;
    socklen_t clnt_addr_size;

    serv_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (serv_sock == -1) error_handling("socket() error");

    int opt = 1;
    setsockopt(serv_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port);

    if (bind(serv_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == -1)
        error_handling("bind() error");

    if (listen(serv_sock, 5) == -1)
        error_handling("listen() error");

    printf("🚀 ChatOps server listening on port %d...\n", port);

    while (1)
    {
        clnt_addr_size = sizeof(clnt_addr);
        clnt_sock = accept(serv_sock, (struct sockaddr *)&clnt_addr, &clnt_addr_size);
        if (clnt_sock == -1) continue;

        pthread_mutex_lock(&lock);
        ClientSlot *target_slot = NULL;
        for (int i = 0; i < MAX_CLIENTS; i++)
        {
            if (clients[i].sock == 0)
            {
                clients[i].sock = clnt_sock;
                // 초기화
                clients[i].authenticated = false;
                clients[i].username[0] = '\0';
                clients[i].pending_upload_file[0] = '\0';
                target_slot = &clients[i];
                break;
            }
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

    close(serv_sock);
    return 0;
}