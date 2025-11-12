#ifndef DIR_MANAGER_H
#define DIR_MANAGER_H

#include <ncurses.h>
#include <limits.h>
#include <stdbool.h>

typedef struct {
    char **items;    // 디렉토리(왼쪽 상단) 목록: 절대경로
    int count, cap;
    int selected;    // 포커스된 인덱스
    char cwd[PATH_MAX];
} DirList;

typedef struct {
    char **items;    // 파일/하위디렉토리(왼쪽 하단) 목록: 이름(상대)
    int count, cap;
    int selected;
    char base[PATH_MAX]; // 기준 절대경로
} FileList;

void dirlist_init(DirList *dl);
void dirlist_free(DirList *dl);
void dirlist_scan(DirList *dl, const char *cwd_abs);
void dirlist_draw(WINDOW *win, const DirList *dl, bool focused);

void filelist_init(FileList *fl);
void filelist_free(FileList *fl);
void filelist_scan(FileList *fl, const char *dir_abs);
void filelist_draw(WINDOW *win, const FileList *fl, bool focused);
int socket_is_connected(void);

#endif
