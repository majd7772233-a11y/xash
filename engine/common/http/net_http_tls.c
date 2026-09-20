/*
net_http_tls.c - TLS backend for the built-in HTTP client (mbedTLS)
*/

#include "common.h"
#include "net_ws_private.h"
#include "net_http_tls.h"

#if XASH_MBEDTLS

#include <psa/crypto.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/debug.h>
#include <mbedtls/error.h>
#include <errno.h>

extern poolhandle_t http_mempool;

static CVAR_DEFINE_AUTO(http_tls_cafile, "cacert.pem", FCVAR_PRIVILEGED,
	"path to CA bundle (PEM) used to verify HTTPS/WSS servers");
static CVAR_DEFINE_AUTO(http_tls_insecure, "0", FCVAR_PRIVILEGED,
	"skip TLS certificate verification (debug only)");
static CVAR_DEFINE_AUTO(http_tls_verbose, "0", FCVAR_PRIVILEGED,
	"mbedTLS debug verbosity (0=off)");

typedef int (*pin_verify_fn_t)(void *cert, int depth, uint32_t *flags);

static struct
{
	qboolean inited;
	qboolean has_ca;
	mbedtls_x509_crt cacert;
	pin_verify_fn_t pin_verify;
} g_tls;

struct tlsctx_s
{
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	int socket;
};

static void HTTP_TlsLogDebug(void *ctx, int level, const char *file, int line, const char *str)
{
	(void)ctx;
	Con_Printf("TLS[%d] %s:%d %s", level, COM_FileWithoutPath(file), line, str);
}

static void HTTP_TlsLogErr(const char *what, int ret)
{
	char msg[128];
	mbedtls_strerror(ret, msg, sizeof(msg));
	Con_Printf(S_ERROR "TLS %s failed: %s (-0x%04x)\n", what, msg, (unsigned int)-ret);
}

static int HTTP_TlsSocketError(void)
{
#if XASH_WIN32
	return WSAGetLastError();
#else
	return errno;
#endif
}

static qboolean HTTP_TlsWouldBlockError(int err)
{
#if XASH_WIN32
	return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEALREADY;
#else
	return err == EAGAIN || err == EWOULDBLOCK || err == EINPROGRESS || err == EALREADY;
#endif
}

