#define _XOPEN_SOURCE 700
#include <locale.h>
#include <ncurses.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

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
    FocusArea last_list_focus;
    char username[64];
    bool logged_in;
    bool upload_mode;
} App;

static void redraw_all(App *a);
static void change_focus(App *a, FocusArea next);
static void delete_selected_entry(App *a);
static int delete_local_path(const char *target_path);
static void handle_dls_command(App *app, const char *linebuf);

static int capture_masked_input(WINDOW *win, int y, int x, char *out, int maxlen)
{
    int pos = 0;
    keypad(win, TRUE);
    wmove(win, y, x);
    wrefresh(win);

    while (1)
    {
        int ch = wgetch(win);
        if (ch == '\n' || ch == KEY_ENTER) break;
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
    int h, w; getmaxyx(stdscr, h, w);
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
        mvwgetnstr(login, 2, 8, user, sizeof(user) - 1); 
        noecho();

        capture_masked_input(login, 3, 8, pass, sizeof(pass));

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

static void layout_create(void)
{
    int h, w; 
    getmaxyx(stdscr, h, w);

    int left_w = w / 3;
    int right_w = w - left_w;
    int chat_h = h - 3;

    win_dir  = newwin(h / 2, left_w, 0, 0);
    win_file = newwin(h / 2, left_w, h / 2, 0);
    win_chat = newwin(chat_h, right_w, 0, left_w);
    win_input= newwin(3, right_w, chat_h, left_w);

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

    const char *help =
        a->upload_mode
            ? "Upload: ↑↓ 이동 Enter 선택 Space 업로드, Ctrl+Z 상위, q 취소"
            : "F1:위치 F2:선택 F3:입력 Tab 이동 Enter 실행 Ctrl+Z 상위 q 종료";

    status_bar(win_chat, help);
}

static void change_focus(App *a, FocusArea next)
{
    if (a->upload_mode && next != FOCUS_FILE)
        next = FOCUS_FILE;

    if (next == FOCUS_DIR || next == FOCUS_FILE)
        a->last_list_focus = next;

    a->focus = next;
    redraw_all(a);
}

static void open_selected_dir(App *a)
{
    if (a->dl.selected < 0 || a->dl.selected >= a->dl.count) {
        filelist_scan(&a->fl, a->dl.cwd);
        chat_init(&a->chat, a->dl.cwd);
    }
    else {
        const char *dir_abs = a->dl.items[a->dl.selected];
        filelist_scan(&a->fl, dir_abs);
        chat_init(&a->chat, dir_abs);
    }

    redraw_all(a);
}

static void go_parent_dir(App *a)
{
    char parent[PATH_MAX];
    dirname_of(parent, a->dl.cwd);

    if (!is_directory(parent))
        return;

    if (strcmp(parent, a->dl.cwd) == 0)
        return;

    dirlist_scan(&a->dl, parent);
    open_selected_dir(a);
}

static int delete_local_path(const char *target_path)
{
    struct stat st;

    if (lstat(target_path, &st) < 0)
        return -1;

    if (S_ISDIR(st.st_mode))
    {
        DIR *dir = opendir(target_path);
        if (!dir)
            return -1;

        struct dirent *ent;
        while ((ent = readdir(dir)))
        {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;

            char child[PATH_MAX];
            path_join(child, target_path, ent->d_name);

            if (delete_local_path(child) != 0)
            {
                closedir(dir);
                return -1;
            }
        }

        closedir(dir);

        if (rmdir(target_path) != 0)
            return -1;
    }
    else
    {
        if (remove(target_path) != 0)
            return -1;
    }

    return 0;
}

static bool strip_eof_marker(char *buffer)
{
    char *marker = strstr(buffer, "\nEOF\n");
    if (marker)
    {
        *marker = '\0';
        return true;
    }

    marker = strstr(buffer, "EOF\n");
    if (marker == buffer)
    {
        *marker = '\0';
        return true;
    }

    return false;
}

static void request_dls(App *app, const char *target_dir)
{
    if (!target_dir || !*target_dir)
        return;

    status_bar(win_chat, "디스크 사용량 분석 중...");

    char cmd[PATH_MAX + 8];
    snprintf(cmd, sizeof(cmd), "dls %s", target_dir);
    socket_send_cmd(cmd);

    size_t cap = 4096, len = 0;
    char *acc = calloc(cap, 1);
    char buf[512];

    while (acc && socket_recv_response(buf, sizeof(buf)) > 0)
    {
        size_t need = len + strlen(buf) + 1;
        if (need > cap)
        {
            size_t new_cap = cap * 2;
            while (new_cap < need)
                new_cap *= 2;

            char *tmp = realloc(acc, new_cap);
            if (!tmp)
            {
                free(acc);
                acc = NULL;
                break;
            }

            acc = tmp;
            cap = new_cap;
        }

        memcpy(acc + len, buf, strlen(buf));
        len += strlen(buf);
        acc[len] = '\0';

        if (strip_eof_marker(acc))
            break;

        if (len >= cap - 1)
            break;
    }

    if (!acc)
        return;

    char *line = strtok(acc, "\n");
    while (line)
    {
        chat_append_raw(&app->chat, line);
        line = strtok(NULL, "\n");
    }

    app->chat.dirty = 1;
    chat_draw(win_chat, &app->chat, app->focus == FOCUS_CHAT);

    free(acc);
}

static void handle_dls_command(App *app, const char *linebuf)
{
    char target[PATH_MAX];

    if (strcmp(linebuf, "/dls") == 0)
    {
        snprintf(target, sizeof(target), "%s", app->dl.cwd);
    }
    else
    {
        const char *raw = linebuf + 4;
        while (*raw == ' ')
            raw++;

        if (!*raw)
            snprintf(target, sizeof(target), "%s", app->dl.cwd);
        else if (raw[0] == '/')
            snprintf(target, sizeof(target), "%s", raw);
        else
            path_join(target, app->dl.cwd, raw);
    }

    request_dls(app, target);
}

// [수정됨] 경로 저장 및 복구 로직 적용
static void delete_selected_entry(App *a)
{
    if (a->upload_mode)
    {
        status_bar(win_chat, "업로드 모드에서는 삭제를 사용할 수 없습니다.");
        return;
    }

    // 1. 현재 보고 있는 경로(삭제 후 돌아올 곳)를 미리 저장
    char saved_path[PATH_MAX];
    snprintf(saved_path, sizeof(saved_path), "%s", a->dl.cwd);

    char target_path[PATH_MAX] = {0};
    bool from_file_panel = false;

    if (a->last_list_focus == FOCUS_FILE)
    {
        if (a->fl.selected < 0 || a->fl.selected >= a->fl.count) return;
        path_join(target_path, a->fl.base, a->fl.items[a->fl.selected].name);
        from_file_panel = true;
    }
    else if (a->last_list_focus == FOCUS_DIR)
    {
        if (a->dl.selected < 0 || a->dl.selected >= a->dl.count) return;
        snprintf(target_path, sizeof(target_path), "%s", a->dl.items[a->dl.selected]);
        
        // 현재 경로 자체를 삭제하려는 경우 상위로 이동 준비
        if (strcmp(target_path, a->dl.cwd) == 0) {
             dirname_of(saved_path, a->dl.cwd); // 상위 폴더를 저장
        }
    }
    else
    {
        status_bar(win_chat, "삭제할 항목을 선택하세요.");
        return;
    }

    char logmsg[PATH_MAX + 64];
    snprintf(logmsg, sizeof(logmsg), "삭제 시도: %s", target_path);
    chat_append(&a->chat, "system", logmsg);
    a->chat.dirty = 1;

    bool success = false;
    char errmsg[256] = {0};

    if (socket_is_connected())
    {
        char cmd[PATH_MAX + 16];
        snprintf(cmd, sizeof(cmd), "DELETE %s", target_path);
        socket_send_cmd(cmd);

        // [중요] 응답 누적 및 ACK 확인 (멈춤 방지)
        char resp[512];
        char acc_buf[4096] = {0};
        int rn;
        while ((rn = socket_recv_response(resp, sizeof(resp))) > 0)
        {
            strncat(acc_buf, resp, sizeof(acc_buf) - strlen(acc_buf) - 1);
            chat_append(&a->chat, "server", resp);

            if (strstr(acc_buf, "OK")) { success = true; break; }
            if (strstr(acc_buf, "ERR")) { snprintf(errmsg, sizeof(errmsg), "%s", acc_buf); break; }
            if (strstr(acc_buf, "ACK")) { 
                // 서버가 명령을 채팅으로 오인함 -> 실패로 처리하고 루프 탈출 (멈춤 방지)
                snprintf(errmsg, sizeof(errmsg), "서버가 명령을 인식하지 못했습니다 (ACK).");
                break; 
            }
        }
    }
    else
    {
        if (delete_local_path(target_path) == 0) success = true;
        else snprintf(errmsg, sizeof(errmsg), "%s", strerror(errno));
    }

    if (success)
    {
        status_bar(win_chat, "삭제 완료");
        
        // 2. 저장해둔 경로로 복구 (새로고침)
        if (from_file_panel) {
            // 파일 패널에서 삭제했으면 현재 디렉토리 파일 목록 갱신
            filelist_scan(&a->fl, a->fl.base);
        } else {
            // 디렉토리 패널에서 삭제했으면 저장된 경로로 이동
            dirlist_scan(&a->dl, saved_path);
            open_selected_dir(a);
        }
        
        redraw_all(a);
        snprintf(logmsg, sizeof(logmsg), "삭제 완료: %s", target_path);
        chat_append(&a->chat, "system", logmsg);
    }
    else
    {
        snprintf(logmsg, sizeof(logmsg), "삭제 실패: %s (%s)", target_path, errmsg);
        status_bar(win_chat, logmsg);
        chat_append(&a->chat, "system", logmsg);
    }
    a->chat.dirty = 1;
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

static void upload_file_data(App *a, const char *filepath)
{
    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        upload_log(a, "[system/upload] Error: Cannot open local file");
        return;
    }

    fseek(fp, 0, SEEK_END);
    long filesize = ftell(fp);
    rewind(fp);

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "UPLOAD START %ld", filesize);
    socket_send_cmd(cmd);

    char ack[64];
    socket_recv_response(ack, sizeof(ack));

    if (strncmp(ack, "ACK: READY", 10) != 0) {
        upload_log(a, "[system/upload] Error: Server not ready");
        fclose(fp);
        return;
    }

    char buf[4096];
    size_t n;

    char log_buf[100];
    snprintf(log_buf, sizeof(log_buf),
             "[system/upload] Sending %ld bytes...", filesize);

    upload_log(a, log_buf);

    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        send(sockfd, buf, n, 0);

    fclose(fp);

    char resp[256];
    int rn = socket_recv_response(resp, sizeof(resp));

    if (rn > 0)
        upload_log(a, "[system/upload] Server: Upload Complete");
}

static void send_upload_plan(App *a, const char *path, bool is_dir)
{
    if (is_dir) {
        upload_log(a, "[system/upload] Directory upload is not supported yet.");
        return;
    }

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    char base_copy[256];
    snprintf(base_copy, sizeof(base_copy), "%.255s", base);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "UPLOAD PLAN FILE %s", base_copy);
    socket_send_cmd(cmd);

    char response[256];
    int rn = socket_recv_response(response, sizeof(response));

    if (rn > 0)
    {
        response[rn] = '\0';

        if (strncmp(response, "OK:", 3) == 0)
        {
            upload_log(a, "[system/upload] Plan accepted. Starting transfer...");
            upload_file_data(a, path);

            char current_cwd[PATH_MAX];
            snprintf(current_cwd, sizeof(current_cwd), "%s", a->dl.cwd);
            dirlist_scan(&a->dl, current_cwd);

            open_selected_dir(a);
        }
        else
        {
            char msg[512];
            snprintf(msg, sizeof(msg),
                     "[system/upload] Server rejected plan: %s",
                     response);
            upload_log(a, msg);
        }
    }
    else
    {
        upload_log(a,
                   "[system/upload] No response from server for upload plan");
    }
}

