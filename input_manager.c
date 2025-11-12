#define _XOPEN_SOURCE 700
#include "input_manager.h"
#include <string.h>

void input_draw(WINDOW *win) {
    werase(win); box(win,0,0);
    mvwprintw(win,0,2," 입력 ");
    mvwprintw(win,1,2,"> ");
    wmove(win,1,4);
    wrefresh(win);
}

int input_capture_line(WINDOW *win, char *out, int maxlen) {
    // 호출 전: 커서 위치는 1,4
    echo();
    wgetnstr(win, out, maxlen-1);
    noecho();
    return (int)strlen(out);
}

void status_bar(WINDOW *chat_win, const char *msg) {
    int h,w; getmaxyx(chat_win,h,w);
    mvwprintw(chat_win, h-1, 2, "%-*s", w-4, msg?msg:"");
    wrefresh(chat_win);
}

const char* focus_name(FocusArea f) {
    switch(f) {
        case FOCUS_DIR: return "DIR";
        case FOCUS_FILE: return "FILES";
        case FOCUS_CHAT: return "CHAT";
        case FOCUS_INPUT: return "INPUT";
        default: return "?";
    }
}
