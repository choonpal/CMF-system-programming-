#define _XOPEN_SOURCE 700
#include <locale.h> //한글 인코딩
#include <ncurses.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>

#include "socket_client.h"

#include "dir_manager.h"
#include "chat_manager.h"
#include "input_manager.h"
#include "utils.h"
#include "auth.h"

#ifdef USE_INOTIFY
#include <sys/inotify.h>
#include <fcntl.h>
#endif

// 키 코드 정의
#define KEY_CTRL_Z 26
#define KEY_CTRL_C 3

static WINDOW *win_dir, *win_file, *win_chat, *win_input;

typedef struct
{
    DirList dl;
    FileList fl;
    LocalBrowser lbrowser;
    ChatState chat;
    FocusArea focus;
    FocusArea prev_focus;
    char username[64];
    bool logged_in;
    bool upload_mode;
} App;

static void redraw_all(App *a);
static void change_focus(App *a, FocusArea next);

static int capture_masked_input(WINDOW *win, int y, int x, char *out, int maxlen)
{
    int pos = 0;
    keypad(win, TRUE);
    wmove(win, y, x);
    wrefresh(win);
    while (1)
    {
        int ch = wgetch(win);
        if (ch == '\n' || ch == KEY_ENTER)
        {
            break;
        }
        else if ((ch == KEY_BACKSPACE || ch == 127) && pos > 0)
        {
            pos--;
            mvwaddch(win, y, x + pos, ' ');
            wmove(win, y, x + pos);
            wrefresh(win);
        }
        else if (isprint(ch) && pos < maxlen - 1)
        {
            out[pos++] = (char)ch;
            mvwaddch(win, y, x + pos - 1, '*');
            wrefresh(win);
        }
    }
    out[pos] = '\0';
    return pos;
}

static bool login_prompt(App *app)
{
    int h, w;
    getmaxyx(stdscr, h, w);
    int win_w = 60, win_h = 9;
    int sy = (h - win_h) / 2;
    int sx = (w - win_w) / 2;

    for (int attempt = 0; attempt < 3; attempt++)
    {
        WINDOW *login = newwin(win_h, win_w, sy, sx);
        box(login, 0, 0);
        mvwprintw(login, 0, 2, " 로그인 ");
        mvwprintw(login, 2, 2, "ID : ");
        mvwprintw(login, 3, 2, "PW : ");
        mvwprintw(login, 5, 2, "(demo 계정: admin1 / opslead)");
        wrefresh(login);

        char user[64] = {0};
        char pass[128] = {0};
        echo();
        mvwgetnstr(login, 2, 8, user, (int)sizeof(user) - 1);
        noecho();
        capture_masked_input(login, 3, 8, pass, (int)sizeof(pass));

        char hash[65];
        hash_password(pass, hash);
        memset(pass, 0, sizeof(pass));

        char cmd[256];
        snprintf(cmd, sizeof(cmd), "LOGIN %s %s", user, hash);
        socket_send_cmd(cmd);

        char resp[256];
        int rn = socket_recv_response(resp, sizeof(resp));
        while (rn > 0 && strncmp(resp, "INFO:", 5) == 0)
            rn = socket_recv_response(resp, sizeof(resp));

        if (rn > 0 && strncmp(resp, "OK:", 3) == 0)
        {
            snprintf(app->username, sizeof(app->username), "%s", user);
            app->logged_in = true;
            delwin(login);
            return true;
        }

        const char *err_msg = rn > 0 ? resp : "로그인 응답 없음";
        mvwprintw(login, 6, 2, "서버 응답: %-50.50s", err_msg);
        
        mvwprintw(login, 7, 2, "로그인 실패(%d/3) - 다시 시도", attempt + 1);
        wrefresh(login);
        napms(1000);
        delwin(login);

        if (rn > 0 && strncmp(resp, "ERR: account locked", 20) == 0)
            break;
    }
    return false;
}

/* =======================================================
   레이아웃 구성
   ======================================================= */
