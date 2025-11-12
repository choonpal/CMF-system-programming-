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

void input_draw(WINDOW *win);
int  input_capture_line(WINDOW *win, char *out, int maxlen); // Enter로 종료
void status_bar(WINDOW *chat_win, const char *msg);
const char* focus_name(FocusArea f);

#endif
