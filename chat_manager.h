#ifndef CHAT_MANAGER_H
#define CHAT_MANAGER_H

#include <ncurses.h>
#include <limits.h>
#include <stdbool.h>
#include <time.h>

typedef struct {
    char dir_abs[PATH_MAX];   // 현재 채팅 대상 디렉토리(절대경로)
    char log_path[PATH_MAX];  // 로그 파일 경로
    time_t last_mtime;        // 마지막 수정 시간 (변경 감지용)
    volatile int dirty;       // 외부 변경 플래그
} ChatState;

void chat_init(ChatState *st, const char *dir_abs);
void chat_draw(WINDOW *win, const ChatState *st);
bool chat_append(const ChatState *st, const char *user, const char *msg);
void chat_check_update(ChatState *st); // 파일 변경 감지

#endif