static void layout_create(void)
{
    int h, w;
    getmaxyx(stdscr, h, w);
    int left_w = w / 3;
    int right_w = w - left_w;
    int chat_h = h - 3;

    win_dir = newwin(h / 2, left_w, 0, 0);
    win_file = newwin(h / 2, left_w, h / 2, 0);
    win_chat = newwin(chat_h, right_w, 0, left_w);
    win_input = newwin(3, right_w, chat_h, left_w);

    // BUGFIX: 입력창에서도 펑션키/BTAB 등을 정상 감지
    keypad(win_input, TRUE);

    box(win_dir, 0, 0);
    mvwprintw(win_dir, 0, 2, " 현재위치 (F1) ");
    box(win_file, 0, 0);
    mvwprintw(win_file, 0, 2, " 선택/로컬 (F2) ");
    box(win_chat, 0, 0);
    mvwprintw(win_chat, 0, 2, " 채팅 로그 ");
    box(win_input, 0, 0);
    mvwprintw(win_input, 0, 2, " 입력 (F3, Tab) ");

    wrefresh(win_dir);
    wrefresh(win_file);
    wrefresh(win_chat);
    wrefresh(win_input);
}

/* =======================================================
   초기화 (시작 시 바로 디렉토리+채팅 표시)
   ======================================================= */
static void app_init(App *a)
{
    a->focus = FOCUS_DIR;
    a->prev_focus = FOCUS_DIR;
    a->upload_mode = false;
    localbrowser_init(&a->lbrowser);

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
    redraw_all(a);
}

/* =======================================================
   종료 처리
   ======================================================= */
static void app_free(App *a)
{
    dirlist_free(&a->dl);
    filelist_free(&a->fl);
    localbrowser_free(&a->lbrowser);
}

/* =======================================================
   포커스 이동 헬퍼 (제목 줄 강조 및 도움말 업데이트)
   ======================================================= */
static void redraw_all(App *a)
{
    dirlist_draw(win_dir, &a->dl, a->focus == FOCUS_DIR);
    if (a->upload_mode)
        localbrowser_draw(win_file, &a->lbrowser, a->focus == FOCUS_FILE);
    else
        filelist_draw(win_file, &a->fl, a->focus == FOCUS_FILE);
    chat_draw(win_chat, &a->chat, a->focus == FOCUS_CHAT);
    input_draw(win_input, a->focus == FOCUS_INPUT);
    if (a->focus == FOCUS_INPUT)
        wmove(win_input, 1, 4);

    const char *help = a->upload_mode ? "Upload mode: ↑/↓ move, Enter dir, Space select, q cancel" :
                                   "F1:위치 F2:선택 F3:입력 Tab:이동 Ent:이동 Ctrl+Z:상위 q:종료";
    status_bar(win_chat, help);
}

static void change_focus(App *a, FocusArea next)
{
    if (a->upload_mode && next != FOCUS_FILE)
        next = FOCUS_FILE;
    a->focus = next;
    redraw_all(a);
}

/* =======================================================
   디렉토리 선택 및 상위 이동
   ======================================================= */
// 하단 창(Preview) 갱신용
static void open_selected_dir(App *a)
{
    if (a->dl.selected < 0 || a->dl.selected >= a->dl.count)
        return;
    const char *dir_abs = a->dl.items[a->dl.selected];
    filelist_scan(&a->fl, dir_abs);
    chat_init(&a->chat, dir_abs);
    redraw_all(a);
}

static void go_parent_dir(App *a)
{
    char parent[PATH_MAX];
    dirname_of(parent, a->dl.cwd);
    if (socket_is_connected())
    {
        if (strcmp(parent, a->dl.cwd) == 0)
            return;
    }
    else if (!is_directory(parent) || strcmp(parent, a->dl.cwd) == 0)
    {
        return;
    }
    dirlist_scan(&a->dl, parent);
    open_selected_dir(a);
}

static void upload_log(App *a, const char *msg)
{
    chat_append(&a->chat, "system/upload", msg);
    a->chat.dirty = 1;
    chat_draw(win_chat, &a->chat, a->focus == FOCUS_CHAT);
}