static void start_upload_mode(App *a)
{
    a->prev_focus = a->focus;
    a->upload_mode = true;
    change_focus(a, FOCUS_FILE);

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
        snprintf(cwd, sizeof(cwd), ".");

    if (localbrowser_scan(&a->lbrowser, cwd) >= 0)
        localbrowser_draw(win_file, &a->lbrowser, true);
    else
        upload_log(a, "[system/upload] Failed to read local directory");

    upload_log(a, "[system/upload] Upload mode started");
    upload_log(a,
               "[system/upload] Bottom-left window shows LOCAL filesystem.");

    redraw_all(a);
}

static void handle_upload_mode_key(App *a, int ch)
{
    if (ch == 'q' || ch == 'Q') {
        upload_log(a, "[system/upload] Upload mode cancelled");
        exit_upload_mode(a);
        return;
    }

    if (a->lbrowser.count == 0)
    {
        if (ch == KEY_BACKSPACE || ch == KEY_CTRL_Z)
        {
            char parent[PATH_MAX];
            dirname_of(parent, a->lbrowser.cwd);

            if (strcmp(parent, a->lbrowser.cwd) != 0)
            {
                if (localbrowser_scan(&a->lbrowser, parent) >= 0)
                    localbrowser_draw(win_file, &a->lbrowser, true);
            }
        }
        else if (ch == ' ')
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
    else if (ch == KEY_BACKSPACE || ch == KEY_CTRL_Z || ch == KEY_LEFT)
    {
        char parent[PATH_MAX];
        dirname_of(parent, a->lbrowser.cwd);

        if (strcmp(parent, a->lbrowser.cwd) != 0)
        {
            if (localbrowser_scan(&a->lbrowser, parent) >= 0)
                localbrowser_draw(win_file, &a->lbrowser, true);
        }
    }
    else if (ch == '\n' || ch == KEY_ENTER || ch == KEY_RIGHT)
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
            else if (ch != KEY_RIGHT)
            {
                char selected_path[PATH_MAX];
                path_join(selected_path,
                          a->lbrowser.cwd,
                          ent->name);

                char msg[PATH_MAX + 64];
                snprintf(msg, sizeof(msg),
                         "[system/upload] Selected file: %s",
                         selected_path);

                upload_log(a, msg);

                send_upload_plan(a, selected_path, false);
                exit_upload_mode(a);
            }
        }
    }
    else if (ch == ' ')
    {
        const LocalEntry *ent = &a->lbrowser.items[a->lbrowser.selected];

        char selected_path[PATH_MAX];
        path_join(selected_path,
                  a->lbrowser.cwd,
                  ent->name);

        char msg[PATH_MAX + 64];

        if (ent->is_dir)
            snprintf(msg, sizeof(msg),
                     "[system/upload] Selected directory: %s",
                     selected_path);
        else
            snprintf(msg, sizeof(msg),
                     "[system/upload] Selected file: %s",
                     selected_path);

        upload_log(a, msg);

        send_upload_plan(a, selected_path, ent->is_dir);
        exit_upload_mode(a);
    }
}

