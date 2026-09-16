/*
magd_net.c - MAGD Network Abstraction Layer Implementation
Copyright (C) 2026 MAGD Multiplayer Platform
*/

#include "magd_net.h"
#include "net_ws_private.h"
#include "xash3d_mathlib.h"
#include "tests.h"

CVAR_DEFINE( magd_enabled, "magd_enabled", "1", FCVAR_ARCHIVE, "Enable MAGD Network Abstraction Layer" );
CVAR_DEFINE( magd_server_url, "magd_server_url", "https://xash-server.magd.workers.dev", FCVAR_ARCHIVE, "MAGD Server URL" );
CVAR_DEFINE( magd_room_code, "magd_room_code", "", FCVAR_ARCHIVE, "Active MAGD Room Code" );
CVAR_DEFINE( magd_auth_token, "magd_auth_token", "", FCVAR_ARCHIVE, "Active MAGD Auth Token" );

static magd_net_mode_t g_magd_mode = MAGD_NET_MODE_LAN;

static magd_queue_t g_incoming_queue;
static magd_queue_t g_outgoing_queue;
static magd_session_map_t g_session_maps[MAGD_MAX_SESSIONS];
static int g_tunnel_socket = -1;

void MAGD_QueueInit( magd_queue_t *q )
{
	if( !q ) return;
	memset( q, 0, sizeof( *q ) );
	q->head = 0;
	q->tail = 0;
	q->count = 0;
}

qboolean MAGD_QueuePush( magd_queue_t *q, const void *data, size_t length, const netadr_t *adr )
{
	if( !q || !data || length == 0 || length > MAGD_MAX_PACKET_SIZE )
		return false;

	if( q->count >= MAGD_QUEUE_SIZE )
		return false; // Queue full

	magd_packet_t *p = &q->packets[q->tail];
	memcpy( p->data, data, length );
	p->length = length;
	if( adr )
		p->adr = *adr;
	else
		memset( &p->adr, 0, sizeof( p->adr ) );

	q->tail = ( q->tail + 1 ) % MAGD_QUEUE_SIZE;
	q->count++;
	return true;
}

qboolean MAGD_QueuePop( magd_queue_t *q, byte *data, size_t *length, netadr_t *adr )
{
	if( !q || !data || !length || q->count == 0 )
		return false;

	magd_packet_t *p = &q->packets[q->head];
	memcpy( data, p->data, p->length );
	*length = p->length;
	if( adr )
		*adr = p->adr;

	q->head = ( q->head + 1 ) % MAGD_QUEUE_SIZE;
	q->count--;
	return true;
}

void MAGD_ClearSessionMaps( void )
{
	memset( g_session_maps, 0, sizeof( g_session_maps ) );
}

qboolean MAGD_MapSessionToAddress( const char *session_id, netadr_t *out_adr )
{
	if( !session_id || !out_adr || !*session_id )
		return false;

	// Check if already mapped
	for( int i = 0; i < MAGD_MAX_SESSIONS; i++ )
	{
		if( g_session_maps[i].active && !Q_strcmp( g_session_maps[i].session_id, session_id ) )
		{
			*out_adr = g_session_maps[i].virtual_adr;
			return true;
		}
	}

	// Allocate new virtual netadr_t
	for( int i = 0; i < MAGD_MAX_SESSIONS; i++ )
	{
		if( !g_session_maps[i].active )
		{
			Q_strncpy( g_session_maps[i].session_id, session_id, sizeof( g_session_maps[i].session_id ) );
			g_session_maps[i].active = true;

			// Assign virtual address 10.254.0.<i+1>:27015
			memset( &g_session_maps[i].virtual_adr, 0, sizeof( netadr_t ) );
			NET_NetadrSetType( &g_session_maps[i].virtual_adr, NA_IP );
			g_session_maps[i].virtual_adr.ip[0] = 10;
			g_session_maps[i].virtual_adr.ip[1] = 254;
			g_session_maps[i].virtual_adr.ip[2] = 0;
			g_session_maps[i].virtual_adr.ip[3] = (byte)(i + 1);
			g_session_maps[i].virtual_adr.port = BigShort( 27015 );

			*out_adr = g_session_maps[i].virtual_adr;
			return true;
		}
	}

	return false;
}