static void exit_upload_mode(App *a)
{
    a->upload_mode = false;
    localbrowser_free(&a->lbrowser);
    localbrowser_init(&a->lbrowser);
    change_focus(a, a->prev_focus);
}

static void start_upload_mode(App *a)
{
    a->prev_focus = a->focus;
    a->upload_mode = true;
    a->focus = FOCUS_FILE;

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
        snprintf(cwd, sizeof(cwd), ".");
    if (localbrowser_scan(&a->lbrowser, cwd) >= 0)
        localbrowser_draw(win_file, &a->lbrowser, true);
    else
        upload_log(a, "[system/upload] Failed to read local directory");

    upload_log(a, "[system/upload] Upload mode started");
    upload_log(a, "[system/upload] Bottom-left window shows LOCAL filesystem.");
    upload_log(a, "[system/upload] Use \u2191/\u2193 to move, Enter to enter directory, Space to select target, q to cancel.");
    redraw_all(a);
}

static void send_upload_plan(App *a, const char *path, bool is_dir)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (!base || !*base)
        base = path;

    char base_copy[256];
    snprintf(base_copy, sizeof(base_copy), "%.255s", base);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "UPLOAD PLAN %s %s", is_dir ? "DIR" : "FILE", base_copy);
    socket_send_cmd(cmd);

    char response[256];
    int rn = socket_recv_response(response, sizeof(response));
    if (rn > 0)
    {
        response[rn] = '\0';
        char msg[512];
        snprintf(msg, sizeof(msg), "[system/upload] Server acknowledged upload plan: %s", response);
        upload_log(a, msg);
    }
    else
    {
        upload_log(a, "[system/upload] No response from server for upload plan");
    }
}

static void handle_upload_mode_key(App *a, int ch)
{
    if (ch == 'q' || ch == 'Q')
    {
        upload_log(a, "[system/upload] Upload mode cancelled");
        exit_upload_mode(a);
        return;
    }

    if (a->lbrowser.count == 0)
    {
        if (ch == ' ')
        {
            upload_log(a, "[system/upload] No items to select");
            exit_upload_mode(a);
        }
        return;
    }

    if (ch == KEY_UP)
    {
        if (a->lbrowser.selected > 0)
            a->lbrowser.selected--;
        localbrowser_draw(win_file, &a->lbrowser, true);
    }
    else if (ch == KEY_DOWN)
    {
        if (a->lbrowser.selected < a->lbrowser.count - 1)
            a->lbrowser.selected++;
        localbrowser_draw(win_file, &a->lbrowser, true);
    }
    else if (ch == '\n' || ch == KEY_ENTER)
    {
        if (a->lbrowser.selected >= 0 && a->lbrowser.selected < a->lbrowser.count)
        {
            const LocalEntry *ent = &a->lbrowser.items[a->lbrowser.selected];
            if (ent->is_dir)
            {
                char next[PATH_MAX];
                path_join(next, a->lbrowser.cwd, ent->name);
                if (localbrowser_scan(&a->lbrowser, next) >= 0)
                    localbrowser_draw(win_file, &a->lbrowser, true);
            }
        }
    }
    else if (ch == ' ')
    {
        const LocalEntry *ent = &a->lbrowser.items[a->lbrowser.selected];
        char selected_path[PATH_MAX];
        path_join(selected_path, a->lbrowser.cwd, ent->name);
        char msg[PATH_MAX + 64];
        if (ent->is_dir)
            snprintf(msg, sizeof(msg), "[system/upload] Selected directory: %s", selected_path);
        else
            snprintf(msg, sizeof(msg), "[system/upload] Selected file: %s", selected_path);
        upload_log(a, msg);
        send_upload_plan(a, selected_path, ent->is_dir);
        exit_upload_mode(a);
    }
}

/* =======================================================
   inotify (Linux용)
   ======================================================= */
