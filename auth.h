#ifndef AUTH_H
#define AUTH_H

#include <stdbool.h>

#define AUTH_SALT "ufms-demo-salt"
// 해시 값 생성 -> 터미널에 아래 라인 입력
// echo -n 'ufms-demo-salt<문자열>' | shasum -a 256

typedef struct {
    char username[64];
    char password_hash[65];
    int permission_level;
    bool locked;
    int failed_attempts;
} UserAccount;

typedef enum {
    AUTH_OK,
    AUTH_LOCKED,
    AUTH_INVALID,
} AuthResult;

void hash_password(const char *password, char out_hex[65]);
AuthResult verify_credentials(const char *user, const char *provided_hash, int *out_permission_level, int *out_remaining_attempts);
bool get_user_info(const char *user, UserAccount *out);

#endif
