/*
 * magd_net.c - MAGD Network Abstraction Layer
 *
 * Transport:
 *   Xash UDP datagram -> MAGD envelope -> WebSocket binary frame
 *
 * The engine intentionally implements only a small WebSocket client here.
 * The actual room/router/authentication logic lives in xash-server.
 */

#include "magd_net.h"
#include "net_ws_private.h"
#include "net_http_tls.h"
#include "xash3d_mathlib.h"
#include "tests.h"

#include <errno.h>
#include <stdint.h>

CVAR_DEFINE(magd_enabled,
	"magd_enabled",
	"1",
	FCVAR_ARCHIVE,
	"Enable MAGD Network Layer");

CVAR_DEFINE(magd_server_url,
	"magd_server_url",
	"https://xash-server.magd.workers.dev",
	FCVAR_ARCHIVE,
	"MAGD Server URL");

CVAR_DEFINE(magd_room_code,
	"magd_room_code",
	"",
	FCVAR_ARCHIVE,
	"MAGD Room Code");

CVAR_DEFINE(magd_auth_token,
	"magd_auth_token",
	"",
	0,
	"MAGD Authentication Token");

CVAR_DEFINE(magd_room_password,
	"magd_room_password",
	"",
	0,
	"MAGD Room Password");

#define MAGD_RX_BUFFER_SIZE (128 * 1024)
#define MAGD_FRAGMENT_SIZE (MAGD_MAX_PACKET_SIZE + 16)
#define MAGD_DATA_TX_SIZE (MAGD_MAX_PACKET_SIZE + 16)
#define MAGD_CONTROL_TX_SIZE 256
#define MAGD_HTTP_REQUEST_SIZE 2048
#define MAGD_HTTP_HEADER_SIZE 8192

typedef enum magd_state_e
{
	MAGD_STATE_IDLE = 0,
	MAGD_STATE_TCP_CONNECTING,
	MAGD_STATE_TLS,
	MAGD_STATE_WS_SEND,
	MAGD_STATE_WS_RECV,
	MAGD_STATE_OPEN,
	MAGD_STATE_ERROR
} magd_state_t;

static magd_net_mode_t g_mode = MAGD_NET_MODE_LAN;
static magd_state_t g_state = MAGD_STATE_IDLE;

static magd_queue_t g_incoming;
static magd_queue_t g_outgoing;
static magd_session_map_t g_sessions[MAGD_MAX_SESSIONS];

static int g_socket = -1;
static tlsctx_t *g_tls = NULL;

static qboolean g_host = false;
static qboolean g_open = false;

static byte g_rx[MAGD_RX_BUFFER_SIZE];
static size_t g_rx_len = 0;

static byte g_fragment[MAGD_FRAGMENT_SIZE];
static qboolean g_ws_fragmented = false;
static int g_ws_fragment_opcode = 0;
static size_t g_ws_fragment_len = 0;

static byte g_data_tx[MAGD_DATA_TX_SIZE];
static size_t g_data_tx_len = 0;
static size_t g_data_tx_pos = 0;

static byte g_control_tx[MAGD_CONTROL_TX_SIZE];
static size_t g_control_tx_len = 0;
static size_t g_control_tx_pos = 0;

static char g_request[MAGD_HTTP_REQUEST_SIZE];
static size_t g_request_len = 0;
static size_t g_request_pos = 0;

static char g_expected_accept[64];

static char g_host_name[256];
static char g_host_header[256];
static int g_port = 0;
static qboolean g_use_tls = false;

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static qboolean MAGD_WouldBlock(void)
{
#if XASH_WIN32
	int e = WSAGetLastError();
	return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
#else
	return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EALREADY;
#endif
}

static qboolean MAGD_HeaderSafe(const char *value)
{
	if (!value)
		return false;

	for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
	{
		if (*p == '\r' || *p == '\n')
			return false;
	}

	return true;
}

