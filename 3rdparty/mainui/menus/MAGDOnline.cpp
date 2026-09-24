#include "Framework.h"
#include "Bitmap.h"
#include "Field.h"
#include "CheckBox.h"
#include "Table.h"
#include "YesNoMessageBox.h"
#include "keydefs.h"
#include "utlvector.h"

#include <stdlib.h>
#include <string.h>

#define ART_BANNER_MAGD "gfx/shell/head_inetgames"
#define MAGD_ROOM_CACHE "magd_rooms.json"

static void UI_MAGDCreate_Menu( void );
static void UI_MAGDBrowser_Menu( void );
static void UI_MAGDJoin_Menu( void );
static void UI_MAGDSettings_Menu( void );
static void UI_MAGDJoinRoom( const char *code );

static void MAGD_SetFieldStatus( CMenuField &field, const char *text )
{
	field.SetBuffer( text ? text : "" );
}

static bool MAGD_IsLowerAlpha( char c )
{
	return c >= 'a' && c <= 'z';
}

static bool MAGD_IsUpperAlpha( char c )
{
	return c >= 'A' && c <= 'Z';
}

static bool MAGD_IsDigit( char c )
{
	return c >= '0' && c <= '9';
}

static bool MAGD_ValidRoomCode( const char *code )
{
	size_t len;

	if( !code )
		return false;

	len = strlen( code );

	if( len < 3 || len > 64 )
		return false;

	for( size_t i = 0; i < len; ++i )
	{
		char c = code[i];

		if( !(MAGD_IsLowerAlpha( c ) ||
			MAGD_IsUpperAlpha( c ) ||
			MAGD_IsDigit( c ) ||
			c == '_' ||
			c == '-' ) )
		{
			return false;
		}
	}

	return true;
}

static void MAGD_NormalizeRoomCode( char *code, size_t size )
{
	if( !code || !size )
		return;

	for( size_t i = 0; i + 1 < size && code[i]; ++i )
	{
		if( MAGD_IsLowerAlpha( code[i] ) )
			code[i] = (char)(code[i] - 'a' + 'A');
	}
}

/* ------------------------------------------------------------------------- */
/* Minimal JSON reader for the MAGD room cache                               */
/* ------------------------------------------------------------------------- */

static const char *MAGD_JSONValue( const char *object, const char *key )
{
	char needle[128];
	const char *p;

	if( !object || !key )
		return NULL;

	snprintf( needle, sizeof( needle ), "\"%s\"", key );

	p = object;

	while( (p = strstr( p, needle )) != NULL )
	{
		const char *q = p + strlen( needle );

		while( *q == ' ' || *q == '\t' || *q == '\r' || *q == '\n' )
			++q;

		if( *q != ':' )
		{
			p += strlen( needle );
			continue;
		}

		++q;

		while( *q == ' ' || *q == '\t' || *q == '\r' || *q == '\n' )
			++q;

		return q;
	}

	return NULL;
}

static bool MAGD_JSONString(
	const char *object,
	const char *key,
	char *out,
	size_t cap
)
{
	const char *p;
	size_t pos = 0;

	if( !out || cap < 2 )
		return false;

	out[0] = 0;

	p = MAGD_JSONValue( object, key );

	if( !p || *p != '"' )
		return false;

	++p;

	while( *p )
	{
		char c = *p++;

		if( c == '"' )
		{
			out[pos] = 0;
			return true;
		}

		if( c == '\\' )
		{
			char escaped = *p++;

			switch( escaped )
			{
			case '"': c = '"'; break;
			case '\\': c = '\\'; break;
			case '/': c = '/'; break;
			case 'b': c = '\b'; break;
			case 'f': c = '\f'; break;
			case 'n': c = '\n'; break;
			case 'r': c = '\r'; break;
			case 't': c = '\t'; break;
			default:
				c = escaped;
				break;
			}
		}

		if( pos + 1 >= cap )
			break;

		out[pos++] = c;
	}

	out[pos] = 0;
	return false;
}

