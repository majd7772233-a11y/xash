/*
 * magd_net.c - MAGD Network Abstraction Layer
 *
 * Transport:
 *   Xash UDP datagram
 *      -> MAGD application envelope
 *      -> masked WebSocket binary frame
 *      -> optional mbedTLS TLS stream
 *      -> Cloudflare Worker
 *      -> Durable Object room
 *      -> WebSocket
 *      -> MAGD application envelope
 *      -> Xash UDP datagram
 *
 * The existing Xash NET_GetPacket/NET_SendPacketEx integration calls the
 * public MAGD functions below. LAN/direct-IP behavior remains unchanged.
 */

#include "magd_net.h"
#include "net_ws_private.h"
#include "net_http_tls.h"
#include "xash3d_mathlib.h"
#include "tests.h"

#include <errno.h>
#include <stdint.h>

CVAR_DEFINE(magd_enabled, "magd_enabled", "1", FCVAR_ARCHIVE,
	"Enable MAGD Network Layer");
CVAR_DEFINE(magd_server_url, "magd_server_url",
	"https://xash-server.magd.workers.dev", FCVAR_ARCHIVE,
	"MAGD Worker URL (https:// for production)");
CVAR_DEFINE(magd_room_code, "magd_room_code", "", FCVAR_ARCHIVE,
	"MAGD Room Code");
CVAR_DEFINE(magd_auth_token, "magd_auth_token", "", 0,
	"MAGD Authentication Token");
CVAR_DEFINE(magd_room_password, "magd_room_password", "", 0,
	"MAGD Room Password");
CVAR_DEFINE(magd_room_name, "magd_room_name", "MAGD Server", FCVAR_ARCHIVE,
	"MAGD Room Name");
CVAR_DEFINE(magd_room_map, "magd_room_map", "crossfire", FCVAR_ARCHIVE,
	"MAGD Room Map");
CVAR_DEFINE(magd_room_game, "magd_room_game", "valve", FCVAR_ARCHIVE,
	"MAGD Room Game");
CVAR_DEFINE(magd_host_name, "magd_host_name", "Host", FCVAR_ARCHIVE,
	"MAGD Host Display Name");
CVAR_DEFINE(magd_max_players, "magd_max_players", "16", FCVAR_ARCHIVE,
	"MAGD Maximum Players");
CVAR_DEFINE(magd_reconnect, "magd_reconnect", "1", FCVAR_ARCHIVE,
	"Reconnect MAGD tunnel after a transient network failure");
CVAR_DEFINE(magd_auto_connect, "magd_auto_connect", "1", FCVAR_ARCHIVE,
	"After joining a room, connect the Xash client to the virtual MAGD host");
CVAR_DEFINE(magd_allow_insecure_ws, "magd_allow_insecure_ws", "0", 0,
	"Allow plain ws:// only for local debugging; keep disabled in production");
CVAR_DEFINE(magd_auto_start_server, "magd_auto_start_server", "1", FCVAR_ARCHIVE,
	"Automatically start the local Xash server after creating a MAGD room");

CVAR_DEFINE(magd_connection_state, "magd_connection_state", "disconnected", 0,
	"Current MAGD connection state");

CVAR_DEFINE(magd_last_error, "magd_last_error", "", 0,
	"Last MAGD connection error");

CVAR_DEFINE(magd_room_list_revision, "magd_room_list_revision", "0", 0,
	"Increments whenever the MAGD room cache is refreshed");

CVAR_DEFINE(magd_room_list_state, "magd_room_list_state", "idle", 0,
	"Current MAGD room list refresh state");

CVAR_DEFINE(magd_room_list_error, "magd_room_list_error", "", 0,
	"Last MAGD room list error");

#define MAGD_RX_BUFFER_SIZE (128 * 1024)
#define MAGD_FRAGMENT_SIZE (MAGD_MAX_PACKET_SIZE + 32)
#define MAGD_DATA_TX_SIZE (MAGD_MAX_PACKET_SIZE + 64)
#define MAGD_CONTROL_TX_SIZE 512
#define MAGD_HTTP_REQUEST_SIZE 4096
#define MAGD_HTTP_HEADER_SIZE 8192
#define MAGD_RECONNECT_MIN 1.0
#define MAGD_RECONNECT_MAX 15.0
#define MAGD_CONNECT_TIMEOUT 12.0
#define MAGD_KEEPALIVE_INTERVAL 20.0
#define MAGD_KEEPALIVE_TIMEOUT 65.0
#define MAGD_VIRTUAL_HOST_PORT 27015
#define MAGD_ROOM_LIST_FILE "magd_rooms.json"
#define MAGD_ROOM_LIST_MAX_SIZE (2 * 1024 * 1024)

typedef enum magd_state_e
{
	MAGD_STATE_IDLE = 0,
	MAGD_STATE_TCP_CONNECTING,
	MAGD_STATE_TLS,
	MAGD_STATE_WS_SEND,
	MAGD_STATE_WS_RECV,
	MAGD_STATE_OPEN,
	MAGD_STATE_BACKOFF
} magd_state_t;

static magd_net_mode_t g_mode = MAGD_NET_MODE_LAN;
static magd_state_t g_state = MAGD_STATE_IDLE;
static magd_queue_t g_incoming;
static magd_queue_t g_outgoing;
static magd_session_map_t g_sessions[MAGD_MAX_SESSIONS];

static int g_socket = -1;
static tlsctx_t *g_tls = NULL;
static qboolean g_host = false;
static qboolean g_wanted = false;
static qboolean g_auto_connect_pending = false;
static qboolean g_welcome_seen = false;
static qboolean g_start_server_pending = false;

static double g_connect_started = 0.0;
static double g_next_retry = 0.0;
static double g_last_rx = 0.0;
static double g_last_pong = 0.0;
static double g_next_keepalive = 0.0;
static int g_retry_count = 0;

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

