/* lx_bknet.h - the backend's sockets (ESOCK on EPOC, BSD sockets on the
 * host), for lx_net.c. Every call returns at once: connect, accept and the
 * transfers start the work and report progress through bk_sock_poll; the
 * backend calls lx_k_event() when anything changes, and lx_net.c tries the
 * parked calls again. Errors are -LX_E*. */
#ifndef LX_BKNET_H
#define LX_BKNET_H

#define BK_SOCK_TCP 1
#define BK_SOCK_UDP 2

void *bk_sock_new(int kind, int *err);
void bk_sock_close(void *s);
int bk_sock_connect(void *s, lx_u32 addr, lx_u16 port);      /* 0 or -EINPROGRESS: poll for LX_POLLOUT */
int bk_sock_bind(void *s, lx_u32 addr, lx_u16 port);
int bk_sock_listen(void *s, int backlog);
void *bk_sock_accept(void *s, int *err, lx_u32 *addr, lx_u16 *port);   /* NULL + -EAGAIN while none */
int bk_sock_send(void *s, const char *buf, lx_u32 n);       /* bytes taken, or -EAGAIN */
int bk_sock_recv(void *s, char *buf, lx_u32 n, int peek);   /* bytes, 0 at the end, -EAGAIN */
int bk_sock_sendto(void *s, const char *buf, lx_u32 n, lx_u32 addr, lx_u16 port);
int bk_sock_recvfrom(void *s, char *buf, lx_u32 n, lx_u32 *addr, lx_u16 *port);
int bk_sock_shutdown(void *s, int how);
int bk_sock_poll(void *s);                                  /* LX_POLL* now */
int bk_sock_error(void *s);                                 /* SO_ERROR: the pending error, cleared */
int bk_sock_name(void *s, int peer, lx_u32 *addr, lx_u16 *port);
/* name lookup: starts it; bk_resolve_poll gives -EAGAIN until it is done */
void *bk_resolve_start(const char *name, int *err);
int bk_resolve_poll(void *r, lx_u32 *addrs, int max);       /* count, or -errno */
void bk_resolve_free(void *r);
int bk_dns_server(lx_u32 *addr);                            /* 0 if one is known */

#endif