static int MAGD_JSONInt(
	const char *object,
	const char *key,
	int fallback
)
{
	const char *p = MAGD_JSONValue( object, key );

	if( !p )
		return fallback;

	return atoi( p );
}

static bool MAGD_JSONBool(
	const char *object,
	const char *key,
	bool fallback
)
{
	const char *p = MAGD_JSONValue( object, key );

	if( !p )
		return fallback;

	if( !strncmp( p, "true", 4 ) )
		return true;

	if( !strncmp( p, "false", 5 ) )
		return false;

	return fallback;
}

static const char *MAGD_FindObjectEnd( const char *start )
{
	int depth = 0;
	bool inString = false;
	bool escaped = false;

	if( !start || *start != '{' )
		return NULL;

	for( const char *p = start; *p; ++p )
	{
		char c = *p;

		if( inString )
		{
			if( escaped )
			{
				escaped = false;
				continue;
			}

			if( c == '\\' )
			{
				escaped = true;
				continue;
			}

			if( c == '"' )
				inString = false;

			continue;
		}

		if( c == '"' )
		{
			inString = true;
			continue;
		}

		if( c == '{' )
			++depth;
		else if( c == '}' )
		{
			--depth;

			if( depth == 0 )
				return p + 1;
		}
	}

	return NULL;
}

/* ------------------------------------------------------------------------- */
/* MAGD room model                                                           */
/* ------------------------------------------------------------------------- */

struct magd_room_t
{
	char code[64];
	char name[64];
	char map[64];
	char game[32];
	char hostName[64];
	char players[32];
	char status[32];

	int playerCount;
	int maxPlayers;

	bool hasPassword;
	bool hostConnected;
};

class CMenuMAGDRoomModel : public CMenuBaseModel
{
public:
	CMenuMAGDRoomModel()
	{
		;
	}

	void Update() override
	{
		byte *data;
		int length = 0;

		rooms.RemoveAll();

		data = EngFuncs::COM_LoadFile( MAGD_ROOM_CACHE, &length );

		if( !data || length <= 0 )
		{
			if( data )
				EngFuncs::COM_FreeFile( data );

			return;
		}

		const char *json = reinterpret_cast<const char *>(data);
		const char *roomsArray = strstr( json, "\"rooms\"" );

		if( !roomsArray )
		{
			EngFuncs::COM_FreeFile( data );
			return;
		}

		roomsArray = strchr( roomsArray, '[' );

		if( !roomsArray )
		{
			EngFuncs::COM_FreeFile( data );
			return;
		}

		const char *p = roomsArray + 1;

		while( *p && *p != ']' )
		{
			while( *p == ' ' || *p == '\t' ||
				*p == '\r' || *p == '\n' || *p == ',' )
			{
				++p;
			}

			if( *p != '{' )
				break;

			const char *end = MAGD_FindObjectEnd( p );

			if( !end )
				break;

			magd_room_t room;
			memset( &room, 0, sizeof( room ) );

			if( MAGD_JSONString( p, "code", room.code, sizeof( room.code ) ) &&
				MAGD_ValidRoomCode( room.code ) )
			{
				MAGD_JSONString( p, "name", room.name, sizeof( room.name ) );
				MAGD_JSONString( p, "map", room.map, sizeof( room.map ) );
				MAGD_JSONString( p, "game", room.game, sizeof( room.game ) );
				MAGD_JSONString( p, "hostName", room.hostName, sizeof( room.hostName ) );

				room.playerCount =
					MAGD_JSONInt( p, "players", 0 );

				room.maxPlayers =
					MAGD_JSONInt( p, "maxPlayers", 0 );

				room.hasPassword =
					MAGD_JSONBool( p, "hasPassword", false );

				room.hostConnected =
					MAGD_JSONBool( p, "hostConnected", false );

				snprintf(
					room.players,
					sizeof( room.players ),
					"%d/%d",
					room.playerCount,
					room.maxPlayers
				);

				snprintf(
					room.status,
					sizeof( room.status ),
					"%s",
					room.hostConnected ? "Online" : "Waiting"
				);

				rooms.AddToTail( room );
			}

			p = end;
		}

		EngFuncs::COM_FreeFile( data );
	}

