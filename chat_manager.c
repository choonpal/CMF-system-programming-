#define _XOPEN_SOURCE 700
#include "chat_manager.h"
#include "utils.h"
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void ensure_log_ready(const char *path) {
    char dir[PATH_MAX]; snprintf(dir, sizeof(dir), "%s", path);
    // 마지막 '/' 이전을 디렉토리로 보고 생성
    for (int i=strlen(dir)-1;i>=0;--i) {
        if (dir[i]=='/') { dir[i]='\0'; break; }
    }
    ensure_dir(dir);
    // 파일 없으면 생성
    FILE *f = fopen(path, "a"); if (f) fclose(f);
}

void chat_init(ChatState *st, const char *dir_abs) {
    memset(st, 0, sizeof(*st));
    snprintf(st->dir_abs, sizeof(st->dir_abs), "%s", dir_abs);
    make_log_path(st->log_path, dir_abs);
    ensure_log_ready(st->log_path);
    struct stat s;
    st->last_mtime = (stat(st->log_path, &s)==0)? s.st_mtime : 0;
    st->dirty = 1;
}

static void draw_centered(WINDOW *win, int row, const char *msg) {
    int h,w; getmaxyx(win,h,w);
    int x = (w - (int)strlen(msg)) / 2;
    if (x<1) x=1;
    mvwprintw(win, row, x, "%s", msg);
}

void chat_draw(WINDOW *win, const ChatState *st) {
    werase(win); box(win,0,0);
    mvwprintw(win,0,2," 채팅: %s ", st->dir_abs);

    FILE *fp = fopen(st->log_path, "r");
    int h,w; getmaxyx(win,h,w);
    int maxlines = h-2;
    if (!fp) {
        draw_centered(win, h/2, "(로그 파일을 열 수 없습니다)");
        wrefresh(win); return;
    }
    // 최근 maxlines줄만 출력
    char **lines = calloc(maxlines, sizeof(char*));
    int cnt=0;
    char buf[8192];
    while (fgets(buf, sizeof(buf), fp)) {
        if (cnt < maxlines) {
            lines[cnt++] = strdup(buf);
        } else {
            free(lines[0]);
            memmove(&lines[0], &lines[1], sizeof(char*)*(maxlines-1));
            lines[maxlines-1] = strdup(buf);
        }
    }
    fclose(fp);

    for (int i=0;i<cnt;i++) {
        // 개행 제거
        char *s = lines[i]; size_t L = strlen(s);
        if (L && (s[L-1]=='\n' || s[L-1]=='\r')) s[L-1]='\0';
        mvwprintw(win, i+1, 1, "%.*s", w-2, s);
        free(lines[i]);
    }
    free(lines);
    wrefresh(win);
}

bool chat_append(const ChatState *st, const char *user, const char *msg) {
    FILE *fp = fopen(st->log_path, "a");
    if (!fp) return false;
    time_t now = time(NULL);
    struct tm tm; localtime_r(&now, &tm);
    char ts[64]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(fp, "[%s] %s: %s\n", ts, user?user:"user", msg?msg:"");
    fclose(fp);
    return true;
}

void chat_check_update(ChatState *st) {
    struct stat s;
    if (stat(st->log_path, &s)==0) {
        if (s.st_mtime != st->last_mtime) {
            st->last_mtime = s.st_mtime;
            st->dirty = 1;
        }
    }
}
