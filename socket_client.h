#ifndef SOCKET_CLIENT_H
#define SOCKET_CLIENT_H

#include <stddef.h>
extern int sockfd;
int socket_connect_to(const char *server_ip, int port);
void socket_send_cmd(const char *cmd);
int socket_recv_response(char *outbuf, size_t size);
void socket_close(void);

#endif
