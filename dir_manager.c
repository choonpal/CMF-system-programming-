#define _XOPEN_SOURCE 700
#include "dir_manager.h"
#include "utils.h"
#include "socket_client.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>

static void vec_push(char ***arr, int *count, int *cap, const char *s)
{
    if (*count + 1 > *cap)
    {
        *cap = (*cap == 0) ? 16 : (*cap * 2);
        *arr = realloc(*arr, sizeof(char *) * (*cap));
    }
    (*arr)[*count] = strdup(s);
    (*count)++;
}

static int cmp_str(const void *a, const void *b)
{
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;
    return strcasecmp(sa, sb);
}

static bool strip_eof_marker(char *buffer)
{
    char *marker = strstr(buffer, "\nEOF\n");
    if (marker)
    {
        *marker = '\0';
        return true;
    }

    // EOF가 버퍼의 시작에 바로 붙어 들어올 수도 있음
    marker = strstr(buffer, "EOF\n");
    if (marker == buffer)
    {
        *marker = '\0';
        return true;
    }

    return false;
}

/* ============================================================
   공통: 서버 연결 감지 함수
   ============================================================ */
extern int socket_is_connected(void); // socket_client.c에 구현 필요

/* ============================================================
   상단: 현재 위치의 디렉토리 목록
   ============================================================ */

void dirlist_init(DirList *dl)
{
    memset(dl, 0, sizeof(*dl));
    dl->selected = 0;
}

void dirlist_free(DirList *dl)
{
    for (int i = 0; i < dl->count; i++)
        free(dl->items[i]);
    free(dl->items);
    memset(dl, 0, sizeof(*dl));
}

void dirlist_scan(DirList *dl, const char *cwd_abs)
{
    dirlist_free(dl);
    dirlist_init(dl);
    snprintf(dl->cwd, sizeof(dl->cwd), "%s", cwd_abs);

    if (socket_is_connected())
    {
        // 🌐 서버에 요청: 원격에서도 실제 경로를 맞춰주기 위해 cd 후 ls 수행
        char cd_cmd[PATH_MAX + 4];
        snprintf(cd_cmd, sizeof(cd_cmd), "cd %s", cwd_abs);
        socket_send_cmd(cd_cmd);

        // cd 결과는 단순 확인만 하고 무시(OK/ERR 문구만 받아서 비워줌)
        char cd_resp[512];
        while (1)
        {
            int rn = socket_recv_response(cd_resp, sizeof(cd_resp));
            if (rn <= 0)
                break;
            cd_resp[rn] = '\0';
            if (strstr(cd_resp, "OK") || strstr(cd_resp, "ERR"))
                break;
        }

        // 경로가 맞춰진 상태에서 디렉터리 목록 조회
        socket_send_cmd("ls -al");
        char buf[4096] = {0}, recvbuf[8192] = {0};
        while (1)
        {
            int n = socket_recv_response(buf, sizeof(buf));
            if (n <= 0)
                break;
            buf[n] = '\0';
            strncat(recvbuf, buf, sizeof(recvbuf) - strlen(recvbuf) - 1);

            if (strip_eof_marker(recvbuf))
                break;

            if (strlen(recvbuf) >= sizeof(recvbuf) - 1)
                break;
        }

        // 서버에서 받은 결과 파싱
        char *line = strtok(recvbuf, "\n");
        while (line)
        {
            if (line[0] == 'd')
            { // 디렉토리만 표시
                char name[256];
                if (sscanf(line, "%*s %*s %*s %*s %*s %*s %*s %*s %255s", name) == 1)
                {
                    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                    {
                        line = strtok(NULL, "\n");
                        continue;
                    }

                    // 서버 기준 절대경로를 넣어 탐색 시 경로가 꼬이지 않게 함
                    char abs[PATH_MAX];
                    path_join(abs, cwd_abs, name);
                    vec_push(&dl->items, &dl->count, &dl->cap, abs);
                }
            }
            line = strtok(NULL, "\n");
        }
    }
    else
    {
        // 📁 로컬 탐색 모드
        DIR *d = opendir(cwd_abs);
        if (!d)
            return;
        struct dirent *e;
        while ((e = readdir(d)))
        {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            char p[PATH_MAX];
            path_join(p, cwd_abs, e->d_name);
            if (is_directory(p))
                vec_push(&dl->items, &dl->count, &dl->cap, p);
        }
        closedir(d);
    }

    qsort(dl->items, dl->count, sizeof(char *), cmp_str);
    dl->selected = (dl->count > 0) ? 0 : -1;
}