	int GetColumns() const override
	{
		return 7;
	}

	int GetRows() const override
	{
		return rooms.Count();
	}

	const char *GetCellText( int line, int column ) override
	{
		if( !rooms.IsValidIndex( line ) )
			return NULL;

		switch( column )
		{
		case 0:
			return rooms[line].hasPassword ? "gfx/shell/lock" : NULL;
		case 1:
			return rooms[line].name;
		case 2:
			return rooms[line].map;
		case 3:
			return rooms[line].game;
		case 4:
			return rooms[line].players;
		case 5:
			return rooms[line].hostName;
		case 6:
			return rooms[line].status;
		default:
			return NULL;
		}
	}

	ECellType GetCellType( int line, int column ) override
	{
		if( column == 0 )
			return CELL_IMAGE_ADDITIVE;

		return CELL_TEXT;
	}

	unsigned int GetAlignmentForColumn( int column ) const override
	{
		if( column == 0 ||
			column == 4 ||
			column == 6 )
		{
			return QM_CENTER;
		}

		return QM_LEFT;
	}

	void OnActivateEntry( int line ) override;

	bool Sort( int column, bool ascend ) override
	{
		(void)column;
		(void)ascend;
		return false;
	}

	magd_room_t &GetRoom( int index )
	{
		return rooms[index];
	}

	CUtlVector<magd_room_t> rooms;
};

/* ------------------------------------------------------------------------- */
/* MAGD root menu                                                            */
/* ------------------------------------------------------------------------- */

class CMenuMAGDOnline : public CMenuFramework
{
public:
	CMenuMAGDOnline() :
		CMenuFramework( "CMenuMAGDOnline" )
	{
		;
	}

	void Show() override
	{
		CMenuFramework::Show();
		UpdateStatus();
	}

	void Draw() override
	{
		UpdateStatus();
		CMenuFramework::Draw();
	}

private:
	void UpdateStatus()
	{
		const char *state =
			EngFuncs::GetCvarString( "magd_connection_state" );

		const char *room =
			EngFuncs::GetCvarString( "magd_room_code" );

		const char *error =
			EngFuncs::GetCvarString( "magd_last_error" );

		char text[256];

		if( error && error[0] )
		{
			snprintf(
				text,
				sizeof( text ),
				"%s - %s",
				state ? state : "disconnected",
				error
			);
		}
		else if( room && room[0] )
		{
			snprintf(
				text,
				sizeof( text ),
				"%s - %s",
				state ? state : "disconnected",
				room
			);
		}
		else
		{
			snprintf(
				text,
				sizeof( text ),
				"%s",
				state ? state : "disconnected"
			);
		}

		MAGD_SetFieldStatus( status, text );

		bool connected =
			state &&
			strcmp( state, "disconnected" ) != 0 &&
			strcmp( state, "disabled" ) != 0;

		disconnect->SetGrayed( !connected );
	}

	void Disconnect()
	{
		EngFuncs::ClientCmd( false, "magd_disconnect\n" );
	}

	void _Init() override;

	CMenuField status;
	CMenuPicButton *disconnect;
};

/* ------------------------------------------------------------------------- */
/* Create room                                                               */
/* ------------------------------------------------------------------------- */

class CMenuMAGDCreate : public CMenuFramework
{
public:
	CMenuMAGDCreate() :
		CMenuFramework( "CMenuMAGDCreate" )
	{
		;
	}

