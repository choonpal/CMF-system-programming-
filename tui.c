// tui.c (수정된 버전)

#define _XOPEN_SOURCE 700
#include <locale.h>//한글 인코딩
#include <ncurses.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "socket_client.h"


#include "dir_manager.h"
#include "chat_manager.h"
#include "input_manager.h"
#include "utils.h"

#ifdef USE_INOTIFY
#include <sys/inotify.h>
#include <fcntl.h>
#endif

static WINDOW *win_dir, *win_file, *win_chat, *win_input;

typedef struct {
    DirList dl;
    FileList fl;
    ChatState chat;
    FocusArea focus;
} App;

/* =======================================================
   레이아웃 구성
   ======================================================= */
static void layout_create(void) {
    int h,w; getmaxyx(stdscr,h,w);
    int left_w = w/3;
    int right_w = w - left_w;
    int chat_h = h - 3;

    win_dir   = newwin(h/2, left_w, 0, 0);
    win_file  = newwin(h/2, left_w, h/2, 0);
    win_chat  = newwin(chat_h, right_w, 0, left_w);
    win_input = newwin(3, right_w, chat_h, left_w);

    box(win_dir,0,0);   mvwprintw(win_dir,0,2," 현재위치 ");
    box(win_file,0,0);  mvwprintw(win_file,0,2," 선택한 디렉토리 ");
    box(win_chat,0,0);  mvwprintw(win_chat,0,2," 채팅 ");
    box(win_input,0,0); mvwprintw(win_input,0,2," 입력 ");

    wrefresh(win_dir); 
    wrefresh(win_file); 
    wrefresh(win_chat); 
    wrefresh(win_input);
}

/* =======================================================
   초기화 (시작 시 바로 디렉토리+채팅 표시)
   ======================================================= */
static void app_init(App *a) {
    memset(a, 0, sizeof(*a));
    a->focus = FOCUS_DIR;

    // 시작 디렉토리 지정
    const char *start_dir = "/home";
    char absdir[PATH_MAX];
    abspath(absdir, start_dir);

    // 디렉토리 목록 초기화
    dirlist_init(&a->dl);
    dirlist_scan(&a->dl, absdir);

    // 파일 목록 초기화
    filelist_init(&a->fl);
    const char *base_dir = (a->dl.count > 0) ? a->dl.items[a->dl.selected] : absdir;
    filelist_scan(&a->fl, base_dir);

    // 채팅창 초기화
    chat_init(&a->chat, base_dir);

    // 즉시 전체 화면 갱신
    dirlist_draw(win_dir, &a->dl, true);
    filelist_draw(win_file, &a->fl, false);
    chat_draw(win_chat, &a->chat);
    input_draw(win_input);

    // 강제 flush
    wrefresh(win_dir);
    wrefresh(win_file);
    wrefresh(win_chat);
    wrefresh(win_input);

    // 상태바 표시
    status_bar(win_chat, "[Tab] 포커스 이동  [Enter] 선택/전송  [Backspace] 상위  [q] 종료");
}

/* =======================================================
   종료 처리
   ======================================================= */
static void app_free(App *a) {
    dirlist_free(&a->dl);
    filelist_free(&a->fl);
}

/* =======================================================
   포커스 이동
   ======================================================= */
static void change_focus(App *a, int dir) {
    int f = (int)a->focus;
    f = (f + dir + 4) % 4;
    a->focus = (FocusArea)f;
    dirlist_draw(win_dir, &a->dl, a->focus==FOCUS_DIR);
    filelist_draw(win_file, &a->fl, a->focus==FOCUS_FILE);
    chat_draw(win_chat, &a->chat);
}

/* =======================================================
   디렉토리 선택 및 상위 이동
   ======================================================= */
static void open_selected_dir(App *a) {
    if (a->dl.selected < 0 || a->dl.selected >= a->dl.count) return;
    const char *dir_abs = a->dl.items[a->dl.selected];
    filelist_scan(&a->fl, dir_abs);
    filelist_draw(win_file, &a->fl, a->focus==FOCUS_FILE);
    chat_init(&a->chat, dir_abs);
    chat_draw(win_chat, &a->chat);
}

static void go_parent_dir(App *a) {
    char parent[PATH_MAX]; dirname_of(parent, a->dl.cwd);
    if (!is_directory(parent) || strcmp(parent, a->dl.cwd)==0) return;
    dirlist_scan(&a->dl, parent);
    dirlist_draw(win_dir, &a->dl, a->focus==FOCUS_DIR);
    open_selected_dir(a);
}

/* =======================================================
   inotify (Linux용)
   ======================================================= */
#ifdef USE_INOTIFY
static int setup_inotify(const char *path) {
    int fd = inotify_init1(IN_NONBLOCK);
    if (fd < 0) return -1;
    inotify_add_watch(fd, path, IN_MODIFY|IN_CLOSE_WRITE|IN_MOVE_SELF|IN_DELETE_SELF);
    return fd;
}
#endif

/* =======================================================
   메인 루프
   ======================================================= */
