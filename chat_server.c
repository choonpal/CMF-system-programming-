// chat_server.c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <ctype.h>
#include <limits.h>

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
static char server_root[PATH_MAX] = "/home";
static bool is_path_under_root(const char *path);

// ------------------------------------------------------------
// TODO: TalkShell에 "/dls" 명령 구현
// ------------------------------------------------------------
// 이 주석 바로 아래에 필요한 static 함수/헬퍼 함수 정의를 생성하고,
// 기존 main 루프 및 명령 파싱 switch/case 부분에 자연스럽게 통합되도록 수정하라.

typedef struct
{
    char name[PATH_MAX];
    unsigned long long size;
    bool is_dir;
    bool error;
} DlsEntry;

typedef struct
{
    DlsEntry *items;
    size_t count;
    size_t cap;
} DlsList;

static void dls_list_push(DlsList *list, const char *name, unsigned long long size, bool is_dir, bool error)
{
    if (list->count + 1 > list->cap)
    {
        size_t new_cap = (list->cap == 0) ? 16 : list->cap * 2;
        DlsEntry *n = realloc(list->items, new_cap * sizeof(DlsEntry));
        if (!n)
            return;
        list->items = n;
        list->cap = new_cap;
    }

    DlsEntry *ent = &list->items[list->count++];
    snprintf(ent->name, sizeof(ent->name), "%s", name);
    ent->size = size;
    ent->is_dir = is_dir;
    ent->error = error;
}

static void dls_list_free(DlsList *list)
{
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int dls_cmp_desc(const void *a, const void *b)
{
    const DlsEntry *ea = (const DlsEntry *)a;
    const DlsEntry *eb = (const DlsEntry *)b;

    if (ea->size == eb->size)
        return strcasecmp(ea->name, eb->name);
    return (ea->size < eb->size) ? 1 : -1;
}

static unsigned long long dls_dir_size(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return 0;

    if (S_ISDIR(st.st_mode))
    {
        DIR *dir = opendir(path);
        if (!dir)
            return 0;

        unsigned long long total = 0;
        struct dirent *ent;
        while ((ent = readdir(dir)))
        {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;

            char child[PATH_MAX];
            snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
            total += dls_dir_size(child);
        }

        closedir(dir);
        return total;
    }

    return (unsigned long long)st.st_size;
}

static void dls_human_size(unsigned long long bytes, char *out, size_t len)
{
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int u = 0;
    double val = (double)bytes;

    while (val >= 1024.0 && u < 4)
    {
        val /= 1024.0;
        u++;
    }

    snprintf(out, len, "%.1f %s", val, units[u]);
}

static int dls_resolve_path(const char *raw, char *resolved)
{
    char tmp[PATH_MAX];

    if (!raw || !*raw)
    {
        snprintf(tmp, sizeof(tmp), "%s", server_root);
    }
    else if (raw[0] == '/')
    {
        snprintf(tmp, sizeof(tmp), "%s", raw);
    }
    else
    {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd)))
            return -1;

        snprintf(tmp, sizeof(tmp), "%s/%s", cwd, raw);
    }

    if (!realpath(tmp, resolved))
        return -1;

    if (!is_path_under_root(resolved))
    {
        errno = EPERM;
        return -1;
    }

    return 0;
}

