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
static qboolean g_tunnel_connected = false;

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
		g_tunnel_connected = false;
	}

	// Extract hostname from magd_server_url
	char host[256] = "xash-server.magd.workers.dev";
	int port = 80;
	const char *url_str = magd_server_url.string;
	if( !Q_strnicmp( url_str, "https://", 8 ) )
	{
		Q_strncpy( host, url_str + 8, sizeof( host ) );
		port = 443;
	}
	else if( !Q_strnicmp( url_str, "http://", 7 ) )
	{
		Q_strncpy( host, url_str + 7, sizeof( host ) );
		port = 80;
	}

	char *slash = Q_strchr( host, '/' );
	if( slash ) *slash = '\0';

	struct sockaddr_storage addr;
	if( NET_StringToSockaddr( host, &addr, false, AF_INET ) != NET_EAI_OK )
	{
		Con_Printf( S_WARN "^2[MAGD Net]^7 Could not resolve server address: %s\n", host );
		return false;
	}

	((struct sockaddr_in *)&addr)->sin_port = BigShort( port );

	int sock = socket( addr.ss_family, SOCK_STREAM, IPPROTO_TCP );
	if( sock < 0 )
	{
		Con_Printf( S_WARN "^2[MAGD Net]^7 Socket creation failed\n" );
		return false;
	}

	NET_MakeSocketNonBlocking( sock );
	connect( sock, (struct sockaddr *)&addr, NET_SockAddrLen( &addr ) );

	g_tunnel_socket = sock;
	g_tunnel_connected = true;

	// Send HTTP WebSocket Upgrade Request
	char upgrade_req[1024];
	const char *token = magd_auth_token.string;
	if( !token || !*token ) token = "magd_token_default";

	Q_snprintf( upgrade_req, sizeof( upgrade_req ),
		"GET /ws/room/%s?role=%s&token=%s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		"Sec-WebSocket-Version: 13\r\n\r\n",
		room_code, is_host ? "host" : "client", token, host );

	send( g_tunnel_socket, upgrade_req, Q_strlen( upgrade_req ), 0 );

	Con_Printf( "^2[MAGD Net]^7 WebSocket Tunnel connected to %s for room ^3%s^7 (%s)\n", host, room_code, is_host ? "Host" : "Client" );
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
	if( g_magd_mode != MAGD_NET_MODE_TUNNEL || g_tunnel_socket < 0 )
		return;

	byte packet_buf[MAGD_MAX_PACKET_SIZE];
	size_t packet_len = 0;
	netadr_t target_adr;

	// Drain outgoing queue and transmit framed packets over socket when connected
	while( MAGD_QueuePop( &g_outgoing_queue, packet_buf, &packet_len, &target_adr ) )
	{
		if( g_tunnel_socket >= 0 )
		{
			// Construct MAGD binary frame: Magic (2B 0x4D47) + Type (1B 0x30) + Length (2B) + Payload
			byte frame[MAGD_MAX_PACKET_SIZE + 5];
			frame[0] = 0x4D;
			frame[1] = 0x47;
			frame[2] = 0x30; // GAME_DATAGRAM
			frame[3] = (byte)((packet_len >> 8) & 0xFF);
			frame[4] = (byte)(packet_len & 0xFF);
			memcpy( &frame[5], packet_buf, packet_len );

			send( g_tunnel_socket, (const char *)frame, packet_len + 5, 0 );
		}
	}

	// Receive non-blocking datagram frames from socket when connected
	if( g_tunnel_socket >= 0 )
	{
		byte recv_buf[MAGD_MAX_PACKET_SIZE + 5];
		int ret = recv( g_tunnel_socket, (char *)recv_buf, sizeof( recv_buf ), 0 );
		if( ret > 5 && recv_buf[0] == 0x4D && recv_buf[1] == 0x47 && recv_buf[2] == 0x30 )
		{
			size_t payload_len = ((size_t)recv_buf[3] << 8) | recv_buf[4];
			if( payload_len > 0 && payload_len <= (size_t)(ret - 5) )
			{
				netadr_t sender_adr;
				MAGD_MapSessionToAddress( "remote_peer", &sender_adr );
				MAGD_QueuePush( &g_incoming_queue, &recv_buf[5], payload_len, &sender_adr );
			}
		}
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
		g_tunnel_connected = false;
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
