#ifndef AUTH_MANAGER_H
#define AUTH_MANAGER_H

#include <stdbool.h>
#include <stddef.h>

// 로그인 절차를 진행하고 성공 시 사용자명을 out_username에 채운다.
// 최대 3회까지 입력을 허용하며, 실패 시 false를 반환한다.
bool authenticate_user(char *out_username, size_t out_size);

#endif