static void handle_dls(ClientSlot *slot, const char *buf)
{
    const int BAR_WIDTH = 20;
    const int TOP_N = 10;

    const char *arg = buf;
    while (*arg == ' ')
        arg++;

    char target[PATH_MAX];
    if (dls_resolve_path(arg, target) != 0)
    {
        char msg[PATH_MAX + 64];
        snprintf(msg, sizeof(msg), "ERR: cannot access %s\n", *arg ? arg : "<empty>");
        send(slot->sock, msg, strlen(msg), 0);
        send(slot->sock, "EOF\n", 4, 0);
        return;
    }

    DIR *dir = opendir(target);
    if (!dir)
    {
        char msg[PATH_MAX + 64];
        snprintf(msg, sizeof(msg), "ERR: cannot open %s\n", target);
        send(slot->sock, msg, strlen(msg), 0);
        send(slot->sock, "EOF\n", 4, 0);
        return;
    }

    DlsList list = {0};
    unsigned long long dir_total = 0;

    struct dirent *ent;
    while ((ent = readdir(dir)))
    {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char child[PATH_MAX];
        snprintf(child, sizeof(child), "%s/%s", target, ent->d_name);

        struct stat st;
        bool err = false;
        bool is_dir = false;
        unsigned long long sz = 0;

        if (lstat(child, &st) != 0)
        {
            err = true;
        }
        else
        {
            is_dir = S_ISDIR(st.st_mode);

            if (is_dir)
                sz = dls_dir_size(child);
            else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode))
                sz = (unsigned long long)st.st_size;
            else
                sz = 0;

            dir_total += sz;
        }

        dls_list_push(&list, ent->d_name, sz, is_dir, err);
    }

    closedir(dir);

    struct statvfs vfs;
    unsigned long long fs_total = 0;
    if (statvfs(target, &vfs) == 0)
        fs_total = (unsigned long long)vfs.f_blocks * vfs.f_frsize;

    qsort(list.items, list.count, sizeof(DlsEntry), dls_cmp_desc);

    char header[PATH_MAX + 128];
    char dir_human[64];
    dls_human_size(dir_total, dir_human, sizeof(dir_human));

    double dir_percent = (fs_total > 0 && dir_total > 0)
                             ? ((double)dir_total / (double)fs_total * 100.0)
                             : 0.0;

    snprintf(header, sizeof(header),
             "[dls] 용량 요약 — 기준 디렉토리: %s\n"
             "- 디렉토리 총 용량: %s (전체 파일시스템의 %.0f%%)\n"
             "- 엔트리 수: %zu개 (상위 %d개만 표시)\n",
             target, dir_human, dir_percent, list.count, TOP_N);
    send(slot->sock, header, strlen(header), 0);

    size_t max_entries = (list.count > (size_t)TOP_N) ? (size_t)TOP_N : list.count;
    for (size_t i = 0; i < max_entries; i++)
    {
        const DlsEntry *e = &list.items[i];
        char bar[BAR_WIDTH * 3 + 1];

        int dir_pct = (dir_total > 0) ? (int)((double)e->size / (double)dir_total * 100.0 + 0.5) : 0;
        int bar_fill = (int)((double)dir_pct / 100.0 * BAR_WIDTH + 0.5);
        if (bar_fill > BAR_WIDTH)
            bar_fill = BAR_WIDTH;

        int bar_pos = 0;
        for (int k = 0; k < BAR_WIDTH && bar_pos < (int)sizeof(bar) - 4; k++)
        {
            if (k < bar_fill)
                bar_pos += snprintf(bar + bar_pos, sizeof(bar) - bar_pos, "█");
            else
                bar[bar_pos++] = ' ';
        }
        bar[bar_pos] = '\0';

        char size_str[64];
        dls_human_size(e->size, size_str, sizeof(size_str));

        double fs_pct = (fs_total > 0 && e->size > 0) ? ((double)e->size / (double)fs_total * 100.0) : 0.0;

        char line[PATH_MAX + 200];
        if (e->error)
        {
            snprintf(line, sizeof(line), "%zu) ERR: cannot access %s\n", i + 1, e->name);
        }
        else
        {
            char display_name[PATH_MAX + 4];
            snprintf(display_name, sizeof(display_name), "%s%s", e->name, e->is_dir ? "/" : "");

            if (fs_pct >= 1.0)
                snprintf(line, sizeof(line), "%zu) %-20.20s %s  %8s   (dir: %d%%, fs: %.0f%%)\n",
                         i + 1, display_name, bar, size_str, dir_pct, fs_pct);
            else
                snprintf(line, sizeof(line), "%zu) %-20.20s %s  %8s   (dir: %d%%)\n",
                         i + 1, display_name, bar, size_str, dir_pct);
        }

        send(slot->sock, line, strlen(line), 0);
    }

    send(slot->sock, "EOF\n", 4, 0);
    dls_list_free(&list);
}

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