	void Create()
	{
		int players = atoi( maxPlayers.GetBuffer() );

		if( !serverName.GetBuffer()[0] )
		{
			UI_ShowMessageBox( L( "Please enter a server name." ) );
			return;
		}

		if( !hostName.GetBuffer()[0] )
		{
			UI_ShowMessageBox( L( "Please enter a host name." ) );
			return;
		}

		if( !map.GetBuffer()[0] )
		{
			UI_ShowMessageBox( L( "Please enter a map name." ) );
			return;
		}

		if( !game.GetBuffer()[0] )
		{
			UI_ShowMessageBox( L( "Please enter a game/mod name." ) );
			return;
		}

		if( players < 2 || players > 32 )
		{
			UI_ShowMessageBox(
				L( "Maximum players must be between 2 and 32." )
			);
			return;
		}

		if( !EngFuncs::IsMapValid( map.GetBuffer() ) )
		{
			UI_ShowMessageBox(
				L( "The selected map is not available in this game." )
			);
			return;
		}

		serverName.WriteCvar();
		hostName.WriteCvar();
		map.WriteCvar();
		game.WriteCvar();
		maxPlayers.WriteCvar();
		password.WriteCvar();

		EngFuncs::ClientCmd( false, "magd_create_room\n" );

		Hide();
	}

	void Show() override
	{
		CMenuFramework::Show();

		serverName.UpdateCvar( true );
		hostName.UpdateCvar( true );
		map.UpdateCvar( true );
		game.UpdateCvar( true );
		maxPlayers.UpdateCvar( true );
		password.UpdateCvar( true );
	}

private:
	void _Init() override;

	CMenuField serverName;
	CMenuField hostName;
	CMenuField map;
	CMenuField game;
	CMenuField maxPlayers;
	CMenuField password;

	CMenuYesNoMessageBox msgBox;
};

/* ------------------------------------------------------------------------- */
/* Join room                                                                 */
/* ------------------------------------------------------------------------- */

class CMenuMAGDJoin : public CMenuFramework
{
public:
	CMenuMAGDJoin() :
		CMenuFramework( "CMenuMAGDJoin" )
	{
		;
	}

	void Join()
	{
		char code[128];

		Q_strncpy(
			code,
			roomCode.GetBuffer(),
			sizeof( code )
		);

		MAGD_NormalizeRoomCode( code, sizeof( code ) );

		if( !MAGD_ValidRoomCode( code ) )
		{
			UI_ShowMessageBox(
				L( "Please enter a valid MAGD room code." )
			);
			return;
		}

		roomCode.SetBuffer( code );

		roomCode.WriteCvar();
		password.WriteCvar();

		EngFuncs::ClientCmd( false, "magd_connect\n" );

		Hide();
	}

	void SetRoomCode( const char *code )
	{
		if( !code )
			return;

		roomCode.SetBuffer( code );
		password.SetBuffer( "" );
	}

	void Show() override
	{
		CMenuFramework::Show();

		if( !roomCode.GetBuffer()[0] )
			roomCode.UpdateCvar( true );

		password.UpdateCvar( true );
	}

private:
	void _Init() override;

	CMenuField roomCode;
	CMenuField password;
	CMenuField status;

	CMenuYesNoMessageBox msgBox;
};

/* ------------------------------------------------------------------------- */
/* Settings                                                                  */
/* ------------------------------------------------------------------------- */

class CMenuMAGDSettings : public CMenuFramework
{
public:
	CMenuMAGDSettings() :
		CMenuFramework( "CMenuMAGDSettings" )
	{
		;
	}

	void SaveAndHide()
	{
		serverUrl.WriteCvar();
		maxPlayers.WriteCvar();
		reconnect.WriteCvar();
		autoConnect.WriteCvar();
		autoStart.WriteCvar();
		insecureWs.WriteCvar();

		EngFuncs::ClientCmd( false, "host_writeconfig\n" );

		Hide();
	}

	void Show() override
	{
		CMenuFramework::Show();

		serverUrl.UpdateCvar( true );
		maxPlayers.UpdateCvar( true );
		reconnect.UpdateCvar( true );
		autoConnect.UpdateCvar( true );
		autoStart.UpdateCvar( true );
		insecureWs.UpdateCvar( true );
	}

private:
	void _Init() override;

	CMenuField serverUrl;
	CMenuField maxPlayers;

	CMenuCheckBox reconnect;
	CMenuCheckBox autoConnect;
	CMenuCheckBox autoStart;
	CMenuCheckBox insecureWs;
};

/* ------------------------------------------------------------------------- */
/* Browser                                                                   */
/* ------------------------------------------------------------------------- */