#ifdef USE_INOTIFY
static int setup_inotify(const char *path)
{
    int fd = inotify_init1(IN_NONBLOCK);
    if (fd < 0)
        return -1;
    inotify_add_watch(fd, path, IN_MODIFY | IN_CLOSE_WRITE | IN_MOVE_SELF | IN_DELETE_SELF);
    return fd;
}
#endif

/* =======================================================
   메인 루프
   ======================================================= */
int main(int argc, char *argv[])
{

    char host[256] = "127.0.0.1";
    int port = 5050;

    if (argc >= 3)
    { 
        strncpy(host, argv[1], sizeof(host) - 1);
        host[sizeof(host) - 1] = '\0';
        int p = atoi(argv[2]);
        if (p > 0)
            port = p;
    }
    else if (argc >= 2)
    { 
        strncpy(host, argv[1], sizeof(host) - 1);
        host[sizeof(host) - 1] = '\0';
        char *colon = strrchr(host, ':');
        if (colon)
        {
            *colon = '\0';
            int p = atoi(colon + 1);
            if (p > 0)
                port = p;
        }
    }

    if (socket_connect_to(host, port) < 0)
    {
        fprintf(stderr, "[tui] connect failed: %s:%d\n", host, port);
        return 1;
    }

    setlocale(LC_ALL, "");
    initscr();
    noecho();
    raw(); // Ctrl+Z 사용을 위해 raw 모드
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(200); 

    clear();
    refresh();

    App app;
    memset(&app, 0, sizeof(app));

    if (!login_prompt(&app))
    {
        endwin();
        socket_close();
        fprintf(stderr, "[tui] login failed\n");
        return 1;
    }

    clear();
    refresh();
    layout_create();

    app_init(&app); 

    refresh();

#ifdef USE_INOTIFY
    int inofd = setup_inotify(app.chat.log_path);
#endif

    char linebuf[4096] = {0};

    for (;;)
    {
        chat_check_update(&app.chat);
        if (app.chat.dirty)
        {
            app.chat.dirty = 0;
            chat_draw(win_chat, &app.chat, app.focus == FOCUS_CHAT);
        }

#ifdef USE_INOTIFY
        if (inofd >= 0)
        {
            char buf[1024];
            int n = read(inofd, buf, sizeof(buf));
            if (n > 0)
                app.chat.dirty = 1;
        }
#endif

        int ch = getch();
        if (ch == ERR)
            continue;

        if (app.upload_mode)
        {
            handle_upload_mode_key(&app, ch);
            if (!app.upload_mode)
                redraw_all(&app);
            continue;
        }

        if (ch == 'q' || ch == 'Q' || ch == KEY_CTRL_C)
            break;

        if (ch == KEY_F(1))
        {
            change_focus(&app, FOCUS_DIR);
            continue;
        }
        if (ch == KEY_F(2))
        {
            change_focus(&app, FOCUS_FILE);
            continue;
        }
        if (ch == KEY_F(3))
        {
            change_focus(&app, FOCUS_INPUT);
            continue;
        }
        if (ch == '\t' && app.focus != FOCUS_INPUT)
        {
            change_focus(&app, FOCUS_INPUT);
            continue;
        }

        switch (app.focus)
        {
        case FOCUS_DIR:
            if (ch == KEY_UP)
            {
                if (app.dl.selected > 0) {
                    app.dl.selected--;
                    // [수정] 위로 이동 시 즉시 하단 미리보기 갱신
                    open_selected_dir(&app);
                }
            }
            else if (ch == KEY_DOWN)
            {
                if (app.dl.selected < app.dl.count - 1) {
                    app.dl.selected++;
                    // [수정] 아래로 이동 시 즉시 하단 미리보기 갱신
                    open_selected_dir(&app);
                }
            }
            // Enter 입력 시 해당 디렉토리로 진입
            else if (ch == '\n')
            {
                if (app.dl.selected >= 0 && app.dl.selected < app.dl.count)
                {
                    char target[PATH_MAX];
                    snprintf(target, sizeof(target), "%s", app.dl.items[app.dl.selected]);
                    
                    // 상단 목록을 선택된 경로로 갱신 (디렉토리 이동)
                    dirlist_scan(&app.dl, target);
                    
                    // 하단 목록 갱신
                    if (app.dl.count > 0) {
                        open_selected_dir(&app);
                    } else {
                        filelist_scan(&app.fl, target);
                        chat_init(&app.chat, target);
                    }
                    redraw_all(&app);
                }
            }
            else if (ch == KEY_RIGHT)
            {
                open_selected_dir(&app);
                change_focus(&app, FOCUS_FILE);
            }
            // Ctrl+Z 추가
            else if (ch == KEY_BACKSPACE || ch == 127 || ch == KEY_CTRL_Z)
            {
                go_parent_dir(&app);
            }
            break;

        case FOCUS_FILE:
            if (ch == KEY_UP)
            {
                if (app.fl.selected > 0)
                    app.fl.selected--;
                filelist_draw(win_file, &app.fl, true);
            }
            else if (ch == KEY_DOWN)
            {
                if (app.fl.selected < app.fl.count - 1)
                    app.fl.selected++;
                filelist_draw(win_file, &app.fl, true);
            }
            else if (ch == '\n')
            {
                if (app.fl.selected >= 0 && app.fl.selected < app.fl.count)
                {
                    char tgt[PATH_MAX];
                    path_join(tgt, app.fl.base, app.fl.items[app.fl.selected]);
                    if (socket_is_connected() || is_directory(tgt))
                    {
                        dirlist_scan(&app.dl, tgt);
                        open_selected_dir(&app);
                    }
                    else
                    {
                        status_bar(win_chat, "파일은 열지 않고, 채팅의 기준 경로만 유지합니다.");
                    }
                }
            }
            else if (ch == KEY_LEFT)
            {
                change_focus(&app, FOCUS_DIR);
            }
            else if (ch == KEY_CTRL_Z)
            {
                go_parent_dir(&app);
            }
            break;

        case FOCUS_CHAT:
            if (ch == KEY_RIGHT || ch == '\n')
            {
                change_focus(&app, FOCUS_INPUT);
            }
            else if (ch == KEY_LEFT)
            {
                change_focus(&app, FOCUS_FILE);
            }
            break;

        case FOCUS_INPUT:
            {
                input_draw(win_input, true);
                wmove(win_input, 1, 4);
                linebuf[0] = '\0';
                int end_key = input_capture_line(win_input, linebuf, sizeof(linebuf), ch);

                if (end_key == KEY_BTAB)
                {
                    change_focus(&app, FOCUS_FILE);
                    break;
                }
                if (end_key == KEY_F(1))
                {
                    change_focus(&app, FOCUS_DIR);
                    break;
                }
                if (end_key == KEY_F(2))
                {
                    change_focus(&app, FOCUS_FILE);
                    break;
                }
                if (end_key == KEY_F(3))
                {
                    change_focus(&app, FOCUS_INPUT);
                    break;
                }
                if (strcmp(linebuf, "/upload") == 0)
                {
                    start_upload_mode(&app);
                }
                else if (strncmp(linebuf, "cd ", 3) == 0 || strncmp(linebuf, "mkdir ", 6) == 0 || strncmp(linebuf, "ls", 2) == 0)
                {
                    socket_send_cmd(linebuf);
                    char response[2048];
                    while (socket_recv_response(response, sizeof(response)) > 0)
                    {
                        chat_append(&app.chat, "server", response);
                        if (strstr(response, "OK") || strstr(response, "ERR"))
                            break;
                    }
                    app.chat.dirty = 1;
                }
                else
                {
                    const char *user = app.username[0] ? app.username : safe_username();
                    if (strlen(linebuf) > 0)
                    {
                        chat_append(&app.chat, user, linebuf);
                        app.chat.dirty = 1;
                    }
                }
                change_focus(&app, FOCUS_INPUT);
            }
            break;
        }
    }

    app_free(&app);
    endwin();
    return 0;
}