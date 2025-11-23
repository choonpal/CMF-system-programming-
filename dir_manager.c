#define _XOPEN_SOURCE 700
#include "dir_manager.h"
#include "utils.h"
#include "socket_client.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>

#define LS_END_MARKER "ENDLS\n"

/* ============================================================
   작은 벡터 유틸
   ============================================================ */
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

/* ============================================================
   공통: 서버 연결 감지 + ls 전체 받는 함수
   ============================================================ */
extern int sockfd;

int socket_is_connected(void)
{
    return (sockfd >= 0);
}

// 서버에서 ls 결과를 ENDLS 마커까지 모두 받아서 recvbuf에 저장
static void recv_ls_all(char *recvbuf, size_t bufsize)
{
    recvbuf[0] = '\0';
    char chunk[4096];

    for (;;)
    {
        int n = socket_recv_response(chunk, sizeof(chunk));
        if (n <= 0)
            break;

        if (strlen(recvbuf) + (size_t)n + 1 >= bufsize)
        {
            strncat(recvbuf, chunk, bufsize - strlen(recvbuf) - 1);
            break;
        }

        strncat(recvbuf, chunk, bufsize - strlen(recvbuf) - 1);

        // ENDLS 마커가 보이면 거기까지만 사용
        if (strstr(recvbuf, LS_END_MARKER))
            break;
    }

    // 마커 이후는 잘라냄
    char *p = strstr(recvbuf, LS_END_MARKER);
    if (p)
        *p = '\0';
}

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
        // 🌐 서버에 현재 디렉토리의 디렉토리 목록 요청
        socket_send_cmd("ls -al");

        char recvbuf[16384];
        recv_ls_all(recvbuf, sizeof(recvbuf));

        // 서버에서 받은 결과 파싱
        char *line = strtok(recvbuf, "\n");
        while (line)
        {
            // 첫 줄 "total N" 같은 건 건너뜀
            if (line[0] == 'd')
            { // 디렉토리만 표시
                char perms[11];
                char name[256];

                // -rw-r--r-- 1 user group size date name
                if (sscanf(line, "%10s %*s %*s %*s %*s %*s %*s %*s %255s",
                           perms, name) == 2)
                {
                    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                    {
                        line = strtok(NULL, "\n");
                        continue;
                    }
                    vec_push(&dl->items, &dl->count, &dl->cap, name);
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
   하단: 선택 디렉토리의 하위 파일 목록
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
        // 🌐 서버에 파일 목록 요청
        socket_send_cmd("ls -al");

        char recvbuf[16384];
        recv_ls_all(recvbuf, sizeof(recvbuf));

        char *line = strtok(recvbuf, "\n");
        while (line)
        {
            if (line[0] == '-')
            { // 일반 파일만
                char perms[11];
                char name[256];
                if (sscanf(line, "%10s %*s %*s %*s %*s %*s %*s %*s %255s",
                           perms, name) == 2)
                {
                    vec_push(&fl->items, &fl->count, &fl->cap, name);
                }
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