class CMenuMAGDBrowser : public CMenuFramework
{
public:
	CMenuMAGDBrowser() :
		CMenuFramework( "CMenuMAGDBrowser" ),
		model(),
		lastRevision( -1 ),
		refreshUntil( 0 )
	{
		;
	}

	void RefreshRooms()
	{
		EngFuncs::ClientCmd( false, "magd_list_rooms\n" );
		refreshUntil = uiStatic.realTime + 5000;
		refresh->SetGrayed( true );
	}

	void JoinRow( int index )
	{
		if( !model.rooms.IsValidIndex( index ) )
		{
			UI_ShowMessageBox( L( "Please select a MAGD room first." ) );
			return;
		}

		UI_MAGDJoinRoom( model.GetRoom( index ).code );
	}

	void JoinSelected()
	{
		JoinRow( gameList.GetCurrentIndex() );
	}

	void Show() override
	{
		CMenuFramework::Show();

		model.Update();
		lastRevision =
			(int)EngFuncs::GetCvarFloat( "magd_room_list_revision" );

		SyncSelection();
		RefreshRooms();
	}

	void Draw() override
	{
		SyncRooms();
		SyncStatus();

		CMenuFramework::Draw();
	}

private:
	void SyncRooms()
	{
		int revision =
			(int)EngFuncs::GetCvarFloat(
				"magd_room_list_revision"
			);

		if( revision == lastRevision )
		{
			if( uiStatic.realTime >= refreshUntil )
				refresh->SetGrayed( false );

			return;
		}

		lastRevision = revision;

		model.Update();
		refresh->SetGrayed( false );

		SyncSelection();
	}

	void SyncSelection()
	{
		bool valid =
			model.GetRows() > 0 &&
			gameList.GetCurrentIndex() >= 0 &&
			gameList.GetCurrentIndex() < model.GetRows();

		join->SetGrayed( !valid );
	}

	void SyncStatus()
	{
		const char *state =
			EngFuncs::GetCvarString( "magd_room_list_state" );

		const char *error =
			EngFuncs::GetCvarString( "magd_room_list_error" );

		const char *connection =
			EngFuncs::GetCvarString(
				"magd_connection_state"
			);

		char text[256];

		if( state && !strcmp( state, "loading" ) )
		{
			snprintf(
				text,
				sizeof( text ),
				"Refreshing rooms... Connection: %s",
				connection ? connection : "disconnected"
			);
		}
		else if( state && !strcmp( state, "error" ) )
		{
			snprintf(
				text,
				sizeof( text ),
				"Room refresh failed: %s",
				error && error[0] ? error : "unknown error"
			);
		}
		else
		{
			snprintf(
				text,
				sizeof( text ),
				"%d rooms - Connection: %s",
				model.GetRows(),
				connection ? connection : "disconnected"
			);
		}

		MAGD_SetFieldStatus( status, text );

		SyncSelection();
	}

	void OnChanged()
	{
		SyncSelection();
	}

	void _Init() override;

	CMenuTable gameList;
	CMenuMAGDRoomModel model;

	CMenuPicButton *join;
	CMenuPicButton *refresh;

	CMenuField status;

	int lastRevision;
	int refreshUntil;
};

/* ------------------------------------------------------------------------- */
/* Menu init                                                                 */
/* ------------------------------------------------------------------------- */

void CMenuMAGDOnline::_Init()
{
	banner.SetPicture( ART_BANNER_MAGD );

	status.szName = L( "Connection" );
	status.iFlags |= QMF_INACTIVE;
	status.SetCharSize( QM_SMALLFONT );
	status.SetRect( 300, 185, -20, 42 );

	AddItem( banner );
	AddItem( status );

	AddButton(
		L( "Create Room" ),
		L( "Create an online MAGD room and host the selected map" ),
		PC_CREATE_GAME,
		VoidCb( UI_MAGDCreate_Menu ),
		QMF_NOTIFY
	);

	AddButton(
		L( "Browse Rooms" ),
		L( "Browse active MAGD online rooms" ),
		PC_REFRESH,
		VoidCb( UI_MAGDBrowser_Menu ),
		QMF_NOTIFY