static bool is_path_under_root(const char *path)
{
    if (!path || !server_root[0])
        return false;

    size_t root_len = strlen(server_root);
    if (root_len == 1 && server_root[0] == '/')
        return path[0] == '/';

    if (strncmp(path, server_root, root_len) != 0)
        return false;

    return path[root_len] == '\0' || path[root_len] == '/';
}

static int delete_path_recursive(const char *target)
{
    struct stat st;
    if (lstat(target, &st) != 0)
        return -1;

    if (S_ISDIR(st.st_mode))
    {
        DIR *dir = opendir(target);
        if (!dir)
            return -1;

        struct dirent *ent;
        while ((ent = readdir(dir)))
        {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;

            char child[PATH_MAX];
            snprintf(child, sizeof(child), "%s/%s", target, ent->d_name);

            if (delete_path_recursive(child) != 0)
            {
                closedir(dir);
                return -1;
            }
        }

        closedir(dir);

        if (rmdir(target) != 0)
            return -1;
    }
    else
    {
        if (remove(target) != 0)
            return -1;
    }

    return 0;
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
    // [수정됨] cd, mkdir 인식 로직 개선 (공백 유무와 상관없이 처리)
    if (strncmp(buf, "cd", 2) == 0 && (buf[2] == ' ' || buf[2] == '\0'))
    {
        char *path = (char*)buf + 2;
        while (*path == ' ') path++; // 공백 건너뛰기

        if (*path == '\0') {
             // 경로가 없으면 에러 (혹은 홈 디렉토리로 이동 구현 가능)
             send(slot->sock, "ERR: path required\n", 19, 0);
        }
        else if (chdir(path) == 0)
            send(slot->sock, "OK: changed directory\n", 22, 0);
        else
            send(slot->sock, "ERR: invalid path\n", 18, 0);
    }
    else if (strncmp(buf, "mkdir", 5) == 0 && (buf[5] == ' ' || buf[5] == '\0'))
    {
        char *path = (char*)buf + 5;
        while (*path == ' ') path++;

        if (*path == '\0') {
             send(slot->sock, "ERR: path required\n", 19, 0);
        }
        else if (mkdir(path, 0755) == 0)
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
    else if (strncmp(buf, "dls", 3) == 0 && (buf[3] == ' ' || buf[3] == '\0'))
    {
        handle_dls(slot, buf + 3);
    }
    else if (strncasecmp(buf, "UPLOAD PLAN", 11) == 0)
    {
        handle_upload_plan(slot, buf);
    }
    else if (strncasecmp(buf, "UPLOAD START", 12) == 0)
    {
        handle_upload_start(slot, buf);
    }
    else if (strncasecmp(buf, "DELETE ", 7) == 0)
    {
        const char *raw_path = buf + 7;
        while (*raw_path == ' ')
            raw_path++;

        if (!*raw_path)
        {
            const char *msg = "ERR DELETE : invalid path\n";
            send(slot->sock, msg, strlen(msg), 0);
            return;
        }

        char resolved[PATH_MAX];
        if (!realpath(raw_path, resolved))
        {
            char msg[256];
            snprintf(msg, sizeof(msg), "ERR DELETE %s : %s\n", raw_path, strerror(errno));
            send(slot->sock, msg, strlen(msg), 0);
            return;
        }

        if (!is_path_under_root(resolved) || strcmp(resolved, server_root) == 0)
        {
            const char *msg = "ERR INVALID_PATH\n";
            send(slot->sock, msg, strlen(msg), 0);
            return;
        }

        if (delete_path_recursive(resolved) == 0)
        {
            char msg[PATH_MAX + 32];
            snprintf(msg, sizeof(msg), "OK DELETE %s\n", resolved);
            send(slot->sock, msg, strlen(msg), 0);
        }
        else
        {
            char msg[PATH_MAX + 64];
            snprintf(msg, sizeof(msg), "ERR DELETE %s : %s\n", resolved, strerror(errno));
            send(slot->sock, msg, strlen(msg), 0);
        }
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
    else
    {
        getcwd(server_root, sizeof(server_root));
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