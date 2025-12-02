#define _XOPEN_SOURCE 700
#include "input_manager.h"
#include <string.h>
#include <ctype.h>

void input_draw(WINDOW *win, bool focused) {
    werase(win); box(win,0,0);
    if (focused) wattron(win, A_BOLD | A_STANDOUT);
    // BUGFIX: F3만 입력창으로 포커스 이동 안내
    mvwprintw(win,0,2," 입력 (F3, Tab) ");
    if (focused) wattroff(win, A_BOLD | A_STANDOUT);
    mvwprintw(win,1,2,"> ");
    wmove(win,1,4);
    wrefresh(win);
}

int input_capture_line(WINDOW *win, char *out, int maxlen, int first_ch) {
    // BUGFIX: 첫 글자가 사라지던 문제를 포함해 특수키를 직접 처리
    keypad(win, TRUE);
    int pos = 0;
    int ch = first_ch;

    // 입력 영역 초기화 (기존 내용 지우기)
    int y = 1, x = 4;
    int h, w; getmaxyx(win, h, w); (void)h;
    for (int cx = x; cx < w - 1; cx++) {
        mvwaddch(win, y, cx, ' ');
    }
    wmove(win, y, x);
    wrefresh(win);

    while (1) {
        if (ch == -1)
            ch = wgetch(win);

        // 입력 종료 조건 (특수키는 버퍼에 넣지 않고 즉시 종료)
        if (ch == '\n' || ch == KEY_ENTER || ch == KEY_F(1) || ch == KEY_F(2) ||
            ch == KEY_F(3) || ch == KEY_BTAB) {
            out[pos] = '\0';
            return ch;
        }

        // BUGFIX: F4는 더 이상 포커스를 바꾸지 않고 입력을 계속하도록 무시
        if (ch == KEY_F(4)) {
            ch = -1;
            continue;
        }

        if ((ch == KEY_BACKSPACE || ch == 127 || ch == '\b') && pos > 0) {
            pos--;
            mvwaddch(win, y, x + pos, ' ');
            wmove(win, y, x + pos);
            wrefresh(win);
        }
        else if ((isprint(ch) || ch == '\t') && pos < maxlen - 1) {
            out[pos++] = (char)ch;
            mvwaddch(win, y, x + pos - 1, ch);
            wrefresh(win);
        }

        ch = -1; // 다음 루프에서 wgetch 사용
    }
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
