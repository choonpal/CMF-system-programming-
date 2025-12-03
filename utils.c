#define _XOPEN_SOURCE 700
#include "utils.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <pwd.h>
#include <libgen.h>

bool is_directory(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0) && S_ISDIR(st.st_mode);
}
bool is_regular(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0) && S_ISREG(st.st_mode);
}

void path_join(char out[PATH_MAX], const char *a, const char *b) {
    if (!a || !*a) { snprintf(out, PATH_MAX, "%s", b?b:""); return; }
    if (!b || !*b) { snprintf(out, PATH_MAX, "%s", a); return; }
    size_t al = strlen(a);
    if (a[al-1] == '/') snprintf(out, PATH_MAX, "%s%s", a, b);
    else               snprintf(out, PATH_MAX, "%s/%s", a, b);
}

void abspath(char out[PATH_MAX], const char *path) {
    if (!path || !*path) { getcwd(out, PATH_MAX); return; }
    if (path[0] == '/') { snprintf(out, PATH_MAX, "%s", path); return; }
    char cwd[PATH_MAX]; getcwd(cwd, sizeof(cwd));
    path_join(out, cwd, path);
}

void dirname_of(char out[PATH_MAX], const char *path) {
    char tmp[PATH_MAX]; snprintf(tmp, sizeof(tmp), "%s", path);
    char *d = dirname(tmp);
    snprintf(out, PATH_MAX, "%s", d);
}

void ensure_dir(const char *path) {
    // mkdir -p
    char buf[PATH_MAX]; snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; ++p) {
        if (*p == '/') { *p = '\0'; mkdir(buf, 0700); *p = '/'; }
    }
    mkdir(buf, 0700);
}

void get_home(char out[PATH_MAX]) {
    const char *h = getenv("HOME");
    if (h && *h) { snprintf(out, PATH_MAX, "%s", h); return; }
    struct passwd *pw = getpwuid(getuid());
    snprintf(out, PATH_MAX, "%s", pw? pw->pw_dir : "/tmp");
}

static void sanitize(char *s) {
    for (char *p=s; *p; ++p) {
        if (*p=='/') *p='_';
        else if (*p==' ') *p='-';
    }
}

void make_log_path(char out[PATH_MAX], const char *dir_abs) {
    char home[PATH_MAX]; get_home(home);
    char base[PATH_MAX]; snprintf(base, sizeof(base), "%s", dir_abs);
    sanitize(base);
    char root[PATH_MAX]; snprintf(root, sizeof(root), "%s/.tui_chatops/chatlogs", home);
    ensure_dir(root);
    snprintf(out, PATH_MAX, "%s/%s.log", root, base[0]?base:"root");
}

const char* safe_username(void) {
    const char *u = getenv("USER");
    if (u && *u) return u;
    struct passwd *pw = getpwuid(getuid());
    return pw && pw->pw_name ? pw->pw_name : "user";
}
