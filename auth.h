#ifndef AUTH_H
#define AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include "auth_manager.h" // UserAccount 구조체 정의가 필요하다면 포함, 혹은 아래처럼 전방 선언 사용

// auth.c에서 구현된 함수 선언 추가
bool auth_init(void);

// 로그인 절차를 진행하고 성공 시 사용자명을 out_username에 채운다.
// 최대 3회까지 입력을 허용하며, 실패 시 false를 반환한다.
// (참고: 아래 함수들은 auth_manager.h나 auth.c에 따라 다를 수 있으나, 기존 auth.h 내용을 유지하며 auth_init만 추가함)

#define AUTH_SALT "ufms-demo-salt"

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