static double MAGD_Now(void)
{
	return Sys_DoubleTime();
}

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

		if (out + 4 > cap)
			return 0;

		dst[out++] = alphabet[(value >> 18) & 63];
		dst[out++] = alphabet[(value >> 12) & 63];
		dst[out++] = i + 1 < len ? alphabet[(value >> 6) & 63] : '=';
		dst[out++] = i + 2 < len ? alphabet[value & 63] : '=';
	}

	if (out >= cap)
		return 0;

	dst[out] = 0;
	return out;
}

/* ------------------------------------------------------------------------- */
/* Small SHA-1 implementation for Sec-WebSocket-Accept                      */
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
		uint32_t f, k;
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
/* JSON helpers for the tiny auth/create responses used by the engine       */
/* ------------------------------------------------------------------------- */

static qboolean MAGD_JsonString(const byte *data, size_t len, const char *key,
	char *out, size_t cap)
{
	char text[4096];
	char needle[128];
	size_t n;
	char *p, *end;

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
	if (*p != '"')
		return false;
	++p;
	end = p;
	while (*end)
	{
		if (*end == '"' && end == p)
			break;
		if (*end == '"' && end > p && end[-1] != '\\')
			break;
		++end;
	}
	if (!*end)
		return false;
	n = Q_min((int)(end - p), (int)cap - 1);
	memcpy(out, p, n);
	out[n] = 0;
	return true;
}

/* ------------------------------------------------------------------------- */
/* Queue and deterministic virtual addresses                                */
/* ------------------------------------------------------------------------- */

void MAGD_QueueInit(magd_queue_t *q)
{
	if (q)
		memset(q, 0, sizeof(*q));
}

qboolean MAGD_QueuePush(magd_queue_t *q, const void *data, size_t length,
	const netadr_t *adr)
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

qboolean MAGD_QueuePop(magd_queue_t *q, byte *data, size_t *length,
	netadr_t *adr)
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

static qboolean MAGD_SetVirtualAddress(netadr_t *out, int host_or_peer)
{
	int host = host_or_peer == 0;
	int last = host ? 1 : host_or_peer + 1;

	if (!out || last < 1 || last > 32)
		return false;

	memset(out, 0, sizeof(*out));
	NET_NetadrSetType(out, NA_IP);
	out->ip[0] = 10;
	out->ip[1] = 254;
	out->ip[2] = 0;
	out->ip[3] = (byte)last;
	out->port = BigShort(MAGD_VIRTUAL_HOST_PORT);
	return true;
}

qboolean MAGD_MapSessionToAddress(const char *session_id, netadr_t *out_adr)
{
	int peer;

	if (!session_id || !session_id[0] || !out_adr)
		return false;

	if (!Q_stricmp(session_id, "host"))
		return MAGD_SetVirtualAddress(out_adr, 0);

	if (!Q_strnicmp(session_id, "peer_", 5))
	{
		peer = Q_atoi(session_id + 5);
		if (peer < 1 || peer > 31)
			return false;
		return MAGD_SetVirtualAddress(out_adr, peer);
	}

	return false;
}

const char *MAGD_MapAddressToSession(const netadr_t *adr)
{
	static char session_id[64];

	if (!adr || NET_NetadrType(adr) != NA_IP)
		return NULL;
	if (adr->port != BigShort(MAGD_VIRTUAL_HOST_PORT))
		return NULL;
	if (adr->ip[0] != 10 || adr->ip[1] != 254 || adr->ip[2] != 0)
		return NULL;

	if (adr->ip[3] == 1)
		return "host";
	if (adr->ip[3] >= 2 && adr->ip[3] <= 32)
	{
		Q_snprintf(session_id, sizeof(session_id), "peer_%u",
			(unsigned int)(adr->ip[3] - 1));
		return session_id;
	}

	return NULL;
}

static void MAGD_SetConnectionState(const char *state, const char *error)
{
	Cvar_DirectSet(&magd_connection_state, state && state[0] ? state : "disconnected");

	if (error && error[0])
		Cvar_DirectSet(&magd_last_error, error);
	else
		Cvar_DirectSet(&magd_last_error, "");
}

/* ------------------------------------------------------------------------- */
/* Transport cleanup / retry                                                 */
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
	g_connect_started = 0.0;
	g_last_rx = 0.0;
	g_last_pong = 0.0;
	g_next_keepalive = 0.0;
	g_welcome_seen = false;
}

static void MAGD_ScheduleRetry(const char *reason)
{
	double delay;
	qboolean reconnect;

	if (reason && reason[0])
		Con_Printf(S_WARN "[MAGD] %s\n", reason);

	MAGD_ResetTransport();

	reconnect = g_wanted && magd_reconnect.value;

	if (!reconnect)
	{
		g_state = MAGD_STATE_IDLE;

		if (g_wanted)
			MAGD_SetConnectionState("error", reason);
		else
			MAGD_SetConnectionState("disconnected", reason);

		return;
	}

	delay = MAGD_RECONNECT_MIN * (double)(1 << bound(0, g_retry_count, 4));

	if (delay > MAGD_RECONNECT_MAX)
		delay = MAGD_RECONNECT_MAX;

	++g_retry_count;

	g_next_retry = MAGD_Now() + delay;
	g_state = MAGD_STATE_BACKOFF;

	MAGD_SetConnectionState("reconnecting", reason);

	Con_Printf("[MAGD] reconnect scheduled in %.1fs\n", delay);
}

static void MAGD_StopTunnel(void)
{
	g_wanted = false;
	g_auto_connect_pending = false;
	g_start_server_pending = false;

	MAGD_ResetTransport();
	MAGD_ClearSessionMaps();

	g_state = MAGD_STATE_IDLE;

	MAGD_SetConnectionState("disconnected", NULL);
}

static qboolean MAGD_ValidRoomCode(const char *code)
{
	size_t len;

	if (!code)
		return false;

	len = Q_strlen(code);

	if (len < 3 || len > 64)
		return false;

	for (const unsigned char *p = (const unsigned char *)code; *p; ++p)
	{
		if (!((*p >= 'A' && *p <= 'Z') ||
			(*p >= 'a' && *p <= 'z') ||
			(*p >= '0' && *p <= '9') ||
			*p == '_' || *p == '-'))
		{
			return false;
		}
	}

	return true;
}