int main(void) {
    setlocale(LC_ALL, "");
    initscr(); 
    noecho(); 
    cbreak(); 
    keypad(stdscr, TRUE); 
    curs_set(0);
    timeout(200); // getch() polling 주기
    
    clear();
    refresh();
    
    layout_create();

    App app; 
    app_init(&app);   // ✅ 실행 즉시 바로 화면 표시
    
    refresh();

#ifdef USE_INOTIFY
    int inofd = setup_inotify(app.chat.log_path);
#endif

    char linebuf[4096]={0};

    for (;;) {
        // 외부 로그 변경 감지
        chat_check_update(&app.chat);
        if (app.chat.dirty) {
            app.chat.dirty = 0;
            chat_draw(win_chat, &app.chat);
        }

#ifdef USE_INOTIFY
        if (inofd >= 0) {
            char buf[1024];
            int n = read(inofd, buf, sizeof(buf));
            if (n > 0) app.chat.dirty = 1;
        }
#endif

        int ch = getch();
        if (ch == ERR) continue;
        if (ch == 'q' || ch == 'Q') break;

        switch (app.focus) {
        case FOCUS_DIR:
            if (ch == KEY_UP)   { if (app.dl.selected>0) app.dl.selected--; dirlist_draw(win_dir,&app.dl,true); }
            else if (ch==KEY_DOWN){ if (app.dl.selected<app.dl.count-1) app.dl.selected++; dirlist_draw(win_dir,&app.dl,true); }
            else if (ch=='\n' || ch==KEY_RIGHT) { open_selected_dir(&app); app.focus = FOCUS_FILE; filelist_draw(win_file,&app.fl,true); }
            else if (ch==KEY_BACKSPACE || ch==127) { go_parent_dir(&app); }
            break;

        case FOCUS_FILE:
            if (ch == KEY_UP)   { if (app.fl.selected>0) app.fl.selected--; filelist_draw(win_file,&app.fl,true); }
            else if (ch==KEY_DOWN){ if (app.fl.selected<app.fl.count-1) app.fl.selected++; filelist_draw(win_file,&app.fl,true); }
            else if (ch=='\n') {
                if (app.fl.selected>=0 && app.fl.selected<app.fl.count) {
                    char tgt[PATH_MAX]; path_join(tgt, app.fl.base, app.fl.items[app.fl.selected]);
                    if (is_directory(tgt)) {
                        dirlist_scan(&app.dl, tgt);
                        dirlist_draw(win_dir, &app.dl, app.focus==FOCUS_DIR);
                        open_selected_dir(&app);
                    } else {
                        status_bar(win_chat, "파일은 열지 않고, 채팅의 기준 경로만 유지합니다.");
                    }
                }
            } else if (ch==KEY_LEFT) { app.focus = FOCUS_DIR; dirlist_draw(win_dir,&app.dl,true); }
            break;

        case FOCUS_CHAT:
            if (ch=='\t' || ch==KEY_RIGHT || ch=='\n') { app.focus = FOCUS_INPUT; input_draw(win_input); }
            else if (ch==KEY_LEFT) { app.focus = FOCUS_FILE; filelist_draw(win_file,&app.fl,true); }
            break;

        case FOCUS_INPUT:
            if (ch=='\n') {
                // 빈 줄 방지 (원래 로직)
            } 
            else {
                // ----------------------------------------------------
                // [수정 시작] 
                // ----------------------------------------------------
                
                input_draw(win_input);
                wmove(win_input,1,4);
                linebuf[0]='\0';
                input_capture_line(win_input, linebuf, sizeof(linebuf));

                // [수정 1] 빈 줄은 아무것도 안 함
                if (strlen(linebuf) == 0) {
                    app.focus = FOCUS_CHAT;
                    break; 
                }

                // [수정 2] 명령어든 채팅이든 서버로 전송
                socket_send_cmd(linebuf);
                
                // [수정 3] 내 메시지를 내 화면(로컬 로그)에 먼저 표시
                // (서버는 보낸 사람에겐 다시 브로드캐스트 안 함)
                chat_append(&app.chat, safe_username(), linebuf);

                // [수정 4] 서버의 응답을 받아서 처리
                char response[2048];
                // 'ls' 명령인지 미리 확인 (ls는 여러 줄의 응답을 가질 수 있음)
                bool is_ls_cmd = (strncmp(linebuf, "ls", 2) == 0); 

                while (socket_recv_response(response, sizeof(response)) > 0) {
                    
                    // 서버가 보낸 채팅 확인("ACK:") 응답은 화면에 띄우지 않고 종료
                    if (strncmp(response, "ACK:", 4) == 0) {
                        break;
                    }

                    // "ls" 결과나 "OK/ERR" 같은 명령어 응답은 화면에 띄움
                    chat_append(&app.chat, "server", response);

                    // 'ls' 명령이 *아닌데* (cd, mkdir) "OK"나 "ERR"를 받으면 응답이 끝난 것이므로 종료
                    if (!is_ls_cmd && (strstr(response, "OK") || strstr(response, "ERR"))) {
                        break;
                    }
                }
                
                app.chat.dirty = 1;
                
                // ----------------------------------------------------
                // [수정 끝]
                // ----------------------------------------------------
                
                // [수정 5] 아래의 중복된 chat_append() 및 if/else 로직 제거
                /*
                if (strncmp(linebuf, "cd ", 3) == 0 ||  strncmp(linebuf, "mkdir ", 6) == 0 ||strncmp(linebuf, "ls", 2) == 0) {
                    ...
                }
                else {
                    chat_append(&app.chat, safe_username(), linebuf);
                }
                if (strlen(linebuf)>0) {
                    chat_append(&app.chat, safe_username(), linebuf);
                    app.chat.dirty = 1;
                }
                */

                app.focus = FOCUS_CHAT;
            }
            break;
        }

        if (ch=='\t') change_focus(&app, +1);
        else if (ch==KEY_BTAB) change_focus(&app, -1);
    }

    app_free(&app);
    endwin();
    return 0;
}