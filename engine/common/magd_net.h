/*
 * magd_net.h - MAGD Network Abstraction Layer
 *
 * The MAGD transport wraps Xash datagrams in a small binary envelope and
 * carries them over a WebSocket tunnel. LAN/direct-IP operation stays
 * unchanged; MAGD is only active when the mode is TUNNEL.
 */
#ifndef MAGD_NET_H
#define MAGD_NET_H

#include "common.h"
#include "netadr.h"

#define MAGD_MAX_PACKET_SIZE 16384
#define MAGD_QUEUE_SIZE 128
#define MAGD_MAX_SESSIONS 32

#define MAGD_MAGIC 0x4D47
#define MAGD_TYPE_HELLO 0x01
#define MAGD_TYPE_WELCOME 0x02
#define MAGD_TYPE_PING 0x03
#define MAGD_TYPE_PONG 0x04
#define MAGD_TYPE_HOST_REGISTER 0x10
#define MAGD_TYPE_HOST_UPDATE 0x11
#define MAGD_TYPE_JOIN_ROOM 0x20
#define MAGD_TYPE_READY 0x21
#define MAGD_TYPE_GAME_DATAGRAM 0x30
#define MAGD_TYPE_ERROR 0xE0

#define MAGD_GAME_HAS_SENDER 0x01
#define MAGD_GAME_HAS_TARGET 0x02
#define MAGD_PEER_BROADCAST 0xFF

#define MAGD_WS_OPCODE_CONTINUATION 0x0
#define MAGD_WS_OPCODE_TEXT 0x1
#define MAGD_WS_OPCODE_BINARY 0x2
#define MAGD_WS_OPCODE_CLOSE 0x8
#define MAGD_WS_OPCODE_PING 0x9
#define MAGD_WS_OPCODE_PONG 0xA

typedef enum magd_net_mode_e
{
	MAGD_NET_MODE_LAN = 0,
	MAGD_NET_MODE_DIRECT_IP = 1,
	MAGD_NET_MODE_TUNNEL = 2
} magd_net_mode_t;

typedef struct magd_packet_s
{
	byte data[MAGD_MAX_PACKET_SIZE];
	size_t length;
	netadr_t adr;
} magd_packet_t;

typedef struct magd_queue_s
{
	magd_packet_t packets[MAGD_QUEUE_SIZE];
	int head;
	int tail;
	int count;
} magd_queue_t;

typedef struct magd_session_map_s
{
	char session_id[64];
	netadr_t virtual_adr;
	qboolean active;
} magd_session_map_t;

extern convar_t magd_enabled;
extern convar_t magd_server_url;
extern convar_t magd_room_code;
extern convar_t magd_auth_token;
extern convar_t magd_room_password;

void MAGD_Init(void);
void MAGD_Shutdown(void);
magd_net_mode_t MAGD_GetMode(void);
void MAGD_SetMode(magd_net_mode_t mode);
void MAGD_ProcessTunnel(void);

void MAGD_QueueInit(magd_queue_t *q);
qboolean MAGD_QueuePush(magd_queue_t *q, const void *data, size_t length, const netadr_t *adr);
qboolean MAGD_QueuePop(magd_queue_t *q, byte *data, size_t *length, netadr_t *adr);

qboolean MAGD_MapSessionToAddress(const char *session_id, netadr_t *out_adr);
const char *MAGD_MapAddressToSession(const netadr_t *adr);
void MAGD_ClearSessionMaps(void);

qboolean MAGD_SendDatagram(const void *data, size_t length, const netadr_t *to);
qboolean MAGD_GetDatagram(byte *data, size_t *length, netadr_t *from);

#endif /* MAGD_NET_H */
