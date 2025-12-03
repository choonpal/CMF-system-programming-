#include "auth.h"
#include <openssl/sha.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void to_hex(const unsigned char *in, size_t len, char out_hex[65]) {
    for (size_t i = 0; i < len; i++)
        sprintf(out_hex + (i * 2), "%02x", in[i]);
    out_hex[len * 2] = '\0';
}

void hash_password(const char *password, char out_hex[65]) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, AUTH_SALT, strlen(AUTH_SALT));
    SHA256_Update(&ctx, password, strlen(password));
    SHA256_Final(digest, &ctx);
    to_hex(digest, SHA256_DIGEST_LENGTH, out_hex);
}

static const char *STATE_FILE = "/home/talkshell_accounts.txt";

static const UserAccount default_users[] = {
    {.username = "admin1", .password_hash = "bc7fc5f56a1b1aa1d100bf814f3b287021be90b1bbdd7f9caa5583361af6eae2", .permission_level = 10, .locked = false, .failed_attempts = 0},
    {"admin2", "62b1d9e38c47d84939dd17ec12806e3c64ef63d31a53de4035d7175732e2246b", 9, false, 0},
    {.username = "opslead", .password_hash = "62b1d9e38c47d84939dd17ec12806e3c64ef63d31a53de4035d7175732e2246b", .permission_level = 7, .locked = false, .failed_attempts = 0},
};

static UserAccount users[sizeof(default_users) / sizeof(default_users[0])];

static pthread_mutex_t users_lock = PTHREAD_MUTEX_INITIALIZER;
static const int MAX_ATTEMPTS = 3;
static const size_t USER_COUNT = sizeof(users) / sizeof(users[0]);

static void reset_to_defaults_locked(void)
{
    memcpy(users, default_users, sizeof(default_users));
}

static UserAccount *find_user(const char *user);

static void load_users_locked(void)
{
    FILE *fp = fopen(STATE_FILE, "r");
    if (!fp)
        return;

    char line[256];
    while (fgets(line, sizeof(line), fp))
    {
        char username[64] = {0};
        char pw[65] = {0};
        int perm = 0;
        int locked = 0;
        int attempts = 0;

        if (sscanf(line, "%63[^,],%64[^,],%d,%d,%d", username, pw, &perm, &locked, &attempts) == 5)
        {
            UserAccount *u = find_user(username);
            if (u)
            {
                strncpy(u->password_hash, pw, sizeof(u->password_hash) - 1);
                u->password_hash[sizeof(u->password_hash) - 1] = '\0';
                u->permission_level = perm;
                u->locked = locked;
                u->failed_attempts = attempts;
            }
        }
    }

    fclose(fp);
}

static void save_users_locked(void)
{
    FILE *fp = fopen(STATE_FILE, "w");
    if (!fp)
        return;

    for (size_t i = 0; i < USER_COUNT; i++)
    {
        UserAccount *u = &users[i];
        fprintf(fp, "%s,%s,%d,%d,%d\n",
                u->username,
                u->password_hash,
                u->permission_level,
                u->locked ? 1 : 0,
                u->failed_attempts);
    }

    fclose(fp);
}

static UserAccount *find_user(const char *user)
{
    if (!user)
        return NULL;

    for (size_t i = 0; i < sizeof(users) / sizeof(users[0]); i++)
    {
        if (strcmp(users[i].username, user) == 0)
            return &users[i];
    }
    return NULL;
}

bool auth_init(void)
{
    pthread_mutex_lock(&users_lock);
    reset_to_defaults_locked();
    load_users_locked();
    save_users_locked();
    pthread_mutex_unlock(&users_lock);
    return true;
}

bool get_user_info(const char *user, UserAccount *out)
{
    bool found = false;
    pthread_mutex_lock(&users_lock);
    UserAccount *u = find_user(user);
    if (u && out)
    {
        *out = *u;
        found = true;
    }
    pthread_mutex_unlock(&users_lock);
    return found;
}

AuthResult verify_credentials(const char *user, const char *provided_hash, int *out_permission_level, int *out_remaining_attempts)
{
    if (out_permission_level)
        *out_permission_level = 0;
    if (out_remaining_attempts)
        *out_remaining_attempts = -1;

    if (!user || !provided_hash)
        return AUTH_INVALID;

    pthread_mutex_lock(&users_lock);
    UserAccount *u = find_user(user);
    if (!u)
    {
        pthread_mutex_unlock(&users_lock);
        return AUTH_INVALID;
    }

    if (u->locked)
    {
        pthread_mutex_unlock(&users_lock);
        if (out_remaining_attempts)
            *out_remaining_attempts = 0;
        return AUTH_LOCKED;
    }

    if (strcmp(u->password_hash, provided_hash) == 0)
    {
        u->failed_attempts = 0;
        if (out_permission_level)
            *out_permission_level = u->permission_level;
        save_users_locked();
        pthread_mutex_unlock(&users_lock);
        return AUTH_OK;
    }

    u->failed_attempts += 1;
    if (u->failed_attempts >= MAX_ATTEMPTS)
        u->locked = true;

    save_users_locked();

    int remaining = u->locked ? 0 : (MAX_ATTEMPTS - u->failed_attempts);
    pthread_mutex_unlock(&users_lock);

    if (out_remaining_attempts)
        *out_remaining_attempts = remaining;

    return u->locked ? AUTH_LOCKED : AUTH_INVALID;
}