static size_t MAGD_Base64(const byte *src, size_t len, char *dst, size_t cap)
{
	static const char alphabet[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	size_t out = 0;

	for (size_t i = 0; i < len; i += 3)
	{
		unsigned int value = (unsigned int)src[i] << 16;

		if (i + 1 < len)
			value |= (unsigned int)src[i + 1] << 8;

		if (i + 2 < len)
			value |= (unsigned int)src[i + 2];

		if (out + 4 >= cap)
			return 0;

		dst[out++] = alphabet[(value >> 18) & 63];
		dst[out++] = alphabet[(value >> 12) & 63];
		dst[out++] = i + 1 < len ? alphabet[(value >> 6) & 63] : '=';
		dst[out++] = i + 2 < len ? alphabet[value & 63] : '=';
	}

	if (!cap)
		return 0;

	dst[out] = 0;
	return out;
}

/* ------------------------------------------------------------------------- */
/* SHA-1 for Sec-WebSocket-Accept                                           */
/* ------------------------------------------------------------------------- */

typedef struct magd_sha1_s
{
	uint32_t h[5];
	uint64_t total;
	byte block[64];
	size_t used;
} magd_sha1_t;

static uint32_t MAGD_ROL(uint32_t x, int n)
{
	return (x << n) | (x >> (32 - n));
}

static void MAGD_SHA1Block(magd_sha1_t *s, const byte *block)
{
	uint32_t w[80];
	uint32_t a, b, c, d, e;

	for (int i = 0; i < 16; ++i)
	{
		w[i] = ((uint32_t)block[i * 4] << 24) |
			((uint32_t)block[i * 4 + 1] << 16) |
			((uint32_t)block[i * 4 + 2] << 8) |
			(uint32_t)block[i * 4 + 3];
	}

	for (int i = 16; i < 80; ++i)
		w[i] = MAGD_ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

	a = s->h[0];
	b = s->h[1];
	c = s->h[2];
	d = s->h[3];
	e = s->h[4];

	for (int i = 0; i < 80; ++i)
	{
		uint32_t f;
		uint32_t k;
		uint32_t temp;

		if (i < 20)
		{
			f = (b & c) | ((~b) & d);
			k = 0x5A827999;
		}
		else if (i < 40)
		{
			f = b ^ c ^ d;
			k = 0x6ED9EBA1;
		}
		else if (i < 60)
		{
			f = (b & c) | (b & d) | (c & d);
			k = 0x8F1BBCDC;
		}
		else
		{
			f = b ^ c ^ d;
			k = 0xCA62C1D6;
		}

		temp = MAGD_ROL(a, 5) + f + e + k + w[i];
		e = d;
		d = c;
		c = MAGD_ROL(b, 30);
		b = a;
		a = temp;
	}

	s->h[0] += a;
	s->h[1] += b;
	s->h[2] += c;
	s->h[3] += d;
	s->h[4] += e;
}

static void MAGD_SHA1Init(magd_sha1_t *s)
{
	memset(s, 0, sizeof(*s));

	s->h[0] = 0x67452301;
	s->h[1] = 0xEFCDAB89;
	s->h[2] = 0x98BADCFE;
	s->h[3] = 0x10325476;
	s->h[4] = 0xC3D2E1F0;
}

static void MAGD_SHA1Update(magd_sha1_t *s, const void *data_, size_t len)
{
	const byte *data = (const byte *)data_;

	s->total += len;

	while (len)
	{
		size_t n = Q_min((int)(64 - s->used), (int)len);

		memcpy(s->block + s->used, data, n);
		s->used += n;
		data += n;
		len -= n;

		if (s->used == 64)
		{
			MAGD_SHA1Block(s, s->block);
			s->used = 0;
		}
	}
}

static void MAGD_SHA1Final(magd_sha1_t *s, byte out[20])
{
	uint64_t bits = s->total * 8;
	byte one = 0x80;
	byte zero = 0;
	byte length[8];

	MAGD_SHA1Update(s, &one, 1);

	while (s->used != 56)
		MAGD_SHA1Update(s, &zero, 1);

	for (int i = 0; i < 8; ++i)
		length[7 - i] = (byte)(bits >> (i * 8));

	MAGD_SHA1Update(s, length, sizeof(length));

	for (int i = 0; i < 5; ++i)
	{
		out[i * 4] = (byte)(s->h[i] >> 24);
		out[i * 4 + 1] = (byte)(s->h[i] >> 16);
		out[i * 4 + 2] = (byte)(s->h[i] >> 8);
		out[i * 4 + 3] = (byte)s->h[i];
	}
}

static qboolean MAGD_MakeWebSocketAccept(const char *key, char *out, size_t cap)
{
	static const char websocket_guid[] =
		"258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

	char input[256];
	byte digest[20];
	magd_sha1_t sha;

	if (!key || !key[0])
		return false;

	Q_snprintf(input, sizeof(input), "%s%s", key, websocket_guid);

	MAGD_SHA1Init(&sha);
	MAGD_SHA1Update(&sha, input, Q_strlen(input));
	MAGD_SHA1Final(&sha, digest);

	return MAGD_Base64(digest, sizeof(digest), out, cap) != 0;
}

/* ------------------------------------------------------------------------- */
/* Minimal JSON string extraction                                            */
/* ------------------------------------------------------------------------- */

static qboolean MAGD_JsonString(const byte *data, size_t len, const char *key, char *out, size_t cap)
{
	char text[4096];
	char needle[128];
	size_t n;
	char *p;
	char *end;

	if (!data || !key || !out || cap < 2)
		return false;

	n = Q_min((int)len, (int)sizeof(text) - 1);
	memcpy(text, data, n);
	text[n] = 0;

	Q_snprintf(needle, sizeof(needle), "\"%s\"", key);

	p = Q_stristr(text, needle);
	if (!p)
		return false;

	p = Q_strchr(p + Q_strlen(needle), ':');
	if (!p)
		return false;

	++p;
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		++p;

	if (*p != '\"')
		return false;

	++p;
	end = Q_strchr(p, '\"');
	if (!end)
		return false;

	n = Q_min((int)(end - p), (int)cap - 1);
	memcpy(out, p, n);
	out[n] = 0;
	return true;
}

/* ------------------------------------------------------------------------- */
/* Queue/session mapping                                                     */
/* ------------------------------------------------------------------------- */

void MAGD_QueueInit(magd_queue_t *q)
{
	if (q)
		memset(q, 0, sizeof(*q));
}

qboolean MAGD_QueuePush(magd_queue_t *q, const void *data, size_t length, const netadr_t *adr)
{
	magd_packet_t *p;

	if (!q || !data || !length || length > MAGD_MAX_PACKET_SIZE)
		return false;

	if (q->count >= MAGD_QUEUE_SIZE)
		return false;

	p = &q->packets[q->tail];
	memcpy(p->data, data, length);
	p->length = length;

	if (adr)
		p->adr = *adr;
	else
		memset(&p->adr, 0, sizeof(p->adr));

	q->tail = (q->tail + 1) % MAGD_QUEUE_SIZE;
	++q->count;
	return true;
}

qboolean MAGD_QueuePop(magd_queue_t *q, byte *data, size_t *length, netadr_t *adr)
{
	magd_packet_t *p;

	if (!q || !data || !length || q->count <= 0)
		return false;

	p = &q->packets[q->head];
	memcpy(data, p->data, p->length);
	*length = p->length;

	if (adr)
		*adr = p->adr;

	q->head = (q->head + 1) % MAGD_QUEUE_SIZE;
	--q->count;
	return true;
}

void MAGD_ClearSessionMaps(void)
{
	memset(g_sessions, 0, sizeof(g_sessions));
}

qboolean MAGD_MapSessionToAddress(const char *session_id, netadr_t *out_adr)
{
	if (!session_id || !session_id[0] || !out_adr)
		return false;

	for (int i = 0; i < MAGD_MAX_SESSIONS; ++i)
	{
		if (g_sessions[i].active && !Q_strcmp(g_sessions[i].session_id, session_id))
		{
			*out_adr = g_sessions[i].virtual_adr;
			return true;
		}
	}

	for (int i = 0; i < MAGD_MAX_SESSIONS; ++i)
	{
		if (!g_sessions[i].active)
		{
			Q_strncpy(g_sessions[i].session_id,
				session_id,
				sizeof(g_sessions[i].session_id));

			memset(&g_sessions[i].virtual_adr, 0, sizeof(netadr_t));
			NET_NetadrSetType(&g_sessions[i].virtual_adr, NA_IP);

			g_sessions[i].virtual_adr.ip[0] = 10;
			g_sessions[i].virtual_adr.ip[1] = 254;
			g_sessions[i].virtual_adr.ip[2] = 0;
			g_sessions[i].virtual_adr.ip[3] = (byte)(i + 1);
			g_sessions[i].virtual_adr.port = BigShort(27015);
			g_sessions[i].active = true;

			*out_adr = g_sessions[i].virtual_adr;
			return true;
		}
	}

	return false;
}

const char *MAGD_MapAddressToSession(const netadr_t *adr)
{
	if (!adr)
		return NULL;

	for (int i = 0; i < MAGD_MAX_SESSIONS; ++i)
	{
		if (g_sessions[i].active && NET_CompareAdr(g_sessions[i].virtual_adr, *adr))
			return g_sessions[i].session_id;
	}

	return NULL;
}

/* ------------------------------------------------------------------------- */
/* Socket / URL                                                              */
/* ------------------------------------------------------------------------- */

static void MAGD_ResetTransport(void)
{
	if (g_tls)
	{
		HTTP_TlsFree(g_tls);
		g_tls = NULL;
	}

	if (NET_IsSocketValid(g_socket))
	{
		closesocket(g_socket);
		g_socket = -1;
	}

	g_open = false;
	g_state = MAGD_STATE_IDLE;

	g_rx_len = 0;
	g_ws_fragmented = false;
	g_ws_fragment_opcode = 0;
	g_ws_fragment_len = 0;

	g_data_tx_len = 0;
	g_data_tx_pos = 0;
	g_control_tx_len = 0;
	g_control_tx_pos = 0;

	g_request_len = 0;
	g_request_pos = 0;
	g_expected_accept[0] = 0;
}

static qboolean MAGD_ParseServerUrl(const char *url,
	char *hostname,
	size_t hostname_cap,
	char *host_header,
	size_t host_header_cap,
	int *port,
	qboolean *use_tls)
{
	const char *authority;
	const char *end;
	char *colon = NULL;
	char authority_buf[256];
	size_t authority_len;

	if (!url || !url[0] || !hostname || !host_header || !port || !use_tls)
		return false;

	if (!Q_strnicmp(url, "https://", 8))
	{
		authority = url + 8;
		*port = 443;
		*use_tls = true;
	}
	else if (!Q_strnicmp(url, "http://", 7))
	{
		authority = url + 7;
		*port = 80;
		*use_tls = false;
	}
	else
	{
		return false;
	}

	end = authority;
	while (*end && *end != '/' && *end != '?' && *end != '#')
		++end;

	authority_len = (size_t)(end - authority);
	if (!authority_len || authority_len >= sizeof(authority_buf))
		return false;

	memcpy(authority_buf, authority, authority_len);
	authority_buf[authority_len] = 0;

	/* Bracketed IPv6: [::1] or [::1]:8787 */
	if (authority_buf[0] == '[')
	{
		char *close = Q_strchr(authority_buf, ']');
		qboolean explicit_port = false;

		if (!close)
			return false;

		if (close[1] == ':')
		{
			int parsed = Q_atoi(close + 2);
			if (parsed < 1 || parsed > 65535)
				return false;
			*port = parsed;
			explicit_port = true;
		}
		else if (close[1] != 0)
		{
			return false;
		}

		{
			char ipv6[256];
			size_t ipv6_len = (size_t)(close - authority_buf - 1);
			if (ipv6_len == 0 || ipv6_len >= sizeof(ipv6))
				return false;
			memcpy(ipv6, authority_buf + 1, ipv6_len);
			ipv6[ipv6_len] = 0;
			Q_strncpy(hostname, ipv6, hostname_cap);
		}

		if (explicit_port)
			Q_snprintf(host_header, host_header_cap, "[%s]:%d", hostname, *port);
		else
			Q_snprintf(host_header, host_header_cap, "[%s]", hostname);

		return hostname[0] != 0;
	}

	/* IPv4/hostname with optional final :port. */
	for (char *p = authority_buf; *p; ++p)
		if (*p == ':')
			colon = p;

	if (colon && colon[1])
	{
		qboolean digits = true;
		for (const char *p = colon + 1; *p; ++p)
		{
			if (*p < '0' || *p > '9')
			{
				digits = false;
				break;
			}
		}

		if (digits)
		{
			int parsed = Q_atoi(colon + 1);
			if (parsed < 1 || parsed > 65535)
				return false;
			*colon = 0;
			*port = parsed;
		}
	}

	Q_strncpy(hostname, authority_buf, hostname_cap);

	if (!hostname[0])
		return false;

	/* Host header keeps an explicit/non-default port. */
	if ((!*use_tls && *port != 80) || (*use_tls && *port != 443))
		Q_snprintf(host_header, host_header_cap, "%s:%d", hostname, *port);
	else
		Q_strncpy(host_header, hostname, host_header_cap);

	return true;
}

static qboolean MAGD_OpenSocket(void)
{
	struct sockaddr_storage addr;

	memset(&addr, 0, sizeof(addr));

	if (NET_StringToSockaddr(g_host_name,
		&addr,
		false,
		AF_UNSPEC) != NET_EAI_OK)
		return false;

	if (addr.ss_family == AF_INET)
	{
		((struct sockaddr_in *)&addr)->sin_port = BigShort(g_port);
	}
#if defined(AF_INET6)
	else if (addr.ss_family == AF_INET6)
	{
		((struct sockaddr_in6 *)&addr)->sin6_port = BigShort(g_port);
	}
#endif

	g_socket = socket(addr.ss_family, SOCK_STREAM, IPPROTO_TCP);

	if (!NET_IsSocketValid(g_socket))
	{
		g_socket = -1;
		return false;
	}

	if (!NET_MakeSocketNonBlocking(g_socket))
	{
		closesocket(g_socket);
		g_socket = -1;
		return false;
	}

	{
		int result = connect(g_socket,
			(const struct sockaddr *)&addr,
			NET_SockAddrLen(&addr));

		if (result == 0)
		{
			g_state = g_use_tls ? MAGD_STATE_TLS : MAGD_STATE_WS_SEND;
			return true;
		}

		if (MAGD_WouldBlock())
		{
			g_state = MAGD_STATE_TCP_CONNECTING;
			return true;
		}
	}

	MAGD_ResetTransport();
	return false;
}

/* 1 = connected, 0 = still pending, -1 = failed. */
static int MAGD_CheckTcpConnected(void)
{
	int error_code = 0;

#if XASH_WIN32
	int opt_len = (int)sizeof(error_code);
#else
	socklen_t opt_len = (socklen_t)sizeof(error_code);
#endif

	if (!NET_IsSocketValid(g_socket))
		return -1;

	if (getsockopt(g_socket,
		SOL_SOCKET,
		SO_ERROR,
		(char *)&error_code,
		&opt_len) < 0)
		return -1;

	if (error_code == 0)
		return 1;

#if XASH_WIN32
	if (error_code == WSAEINPROGRESS ||
		error_code == WSAEALREADY ||
		error_code == WSAEWOULDBLOCK)
		return 0;
#else
	if (error_code == EINPROGRESS ||
		error_code == EALREADY ||
		error_code == EAGAIN ||
		error_code == EWOULDBLOCK)
		return 0;
#endif

	return -1;
}

/* ------------------------------------------------------------------------- */
/* WebSocket handshake                                                      */
/* ------------------------------------------------------------------------- */

static qboolean MAGD_BuildHandshakeRequest(void)
{
	byte random_key[16];
	char key[64];

	for (int i = 0; i < (int)sizeof(random_key); ++i)
		random_key[i] = (byte)COM_RandomLong(0, 255);

	if (!MAGD_Base64(random_key,
		sizeof(random_key),
		key,
		sizeof(key)))
		return false;

	if (!MAGD_MakeWebSocketAccept(key,
		g_expected_accept,
		sizeof(g_expected_accept)))
		return false;

	if (!MAGD_HeaderSafe(magd_auth_token.string) ||
		!MAGD_HeaderSafe(magd_room_password.string) ||
		!MAGD_HeaderSafe(key))
		return false;

	Q_snprintf(g_request,
		sizeof(g_request),
		"GET /ws/room/%s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Authorization: Bearer %s\r\n"
		"Sec-WebSocket-Key: %s\r\n"
		"Sec-WebSocket-Version: 13\r\n"
		"%s"
		"\r\n",
		magd_room_code.string,
		g_host_header,
		magd_auth_token.string,
		key,
		(!g_host && magd_room_password.string[0])
			? va("X-MAGD-Room-Password: %s\r\n", magd_room_password.string)
			: "");

	g_request_len = Q_strlen(g_request);
	g_request_pos = 0;
	g_state = MAGD_STATE_WS_SEND;
	return true;
}

static qboolean MAGD_HeaderValue(char *headers,
	const char *name,
	char *out,
	size_t cap)
{
	size_t name_len;
	char *line = headers;

	if (!headers || !name || !out || cap < 2)
		return false;

	name_len = Q_strlen(name);

	while (line && *line)
	{
		char *line_end = Q_strchr(line, '\n');
		char *colon;
		char *value;
		size_t value_len;

		if (line_end)
			*line_end = 0;

		while (*line == '\r')
			++line;

		colon = Q_strchr(line, ':');
		if (colon && (size_t)(colon - line) == name_len &&
			!Q_strnicmp(line, name, (int)name_len))
		{
			value = colon + 1;

			while (*value == ' ' || *value == '\t')
				++value;

			value_len = Q_strlen(value);
			while (value_len &&
				(value[value_len - 1] == '\r' ||
				 value[value_len - 1] == ' ' ||
				 value[value_len - 1] == '\t'))
				--value_len;

			value_len = Q_min((int)value_len, (int)cap - 1);
			memcpy(out, value, value_len);
			out[value_len] = 0;

			if (line_end)
				*line_end = '\n';
			return true;
		}

		if (!line_end)
			break;

		*line_end = '\n';
		line = line_end + 1;

		if (*line == '\r' || *line == '\n' || !*line)
			break;
	}

	return false;
}

static qboolean MAGD_HandshakeComplete(void)
{
	char *end = NULL;

	if (g_rx_len < 4)
		return false;

	for (size_t i = 3; i < g_rx_len; ++i)
	{
		if (g_rx[i - 3] == '\r' &&
			g_rx[i - 2] == '\n' &&
			g_rx[i - 1] == '\r' &&
			g_rx[i] == '\n')
		{
			end = (char *)g_rx + i + 1;
			break;
		}
	}

	if (!end)
		return true; /* Header is incomplete; wait for more bytes. */

	{
		size_t header_len = (size_t)(end - (char *)g_rx);
		char headers[MAGD_HTTP_HEADER_SIZE];
		char upgrade[64];
		char connection[128];
		char accept[128];
		char status_line[32];
		char *line_end;

		if (header_len >= sizeof(headers))
			return false;

		memcpy(headers, g_rx, header_len);
		headers[header_len] = 0;

		line_end = Q_strchr(headers, '\n');
		if (!line_end)
			return false;

		{
			size_t n = Q_min((int)(line_end - headers), (int)sizeof(status_line) - 1);
			memcpy(status_line, headers, n);
			status_line[n] = 0;
		}

		if (Q_strnicmp(status_line, "HTTP/1.1 101", 12) &&
			Q_strnicmp(status_line, "HTTP/1.0 101", 12))
			return false;

		if (!MAGD_HeaderValue(headers,
			"Upgrade",
			upgrade,
			sizeof(upgrade)) ||
			Q_stricmp(upgrade, "websocket"))
			return false;

		if (!MAGD_HeaderValue(headers,
			"Connection",
			connection,
			sizeof(connection)) ||
			!Q_stristr(connection, "Upgrade"))
			return false;

		if (!MAGD_HeaderValue(headers,
			"Sec-WebSocket-Accept",
			accept,
			sizeof(accept)) ||
			Q_strcmp(accept, g_expected_accept))
			return false;

		if (g_rx_len > header_len)
		{
			memmove(g_rx,
				g_rx + header_len,
				g_rx_len - header_len);
			g_rx_len -= header_len;
		}
		else
		{
			g_rx_len = 0;
		}
	}

	g_open = true;
	g_state = MAGD_STATE_OPEN;
	return true;
}

/* ------------------------------------------------------------------------- */
/* WebSocket frames                                                         */
/* ------------------------------------------------------------------------- */

static qboolean MAGD_QueueWsFrame(int opcode, const byte *payload, size_t len)
{
	byte mask[4];
	byte *buffer;
	size_t *out_len;
	size_t *out_pos;
	size_t header_len;
	size_t total_len;
	size_t p = 0;
	qboolean control;

	if (!payload && len)
		return false;

	control = opcode >= MAGD_WS_OPCODE_CLOSE;

	if (control && len > 125)
		return false;

	if (!control && len > 65535)
		return false;

	if (control)
	{
		if (g_control_tx_len != g_control_tx_pos)
			return false;
		buffer = g_control_tx;
		out_len = &g_control_tx_len;
		out_pos = &g_control_tx_pos;
	}
	else
	{
		if (g_data_tx_len != g_data_tx_pos)
			return false;
		buffer = g_data_tx;
		out_len = &g_data_tx_len;
		out_pos = &g_data_tx_pos;
	}

	for (int i = 0; i < 4; ++i)
		mask[i] = (byte)COM_RandomLong(0, 255);

	header_len = len <= 125 ? 2 : 4;
	total_len = header_len + 4 + len;

	if ((control && total_len > sizeof(g_control_tx)) ||
		(!control && total_len > sizeof(g_data_tx)))
		return false;

	buffer[p++] = (byte)(0x80 | (opcode & 0x0f));

	if (len <= 125)
	{
		buffer[p++] = (byte)(0x80 | (byte)len);
	}
	else
	{
		buffer[p++] = 0xFE; /* FIN + MASK + 126 */
		buffer[p++] = (byte)(len >> 8);
		buffer[p++] = (byte)len;
	}

	memcpy(buffer + p, mask, sizeof(mask));
	p += sizeof(mask);

	for (size_t i = 0; i < len; ++i)
		buffer[p + i] = payload[i] ^ mask[i & 3];

	*out_len = total_len;
	*out_pos = 0;
	return true;
}

static void MAGD_FlushTx(void)
{
	for (;;)
	{
		byte *buffer;
		size_t *pos;
		size_t len;

		if (g_control_tx_pos < g_control_tx_len)
		{
			buffer = g_control_tx;
			pos = &g_control_tx_pos;
			len = g_control_tx_len;
		}
		else if (g_data_tx_pos < g_data_tx_len)
		{
			buffer = g_data_tx;
			pos = &g_data_tx_pos;
			len = g_data_tx_len;
		}
		else
		{
			g_control_tx_len = g_control_tx_pos = 0;
			g_data_tx_len = g_data_tx_pos = 0;
			return;
		}

		while (*pos < len)
		{
			int result;

			if (g_tls)
				result = HTTP_TlsSend(g_tls,
					buffer + *pos,
					(int)(len - *pos));
			else
				result = send(g_socket,
					(const char *)buffer + *pos,
					(int)(len - *pos),
					0);

			if (result == HTTP_TLS_WANT ||
				(result < 0 && MAGD_WouldBlock()))
				return;

			if (result <= 0)
			{
				g_state = MAGD_STATE_ERROR;
				return;
			}

			*pos += (size_t)result;
		}

		if (pos == &g_control_tx_pos)
		{
			g_control_tx_pos = 0;
			g_control_tx_len = 0;
		}
		else
		{
			g_data_tx_pos = 0;
			g_data_tx_len = 0;
		}
	}
}

static int MAGD_Recv(byte *buffer, size_t cap)
{
	if (g_tls)
		return HTTP_TlsRecv(g_tls, buffer, (int)cap);

	{
		int result = recv(g_socket, (char *)buffer, (int)cap, 0);

		if (result < 0 && MAGD_WouldBlock())
			return HTTP_TLS_WANT;

		if (result < 0)
			return HTTP_TLS_ERROR;

		return result;
	}
}

/* ------------------------------------------------------------------------- */
/* MAGD application protocol                                                */
/* ------------------------------------------------------------------------- */

static void MAGD_HandleApplication(const byte *data, size_t len)
{
	size_t payload_len;
	int type;
	const byte *payload;

	if (!data || len < 5 || data[0] != 0x4D || data[1] != 0x47)
		return;

	payload_len = ((size_t)data[3] << 8) | data[4];
	if (payload_len != len - 5)
		return;

	type = data[2];
	payload = data + 5;

	if (type == MAGD_TYPE_PING)
	{
		MAGD_QueueWsFrame(MAGD_WS_OPCODE_PONG, payload, payload_len);
		return;
	}

	if (type == MAGD_TYPE_PONG)
		return;

	if (type == MAGD_TYPE_WELCOME)
	{
		/* Welcome is intentionally informational; peer routing uses the game envelope. */
		return;
	}

	if (type != MAGD_TYPE_GAME_DATAGRAM || payload_len < 2)
		return;

	{
		byte flags = payload[0];
		byte peer = payload[1];
		netadr_t from;

		if (g_host)
		{
			if (!(flags & MAGD_GAME_HAS_SENDER) || peer == MAGD_PEER_BROADCAST)
				return;

			{
				char session_id[32];
				Q_snprintf(session_id, sizeof(session_id), "peer_%u", (unsigned int)peer);
				if (!MAGD_MapSessionToAddress(session_id, &from))
					return;
			}
		}
		else
		{
			if (!MAGD_MapSessionToAddress("host", &from))
				return;
		}

		if (!MAGD_QueuePush(&g_incoming,
			payload + 2,
			payload_len - 2,
			&from))
		{
			Con_DPrintf("[MAGD] incoming queue full; dropping packet\n");
		}
	}
}

static qboolean MAGD_ParseWebSocketFrames(void)
{
	while (g_rx_len >= 2)
	{
		byte b0 = g_rx[0];
		byte b1 = g_rx[1];
		qboolean fin = (b0 & 0x80) != 0;
		int rsv = (b0 >> 4) & 0x07;
		int opcode = b0 & 0x0F;
		qboolean masked = (b1 & 0x80) != 0;
		uint64_t payload_len = b1 & 0x7F;
		size_t header_len = 2;
		byte mask[4];
		byte *payload;
		size_t frame_len;

		if (rsv != 0)
			return false;

		/* Server-to-client frames must not be masked. */
		if (masked)
			return false;

		if (payload_len == 126)
		{
			if (g_rx_len < 4)
				return true;

			payload_len = ((uint64_t)g_rx[2] << 8) | g_rx[3];
			header_len = 4;
		}
		else if (payload_len == 127)
		{
			return false;
		}

		if (opcode >= MAGD_WS_OPCODE_CLOSE)
		{
			if (!fin || payload_len > 125)
				return false;
		}

		if (payload_len > MAGD_MAX_PACKET_SIZE + 64)
			return false;

		frame_len = header_len + (size_t)payload_len;
		if (g_rx_len < frame_len)
			return true;

		payload = g_rx + header_len;

		(void)memset(mask, 0, sizeof(mask));

		if (opcode == MAGD_WS_OPCODE_CLOSE)
		{
			/* Echo a close response before shutting the local transport down. */
			if (!g_control_tx_len)
				MAGD_QueueWsFrame(MAGD_WS_OPCODE_CLOSE, payload, (size_t)payload_len);
			MAGD_FlushTx();
			g_state = MAGD_STATE_ERROR;
		}
		else if (opcode == MAGD_WS_OPCODE_PING)
		{
			MAGD_QueueWsFrame(MAGD_WS_OPCODE_PONG, payload, (size_t)payload_len);
		}
		else if (opcode == MAGD_WS_OPCODE_PONG)
		{
			/* No explicit keepalive state is required by the MAGD protocol. */
		}
		else if (opcode == MAGD_WS_OPCODE_CONTINUATION)
		{
			if (!g_ws_fragmented)
				return false;

			if (g_ws_fragment_len + (size_t)payload_len > sizeof(g_fragment))
				return false;

			memcpy(g_fragment + g_ws_fragment_len,
				payload,
				(size_t)payload_len);

			g_ws_fragment_len += (size_t)payload_len;

			if (fin)
			{
				if (g_ws_fragment_opcode == MAGD_WS_OPCODE_BINARY)
					MAGD_HandleApplication(g_fragment, g_ws_fragment_len);

				g_ws_fragmented = false;
				g_ws_fragment_opcode = 0;
				g_ws_fragment_len = 0;
			}
		}
		else if (opcode == MAGD_WS_OPCODE_BINARY)
		{
			if (g_ws_fragmented)
				return false;

			if (fin)
			{
				MAGD_HandleApplication(payload, (size_t)payload_len);
			}
			else
			{
				if (payload_len > sizeof(g_fragment))
					return false;

				memcpy(g_fragment,
					payload,
					(size_t)payload_len);

				g_ws_fragmented = true;
				g_ws_fragment_opcode = MAGD_WS_OPCODE_BINARY;
				g_ws_fragment_len = (size_t)payload_len;
			}
		}
		else if (opcode == MAGD_WS_OPCODE_TEXT)
		{
			/* Text is not part of the MAGD game tunnel. */
			if (!fin)
				return false;
		}
		else
		{
			return false;
		}

		if (g_rx_len > frame_len)
		{
			memmove(g_rx,
				g_rx + frame_len,
				g_rx_len - frame_len);
		}
		g_rx_len -= frame_len;

		if (g_state == MAGD_STATE_ERROR)
			return false;
	}

	return true;
}

static qboolean MAGD_ReadSocket(void)
{
	byte buffer[8192];

	for (;;)
	{
		int result = MAGD_Recv(buffer, sizeof(buffer));

		if (result == HTTP_TLS_WANT)
			return true;

		if (result <= 0)
			return false;

		if (g_rx_len + (size_t)result > sizeof(g_rx))
			return false;

		memcpy(g_rx + g_rx_len, buffer, (size_t)result);
		g_rx_len += (size_t)result;

		if (g_state == MAGD_STATE_WS_RECV)
		{
			if (!MAGD_HandshakeComplete())
				return false;

			if (g_state == MAGD_STATE_OPEN && !MAGD_ParseWebSocketFrames())
				return false;
		}
		else if (g_state == MAGD_STATE_OPEN)
		{
			if (!MAGD_ParseWebSocketFrames())
				return false;
		}
	}
}

static void MAGD_SendQueued(void)
{
	byte packet[MAGD_MAX_PACKET_SIZE];
	size_t packet_len = 0;
	netadr_t to;
	byte envelope[MAGD_MAX_PACKET_SIZE + 2];
	byte peer = MAGD_PEER_BROADCAST;
	byte flags = 0;

	if (g_data_tx_len != g_data_tx_pos)
		return;

	if (!MAGD_QueuePop(&g_outgoing,
		packet,
		&packet_len,
		&to))
		return;

	if (g_host)
	{
		const char *session_id = MAGD_MapAddressToSession(&to);

		if (session_id && !Q_strnicmp(session_id, "peer_", 5))
		{
			int id = Q_atoi(session_id + 5);
			if (id >= 1 && id <= 31)
			{
				flags |= MAGD_GAME_HAS_TARGET;
				peer = (byte)id;
			}
		}
	}

	envelope[0] = flags;
	envelope[1] = peer;
	memcpy(envelope + 2, packet, packet_len);

	{
		byte message[MAGD_MAX_PACKET_SIZE + 7];
		size_t message_len = packet_len + 7;

		message[0] = 0x4D;
		message[1] = 0x47;
		message[2] = MAGD_TYPE_GAME_DATAGRAM;
		message[3] = (byte)((packet_len + 2) >> 8);
		message[4] = (byte)(packet_len + 2);
		memcpy(message + 5,
			envelope,
			packet_len + 2);

		if (!MAGD_QueueWsFrame(MAGD_WS_OPCODE_BINARY,
			message,
			message_len))
		{
			/* Put the packet back only if possible; otherwise drop it. */
			MAGD_QueuePush(&g_outgoing,
				packet,
				packet_len,
				&to);
			return;
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Public tunnel control                                                     */
/* ------------------------------------------------------------------------- */

static qboolean MAGD_StartTunnel(const char *room_code, qboolean is_host)
{
	if (!room_code || !room_code[0] || !magd_auth_token.string[0])
		return false;

	if (!MAGD_ParseServerUrl(magd_server_url.string,
		g_host_name,
		sizeof(g_host_name),
		g_host_header,
		sizeof(g_host_header),
		&g_port,
		&g_use_tls))
		return false;

	if (!MAGD_HeaderSafe(magd_auth_token.string) ||
		!MAGD_HeaderSafe(magd_room_password.string))
		return false;

	MAGD_ResetTransport();
	MAGD_ClearSessionMaps();

	g_host = is_host;

	if (g_use_tls)
	{
		if (!HTTP_TlsAvailable())
			return false;
	}

	if (!MAGD_OpenSocket())
		return false;

	if (g_use_tls)
	{
		g_tls = HTTP_TlsNew((int)g_socket, g_host_name);
		if (!g_tls)
		{
			MAGD_ResetTransport();
			return false;
		}
	}

	if (g_state != MAGD_STATE_TCP_CONNECTING)
	{
		if (!MAGD_BuildHandshakeRequest())
		{
			MAGD_ResetTransport();
			return false;
		}
	}

	Con_Printf("^2[MAGD]^7 Connecting to room ^3%s^7 at %s...\n",
		room_code,
		g_host_header);

	return true;
}

static void MAGD_CreateCallback(const char *url,
	qboolean success,
	const byte *data,
	size_t size,
	void *userdata)
{
	char token[1024];
	(void)url;
	(void)userdata;

	if (!success || !data || !size)
	{
		Con_Printf(S_ERROR "[MAGD] room creation failed\n");
		return;
	}

	if (!MAGD_JsonString(data,
		size,
		"hostToken",
		token,
		sizeof(token)))
	{
		Con_Printf(S_ERROR "[MAGD] host token missing\n");
		return;
	}

	Cvar_DirectSet(&magd_auth_token, token);
	MAGD_SetMode(MAGD_NET_MODE_TUNNEL);

	if (!MAGD_StartTunnel(magd_room_code.string, true))
		Con_Printf(S_ERROR "[MAGD] could not start host tunnel\n");
}

static void MAGD_GuestCallback(const char *url,
	qboolean success,
	const byte *data,
	size_t size,
	void *userdata)
{
	char token[1024];
	(void)url;
	(void)userdata;

	if (!success || !data || !size)
	{
		Con_Printf(S_ERROR "[MAGD] guest authentication failed\n");
		return;
	}

	if (!MAGD_JsonString(data,
		size,
		"token",
		token,
		sizeof(token)))
	{
		Con_Printf(S_ERROR "[MAGD] guest token missing\n");
		return;
	}

	Cvar_DirectSet(&magd_auth_token, token);
	MAGD_SetMode(MAGD_NET_MODE_TUNNEL);

	if (!MAGD_StartTunnel(magd_room_code.string, false))
		Con_Printf(S_ERROR "[MAGD] could not start client tunnel\n");
}

static qboolean MAGD_UrlEncode(const char *src, char *dst, size_t cap)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t out = 0;

	if (!src || !dst || cap < 1)
		return false;

	while (*src)
	{
		unsigned char c = (unsigned char)*src++;

		if ((c >= 'A' && c <= 'Z') ||
			(c >= 'a' && c <= 'z') ||
			(c >= '0' && c <= '9') ||
			c == '-' || c == '_' || c == '.' || c == '~')
		{
			if (out + 1 >= cap)
				return false;
			dst[out++] = (char)c;
		}
		else
		{
			if (out + 3 >= cap)
				return false;
			dst[out++] = '%';
			dst[out++] = hex[c >> 4];
			dst[out++] = hex[c & 15];
		}
	}

	dst[out] = 0;
	return true;
}

static void MAGD_CreateRoom_f(void)
{
	char code[32];
	char password[512];
	char url[2048];

	Q_snprintf(code,
		sizeof(code),
		"MAGD-%04X",
		(unsigned int)COM_RandomLong(0x1000, 0xFFFF));

	Q_strncpy(password,
		magd_room_password.string,
		sizeof(password));

	Cvar_DirectSet(&magd_room_code, code);

	{
		char encoded_password[1536];
		if (!MAGD_UrlEncode(password,
			encoded_password,
			sizeof(encoded_password)))
		{
			Con_Printf(S_ERROR "[MAGD] room password is too long\n");
			return;
		}

		Q_snprintf(url,
			sizeof(url),
			"%s/api/v1/rooms/create?code=%s&password=%s",
			magd_server_url.string,
			code,
			encoded_password);
	}

	Con_Printf("^2[MAGD]^7 Creating room ^3%s^7...\n", code);
	HTTP_GetToMemory(url,
		MAGD_CreateCallback,
		NULL);
}

static void MAGD_ConnectRoom_f(void)
{
	char url[1024];

	if (Cmd_Argc() < 2)
	{
		Con_Printf(S_USAGE "magd_connect <room_code> [password]\n");
		return;
	}

	Cvar_DirectSet(&magd_room_code, Cmd_Argv(1));

	if (Cmd_Argc() >= 3)
		Cvar_DirectSet(&magd_room_password, Cmd_Argv(2));
	else
		Cvar_DirectSet(&magd_room_password, "");

	Q_snprintf(url,
		sizeof(url),
		"%s/api/v1/auth/guest",
		magd_server_url.string);

	Con_Printf("^2[MAGD]^7 Authenticating guest for room ^3%s^7...\n",
		magd_room_code.string);

	HTTP_GetToMemory(url,
		MAGD_GuestCallback,
		NULL);
}

void MAGD_Init(void)
{
	Cvar_RegisterVariable(&magd_enabled);
	Cvar_RegisterVariable(&magd_server_url);
	Cvar_RegisterVariable(&magd_room_code);
	Cvar_RegisterVariable(&magd_auth_token);
	Cvar_RegisterVariable(&magd_room_password);

	Cmd_AddCommand("magd_create_room",
		MAGD_CreateRoom_f,
		"Create a MAGD online room");

	Cmd_AddCommand("magd_connect",
		MAGD_ConnectRoom_f,
		"Connect to a MAGD room");

	MAGD_QueueInit(&g_incoming);
	MAGD_QueueInit(&g_outgoing);
	MAGD_ClearSessionMaps();

	MAGD_ResetTransport();

	Con_Printf("^2[MAGD]^7 Network layer initialized\n");
}

void MAGD_Shutdown(void)
{
	MAGD_ResetTransport();
	MAGD_ClearSessionMaps();
	MAGD_QueueInit(&g_incoming);
	MAGD_QueueInit(&g_outgoing);
	g_mode = MAGD_NET_MODE_LAN;
}

magd_net_mode_t MAGD_GetMode(void)
{
	return g_mode;
}

void MAGD_SetMode(magd_net_mode_t mode)
{
	if (mode == g_mode)
		return;

	if (mode != MAGD_NET_MODE_TUNNEL)
		MAGD_ResetTransport();

	g_mode = mode;
}

void MAGD_ProcessTunnel(void)
{
	if (!magd_enabled.value)
	{
		if (g_state != MAGD_STATE_IDLE)
			MAGD_ResetTransport();
		return;
	}

	if (g_mode != MAGD_NET_MODE_TUNNEL)
		return;

	if (!NET_IsSocketValid(g_socket))
		return;

	if (g_state == MAGD_STATE_ERROR)
	{
		MAGD_ResetTransport();
		return;
	}

	if (g_state == MAGD_STATE_TCP_CONNECTING)
	{
		int status = MAGD_CheckTcpConnected();

		if (status < 0)
		{
			g_state = MAGD_STATE_ERROR;
		}
		else if (status > 0)
		{
			if (g_use_tls)
				g_state = MAGD_STATE_TLS;
			else if (!MAGD_BuildHandshakeRequest())
				g_state = MAGD_STATE_ERROR;
		}
	}

	if (g_state == MAGD_STATE_TLS)
	{
		int status = HTTP_TlsHandshake(g_tls);

		if (status == HTTP_TLS_WANT)
			return;

		if (status == HTTP_TLS_ERROR)
		{
			g_state = MAGD_STATE_ERROR;
		}
		else if (!MAGD_BuildHandshakeRequest())
		{
			g_state = MAGD_STATE_ERROR;
		}
	}

	if (g_state == MAGD_STATE_WS_SEND)
	{
		while (g_request_pos < g_request_len)
		{
			int result;

			if (g_tls)
				result = HTTP_TlsSend(g_tls,
					g_request + g_request_pos,
					(int)(g_request_len - g_request_pos));
			else
				result = send(g_socket,
					g_request + g_request_pos,
					(int)(g_request_len - g_request_pos),
					0);

			if (result == HTTP_TLS_WANT ||
				(result < 0 && MAGD_WouldBlock()))
				return;

			if (result <= 0)
			{
				g_state = MAGD_STATE_ERROR;
				break;
			}

			g_request_pos += (size_t)result;
		}

		if (g_state == MAGD_STATE_WS_SEND)
			g_state = MAGD_STATE_WS_RECV;
	}

	if (g_state == MAGD_STATE_WS_RECV || g_state == MAGD_STATE_OPEN)
	{
		if (!MAGD_ReadSocket())
			g_state = MAGD_STATE_ERROR;
	}

	if (g_state == MAGD_STATE_OPEN)
	{
		MAGD_SendQueued();
		MAGD_FlushTx();
	}

	if (g_state == MAGD_STATE_ERROR)
		MAGD_ResetTransport();
}

qboolean MAGD_SendDatagram(const void *data, size_t length, const netadr_t *to)
{
	if (!magd_enabled.value ||
		g_mode != MAGD_NET_MODE_TUNNEL)
		return false;

	return MAGD_QueuePush(&g_outgoing,
		data,
		length,
		to);
}

qboolean MAGD_GetDatagram(byte *data, size_t *length, netadr_t *from)
{
	if (!magd_enabled.value ||
		g_mode != MAGD_NET_MODE_TUNNEL)
		return false;

	MAGD_ProcessTunnel();

	return MAGD_QueuePop(&g_incoming,
		data,
		length,
		from);
}

#if XASH_ENGINE_TESTS
void Test_RunMagd(void)
{
	magd_queue_t q;
	byte input[] = "MAGD";
	byte output[16];
	size_t output_len = 0;
	netadr_t a;
	netadr_t b;

	MAGD_QueueInit(&q);
	NET_StringToAdr("127.0.0.1:27015", &a);

	TASSERT_EQi(MAGD_QueuePush(&q, input, sizeof(input), &a), true);
	TASSERT_EQi(MAGD_QueuePop(&q, output, &output_len, &b), true);
	TASSERT_EQi(output_len, sizeof(input));
	TASSERT_STR((char *)output, (char *)input);

	MAGD_ClearSessionMaps();
	TASSERT_EQi(MAGD_MapSessionToAddress("peer_1", &a), true);
	TASSERT_EQi(a.ip[0], 10);
	TASSERT_EQi(a.ip[1], 254);
	TASSERT_EQi(a.ip[2], 0);
	TASSERT_EQi(a.ip[3], 1);
	TASSERT_STR(MAGD_MapAddressToSession(&a), "peer_1");
}
#endif
