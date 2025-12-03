#define _XOPEN_SOURCE 700
#include "dir_manager.h"
#include "utils.h"
#include "socket_client.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include <unistd.h>

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
    dl->top_index = 0; // 스크롤 초기화
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
        char cd_cmd[PATH_MAX + 4];
        snprintf(cd_cmd, sizeof(cd_cmd), "cd %s", cwd_abs);
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
            if (line[0] == 'd')
            { 
                char name[256];
                if (sscanf(line, "%*s %*s %*s %*s %*s %*s %*s %*s %255s", name) == 1)
                {
                    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                    {
                        line = strtok(NULL, "\n");
                        continue;
                    }

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
    dl->top_index = 0; // 목록 갱신 시 스크롤 맨 위로
}

void dirlist_draw(WINDOW *win, DirList *dl, bool focused)
{
    werase(win);
    box(win, 0, 0);
    if (focused) wattron(win, A_BOLD | A_STANDOUT);
    mvwprintw(win, 0, 2, " 현재위치 (F1): %s ", dl->cwd);
    if (focused) wattroff(win, A_BOLD | A_STANDOUT);

    int h, w;
    getmaxyx(win, h, w);
    int list_h = h - 2; // 실제 목록이 표시될 수 있는 높이

    if (list_h < 1) { // 창이 너무 작으면 갱신만 하고 리턴
        wrefresh(win);
        return;
    }

    // --- 스크롤 계산 로직 ---
    if (dl->selected < dl->top_index) {
        dl->top_index = dl->selected;
    } 
    else if (dl->selected >= dl->top_index + list_h) {
        dl->top_index = dl->selected - list_h + 1;
    }
    
    // 안전장치: top_index 범위 보정
    if (dl->top_index > dl->count - list_h) dl->top_index = dl->count - list_h;
    if (dl->top_index < 0) dl->top_index = 0;
    // ----------------------

    for (int i = 0; i < list_h; i++)
    {
        int idx = dl->top_index + i;
        if (idx >= dl->count) break;

        const char *name = dl->items[idx];
        int sel = (idx == dl->selected);
        
        if (sel && focused) wattron(win, A_REVERSE);
        mvwprintw(win, i + 1, 2, "%c %.*s", sel ? '>' : ' ', w - 4, name);
        if (sel && focused) wattroff(win, A_REVERSE);
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
    fl->top_index = 0;
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
            // 파일(-)과 디렉토리(d) 모두 허용
            if (line[0] == '-' || line[0] == 'd')
            { 
                char name[256];
                if (sscanf(line, "%*s %*s %*s %*s %*s %*s %*s %*s %255s", name) == 1)
                {
                    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                    {
                        line = strtok(NULL, "\n");
                        continue;
                    }
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
    fl->top_index = 0;
}

void filelist_draw(WINDOW *win, FileList *fl, bool focused)
{
    werase(win);
    box(win, 0, 0);
    if (focused) wattron(win, A_BOLD | A_STANDOUT);
    mvwprintw(win, 0, 2, " 선택한 디렉토리 (F2): %s ", fl->base);
    if (focused) wattroff(win, A_BOLD | A_STANDOUT);

    int h, w;
    getmaxyx(win, h, w);
    int list_h = h - 2;

    if (list_h < 1) {
        wrefresh(win);
        return;
    }

    // --- 스크롤 계산 로직 ---
    if (fl->selected < fl->top_index) {
        fl->top_index = fl->selected;
    } 
    else if (fl->selected >= fl->top_index + list_h) {
        fl->top_index = fl->selected - list_h + 1;
    }
    
    if (fl->top_index > fl->count - list_h) fl->top_index = fl->count - list_h;
    if (fl->top_index < 0) fl->top_index = 0;
    // ----------------------

    for (int i = 0; i < list_h; i++)
    {
        int idx = fl->top_index + i;
        if (idx >= fl->count) break;

        const char *name = fl->items[idx];
        int sel = (idx == fl->selected);
        if (sel && focused) wattron(win, A_REVERSE);
        mvwprintw(win, i + 1, 2, "%c %.*s", sel ? '>' : ' ', w - 4, name);
        if (sel && focused) wattroff(win, A_REVERSE);
    }
    wrefresh(win);
}

int socket_is_connected(void)
{
    return (sockfd >= 0);
}

/* ============================================================
   로컬 파일/디렉토리 브라우저 (업로드 모드)
   ============================================================ */

static int cmp_local_entry(const void *a, const void *b)
{
    const LocalEntry *ea = (const LocalEntry *)a;
    const LocalEntry *eb = (const LocalEntry *)b;
    return strcasecmp(ea->name, eb->name);
}

static void lb_push(LocalBrowser *lb, const char *name, bool is_dir)
{
    if (lb->count + 1 > lb->cap)
    {
        lb->cap = (lb->cap == 0) ? 32 : lb->cap * 2;
        lb->items = realloc(lb->items, sizeof(LocalEntry) * lb->cap);
    }
    lb->items[lb->count].name = strdup(name);
    lb->items[lb->count].is_dir = is_dir;
    lb->count++;
}

void localbrowser_init(LocalBrowser *lb)
{
    memset(lb, 0, sizeof(*lb));
    lb->selected = 0;
    lb->top_index = 0;
}

void localbrowser_free(LocalBrowser *lb)
{
    for (int i = 0; i < lb->count; i++)
        free(lb->items[i].name);
    free(lb->items);
    memset(lb, 0, sizeof(*lb));
}

int localbrowser_scan(LocalBrowser *lb, const char *cwd)
{
    localbrowser_free(lb);
    localbrowser_init(lb);

    if (!cwd || !*cwd)
    {
        if (!getcwd(lb->cwd, sizeof(lb->cwd)))
            snprintf(lb->cwd, sizeof(lb->cwd), ".");
    }
    else
        snprintf(lb->cwd, sizeof(lb->cwd), "%s", cwd);

    DIR *d = opendir(lb->cwd);
    if (!d)
        return -1;

    struct dirent *e;
    while ((e = readdir(d)))
    {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;

        char p[PATH_MAX];
        path_join(p, lb->cwd, e->d_name);
        bool is_dir = is_directory(p);
        lb_push(lb, e->d_name, is_dir);
    }
    closedir(d);

    qsort(lb->items, lb->count, sizeof(LocalEntry), cmp_local_entry);
    lb->selected = (lb->count > 0) ? 0 : -1;
    lb->top_index = 0;
    return lb->count;
}

void localbrowser_draw(WINDOW *win, LocalBrowser *lb, bool focused)
{
    werase(win);
    box(win, 0, 0);
    if (focused) wattron(win, A_BOLD | A_STANDOUT);
    mvwprintw(win, 0, 2, " 로컬 선택 (F2): %s ", lb->cwd);
    if (focused) wattroff(win, A_BOLD | A_STANDOUT);

    int h, w;
    getmaxyx(win, h, w);
    int list_h = h - 2;

    if (list_h < 1) {
        wrefresh(win);
        return;
    }

    // --- 스크롤 계산 로직 ---
    if (lb->selected < lb->top_index) {
        lb->top_index = lb->selected;
    } 
    else if (lb->selected >= lb->top_index + list_h) {
        lb->top_index = lb->selected - list_h + 1;
    }

    if (lb->top_index > lb->count - list_h) lb->top_index = lb->count - list_h;
    if (lb->top_index < 0) lb->top_index = 0;
    // ----------------------

    for (int i = 0; i < list_h; i++)
    {
        int idx = lb->top_index + i;
        if (idx >= lb->count) break;

        const LocalEntry *ent = &lb->items[idx];
        int sel = (idx == lb->selected);
        if (sel && focused) wattron(win, A_REVERSE);
        mvwprintw(win, i + 1, 2, "%c [%c] %.*s", sel ? '>' : ' ', ent->is_dir ? 'D' : 'F', w - 7, ent->name);
        if (sel && focused) wattroff(win, A_REVERSE);
    }
    wrefresh(win);
}