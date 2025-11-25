#ifndef SOCKET_CLIENT_H
#define SOCKET_CLIENT_H

#include <stddef.h>
extern int sockfd;
int socket_connect_to(const char *server_ip, int port);
void socket_send_cmd(const char *cmd);
void socket_send_login(const char *username);
void socket_send_chat(const char *msg);
int socket_recv_response(char *outbuf, size_t size);
int socket_recv_nonblock(char *outbuf, size_t size);
int socket_connected(void);
void socket_close(void);

#endif
