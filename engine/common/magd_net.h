/*
magd_net.h - MAGD Network Abstraction Layer Header
Copyright (C) 2026 MAGD Multiplayer Platform
*/

#ifndef MAGD_NET_H
#define MAGD_NET_H

#include "common.h"
#include "netadr.h"

#define MAGD_MAX_PACKET_SIZE 16384
#define MAGD_QUEUE_SIZE 128

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

typedef struct magd_room_info_s
{
	char room_code[16];
	char name[64];
	char map[64];
	char game[32];
	int current_players;
	int max_players;
	qboolean has_password;
	int ping;
} magd_room_info_t;

extern convar_t magd_enabled;
extern convar_t magd_server_url;
extern convar_t magd_room_code;
extern convar_t magd_auth_token;

void MAGD_Init( void );
void MAGD_Shutdown( void );
magd_net_mode_t MAGD_GetMode( void );
void MAGD_SetMode( magd_net_mode_t mode );

void MAGD_QueueInit( magd_queue_t *q );
qboolean MAGD_QueuePush( magd_queue_t *q, const void *data, size_t length, const netadr_t *adr );
qboolean MAGD_QueuePop( magd_queue_t *q, byte *data, size_t *length, netadr_t *adr );

void MAGD_ProcessTunnel( void );

qboolean MAGD_SendDatagram( const void *data, size_t length, const netadr_t *to );
qboolean MAGD_GetDatagram( byte *data, size_t *length, netadr_t *from );

#endif // MAGD_NET_H
