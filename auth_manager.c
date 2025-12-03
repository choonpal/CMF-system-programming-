#define _XOPEN_SOURCE 700
#include "auth_manager.h"
#include "utils.h"
#include <crypt.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

typedef struct {
    const char *id;
    const char *password_hash;
    int permission_level;
    bool locked;
    int failed_attempts;
} DummyAccount;

// 사전에 준비된 더미 계정 (비밀번호는 해시 형태로만 보관)
static DummyAccount accounts[] = {
    {"admin1", "$6$GQj5vxl1NCcrVA8I$FyFttXvfBPrK4z95YBxVxFf77szlqswY5KWpHX8BPSxB18WfbJf/LTzlENpWRxLlWAAaAT0fG7T7F24wfFiDj0", 10, false, 0},
    {"tester", "$6$tGQY7VxGzuqvhT60$6balVGqf6W5X.hFcqVW4hzphMOwys.MAIzQmYoFqiubKvIceRaMWgOGzxveR3XAsIw3sQILgR1xjF6gc7T4Oe/", 5, false, 0}
};
static const size_t account_count = sizeof(accounts) / sizeof(accounts[0]);

static DummyAccount* find_account(const char *id) {
    if (!id || !*id) return NULL;
    for (size_t i = 0; i < account_count; ++i) {
        if (strcmp(id, accounts[i].id) == 0) {
            return &accounts[i];
        }
    }
    return NULL;
}

int get_permission_level(const char *id) {
    DummyAccount *acc = find_account(id);
    if (!acc) return -1;
    return acc->permission_level;
}

bool is_account_locked(const char *id) {
    DummyAccount *acc = find_account(id);
    if (!acc) return false;
    return acc->locked;
}

static void trim_newline(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r')) {
        s[--len] = '\0';
    }
}

static void read_line(const char *prompt, char *out, size_t out_size) {
    printf("%s", prompt);
    fflush(stdout);
    if (fgets(out, (int)out_size, stdin)) {
        trim_newline(out);
    } else {
        out[0] = '\0';
    }
}

static int capture_masked_input(const char *prompt, char *out, size_t out_size) {
    struct termios oldt, newt;
    if (tcgetattr(STDIN_FILENO, &oldt) != 0) return -1;
    newt = oldt;
    newt.c_lflag &= ~(ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    printf("%s", prompt);
    fflush(stdout);

    size_t idx = 0; int ch;
    while ((ch = getchar()) != '\n' && ch != EOF) {
        if (ch == '\b' || ch == 127) {
            if (idx > 0) {
                idx--;
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }
        if (idx < out_size - 1) {
            out[idx++] = (char)ch;
            putchar('*');
            fflush(stdout);
        }
    }
    out[idx] = '\0';
    printf("\n");
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return (int)idx;
}

static bool verify_password(const DummyAccount *acc, const char *password) {
    char *hashed = crypt(password, acc->password_hash);
    return hashed && strcmp(hashed, acc->password_hash) == 0;
}

bool authenticate_user(char *out_username, size_t out_size) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        char id[64];
        char pw[128];

        read_line("ID: ", id, sizeof(id));
        if (capture_masked_input("Password: ", pw, sizeof(pw)) < 0) {
            printf("비밀번호를 읽어오지 못했습니다.\n");
            continue;
        }

        DummyAccount *account = find_account(id);
        if (!account) {
            printf("❌ 존재하지 않는 계정입니다. 다시 시도하세요.\n\n");
            continue;
        }

        if (account->locked) {
            printf("🔒 계정이 잠겨 있어 로그인할 수 없습니다. 관리자에게 문의하세요.\n\n");
            continue;
        }

        if (verify_password(account, pw)) {
            account->failed_attempts = 0;
            snprintf(out_username, out_size, "%s", id);
            set_authenticated_user(id);
            return true;
        }

        account->failed_attempts++;
        if (account->failed_attempts >= 3) {
            account->locked = true;
            printf("❌ 잘못된 계정 정보입니다. 3회 실패로 계정이 잠겼습니다.\n\n");
        } else {
            printf("❌ 잘못된 계정 정보입니다. 다시 시도하세요.\n\n");
        }
    }
    return false;
}
