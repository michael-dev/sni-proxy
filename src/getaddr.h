#ifndef SNI_GETADDR_H
#define SNI_GETADDR_H

typedef void (*reqCb) (void *data, struct addrinfo *res, int ret);

int send_request(const char *node, const char *service, int socktype, int protocol, int flags, reqCb cb, void *cbctx);
bool start_signalfd(struct ev_loop *loop);

#endif