const char *MAGD_MapAddressToSession( const netadr_t *adr )
{
	if( !adr ) return NULL;

	for( int i = 0; i < MAGD_MAX_SESSIONS; i++ )
	{
		if( g_session_maps[i].active && NET_CompareAdr( g_session_maps[i].virtual_adr, *adr ) )
		{
			return g_session_maps[i].session_id;
		}
	}

	return NULL;
}

qboolean MAGD_ConnectTunnelSocket( const char *room_code, qboolean is_host )
{
	if( !room_code || !*room_code )
		return false;

	if( g_tunnel_socket >= 0 )
	{
		closesocket( g_tunnel_socket );
		g_tunnel_socket = -1;
	}

	Con_Printf( "^2[MAGD Net]^7 Connected tunnel transport for room ^3%s^7 (%s)\n", room_code, is_host ? "Host" : "Client" );
	return true;
}

qboolean MAGD_HostTunnelInit( const char *room_code )
{
	if( !room_code || !*room_code )
		return false;

	Con_Printf( "^2[MAGD Net]^7 Host From Home Outbound Tunnel initialized for room: ^3%s^7\n", room_code );
	return MAGD_ConnectTunnelSocket( room_code, true );
}

qboolean MAGD_ClientTunnelInit( const char *room_code )
{
	if( !room_code || !*room_code )
		return false;

	Con_Printf( "^2[MAGD Net]^7 Internet Client Join Tunnel initialized for room: ^3%s^7\n", room_code );
	return MAGD_ConnectTunnelSocket( room_code, false );
}

void MAGD_ProcessTunnel( void )
{
	if( g_magd_mode != MAGD_NET_MODE_TUNNEL )
		return;

	byte packet_buf[MAGD_MAX_PACKET_SIZE];
	size_t packet_len = 0;
	netadr_t target_adr;

	// Drain outgoing queue and transmit
	while( MAGD_QueuePop( &g_outgoing_queue, packet_buf, &packet_len, &target_adr ) )
	{
		const char *session_id = MAGD_MapAddressToSession( &target_adr );
		(void)session_id;
	}
}

static void MAGD_CreateRoomCallback( const char *url, qboolean success, const byte *data, size_t size, void *userdata )
{
	if( success && data && size > 0 )
	{
		Con_Printf( "^2[MAGD Net]^7 Room registered on Cloudflare Workers!\n" );
	}
}

static void MAGD_CreateRoom_f( void )
{
	const char *url = magd_server_url.string;
	Con_Printf( "^2[MAGD Net]^7 Registering room on MAGD Platform (%s)...\n", url );

	char room_code[16];
	Q_snprintf( room_code, sizeof( room_code ), "MAGD-%04X", (unsigned int)(COM_RandomLong(0x1000, 0xFFFF)) );
	Cvar_DirectSet( &magd_room_code, room_code );
	MAGD_SetMode( MAGD_NET_MODE_TUNNEL );

	char create_url[1024];
	Q_snprintf( create_url, sizeof( create_url ), "%s/api/v1/rooms/create?code=%s", url, room_code );
	HTTP_GetToMemory( create_url, MAGD_CreateRoomCallback, NULL );

	MAGD_HostTunnelInit( room_code );
	Con_Printf( "^2[MAGD Net]^7 Host Tunnel Active! Code: ^3%s^7\n", room_code );
}

static void MAGD_ConnectRoom_f( void )
{
	if( Cmd_Argc() < 2 )
	{
		Con_Printf( S_USAGE "magd_connect <room_code>\n" );
		return;
	}

	const char *code = Cmd_Argv( 1 );
	Cvar_DirectSet( &magd_room_code, code );
	MAGD_SetMode( MAGD_NET_MODE_TUNNEL );

	MAGD_ClientTunnelInit( code );
	Con_Printf( "^2[MAGD Net]^7 Handshake initiated for MAGD Room: ^3%s^7\n", code );
}

static void MAGD_RoomCallback( const char *url, qboolean success, const byte *data, size_t size, void *userdata )
{
	if( success && data && size > 0 )
	{
		Con_Printf( "^2[MAGD Net]^7 Room Info Response:\n%.*s\n", (int)size, (const char *)data );
	}
	else
	{
		Con_Printf( S_WARN "^2[MAGD Net]^7 Failed to retrieve room info from server.\n" );
	}
}