/* ------------------------------------------------------------------------- */
/* URL / socket / WebSocket handshake                                        */
/* ------------------------------------------------------------------------- */

static qboolean MAGD_ParseServerUrl(const char *url, char *hostname,
	size_t hostname_cap, char *host_header, size_t host_header_cap,
	int *port, qboolean *use_tls)
{
	const char *authority, *end;
	char authority_buf[256];
	size_t authority_len;
	char *colon = NULL;

	if (!url || !url[0] || !hostname || !host_header || !port || !use_tls)
		return false;

	if (!Q_strnicmp(url, "https://", 8) || !Q_strnicmp(url, "wss://", 6))
	{
		authority = !Q_strnicmp(url, "https://", 8) ? url + 8 : url + 6;
		*port = 443;
		*use_tls = true;
	}
	else if (!Q_strnicmp(url, "http://", 7) || !Q_strnicmp(url, "ws://", 5))
	{
		authority = !Q_strnicmp(url, "http://", 7) ? url + 7 : url + 5;
		*port = 80;
		*use_tls = false;
	}
	else
		return false;

	end = authority;
	while (*end && *end != '/' && *end != '?' && *end != '#')
		++end;

	authority_len = (size_t)(end - authority);
	if (!authority_len || authority_len >= sizeof(authority_buf))
		return false;

	memcpy(authority_buf, authority, authority_len);
	authority_buf[authority_len] = 0;

	if (authority_buf[0] == '[')
	{
		char *close = Q_strchr(authority_buf, ']');
		if (!close)
			return false;
		if (close[1] == ':')
		{
			int explicit_port = Q_atoi(close + 2);
			if (explicit_port < 1 || explicit_port > 65535)
				return false;
			*port = explicit_port;
		}
		else if (close[1] != 0)
			return false;

		{
			char ipv6[256];
			size_t n = (size_t)(close - authority_buf - 1);
			if (!n || n >= sizeof(ipv6))
				return false;
			memcpy(ipv6, authority_buf + 1, n);
			ipv6[n] = 0;
			Q_strncpy(hostname, ipv6, hostname_cap);
		}

		if (*port != (*use_tls ? 443 : 80))
			Q_snprintf(host_header, host_header_cap, "[%s]:%d", hostname, *port);
		else
			Q_snprintf(host_header, host_header_cap, "[%s]", hostname);
		return hostname[0] != 0;
	}

	for (char *p = authority_buf; *p; ++p)
		if (*p == ':')
			colon = p;

	if (colon)
	{
		if (!colon[1])
			return false;
		for (const char *p = colon + 1; *p; ++p)
		{
			if (*p < '0' || *p > '9')
				return false;
		}

		*port = Q_atoi(colon + 1);
		if (*port < 1 || *port > 65535)
			return false;
		*colon = 0;
	}

	Q_strncpy(hostname, authority_buf, hostname_cap);
	if (!hostname[0])
		return false;

	if (*port != (*use_tls ? 443 : 80))
		Q_snprintf(host_header, host_header_cap, "%s:%d", hostname, *port);
	else
		Q_strncpy(host_header, hostname, host_header_cap);

	return true;
}

static qboolean MAGD_OpenSocket(void)
{
	struct sockaddr_storage addr;
	int result;

	memset(&addr, 0, sizeof(addr));
	if (NET_StringToSockaddr(g_host_name, &addr, false, AF_UNSPEC) != NET_EAI_OK)
		return false;

	if (addr.ss_family == AF_INET)
		((struct sockaddr_in *)&addr)->sin_port = BigShort(g_port);
#if defined(AF_INET6)
	else if (addr.ss_family == AF_INET6)
		((struct sockaddr_in6 *)&addr)->sin6_port = BigShort(g_port);
#endif
	else
		return false;

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

	result = connect(g_socket, (const struct sockaddr *)&addr, NET_SockAddrLen(&addr));
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

	closesocket(g_socket);
	g_socket = -1;
	return false;
}

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
	if (getsockopt(g_socket, SOL_SOCKET, SO_ERROR, (char *)&error_code, &opt_len) < 0)
		return -1;
	if (error_code == 0)
		return 1;

#if XASH_WIN32
	if (error_code == WSAEINPROGRESS || error_code == WSAEALREADY || error_code == WSAEWOULDBLOCK)
		return 0;
#else
	if (error_code == EINPROGRESS || error_code == EALREADY || error_code == EAGAIN || error_code == EWOULDBLOCK)
		return 0;
#endif
	return -1;
}

