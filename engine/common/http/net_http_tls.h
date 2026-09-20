/*
net_http_tls.h - TLS plumbing for HTTPS and WSS in the built-in HTTP client
*/
#ifndef NET_HTTP_TLS_H
#define NET_HTTP_TLS_H

#include "xash3d_types.h"

typedef struct tlsctx_s tlsctx_t;

enum
{
	HTTP_TLS_OK    =  0,
	HTTP_TLS_WANT  = -1,
	HTTP_TLS_ERROR = -2
};

#if XASH_MBEDTLS

void HTTP_TlsInit(void);
void HTTP_TlsShutdown(void);
qboolean HTTP_TlsAvailable(void);
tlsctx_t *HTTP_TlsNew(int socket, const char *hostname);
void HTTP_TlsFree(tlsctx_t *ctx);
int HTTP_TlsHandshake(tlsctx_t *ctx);
int HTTP_TlsSend(tlsctx_t *ctx, const void *buf, int len);
int HTTP_TlsRecv(tlsctx_t *ctx, void *buf, int len);

#else

static inline void HTTP_TlsInit(void) { }
static inline void HTTP_TlsShutdown(void) { }
static inline qboolean HTTP_TlsAvailable(void) { return false; }
static inline tlsctx_t *HTTP_TlsNew(int socket, const char *hostname)
{
	(void)socket; (void)hostname; return NULL;
}
static inline void HTTP_TlsFree(tlsctx_t *ctx) { (void)ctx; }
static inline int HTTP_TlsHandshake(tlsctx_t *ctx) { (void)ctx; return HTTP_TLS_ERROR; }
static inline int HTTP_TlsSend(tlsctx_t *ctx, const void *buf, int len)
{
	(void)ctx; (void)buf; (void)len; return HTTP_TLS_ERROR;
}
static inline int HTTP_TlsRecv(tlsctx_t *ctx, void *buf, int len)
{
	(void)ctx; (void)buf; (void)len; return HTTP_TLS_ERROR;
}

#endif

#endif /* NET_HTTP_TLS_H */
