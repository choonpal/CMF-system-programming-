#ifndef DIR_MANAGER_H
#define DIR_MANAGER_H

#include <ncurses.h>
#include <limits.h>
#include <stdbool.h>

typedef struct {
    char **items;    // 디렉토리(왼쪽 상단) 목록: 절대경로
    int count, cap;
    int selected;    // 포커스된 인덱스
    int top_index;   // [추가] 화면 스크롤 시작 위치
    char cwd[PATH_MAX];
} DirList;

typedef struct {
    char **items;    // 파일/하위디렉토리(왼쪽 하단) 목록: 이름(상대)
    int count, cap;
    int selected;
    int top_index;   // [추가] 화면 스크롤 시작 위치
    char base[PATH_MAX]; // 기준 절대경로
} FileList;

typedef struct {
    char *name;
    bool is_dir;
} LocalEntry;

typedef struct {
    LocalEntry *items; // 로컬 파일/디렉토리 목록
    int count, cap;
    int selected;
    int top_index;     // [추가] 화면 스크롤 시작 위치
    char cwd[PATH_MAX];
} LocalBrowser;

void dirlist_init(DirList *dl);
void dirlist_free(DirList *dl);
void dirlist_scan(DirList *dl, const char *cwd_abs);
void dirlist_draw(WINDOW *win, DirList *dl, bool focused); // const 제거 (내부 변수 수정 필요)

void filelist_init(FileList *fl);
void filelist_free(FileList *fl);
void filelist_scan(FileList *fl, const char *dir_abs);
void filelist_draw(WINDOW *win, FileList *fl, bool focused); // const 제거

int socket_is_connected(void);

void localbrowser_init(LocalBrowser *lb);
void localbrowser_free(LocalBrowser *lb);
int localbrowser_scan(LocalBrowser *lb, const char *cwd);
void localbrowser_draw(WINDOW *win, LocalBrowser *lb, bool focused); // const 제거

#endif