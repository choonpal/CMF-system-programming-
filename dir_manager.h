#ifndef DIR_MANAGER_H
#define DIR_MANAGER_H

#include <ncurses.h>
#include <limits.h>
#include <stdbool.h>

// [추가] 파일/폴더 정보를 담는 구조체 정의
typedef struct {
    char *name;
    bool is_dir; // 폴더인지 여부 (stat 역할)
} FileEntry;

typedef struct {
    char **items;    // (DirList용, 변경 없음)
    int count, cap;
    int selected;
    int top_index;
    char cwd[PATH_MAX];
} DirList;

typedef struct {
    FileEntry *items; // [변경] char** -> FileEntry* (이름 + 속성)
    int count, cap;
    int selected;
    int top_index;
    char base[PATH_MAX]; 
} FileList;

typedef struct {
    char *name;
    bool is_dir;
} LocalEntry;

typedef struct {
    LocalEntry *items; 
    int count, cap;
    int selected;
    int top_index;
    char cwd[PATH_MAX];
} LocalBrowser;

void dirlist_init(DirList *dl);
void dirlist_free(DirList *dl);
void dirlist_scan(DirList *dl, const char *cwd_abs);
void dirlist_draw(WINDOW *win, DirList *dl, bool focused);

void filelist_init(FileList *fl);
void filelist_free(FileList *fl);
void filelist_scan(FileList *fl, const char *dir_abs);
void filelist_draw(WINDOW *win, FileList *fl, bool focused);

int socket_is_connected(void);

void localbrowser_init(LocalBrowser *lb);
void localbrowser_free(LocalBrowser *lb);
int localbrowser_scan(LocalBrowser *lb, const char *cwd);
void localbrowser_draw(WINDOW *win, LocalBrowser *lb, bool focused);

#endif