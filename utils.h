#ifndef UTILS_H
#define UTILS_H

#include <limits.h>
#include <stdbool.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

bool is_directory(const char *path);
bool is_regular(const char *path);
void path_join(char out[PATH_MAX], const char *a, const char *b);
void abspath(char out[PATH_MAX], const char *path);
void dirname_of(char out[PATH_MAX], const char *path);

void ensure_dir(const char *path);                    // mkdir -p
void get_home(char out[PATH_MAX]);                    // ~ 경로
void make_log_path(char out[PATH_MAX], const char *dir_abs); // ~/.tui_chatops/chatlogs/xxxx.log
const char* safe_username(void);

#endif