void dirlist_draw(WINDOW *win, const DirList *dl, bool focused)
{
    werase(win);
    box(win, 0, 0);
    mvwprintw(win, 0, 2, " 현재위치: %s ", dl->cwd);
    int h, w;
    getmaxyx(win, h, w);
    for (int i = 0; i < dl->count && i < h - 2; i++)
    {
        const char *name = dl->items[i];
        int sel = (i == dl->selected);
        if (sel && focused)
            wattron(win, A_REVERSE);
        mvwprintw(win, i + 1, 2, "%c %.*s", sel ? '>' : ' ', w - 4, name);
        if (sel && focused)
            wattroff(win, A_REVERSE);
    }
    wrefresh(win);
}

/* ============================================================
   하단: 선택 디렉토리의 하위 파일/폴더 목록
   ============================================================ */

void filelist_init(FileList *fl)
{
    memset(fl, 0, sizeof(*fl));
    fl->selected = 0;
}

void filelist_free(FileList *fl)
{
    for (int i = 0; i < fl->count; i++)
        free(fl->items[i]);
    free(fl->items);
    memset(fl, 0, sizeof(*fl));
}

void filelist_scan(FileList *fl, const char *dir_abs)
{
    filelist_free(fl);
    filelist_init(fl);
    snprintf(fl->base, sizeof(fl->base), "%s", dir_abs);

    if (socket_is_connected())
    {
        // 🌐 서버에 요청: 디렉터리 이동 후 파일 목록 조회
        char cd_cmd[PATH_MAX + 4];
        snprintf(cd_cmd, sizeof(cd_cmd), "cd %s", dir_abs);
        socket_send_cmd(cd_cmd);

        char cd_resp[512];
        while (1)
        {
            int rn = socket_recv_response(cd_resp, sizeof(cd_resp));
            if (rn <= 0)
                break;
            cd_resp[rn] = '\0';
            if (strstr(cd_resp, "OK") || strstr(cd_resp, "ERR"))
                break;
        }

        socket_send_cmd("ls -al");
        char buf[4096] = {0}, recvbuf[8192] = {0};
        while (1)
        {
            int n = socket_recv_response(buf, sizeof(buf));
            if (n <= 0)
                break;
            buf[n] = '\0';
            strncat(recvbuf, buf, sizeof(recvbuf) - strlen(recvbuf) - 1);

            if (strip_eof_marker(recvbuf))
                break;

            if (strlen(recvbuf) >= sizeof(recvbuf) - 1)
                break;
        }
        char *line = strtok(recvbuf, "\n");
        while (line)
        {
            if (line[0] == '-')
            { // 일반 파일만
                char name[256];
                if (sscanf(line, "%*s %*s %*s %*s %*s %*s %*s %*s %255s", name) == 1)
                    vec_push(&fl->items, &fl->count, &fl->cap, name);
            }
            line = strtok(NULL, "\n");
        }
    }
    else
    {
        DIR *d = opendir(dir_abs);
        if (!d)
            return;
        struct dirent *e;
        while ((e = readdir(d)))
        {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            vec_push(&fl->items, &fl->count, &fl->cap, e->d_name);
        }
        closedir(d);
    }

    qsort(fl->items, fl->count, sizeof(char *), cmp_str);
    fl->selected = (fl->count > 0) ? 0 : -1;
}

void filelist_draw(WINDOW *win, const FileList *fl, bool focused)
{
    werase(win);
    box(win, 0, 0);
    mvwprintw(win, 0, 2, " 선택한 디렉토리: %s ", fl->base);
    int h, w;
    getmaxyx(win, h, w);
    for (int i = 0; i < fl->count && i < h - 2; i++)
    {
        const char *name = fl->items[i];
        int sel = (i == fl->selected);
        if (sel && focused)
            wattron(win, A_REVERSE);
        mvwprintw(win, i + 1, 2, "%c %.*s", sel ? '>' : ' ', w - 4, name);
        if (sel && focused)
            wattroff(win, A_REVERSE);
    }
    wrefresh(win);
}
int socket_is_connected(void)
{

    return (sockfd >= 0);
}