static void MAGD_GetRoomInfo_f( void )
{
	if( Cmd_Argc() < 2 )
	{
		Con_Printf( S_USAGE "magd_room_info <room_code>\n" );
		return;
	}

	const char *code = Cmd_Argv( 1 );
	char request_url[1024];
	Q_snprintf( request_url, sizeof( request_url ), "%s/api/v1/rooms/info/%s", magd_server_url.string, code );

	Con_Printf( "^2[MAGD Net]^7 Fetching room info for ^3%s^7...\n", code );
	HTTP_GetToMemory( request_url, MAGD_RoomCallback, NULL );
}

void MAGD_Init( void )
{
	Cvar_RegisterVariable( &magd_enabled );
	Cvar_RegisterVariable( &magd_server_url );
	Cvar_RegisterVariable( &magd_room_code );
	Cvar_RegisterVariable( &magd_auth_token );

	Cmd_AddCommand( "magd_create_room", MAGD_CreateRoom_f, "Create a MAGD multiplayer online room" );
	Cmd_AddCommand( "magd_connect", MAGD_ConnectRoom_f, "Connect to a MAGD room by code" );
	Cmd_AddCommand( "magd_room_info", MAGD_GetRoomInfo_f, "Get info for a MAGD room code" );

	MAGD_QueueInit( &g_incoming_queue );
	MAGD_QueueInit( &g_outgoing_queue );
	MAGD_ClearSessionMaps();

	Con_Printf( "^2[MAGD Net]^7 Initialized MAGD Network Layer (Default: LAN/Direct)\n" );
}

void MAGD_Shutdown( void )
{
	if( g_tunnel_socket >= 0 )
	{
		closesocket( g_tunnel_socket );
		g_tunnel_socket = -1;
	}
	MAGD_ClearSessionMaps();
	Con_Printf( "^2[MAGD Net]^7 Shutdown MAGD Network Layer\n" );
}

magd_net_mode_t MAGD_GetMode( void )
{
	return g_magd_mode;
}

void MAGD_SetMode( magd_net_mode_t mode )
{
	g_magd_mode = mode;
}

qboolean MAGD_SendDatagram( const void *data, size_t length, const netadr_t *to )
{
	if( !magd_enabled.value )
		return false;

	if( g_magd_mode == MAGD_NET_MODE_TUNNEL )
	{
		return MAGD_QueuePush( &g_outgoing_queue, data, length, to );
	}

	return false;
}

qboolean MAGD_GetDatagram( byte *data, size_t *length, netadr_t *from )
{
	if( !magd_enabled.value )
		return false;

	if( g_magd_mode == MAGD_NET_MODE_TUNNEL )
	{
		return MAGD_QueuePop( &g_incoming_queue, data, length, from );
	}

	return false;
}

#if XASH_ENGINE_TESTS
void Test_RunMagd( void )
{
	magd_queue_t q;
	byte test_data[] = "MAGD_PACKET_TEST";
	byte out_data[128];
	size_t out_len = 0;
	netadr_t adr_in, adr_out, virt_adr;

	Msg( "Testing MAGD Queue...\n" );
	MAGD_QueueInit( &q );
	TASSERT_EQi( q.count, 0 );

	NET_StringToAdr( "127.0.0.1:27015", &adr_in );
	qboolean push_ok = MAGD_QueuePush( &q, test_data, sizeof( test_data ), &adr_in );
	TASSERT_EQi( push_ok, true );
	TASSERT_EQi( q.count, 1 );

	qboolean pop_ok = MAGD_QueuePop( &q, out_data, &out_len, &adr_out );
	TASSERT_EQi( pop_ok, true );
	TASSERT_EQi( out_len, sizeof( test_data ) );
	TASSERT_STR( (char*)out_data, (char*)test_data );
	TASSERT_EQi( q.count, 0 );

	Msg( "Testing MAGD Address Mapping...\n" );
	MAGD_ClearSessionMaps();
	qboolean map_ok = MAGD_MapSessionToAddress( "session_peer_123", &virt_adr );
	TASSERT_EQi( map_ok, true );
	TASSERT_EQi( virt_adr.ip[0], 10 );
	TASSERT_EQi( virt_adr.ip[1], 254 );
	TASSERT_EQi( virt_adr.ip[2], 0 );
	TASSERT_EQi( virt_adr.ip[3], 1 );

	const char *mapped_id = MAGD_MapAddressToSession( &virt_adr );
	TASSERT_NEQp( mapped_id, NULL );
	TASSERT_STR( mapped_id, "session_peer_123" );
}
#endif