static int HTTP_TlsBioSend(void *ctx, const unsigned char *buf, size_t len)
{
	int fd = *(int *)ctx;
	int n = send(fd, buf, len, 0);
	if (n >= 0)
		return n;

	if (HTTP_TlsWouldBlockError(HTTP_TlsSocketError()))
		return MBEDTLS_ERR_SSL_WANT_WRITE;
	return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int HTTP_TlsBioRecv(void *ctx, unsigned char *buf, size_t len)
{
	int fd = *(int *)ctx;
	int n = recv(fd, buf, len, 0);
	if (n > 0)
		return n;
	if (n == 0)
		return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;

	if (HTTP_TlsWouldBlockError(HTTP_TlsSocketError()))
		return MBEDTLS_ERR_SSL_WANT_READ;
	return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static qboolean HTTP_TlsLoadCA(void)
{
	const char *path = http_tls_cafile.string;
	fs_offset_t len = 0;
	byte *data;
	int ret;

	if (!path || !path[0])
		return false;

	data = FS_LoadFile(path, &len, false);
	if (!data || len <= 0)
	{
		Con_Printf(S_WARN "TLS: CA bundle '%s' not found; secure TLS is unavailable\n", path);
		Mem_Free(data);
		return false;
	}

	ret = mbedtls_x509_crt_parse(&g_tls.cacert, data, len + 1);
	Mem_Free(data);
	if (ret < 0)
	{
		HTTP_TlsLogErr("x509_crt_parse", ret);
		return false;
	}
	if (ret > 0)
		Con_Reportf(S_WARN "TLS: %d certificate(s) in '%s' failed to parse\n", ret, path);
	return true;
}

void HTTP_TlsInit(void)
{
	psa_status_t pstatus;

	if (g_tls.inited)
		return;

	Cvar_RegisterVariable(&http_tls_cafile);
	Cvar_RegisterVariable(&http_tls_insecure);
	Cvar_RegisterVariable(&http_tls_verbose);

	pstatus = psa_crypto_init();
	if (pstatus != PSA_SUCCESS)
	{
		Con_Printf(S_ERROR "TLS psa_crypto_init failed (status %d)\n", (int)pstatus);
		return;
	}

	mbedtls_x509_crt_init(&g_tls.cacert);
	g_tls.has_ca = HTTP_TlsLoadCA();
	g_tls.pin_verify = NULL;
	g_tls.inited = true;

	if (!g_tls.has_ca && http_tls_insecure.value == 0)
		Con_Printf(S_WARN "TLS: secure verification disabled until a CA bundle is available at '%s'\n", http_tls_cafile.string);
}

void HTTP_TlsShutdown(void)
{
	if (!g_tls.inited)
		return;
	mbedtls_x509_crt_free(&g_tls.cacert);
	mbedtls_psa_crypto_free();
	g_tls.inited = false;
	g_tls.has_ca = false;
}

qboolean HTTP_TlsAvailable(void)
{
	return g_tls.inited && (g_tls.has_ca || http_tls_insecure.value != 0);
}

tlsctx_t *HTTP_TlsNew(int socket, const char *hostname)
{
	tlsctx_t *ctx;
	int ret;
	int authmode;

	if (!g_tls.inited)
		return NULL;
	if (!g_tls.has_ca && http_tls_insecure.value == 0)
		return NULL;

	ctx = Mem_Calloc(http_mempool, sizeof(*ctx));
	ctx->socket = socket;
	mbedtls_ssl_init(&ctx->ssl);
	mbedtls_ssl_config_init(&ctx->conf);

	ret = mbedtls_ssl_config_defaults(&ctx->conf, MBEDTLS_SSL_IS_CLIENT,
		MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
	if (ret != 0)
	{
		HTTP_TlsLogErr("ssl_config_defaults", ret);
		HTTP_TlsFree(ctx);
		return NULL;
	}

	authmode = (http_tls_insecure.value != 0) ? MBEDTLS_SSL_VERIFY_NONE : MBEDTLS_SSL_VERIFY_REQUIRED;
	mbedtls_ssl_conf_authmode(&ctx->conf, authmode);
	mbedtls_ssl_conf_ca_chain(&ctx->conf, &g_tls.cacert, NULL);
	mbedtls_ssl_conf_dbg(&ctx->conf, HTTP_TlsLogDebug, NULL);
	mbedtls_debug_set_threshold(http_tls_verbose.value);

	ret = mbedtls_ssl_setup(&ctx->ssl, &ctx->conf);
	if (ret != 0)
	{
		HTTP_TlsLogErr("ssl_setup", ret);
		HTTP_TlsFree(ctx);
		return NULL;
	}

	if (!hostname || !hostname[0] || mbedtls_ssl_set_hostname(&ctx->ssl, hostname) != 0)
	{
		Con_Printf(S_ERROR "TLS: invalid TLS hostname\n");
		HTTP_TlsFree(ctx);
		return NULL;
	}

	mbedtls_ssl_set_bio(&ctx->ssl, &ctx->socket, HTTP_TlsBioSend, HTTP_TlsBioRecv, NULL);
	return ctx;
}

void HTTP_TlsFree(tlsctx_t *ctx)
{
	if (!ctx)
		return;
	mbedtls_ssl_free(&ctx->ssl);
	mbedtls_ssl_config_free(&ctx->conf);
	Mem_Free(ctx);
}

int HTTP_TlsHandshake(tlsctx_t *ctx)
{
	int ret;
	if (!ctx)
		return HTTP_TLS_ERROR;

	ret = mbedtls_ssl_handshake(&ctx->ssl);
	if (ret == 0)
		return HTTP_TLS_OK;
	if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
		return HTTP_TLS_WANT;
	HTTP_TlsLogErr("handshake", ret);
	return HTTP_TLS_ERROR;
}

int HTTP_TlsSend(tlsctx_t *ctx, const void *buf, int len)
{
	int ret;
	if (!ctx || !buf || len <= 0)
		return HTTP_TLS_ERROR;

	ret = mbedtls_ssl_write(&ctx->ssl, (const unsigned char *)buf, (size_t)len);
	if (ret >= 0)
		return ret;
	if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
		return HTTP_TLS_WANT;
	HTTP_TlsLogErr("send", ret);
	return HTTP_TLS_ERROR;
}

int HTTP_TlsRecv(tlsctx_t *ctx, void *buf, int len)
{
	int ret;
	if (!ctx || !buf || len <= 0)
		return HTTP_TLS_ERROR;

	ret = mbedtls_ssl_read(&ctx->ssl, (unsigned char *)buf, (size_t)len);
	if (ret > 0)
		return ret;
	if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
		return 0;
	if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
		return HTTP_TLS_WANT;
	HTTP_TlsLogErr("recv", ret);
	return HTTP_TLS_ERROR;
}

#endif /* XASH_MBEDTLS */
