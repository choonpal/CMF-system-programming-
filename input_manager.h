#ifndef INPUT_MANAGER_H
#define INPUT_MANAGER_H

#include <ncurses.h>
#include "chat_manager.h"

typedef enum {
    FOCUS_DIR = 0,
    FOCUS_FILE = 1,
    FOCUS_CHAT = 2,
    FOCUS_INPUT = 3
} FocusArea;

void input_draw(WINDOW *win, bool focused);
// Enter, F1~F3, Shift+Tab 등의 특수키로 입력 종료 후 종료 키를 반환
int  input_capture_line(WINDOW *win, char *out, int maxlen, int first_ch);
void status_bar(WINDOW *chat_win, const char *msg);
const char* focus_name(FocusArea f);

#endif