static qboolean MAGD_BuildHandshakeRequest(void)
{
	byte random_key[16];
	char key[64];

	for (int i = 0; i < (int)sizeof(random_key); ++i)
		random_key[i] = (byte)COM_RandomLong(0, 255);
	if (!MAGD_Base64(random_key, sizeof(random_key), key, sizeof(key)))
		return false;
	if (!MAGD_MakeWebSocketAccept(key, g_expected_accept, sizeof(g_expected_accept)))
		return false;

	if (!MAGD_HeaderSafe(magd_auth_token.string) ||
		!MAGD_HeaderSafe(magd_room_password.string) ||
		!MAGD_HeaderSafe(key))
		return false;

	Q_snprintf(g_request, sizeof(g_request),
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

static qboolean MAGD_HeaderValue(char *headers, const char *name, char *out, size_t cap)
{
	char *line = headers;
	size_t name_len;

	if (!headers || !name || !out || cap < 2)
		return false;
	name_len = Q_strlen(name);

	while (line && *line)
	{
		char *line_end = Q_strchr(line, '\n');
		char *colon;

		if (line_end)
			*line_end = 0;
		while (*line == '\r')
			++line;

		colon = Q_strchr(line, ':');
		if (colon && (size_t)(colon - line) == name_len &&
			!Q_strnicmp(line, name, (int)name_len))
		{
			char *value = colon + 1;
			size_t value_len;
			while (*value == ' ' || *value == '\t')
				++value;
			value_len = Q_strlen(value);
			while (value_len &&
				(value[value_len - 1] == '\r' || value[value_len - 1] == ' ' || value[value_len - 1] == '\t'))
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
	for (size_t i = 3; i < g_rx_len; ++i)
	{
		if (g_rx[i - 3] == '\r' && g_rx[i - 2] == '\n' &&
			g_rx[i - 1] == '\r' && g_rx[i] == '\n')
		{
			char headers[MAGD_HTTP_HEADER_SIZE];
			char upgrade[64];
			char connection[128];
			char accept[128];
			char status_line[64];
			size_t header_len = i + 1;
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

			if (!MAGD_HeaderValue(headers, "Upgrade", upgrade, sizeof(upgrade)) || Q_stricmp(upgrade, "websocket"))
				return false;
			if (!MAGD_HeaderValue(headers, "Connection", connection, sizeof(connection)) || !Q_stristr(connection, "Upgrade"))
				return false;
			if (!MAGD_HeaderValue(headers, "Sec-WebSocket-Accept", accept, sizeof(accept)) || Q_strcmp(accept, g_expected_accept))
				return false;

			if (g_rx_len > header_len)
			{
				memmove(g_rx, g_rx + header_len, g_rx_len - header_len);
				g_rx_len -= header_len;
			}
			else
				g_rx_len = 0;

			g_state = MAGD_STATE_OPEN;
            g_last_rx = MAGD_Now();
            MAGD_SetConnectionState("connected", NULL);
			g_last_pong = g_last_rx;
			g_next_keepalive = g_last_rx + MAGD_KEEPALIVE_INTERVAL;
			g_retry_count = 0;
			Con_Printf("^2[MAGD]^7 WebSocket tunnel established\n");
			return true;
		}
	}

	return true; /* incomplete header: keep receiving */
}

/* ------------------------------------------------------------------------- */
/* WebSocket frame transmit                                                  */
/* ------------------------------------------------------------------------- */

static qboolean MAGD_QueueWsFrame(int opcode, const byte *payload, size_t len)
{
	byte mask[4];
	byte *buffer;
	size_t *out_len, *out_pos;
	size_t header_len;
	size_t total_len;
	size_t p = 0;
	qboolean control = opcode >= MAGD_WS_OPCODE_CLOSE;

	if (!payload && len)
		return false;
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
	if ((control && total_len > sizeof(g_control_tx)) || (!control && total_len > sizeof(g_data_tx)))
		return false;

	buffer[p++] = (byte)(0x80 | (opcode & 0x0F));
	if (len <= 125)
		buffer[p++] = (byte)(0x80 | (byte)len);
	else
	{
		buffer[p++] = 0xFE; /* MASK | 126 */
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
				result = HTTP_TlsSend(g_tls, buffer + *pos, (int)(len - *pos));
			else
				result = send(g_socket, (const char *)buffer + *pos, (int)(len - *pos), 0);

			if (result == HTTP_TLS_WANT || (result < 0 && MAGD_WouldBlock()))
				return;
			if (result <= 0)
			{
				MAGD_ScheduleRetry("WebSocket send failed");
				return;
			}
			*pos += (size_t)result;
		}

		if (pos == &g_control_tx_pos)
			g_control_tx_len = g_control_tx_pos = 0;
		else
			g_data_tx_len = g_data_tx_pos = 0;
	}
}

/* ------------------------------------------------------------------------- */
/* WebSocket receive / MAGD application protocol                             */
/* ------------------------------------------------------------------------- */

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

static void MAGD_StartLocalServer(void)
{
	char escaped_map[MAX_SYSPATH];
	char escaped_name[MAX_SYSPATH];
	int max_players;

	max_players = bound(2, (int)magd_max_players.value, 32);

	Com_EscapeCommand(escaped_map, magd_room_map.string, sizeof(escaped_map));
	Com_EscapeCommand(escaped_name, magd_room_name.string, sizeof(escaped_name));

	Cbuf_AddTextf(
		"disconnect;wait;wait;wait;"
		"hostname %s;"
		"sv_password \"\";"
		"maxplayers %d;"
		"latch;"
		"map %s\n",
		escaped_name,
		max_players,
		escaped_map
	);

	Con_Printf("^2[MAGD]^7 Starting local server: %s / %s / %d players\n",
		magd_room_name.string,
		magd_room_map.string,
		max_players);
}

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
		byte pong[MAGD_MAX_PACKET_SIZE + 5];
		if (len > sizeof(pong))
			return;
		memcpy(pong, data, len);
		pong[2] = MAGD_TYPE_PONG;
		if (MAGD_QueueWsFrame(MAGD_WS_OPCODE_BINARY, pong, len))
			MAGD_FlushTx();
		return;
	}

	if (type == MAGD_TYPE_PONG)
	{
		g_last_pong = MAGD_Now();
		return;
	}

	if (type == MAGD_TYPE_WELCOME)
    {
	netadr_t host_adr;

	MAGD_SetConnectionState("connected", NULL);

	if (!MAGD_MapSessionToAddress("host", &host_adr))
		return;

	g_welcome_seen = true;

	if (g_host && g_start_server_pending &&
		magd_auto_start_server.value)
	{
		g_start_server_pending = false;
		MAGD_StartLocalServer();
	}

	if (!g_host && g_auto_connect_pending && magd_auto_connect.value)
	{
		g_auto_connect_pending = false;
		Cbuf_AddText("connect 10.254.0.1:27015\n");
	}

	return;
    }

	if (type == MAGD_TYPE_ERROR || type == MAGD_TYPE_READY)
		return;

	if (type != MAGD_TYPE_GAME_DATAGRAM || payload_len < 2)
		return;

	{
		byte flags = payload[0];
		byte peer = payload[1];
		netadr_t from;

		if (g_host)
		{
			char session_id[32];
			if (!(flags & MAGD_GAME_HAS_SENDER) || peer == MAGD_PEER_BROADCAST || peer == 0)
				return;
			Q_snprintf(session_id, sizeof(session_id), "peer_%u", (unsigned int)peer);
			if (!MAGD_MapSessionToAddress(session_id, &from))
				return;
		}
		else
		{
			if (!MAGD_MapSessionToAddress("host", &from))
				return;
		}

		if (!MAGD_QueuePush(&g_incoming, payload + 2, payload_len - 2, &from))
			Con_DPrintf("[MAGD] incoming queue full; dropping packet\n");
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
		size_t frame_len;
		byte *payload;

		if (rsv != 0 || masked)
			return false;
		if (payload_len == 126)
		{
			if (g_rx_len < 4)
				return true;
			payload_len = ((uint64_t)g_rx[2] << 8) | g_rx[3];
			header_len = 4;
		}
		else if (payload_len == 127)
			return false;

		if (opcode >= MAGD_WS_OPCODE_CLOSE && (!fin || payload_len > 125))
			return false;
		if (payload_len > MAGD_MAX_PACKET_SIZE + 64)
			return false;

		frame_len = header_len + (size_t)payload_len;
		if (g_rx_len < frame_len)
			return true;
		payload = g_rx + header_len;
		g_last_rx = MAGD_Now();

		if (opcode == MAGD_WS_OPCODE_CLOSE)
		{
			if (g_control_tx_len == g_control_tx_pos)
				MAGD_QueueWsFrame(MAGD_WS_OPCODE_CLOSE, payload, (size_t)payload_len);
			MAGD_FlushTx();
			g_rx_len = 0;
			MAGD_ScheduleRetry("Server closed WebSocket");
			return false;
		}
		else if (opcode == MAGD_WS_OPCODE_PING)
		{
			MAGD_QueueWsFrame(MAGD_WS_OPCODE_PONG, payload, (size_t)payload_len);
		}
		else if (opcode == MAGD_WS_OPCODE_PONG)
		{
			g_last_pong = MAGD_Now();
		}
		else if (opcode == MAGD_WS_OPCODE_CONTINUATION)
		{
			if (!g_ws_fragmented || g_ws_fragment_len + (size_t)payload_len > sizeof(g_fragment))
				return false;
			memcpy(g_fragment + g_ws_fragment_len, payload, (size_t)payload_len);
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
				MAGD_HandleApplication(payload, (size_t)payload_len);
			else
			{
				if (payload_len > sizeof(g_fragment))
					return false;
				memcpy(g_fragment, payload, (size_t)payload_len);
				g_ws_fragmented = true;
				g_ws_fragment_opcode = MAGD_WS_OPCODE_BINARY;
				g_ws_fragment_len = (size_t)payload_len;
			}
		}
		else if (opcode == MAGD_WS_OPCODE_TEXT)
		{
			if (!fin)
				return false;
		}
		else
			return false;

		if (g_rx_len > frame_len)
			memmove(g_rx, g_rx + frame_len, g_rx_len - frame_len);
		g_rx_len -= frame_len;
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
		else if (g_state == MAGD_STATE_OPEN && !MAGD_ParseWebSocketFrames())
			return false;
	}
}

static void MAGD_SendApplicationPing(void)
{
	byte message[5] = { 0x4D, 0x47, MAGD_TYPE_PING, 0x00, 0x00 };
	if (MAGD_QueueWsFrame(MAGD_WS_OPCODE_BINARY, message, sizeof(message)))
		MAGD_FlushTx();
}

static void MAGD_SendQueued(void)
{
	for (int burst = 0; burst < 16; ++burst)
	{
		byte packet[MAGD_MAX_PACKET_SIZE];
		size_t packet_len = 0;
		netadr_t to;
		byte flags = 0;
		byte peer = MAGD_PEER_BROADCAST;
		byte envelope[MAGD_MAX_PACKET_SIZE + 2];
		byte message[MAGD_MAX_PACKET_SIZE + 7];

		if (g_state != MAGD_STATE_OPEN)
			return;

		if (g_data_tx_len != g_data_tx_pos)
		{
			MAGD_FlushTx();

			if (g_data_tx_len != g_data_tx_pos)
				return;

			if (g_state != MAGD_STATE_OPEN)
				return;
		}

		if (!MAGD_QueuePop(&g_outgoing, packet, &packet_len, &to))
			return;

		if (g_host)
		{
			const char *session_id = MAGD_MapAddressToSession(&to);

			if (session_id && !Q_strnicmp(session_id, "peer_", 5))
			{
				int id = Q_atoi(session_id + 5);

				if (id >= 1 && id <= 31)
				{
					flags = MAGD_GAME_HAS_TARGET;
					peer = (byte)id;
				}
			}
		}

		envelope[0] = flags;
		envelope[1] = peer;

		memcpy(envelope + 2, packet, packet_len);

		message[0] = 0x4D;
		message[1] = 0x47;
		message[2] = MAGD_TYPE_GAME_DATAGRAM;
		message[3] = (byte)((packet_len + 2) >> 8);
		message[4] = (byte)(packet_len + 2);

		memcpy(message + 5, envelope, packet_len + 2);

		if (!MAGD_QueueWsFrame(
			MAGD_WS_OPCODE_BINARY,
			message,
			packet_len + 7))
		{
			MAGD_QueuePush(&g_outgoing, packet, packet_len, &to);
			return;
		}

		MAGD_FlushTx();

		if (g_data_tx_len != g_data_tx_pos)
			return;

		if (g_state != MAGD_STATE_OPEN)
			return;
	}
}

/* ------------------------------------------------------------------------- */
/* Connection attempts                                                       */
/* ------------------------------------------------------------------------- */

static qboolean MAGD_TryConnect(void)
{
	// 1. التحقق من وجود الشروط الأساسية للاتصال (الغرفة، التوكين، ورغبة الاتصال)
	if (!g_wanted || !magd_room_code.string[0] || !magd_auth_token.string[0])
		return false;

	// 2. تحليل الرابط الخاص بالسيرفر لاستخراج Host و Port و TLS status
	if (!MAGD_ParseServerUrl(magd_server_url.string, g_host_name, sizeof(g_host_name),
		g_host_header, sizeof(g_host_header), &g_port, &g_use_tls))
	{
		MAGD_ScheduleRetry("Invalid MAGD server URL");
		return false;
	}

	// 3. منع الاتصالات غير المشفرة Plain WebSocket إذا كان الخيار غير مسموح
	if (!g_use_tls && !magd_allow_insecure_ws.value)
	{
		MAGD_ScheduleRetry("Plain ws:// is disabled; use https:// or wss://");
		return false;
	}

	// 4. التحقق من دعم TLS في المنصة إذا كان الاتصال يطلب التشفير
	if (g_use_tls && !HTTP_TlsAvailable())
	{
		MAGD_ScheduleRetry("TLS is unavailable; provide a valid CA bundle or explicitly enable debug TLS");
		return false;
	}

	// 5. بدء عملية الاتصال الأولي وتصفير المنفذ
	MAGD_SetConnectionState("connecting", NULL);
	MAGD_ResetTransport();

	if (!MAGD_OpenSocket())
	{
		MAGD_ScheduleRetry("TCP connection failed");
		return false;
	}

	// 6. إنشاء سياق TLS للاتصال المشفر عند الحاجة
	if (g_use_tls)
	{
		g_tls = HTTP_TlsNew(g_socket, g_host_name);
		if (!g_tls)
		{
			MAGD_ScheduleRetry("Could not create TLS context");
			return false;
		}

		// تحديث حالة الاتصال بعد إنشاء TLS بنجاح
		MAGD_SetConnectionState("tls", NULL);
	}

	// 7. تسجيل وقت بداية الاتصال وتجهيز مصافحة الـ WebSocket
	g_connect_started = MAGD_Now();

	if (g_state != MAGD_STATE_TCP_CONNECTING && g_state != MAGD_STATE_TLS)
	{
		if (!MAGD_BuildHandshakeRequest())
		{
			MAGD_ScheduleRetry("Could not build WebSocket handshake");
			return false;
		}

		// تحديث حالة الاتصال بعد بناء طلب المصافحة (Handshake) بنجاح
		MAGD_SetConnectionState("handshake", NULL);
	}

	// 8. طباعة سجل النجاح في الـ Console والبدء الفعلي
	Con_Printf("^2[MAGD]^7 Connecting room ^3%s^7 to %s...\n",
		magd_room_code.string, g_host_header);

	return true;
}


/* ------------------------------------------------------------------------- */
/* Room create / guest auth callbacks                                        */
/* ------------------------------------------------------------------------- */

static qboolean MAGD_UrlEncode(const char *src, char *dst, size_t cap)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t out = 0;

	if (!src || !dst || !cap)
		return false;

	while (*src)
	{
		unsigned char c = (unsigned char)*src++;
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
			(c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
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

static void MAGD_CreateCallback(const char *url, qboolean success,
	const byte *data, size_t size, void *userdata)
{
	char token[2048];
	(void)url;
	(void)userdata;

	// Failure branch: التعامل مع فشل إنشاء الغرفة أو فقدان التوكين
	if (!success || !data || !size ||
		!MAGD_JsonString(data, size, "hostToken", token, sizeof(token)))
	{
		Cvar_DirectSet(&magd_connection_state, "error");
		Cvar_DirectSet(&magd_last_error,
			"Room creation failed or host token is missing");

		Con_Printf(S_ERROR
			"[MAGD] room creation failed or host token missing\n");

		return;
	}

	// Success branch: ضبط البيانات وإعداد حالة الاتصال
	Cvar_DirectSet(&magd_auth_token, token);
	MAGD_SetMode(MAGD_NET_MODE_TUNNEL);
	g_host = true;
	g_start_server_pending = magd_auto_start_server.value != 0;
	g_auto_connect_pending = false;
	g_retry_count = 0;
	g_wanted = true;
	g_next_retry = 0.0;

	MAGD_SetConnectionState("connecting", NULL);

	MAGD_TryConnect();
}


static void MAGD_GuestCallback(const char *url, qboolean success,
	const byte *data, size_t size, void *userdata)
{
	char token[2048];
	(void)url;
	(void)userdata;

	// Failure branch: التعامل مع فشل توثيق الضيف أو فقدان التوكين
	if (!success || !data || !size ||
		!MAGD_JsonString(data, size, "token", token, sizeof(token)))
	{
		Cvar_DirectSet(&magd_connection_state, "error");
		Cvar_DirectSet(&magd_last_error,
			"Guest authentication failed or token is missing");

		Con_Printf(S_ERROR
			"[MAGD] guest authentication failed or token missing\n");

		return;
	}

	// Success branch: ضبط البيانات وإعداد حالة اتصال الضيف
	Cvar_DirectSet(&magd_auth_token, token);
	MAGD_SetMode(MAGD_NET_MODE_TUNNEL);
	g_host = false;
	g_start_server_pending = false;
	g_auto_connect_pending = true;
	g_retry_count = 0;
	g_wanted = true;
	g_next_retry = 0.0;

	MAGD_SetConnectionState("connecting", NULL);

	MAGD_TryConnect();
}


static void MAGD_CreateRoom_f(void)
{
	char code[32];
	char name[256], map[128], game[128], host[256], password[512];
	char enc_name[768], enc_map[384], enc_game[384], enc_host[768], enc_password[1536];
	char url[4096];
	int max_players = bound(2, (int)magd_max_players.value, 32);

	// تعيين حالة الاتصال وتصفير آخر خطأ في بداية الدالة
	Cvar_DirectSet(&magd_connection_state, "creating");
	Cvar_DirectSet(&magd_last_error, "");

	Q_snprintf(code, sizeof(code), "MAGD-%04X", (unsigned int)COM_RandomLong(0x1000, 0xFFFF));
	Q_strncpy(name, magd_room_name.string, sizeof(name));
	Q_strncpy(map, magd_room_map.string, sizeof(map));
	Q_strncpy(game, magd_room_game.string, sizeof(game));
	Q_strncpy(host, magd_host_name.string, sizeof(host));
	Q_strncpy(password, magd_room_password.string, sizeof(password));

	if (!MAGD_UrlEncode(name, enc_name, sizeof(enc_name)) ||
		!MAGD_UrlEncode(map, enc_map, sizeof(enc_map)) ||
		!MAGD_UrlEncode(game, enc_game, sizeof(enc_game)) ||
		!MAGD_UrlEncode(host, enc_host, sizeof(enc_host)) ||
		!MAGD_UrlEncode(password, enc_password, sizeof(enc_password)))
	{
		Con_Printf(S_ERROR "[MAGD] room fields are too long\n");
		return;
	}

	Cvar_DirectSet(&magd_room_code, code);
	Cvar_DirectSet(&magd_auth_token, "");
	MAGD_StopTunnel();

	Q_snprintf(url, sizeof(url),
		"%s/api/v1/rooms/create?code=%s&name=%s&map=%s&game=%s&host=%s&maxPlayers=%d&password=%s",
		magd_server_url.string, code, enc_name, enc_map, enc_game, enc_host,
		max_players, enc_password);

	Con_Printf("^2[MAGD]^7 Creating room ^3%s^7...\n", code);

	// التحقق من إضافة الطلب للانتظار (Queue) وتعيين الخطأ عند الفشل
	if (!HTTP_GetToMemory(url, MAGD_CreateCallback, NULL))
	{
		Cvar_DirectSet(&magd_connection_state, "error");
		Cvar_DirectSet(&magd_last_error,
			"Could not start MAGD room creation request");

		Con_Printf(S_ERROR
			"[MAGD] failed to queue room creation request\n");
	}
}


static void MAGD_ConnectRoom_f(void)
{
	char url[1536];
	char room[128];
	char password[256];
	char enc_room[256];
	char base[1024];
	size_t len;

	if (Cmd_Argc() >= 2)
		Q_strncpy(room, Cmd_Argv(1), sizeof(room));
	else
		Q_strncpy(room, magd_room_code.string, sizeof(room));

	if (!room[0])
	{
		Con_Printf(S_USAGE "magd_connect <room_code> [password]\n");
		return;
	}

	for (char *p = room; *p; ++p)
	{
		if (*p >= 'a' && *p <= 'z')
			*p = (char)(*p - 'a' + 'A');
	}

	if (!MAGD_ValidRoomCode(room))
	{
		Con_Printf(S_ERROR "[MAGD] invalid room code: %s\n", room);
		Cvar_DirectSet(&magd_connection_state, "error");
		Cvar_DirectSet(&magd_last_error, "Invalid MAGD room code");
		return;
	}

	if (Cmd_Argc() >= 3)
		Q_strncpy(password, Cmd_Argv(2), sizeof(password));
	else
		Q_strncpy(password, magd_room_password.string, sizeof(password));

	if (!MAGD_UrlEncode(room, enc_room, sizeof(enc_room)))
	{
		Cvar_DirectSet(&magd_connection_state, "error");
		Cvar_DirectSet(&magd_last_error, "Could not encode MAGD room code");
		return;
	}

	Cvar_DirectSet(&magd_room_code, room);
	Cvar_DirectSet(&magd_room_password, password);
	Cvar_DirectSet(&magd_auth_token, "");

	MAGD_StopTunnel();
	MAGD_SetMode(MAGD_NET_MODE_TUNNEL);

	g_host = false;
	g_start_server_pending = false;
	g_auto_connect_pending = true;

	Cvar_DirectSet(&magd_connection_state, "authenticating");
	Cvar_DirectSet(&magd_last_error, "");

	Q_strncpy(base, magd_server_url.string, sizeof(base));

	len = Q_strlen(base);
	while (len > 0 && base[len - 1] == '/')
		base[--len] = 0;

	Q_snprintf(url, sizeof(url),
		"%s/api/v1/auth/guest?room=%s",
		base,
		enc_room);

	Con_Printf("^2[MAGD]^7 Authenticating guest for room ^3%s^7...\n",
		magd_room_code.string);

	if (!HTTP_GetToMemory(url, MAGD_GuestCallback, NULL))
	{
		Cvar_DirectSet(&magd_connection_state, "error");
		Cvar_DirectSet(&magd_last_error,
			"Could not start MAGD guest authentication request");

		Con_Printf(S_ERROR
			"[MAGD] failed to queue guest authentication request\n");
	}
}

static void MAGD_Disconnect_f(void)
{
	MAGD_StopTunnel();
	g_mode = MAGD_NET_MODE_LAN;
	Con_Printf("^2[MAGD]^7 Tunnel disconnected\n");
}

static void MAGD_Status_f(void)
{
	static const char *states[] = { "idle", "tcp", "tls", "ws-send", "ws-recv", "open", "backoff" };
	const char *state = (g_state >= 0 && g_state < (int)ARRAYSIZE(states)) ? states[g_state] : "unknown";
	Con_Printf("^2[MAGD]^7 mode=%d state=%s host=%d wanted=%d room=%s retries=%d\n",
		(int)g_mode, state, g_host ? 1 : 0, g_wanted ? 1 : 0,
		magd_room_code.string, g_retry_count);
}

void MAGD_Init(void)
{
	Cvar_RegisterVariable(&magd_enabled);
	Cvar_RegisterVariable(&magd_server_url);
	Cvar_RegisterVariable(&magd_room_code);
	Cvar_RegisterVariable(&magd_auth_token);
	Cvar_RegisterVariable(&magd_room_password);
	Cvar_RegisterVariable(&magd_room_name);
	Cvar_RegisterVariable(&magd_room_map);
	Cvar_RegisterVariable(&magd_room_game);
	Cvar_RegisterVariable(&magd_host_name);
	Cvar_RegisterVariable(&magd_max_players);
	Cvar_RegisterVariable(&magd_reconnect);
	Cvar_RegisterVariable(&magd_auto_connect);
	Cvar_RegisterVariable(&magd_allow_insecure_ws);
	Cvar_RegisterVariable(&magd_auto_start_server);
    Cvar_RegisterVariable(&magd_connection_state);
    Cvar_RegisterVariable(&magd_last_error);
    Cvar_RegisterVariable(&magd_room_list_revision);
    Cvar_RegisterVariable(&magd_room_list_state);
    Cvar_RegisterVariable(&magd_room_list_error);

	Cmd_AddCommand("magd_create_room", MAGD_CreateRoom_f, "Create a MAGD online room");
	Cmd_AddCommand("magd_connect", MAGD_ConnectRoom_f, "Connect to a MAGD room");
	Cmd_AddCommand("magd_disconnect", MAGD_Disconnect_f, "Disconnect the MAGD tunnel");
	Cmd_AddCommand("magd_status", MAGD_Status_f, "Show MAGD tunnel status");
	Cmd_AddCommand("magd_list_rooms", MAGD_ListRooms_f,
	"Refresh the MAGD online room list");
	

	MAGD_QueueInit(&g_incoming);
	MAGD_QueueInit(&g_outgoing);
	MAGD_ClearSessionMaps();
	MAGD_ResetTransport();
	
    MAGD_SetConnectionState("disconnected", NULL);
    Cvar_DirectSet(&magd_room_list_state, "idle");
    Cvar_DirectSet(&magd_room_list_error, "");

	Con_Printf("^2[MAGD]^7 Network layer initialized\n");
}

void MAGD_Shutdown(void)
{
	MAGD_StopTunnel();
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
		MAGD_StopTunnel();
	g_mode = mode;
}

void MAGD_ProcessTunnel(void)
{
	double now;

	if (!magd_enabled.value)
    {
	if (g_state != MAGD_STATE_IDLE || g_wanted)
		MAGD_StopTunnel();

	Cvar_DirectSet(&magd_connection_state, "disabled");
	return;
    }
	if (g_mode != MAGD_NET_MODE_TUNNEL)
		return;

	now = MAGD_Now();

	if (!NET_IsSocketValid(g_socket))
	{
		if (!g_wanted)
			return;
		if (g_state == MAGD_STATE_BACKOFF && now < g_next_retry)
			return;
		if (g_state == MAGD_STATE_BACKOFF && now >= g_next_retry)
			g_state = MAGD_STATE_IDLE;
		if (g_state == MAGD_STATE_IDLE)
			MAGD_TryConnect();
		return;
	}

	if (g_connect_started > 0.0 && now - g_connect_started > MAGD_CONNECT_TIMEOUT && g_state != MAGD_STATE_OPEN)
	{
		MAGD_ScheduleRetry("Connection timeout");
		return;
	}

	if (g_state == MAGD_STATE_TCP_CONNECTING)
	{
		int status = MAGD_CheckTcpConnected();
		if (status < 0)
		{
			MAGD_ScheduleRetry("TCP connect failed");
			return;
		}
		if (status == 0)
			return;
		if (g_use_tls)
			g_state = MAGD_STATE_TLS;
		else if (!MAGD_BuildHandshakeRequest())
		{
			MAGD_ScheduleRetry("Could not build WebSocket handshake");
			return;
		}
	}

	if (g_state == MAGD_STATE_TLS)
	{
		int status = HTTP_TlsHandshake(g_tls);
		if (status == HTTP_TLS_WANT)
			return;
		if (status == HTTP_TLS_ERROR)
		{
			MAGD_ScheduleRetry("TLS handshake failed");
			return;
		}
		if (!MAGD_BuildHandshakeRequest())
		{
			MAGD_ScheduleRetry("Could not build WebSocket handshake after TLS");
			return;
		}
	}

	if (g_state == MAGD_STATE_WS_SEND)
	{
		while (g_request_pos < g_request_len)
		{
			int result;
			if (g_tls)
				result = HTTP_TlsSend(g_tls, g_request + g_request_pos, (int)(g_request_len - g_request_pos));
			else
				result = send(g_socket, g_request + g_request_pos, (int)(g_request_len - g_request_pos), 0);

			if (result == HTTP_TLS_WANT || (result < 0 && MAGD_WouldBlock()))
				return;
			if (result <= 0)
			{
				MAGD_ScheduleRetry("WebSocket handshake send failed");
				return;
			}
			g_request_pos += (size_t)result;
		}
		g_state = MAGD_STATE_WS_RECV;
	}

	if (g_state == MAGD_STATE_WS_RECV || g_state == MAGD_STATE_OPEN)
	{
		if (!MAGD_ReadSocket())
		{
			if (g_state != MAGD_STATE_BACKOFF)
				MAGD_ScheduleRetry("WebSocket receive failed");
			return;
		}
	}

	if (g_state == MAGD_STATE_OPEN)
	{
		if (now >= g_next_keepalive)
		{
			MAGD_SendApplicationPing();
			g_next_keepalive = now + MAGD_KEEPALIVE_INTERVAL;
		}
		if (now - g_last_pong > MAGD_KEEPALIVE_TIMEOUT)
		{
			MAGD_ScheduleRetry("MAGD keepalive timeout");
			return;
		}
		MAGD_SendQueued();
		MAGD_FlushTx();
	}
}

qboolean MAGD_SendDatagram(const void *data, size_t length, const netadr_t *to)
{
	if (!magd_enabled.value || g_mode != MAGD_NET_MODE_TUNNEL)
		return false;
	return MAGD_QueuePush(&g_outgoing, data, length, to);
}

qboolean MAGD_GetDatagram(byte *data, size_t *length, netadr_t *from)
{
	if (!magd_enabled.value || g_mode != MAGD_NET_MODE_TUNNEL)
		return false;
	return MAGD_QueuePop(&g_incoming, data, length, from);
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

	TASSERT_EQi(MAGD_MapSessionToAddress("host", &a), true);
	TASSERT_EQi(a.ip[0], 10);
	TASSERT_EQi(a.ip[1], 254);
	TASSERT_EQi(a.ip[2], 0);
	TASSERT_EQi(a.ip[3], 1);
	TASSERT_STR(MAGD_MapAddressToSession(&a), "host");

	TASSERT_EQi(MAGD_MapSessionToAddress("peer_1", &a), true);
	TASSERT_EQi(a.ip[3], 2);
	TASSERT_STR(MAGD_MapAddressToSession(&a), "peer_1");
}
#endif