static void app_init(App *a)
{
    a->focus = FOCUS_DIR;
    a->prev_focus = FOCUS_DIR;
    a->last_list_focus = FOCUS_DIR;
    a->upload_mode = false;

    localbrowser_init(&a->lbrowser);

    const char *start_dir = "/home";
    char absdir[PATH_MAX];
    abspath(absdir, start_dir);

    dirlist_init(&a->dl);
    dirlist_scan(&a->dl, absdir);

    filelist_init(&a->fl);

    open_selected_dir(a);
    redraw_all(a);
}

static void app_free(App *a)
{
    dirlist_free(&a->dl);
    filelist_free(&a->fl);
    localbrowser_free(&a->lbrowser);
}

#ifdef USE_INOTIFY
static int setup_inotify(const char *path)
{
    int fd = inotify_init1(IN_NONBLOCK);
    if (fd < 0)
        return -1;

    inotify_add_watch(fd, path,
                      IN_MODIFY |
                      IN_CLOSE_WRITE |
                      IN_MOVE_SELF |
                      IN_DELETE_SELF);

    return fd;
}
#endif

int main(int argc, char *argv[])
{
    char host[256] = "127.0.0.1";
    int port = 5050;

    if (argc >= 3)
    {
        strncpy(host, argv[1], sizeof(host) - 1);
        host[sizeof(host) - 1] = '\0';
        port = atoi(argv[2]);
    }
    else if (argc >= 2)
    {
        strncpy(host, argv[1], sizeof(host) - 1);
        host[sizeof(host) - 1] = '\0';

        char *c = strrchr(host, ':');
        if (c)
        {
            *c = '\0';
            port = atoi(c + 1);
        }
    }

    socket_connect_to(host, port);

    setlocale(LC_ALL, "");
    initscr();
    noecho();
    raw();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(200);

    clear();
    refresh();

    App app;
    memset(&app, 0, sizeof(app));

    login_prompt(&app);

    clear();
    refresh();

    layout_create();
    app_init(&app);

    refresh();

#ifdef USE_INOTIFY
    int inofd = setup_inotify(app.chat.log_path);
#endif

    char linebuf[4096];

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
            if (read(inofd, buf, sizeof(buf)) > 0)
                app.chat.dirty = 1;
        }
