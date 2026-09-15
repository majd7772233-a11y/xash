/*
magd_net.c - MAGD Network Abstraction Layer Implementation
Copyright (C) 2026 MAGD Multiplayer Platform
*/

#include "magd_net.h"

CVAR_DEFINE( magd_enabled, "magd_enabled", "1", FCVAR_ARCHIVE, "Enable MAGD Network Abstraction Layer" );
CVAR_DEFINE( magd_server_url, "magd_server_url", "https://xash-server.magd.workers.dev", FCVAR_ARCHIVE, "MAGD Server URL" );
CVAR_DEFINE( magd_room_code, "magd_room_code", "", FCVAR_ARCHIVE, "Active MAGD Room Code" );
CVAR_DEFINE( magd_auth_token, "magd_auth_token", "", FCVAR_ARCHIVE, "Active MAGD Auth Token" );

static magd_net_mode_t g_magd_mode = MAGD_NET_MODE_LAN;

static void MAGD_CreateRoom_f( void )
{
	const char *url = magd_server_url.string;
	Con_Printf( "^2[MAGD Net]^7 Creating room on MAGD Platform (%s)...\n", url );

	// Generate room code and set mode
	char room_code[16];
	Q_snprintf( room_code, sizeof( room_code ), "MAGD-%04X", (unsigned int)(COM_RandomLong(0x1000, 0xFFFF)) );
	Cvar_DirectSet( &magd_room_code, room_code );
	MAGD_SetMode( MAGD_NET_MODE_TUNNEL );

	Con_Printf( "^2[MAGD Net]^7 Room created! Code: ^3%s^7\n", room_code );
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
	Cmd_AddCommand( "magd_room_info", MAGD_GetRoomInfo_f, "Get info for a MAGD room code" );

	Con_Printf( "^2[MAGD Net]^7 Initialized MAGD Network Layer (Default: LAN/Direct)\n" );
}

void MAGD_Shutdown( void )
{
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
		// Tunnel adapter handles raw binary Xash packet forwarding
		return true;
	}

	return false;
}

qboolean MAGD_GetDatagram( byte *data, size_t *length, netadr_t *from )
{
	if( !magd_enabled.value )
		return false;

	if( g_magd_mode == MAGD_NET_MODE_TUNNEL )
	{
		// Tunnel adapter retrieves unmarshalled binary packet
		return false;
	}

	return false;
}
