#include "utils.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <pwd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

// ------------------------------------------------------------
// 파일/디렉토리 여부
// ------------------------------------------------------------
bool is_directory(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0) return false;
    return S_ISDIR(st.st_mode);
}

bool is_regular(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0) return false;
    return S_ISREG(st.st_mode);
}

// ------------------------------------------------------------
// 경로 조합
// ------------------------------------------------------------
void path_join(char out[PATH_MAX], const char *a, const char *b)
{
    if (!a || !*a)
        snprintf(out, PATH_MAX, "%s", b);
    else if (!b || !*b)
        snprintf(out, PATH_MAX, "%s", a);
    else if (a[strlen(a) - 1] == '/')
        snprintf(out, PATH_MAX, "%s%s", a, b);
    else
        snprintf(out, PATH_MAX, "%s/%s", a, b);
}

// ------------------------------------------------------------
// 절대 경로 변환
// ------------------------------------------------------------
void abspath(char out[PATH_MAX], const char *path)
{
    char cwd[PATH_MAX];

    if (!path || !*path)
    {
        getcwd(out, PATH_MAX);
        return;
    }

    if (path[0] == '/')
    {
        snprintf(out, PATH_MAX, "%s", path);
        return;
    }

    getcwd(cwd, sizeof(cwd));
    snprintf(out, PATH_MAX, "%s/%s", cwd, path);
}

// ------------------------------------------------------------
// dirname 구현
// ------------------------------------------------------------
void dirname_of(char out[PATH_MAX], const char *path)
{
    snprintf(out, PATH_MAX, "%s", path);

    int len = strlen(out);
    if (len == 0) return;

    // 뒤 슬래시 제거
    while (len > 1 && out[len - 1] == '/')
        out[--len] = '\0';

    char *p = strrchr(out, '/');
    if (!p)
    {
        snprintf(out, PATH_MAX, ".");
        return;
    }

    if (p == out)
        strcpy(out, "/");
    else
        *p = '\0';
}

// ------------------------------------------------------------
// mkdir -p 기능
// ------------------------------------------------------------
void ensure_dir(const char *path)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);

    int len = strlen(tmp);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }

    mkdir(tmp, 0755);
}

// ------------------------------------------------------------
// 사용자 홈 경로 (~)
// ------------------------------------------------------------
void get_home(char out[PATH_MAX])
{
    const char *h = getenv("HOME");
    if (h)
    {
        snprintf(out, PATH_MAX, "%s", h);
        return;
    }

    struct passwd *pw = getpwuid(getuid());
    if (pw)
    {
        snprintf(out, PATH_MAX, "%s", pw->pw_dir);
        return;
    }

    snprintf(out, PATH_MAX, "/tmp");
}

// ------------------------------------------------------------
// 채팅 로그 경로 생성
// ~/.tui_chatops/chatlogs/<dir_abs hash>.log
// ------------------------------------------------------------
void make_log_path(char out[PATH_MAX], const char *dir_abs)
{
    char home[PATH_MAX];
    get_home(home);

    char base[PATH_MAX];
    snprintf(base, sizeof(base), "%s/.tui_chatops/chatlogs", home);
    ensure_dir(base);

    unsigned long h = 5381;
    for (const char *p = dir_abs; *p; p++)
        h = ((h << 5) + h) + (unsigned char)*p;

    snprintf(out, PATH_MAX, "%s/%lx.log", base, h);
}

// ------------------------------------------------------------
// 안전한 username
// ------------------------------------------------------------
const char *safe_username(void)
{
    const char *u = getenv("USER");
    if (u && *u) return u;
    return "user";
}

// ------------------------------------------------------------
// 파일 크기 bar 생성
// ------------------------------------------------------------
void build_size_bar(long size, char *out, int out_len)
{
    if (out_len < 10)
    {
        if (out_len > 0) out[0] = '\0';
        return;
    }

    long max_size = 10 * 1024 * 1024; // 10MB 기준
    if (size > max_size) size = max_size;

    int bar_width = out_len - 1;
    int filled = (int)((double)size / max_size * bar_width);

    int i = 0;
    for (int k = 0; k < bar_width; k++)
    {
        if (k < filled)  out[i++] = "█"[0];
        else             out[i++] = "░"[0];
    }

    out[i] = '\0';
}

// ------------------------------------------------------------
// 파일 크기 읽기
// ------------------------------------------------------------
long get_file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0) return -1;
    return (long)st.st_size;
}