#endif

        int ch = getch();
        if (ch == ERR) continue;

        if (app.upload_mode)
        {
            handle_upload_mode_key(&app, ch);
            if (!app.upload_mode)
                redraw_all(&app);
            continue;
        }

        if (ch == 'q' || ch == 'Q' || ch == KEY_CTRL_C)
            break;

        if (ch == KEY_F(1)) { change_focus(&app, FOCUS_DIR); continue; }
        if (ch == KEY_F(2)) { change_focus(&app, FOCUS_FILE); continue; }
        if (ch == KEY_F(3)) { change_focus(&app, FOCUS_INPUT); continue; }
        if (ch == '\t' && app.focus != FOCUS_INPUT) { change_focus(&app, FOCUS_INPUT); continue; }

        switch (app.focus)
        {
        case FOCUS_DIR:
            if (ch == KEY_UP)
            {
                if (app.dl.selected > -1)
                {
                    app.dl.selected--;
                    open_selected_dir(&app);
                }
            }
            else if (ch == KEY_DOWN)
            {
                if (app.dl.selected < app.dl.count - 1)
                {
                    app.dl.selected++;
                    open_selected_dir(&app);
                }
            }
            else if (ch == '\n')
            {
                if (app.dl.selected >= 0 && app.dl.selected < app.dl.count)
                {
                    char target[PATH_MAX];
                    snprintf(target, sizeof(target), "%s",
                             app.dl.items[app.dl.selected]);

                    dirlist_scan(&app.dl, target);

                    open_selected_dir(&app);
                    redraw_all(&app);
                }
            }
            else if (ch == KEY_RIGHT)
            {
                open_selected_dir(&app);
                change_focus(&app, FOCUS_FILE);
            }
            else if (ch == KEY_BACKSPACE || ch == KEY_CTRL_Z)
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
            else if (ch == '\n' || ch == KEY_RIGHT)
            {
                if (app.fl.selected >= 0 &&
                    app.fl.selected < app.fl.count)
                {
                    if (app.fl.items[app.fl.selected].is_dir)
                    {
                        char tgt[PATH_MAX];
                        path_join(tgt,
                                  app.fl.base,
                                  app.fl.items[app.fl.selected].name);

                        dirlist_scan(&app.dl, tgt);
                        open_selected_dir(&app);
                    }
                    else
                    {
                        status_bar(win_chat,
                                   "파일입니다. 디렉토리만 들어갈 수 있습니다.");
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
                change_focus(&app, FOCUS_INPUT);
            else if (ch == KEY_LEFT)
                change_focus(&app, FOCUS_FILE);
            break;

        case FOCUS_INPUT:
            input_draw(win_input, true);
            wmove(win_input, 1, 4);

            memset(linebuf, 0, sizeof(linebuf));
            int end_key = input_capture_line(win_input,
                                             linebuf,
                                             sizeof(linebuf),
                                             ch);

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

            if (strcmp(linebuf, "/dls") == 0 || strncmp(linebuf, "/dls ", 5) == 0)
            {
                handle_dls_command(&app, linebuf);
                break;
            }

            // Upload 명령
            if (strcmp(linebuf, "/upload") == 0)
            {
                start_upload_mode(&app);
                break;
            }

            if (strcmp(linebuf, "/delete") == 0)
            {
                delete_selected_entry(&app);
                change_focus(&app, FOCUS_INPUT);
                break;
            }

            // 서버 명령 처리
            if (strncmp(linebuf, "cd ", 3) == 0 ||
                strncmp(linebuf, "mkdir ", 6) == 0 ||
                strncmp(linebuf, "ls", 2) == 0)
            {
                socket_send_cmd(linebuf);

                // [수정됨] 누적 버퍼 + ACK 확인
                char r[512];
                char acc_buf[4096] = {0};
                while (socket_recv_response(r, sizeof(r)) > 0)
                {
                    strncat(acc_buf, r, sizeof(acc_buf) - strlen(acc_buf) - 1);
                    chat_append(&app.chat, "server", r);
                    
                    if (strstr(acc_buf, "OK") || strstr(acc_buf, "ERR") || strstr(acc_buf, "ACK"))
                        break;
                }

                app.chat.dirty = 1;
            }
            else
            {
                const char *u =
                    app.username[0] ? app.username : safe_username();

                if (strlen(linebuf) > 0)
                {
                    chat_append(&app.chat, u, linebuf);
                    app.chat.dirty = 1;
                }
            }

            change_focus(&app, FOCUS_INPUT);
            break;
        }
    }

    app_free(&app);
    endwin();
    return 0;
}