/*
MAGDOnline.cpp - MAGD Multiplayer Platform UI
*/

#include "Framework.h"
#include "Bitmap.h"
#include "Field.h"
#include "CheckBox.h"
#include "Table.h"
#include "YesNoMessageBox.h"
#include "keydefs.h"
#include "utlvector.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ART_BANNER_MAGD		"gfx/shell/head_inetgames"
#define MAGD_ROOM_CACHE		"magd_rooms.json"

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

		if( !( MAGD_IsLowerAlpha( c ) ||
			MAGD_IsUpperAlpha( c ) ||
			MAGD_IsDigit( c ) ||
			c == '_' ||
			c == '-' ))
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
		if( MAGD_IsLowerAlpha( code[i] ))
			code[i] = (char)( code[i] - 'a' + 'A' );
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

	while( ( p = strstr( p, needle )) != NULL )
	{
		const char *q = p + strlen( needle );

		while( *q == ' ' || *q == '\t' ||
			*q == '\r' || *q == '\n' )
		{
			++q;
		}

		if( *q != ':' )
		{
			p += strlen( needle );
			continue;
		}

		++q;

		while( *q == ' ' || *q == '\t' ||
			*q == '\r' || *q == '\n' )
		{
			++q;
		}

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
			case '"':  c = '"';  break;
			case '\\': c = '\\'; break;
			case '/':  c = '/';  break;
			case 'b':  c = '\b'; break;
			case 'f':  c = '\f'; break;
			case 'n':  c = '\n'; break;
			case 'r':  c = '\r'; break;
			case 't':  c = '\t'; break;
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

	if( !strncmp( p, "true", 4 ))
		return true;

	if( !strncmp( p, "false", 5 ))
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
		{
			++depth;
		}
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
/* Room model                                                                */
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

class CMenuMAGDBrowser;

class CMenuMAGDRoomModel : public CMenuBaseModel
{
public:
	explicit CMenuMAGDRoomModel( CMenuMAGDBrowser *owner ) :
		parent( owner )
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

		const char *json =
			reinterpret_cast<const char *>( data );

		const char *roomsArray =
			strstr( json, "\"rooms\"" );

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
			while( *p == ' ' ||
				*p == '\t' ||
				*p == '\r' ||
				*p == '\n' ||
				*p == ',' )
			{
				++p;
			}

			if( *p != '{' )
				break;

			const char *end =
				MAGD_FindObjectEnd( p );

			if( !end )
				break;

			magd_room_t room;
			memset( &room, 0, sizeof( room ));

			if( MAGD_JSONString(
					p,
					"code",
					room.code,
					sizeof( room.code )) &&
				MAGD_ValidRoomCode( room.code ))
			{
				MAGD_JSONString(
					p,
					"name",
					room.name,
					sizeof( room.name ));

				MAGD_JSONString(
					p,
					"map",
					room.map,
					sizeof( room.map ));

				MAGD_JSONString(
					p,
					"game",
					room.game,
					sizeof( room.game ));

				MAGD_JSONString(
					p,
					"hostName",
					room.hostName,
					sizeof( room.hostName ));

				room.playerCount =
					MAGD_JSONInt(
						p,
						"players",
						0 );

				room.maxPlayers =
					MAGD_JSONInt(
						p,
						"maxPlayers",
						0 );

				room.hasPassword =
					MAGD_JSONBool(
						p,
						"hasPassword",
						false );

				room.hostConnected =
					MAGD_JSONBool(
						p,
						"hostConnected",
						false );

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
					room.hostConnected ?
						"Online" :
						"Waiting"
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

	const char *GetCellText(
		int line,
		int column
	) override
	{
		if( !rooms.IsValidIndex( line ))
			return NULL;

		switch( column )
		{
		case 0:
			return rooms[line].hasPassword ?
				"gfx/shell/lock" :
				NULL;

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

	ECellType GetCellType(
		int line,
		int column
	) override
	{
		(void)line;

		if( column == 0 )
			return CELL_IMAGE_ADDITIVE;

		return CELL_TEXT;
	}

	unsigned int GetAlignmentForColumn(
		int column
	) const override
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

	bool Sort(
		int column,
		bool ascend
	) override
	{
		(void)column;
		(void)ascend;

		return false;
	}

	magd_room_t &GetRoom( int index )
	{
		return rooms[index];
	}

	CMenuMAGDBrowser *parent;
	CUtlVector<magd_room_t> rooms;
};

/* ------------------------------------------------------------------------- */
/* Main MAGD menu                                                            */
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
			EngFuncs::GetCvarString(
				"magd_connection_state" );

		const char *room =
			EngFuncs::GetCvarString(
				"magd_room_code" );

		const char *error =
			EngFuncs::GetCvarString(
				"magd_last_error" );

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

		MAGD_SetFieldStatus(
			status,
			text
		);

		bool connected =
			state &&
			strcmp(
				state,
				"disconnected" ) != 0 &&
			strcmp(
				state,
				"disabled" ) != 0;

		disconnect->SetGrayed(
			!connected
		);
	}

	void Disconnect()
	{
		EngFuncs::ClientCmd(
			false,
			"magd_disconnect\n"
		);
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
		int players =
			atoi( maxPlayers.GetBuffer() );

		if( !serverName.GetBuffer()[0] )
		{
			UI_ShowMessageBox(
				L( "Please enter a server name." )
			);

			return;
		}

		if( !hostName.GetBuffer()[0] )
		{
			UI_ShowMessageBox(
				L( "Please enter a host name." )
			);

			return;
		}

		if( !map.GetBuffer()[0] )
		{
			UI_ShowMessageBox(
				L( "Please enter a map name." )
			);

			return;
		}

		if( !game.GetBuffer()[0] )
		{
			UI_ShowMessageBox(
				L( "Please enter a game/mod name." )
			);

			return;
		}

		if( players < 2 || players > 32 )
		{
			UI_ShowMessageBox(
				L( "Maximum players must be between 2 and 32." )
			);

			return;
		}

		if( !EngFuncs::IsMapValid(
				map.GetBuffer() ))
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

		EngFuncs::ClientCmd(
			false,
			"magd_create_room\n"
		);

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
	void _VidInit() override;

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

		MAGD_NormalizeRoomCode(
			code,
			sizeof( code )
		);

		if( !MAGD_ValidRoomCode( code ))
		{
			UI_ShowMessageBox(
				L( "Please enter a valid MAGD room code." )
			);

			return;
		}

		roomCode.SetBuffer( code );

		roomCode.WriteCvar();
		password.WriteCvar();

		EngFuncs::ClientCmd(
			false,
			"magd_connect\n"
		);

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
			EngFuncs::GetCvarString(
				"magd_connection_state" );

		const char *error =
			EngFuncs::GetCvarString(
				"magd_last_error" );

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
		else
		{
			snprintf(
				text,
				sizeof( text ),
				"%s",
				state ? state : "disconnected"
			);
		}

		MAGD_SetFieldStatus(
			status,
			text
		);
	}

	void _Init() override;
	void _VidInit() override;

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

		EngFuncs::ClientCmd(
			false,
			"host_writeconfig\n"
		);

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
	void _VidInit() override;

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
		model( this ),
		lastRevision( -1 ),
		refreshUntil( 0 )
	{
		;
	}

	void RefreshRooms()
	{
		EngFuncs::ClientCmd(
			false,
			"magd_list_rooms\n"
		);

		refreshUntil =
			uiStatic.realTime + 5000;

		refresh->SetGrayed( true );
	}

	void JoinRow( int index )
	{
		if( !model.rooms.IsValidIndex( index ))
		{
			UI_ShowMessageBox(
				L( "Please select a MAGD room first." )
			);

			return;
		}

		UI_MAGDJoinRoom(
			model.GetRoom( index ).code
		);
	}

	void JoinSelected()
	{
		JoinRow(
			gameList.GetCurrentIndex()
		);
	}

	void Show() override
	{
		CMenuFramework::Show();

		model.Update();

		lastRevision =
			(int)EngFuncs::GetCvarFloat(
				"magd_room_list_revision"
			);

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
			gameList.GetCurrentIndex() <
				model.GetRows();

		join->SetGrayed( !valid );
	}

	void SyncStatus()
	{
		const char *state =
			EngFuncs::GetCvarString(
				"magd_room_list_state" );

		const char *error =
			EngFuncs::GetCvarString(
				"magd_room_list_error" );

		const char *connection =
			EngFuncs::GetCvarString(
				"magd_connection_state" );

		char text[256];

		if( state &&
			!strcmp( state, "loading" ))
		{
			snprintf(
				text,
				sizeof( text ),
				"Refreshing rooms... Connection: %s",
				connection ?
					connection :
					"disconnected"
			);
		}
		else if( state &&
			!strcmp( state, "error" ))
		{
			snprintf(
				text,
				sizeof( text ),
				"Room refresh failed: %s",
				error && error[0] ?
					error :
					"unknown error"
			);
		}
		else
		{
			snprintf(
				text,
				sizeof( text ),
				"%d rooms - Connection: %s",
				model.GetRows(),
				connection ?
					connection :
					"disconnected"
			);
		}

		MAGD_SetFieldStatus(
			status,
			text
		);

		SyncSelection();
	}

	void OnChanged()
	{
		SyncSelection();
	}

	void _Init() override;
	void _VidInit() override;

	CMenuTable gameList;
	CMenuMAGDRoomModel model;

	CMenuPicButton *join;
	CMenuPicButton *refresh;
	CMenuPicButton *joinByCode;

	CMenuField status;

	int lastRevision;
	int refreshUntil;
};

/* ------------------------------------------------------------------------- */
/* MAGD root menu init                                                       */
/* ------------------------------------------------------------------------- */

void CMenuMAGDOnline::_Init()
{
	banner.SetPicture(
		ART_BANNER_MAGD
	);

	status.szName = L( "Connection" );
	status.iFlags |= QMF_INACTIVE;
	status.SetCharSize( QM_SMALLFONT );
	status.SetRect(
		300,
		185,
		-20,
		42
	);

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
	);

	AddButton(
		L( "Join by Code" ),
		L( "Join a MAGD room using its room code" ),
		PC_JOIN_GAME,
		VoidCb( UI_MAGDJoin_Menu ),
		QMF_NOTIFY
	);

	AddButton(
		L( "Settings" ),
		L( "Configure the MAGD multiplayer platform" ),
		PC_OPTIONS,
		VoidCb( UI_MAGDSettings_Menu ),
		QMF_NOTIFY
	);

	disconnect = AddButton(
		L( "Disconnect" ),
		L( "Disconnect the current MAGD tunnel" ),
		PC_DISCONNECT,
		VoidCb( &CMenuMAGDOnline::Disconnect ),
		QMF_NOTIFY
	);

	AddButton(
		L( "Done" ),
		L( "Go back to the Multiplayer menu" ),
		PC_DONE,
		VoidCb( &CMenuMAGDOnline::Hide ),
		QMF_NOTIFY
	);
}

/* ------------------------------------------------------------------------- */
/* MAGD create menu init                                                     */
/* ------------------------------------------------------------------------- */

void CMenuMAGDCreate::_Init()
{
	banner.SetPicture(
		ART_BANNER_MAGD
	);

	AddItem( banner );

	serverName.szName = L( "Server Name" );
	serverName.iMaxLength = 63;
	serverName.bAllowColorstrings = false;

	hostName.szName = L( "Host Name" );
	hostName.iMaxLength = 63;
	hostName.bAllowColorstrings = false;

	map.szName = L( "Map" );
	map.iMaxLength = 63;
	map.bAllowColorstrings = false;

	game.szName = L( "Game / Mod" );
	game.iMaxLength = 31;
	game.bAllowColorstrings = false;

	maxPlayers.szName = L( "Max Players" );
	maxPlayers.iMaxLength = 2;
	maxPlayers.bNumbersOnly = true;

	password.szName = L( "Password" );
	password.iMaxLength = 63;
	password.bHideInput = true;
	password.bAllowColorstrings = false;

	serverName.LinkCvar(
		"magd_room_name"
	);

	hostName.LinkCvar(
		"magd_host_name"
	);

	map.LinkCvar(
		"magd_room_map"
	);

	game.LinkCvar(
		"magd_room_game"
	);

	maxPlayers.LinkCvar(
		"magd_max_players"
	);

	password.LinkCvar(
		"magd_room_password"
	);

	CMenuPicButton *create =
		AddButton(
			L( "Create Room" ),
			L( "Create this MAGD online room" ),
			PC_CREATE_GAME,
			VoidCb( &CMenuMAGDCreate::Create ),
			QMF_NOTIFY
		);

	create->onReleasedClActive =
		msgBox.MakeOpenEvent();

	msgBox.onPositive =
		VoidCb( &CMenuMAGDCreate::Create );

	msgBox.SetMessage(
		L( "Creating a MAGD room will replace the current online session. Continue?" )
	);

	msgBox.Link( this );
	msgBox.Init();

	AddButton(
		L( "Cancel" ),
		L( "Return to MAGD Online" ),
		PC_CANCEL,
		VoidCb( &CMenuMAGDCreate::Hide ),
		QMF_NOTIFY
	);

	AddItem( serverName );
	AddItem( hostName );
	AddItem( map );
	AddItem( game );
	AddItem( maxPlayers );
	AddItem( password );
}

void CMenuMAGDCreate::_VidInit()
{
	serverName.SetRect(
		350,
		230,
		330,
		32
	);

	hostName.SetRect(
		350,
		285,
		330,
		32
	);

	map.SetRect(
		350,
		340,
		330,
		32
	);

	game.SetRect(
		350,
		395,
		330,
		32
	);

	maxPlayers.SetRect(
		350,
		450,
		330,
		32
	);

	password.SetRect(
		350,
		505,
		330,
		32
	);
}

/* ------------------------------------------------------------------------- */
/* MAGD join menu init                                                       */
/* ------------------------------------------------------------------------- */

void CMenuMAGDJoin::_Init()
{
	banner.SetPicture(
		ART_BANNER_MAGD
	);

	AddItem( banner );

	roomCode.szName = L( "Room Code" );
	roomCode.iMaxLength = 63;
	roomCode.bAllowColorstrings = false;

	password.szName = L( "Password" );
	password.iMaxLength = 63;
	password.bHideInput = true;
	password.bAllowColorstrings = false;

	status.szName = L( "Status" );
	status.iFlags |= QMF_INACTIVE;
	status.SetCharSize( QM_SMALLFONT );

	roomCode.LinkCvar(
		"magd_room_code"
	);

	password.LinkCvar(
		"magd_room_password"
	);

	AddItem( roomCode );
	AddItem( password );
	AddItem( status );

	CMenuPicButton *join =
		AddButton(
			L( "Join Room" ),
			L( "Connect to this MAGD room" ),
			PC_JOIN_GAME,
			VoidCb( &CMenuMAGDJoin::Join ),
			QMF_NOTIFY
		);

	join->onReleasedClActive =
		msgBox.MakeOpenEvent();

	msgBox.onPositive =
		VoidCb( &CMenuMAGDJoin::Join );

	msgBox.SetMessage(
		L( "Joining a MAGD room will replace the current online session. Continue?" )
	);

	msgBox.Link( this );
	msgBox.Init();

	AddButton(
		L( "Cancel" ),
		L( "Return to MAGD Online" ),
		PC_CANCEL,
		VoidCb( &CMenuMAGDJoin::Hide ),
		QMF_NOTIFY
	);
}

void CMenuMAGDJoin::_VidInit()
{
	roomCode.SetRect(
		350,
		270,
		330,
		32
	);

	password.SetRect(
		350,
		340,
		330,
		32
	);

	status.SetRect(
		300,
		405,
		-20,
		40
	);
}

/* ------------------------------------------------------------------------- */
/* MAGD settings menu init                                                   */
/* ------------------------------------------------------------------------- */

void CMenuMAGDSettings::_Init()
{
	banner.SetPicture(
		ART_BANNER_MAGD
	);

	AddItem( banner );

	serverUrl.szName = L( "MAGD Server URL" );
	serverUrl.iMaxLength = 255;
	serverUrl.bAllowColorstrings = false;

	maxPlayers.szName = L( "Default Max Players" );
	maxPlayers.iMaxLength = 2;
	maxPlayers.bNumbersOnly = true;

	reconnect.szName = L( "Automatic Reconnect" );
	autoConnect.szName = L( "Auto Connect to Host" );
	autoStart.szName = L( "Auto Start Local Server" );
	insecureWs.szName = L( "Allow insecure ws:// (debug)" );

	serverUrl.LinkCvar(
		"magd_server_url"
	);

	maxPlayers.LinkCvar(
		"magd_max_players"
	);

	reconnect.LinkCvar(
		"magd_reconnect"
	);

	autoConnect.LinkCvar(
		"magd_auto_connect"
	);

	autoStart.LinkCvar(
		"magd_auto_start_server"
	);

	insecureWs.LinkCvar(
		"magd_allow_insecure_ws"
	);

	AddItem( serverUrl );
	AddItem( maxPlayers );
	AddItem( reconnect );
	AddItem( autoConnect );
	AddItem( autoStart );
	AddItem( insecureWs );

	AddButton(
		L( "Save" ),
		L( "Save MAGD settings" ),
		PC_OK,
		VoidCb( &CMenuMAGDSettings::SaveAndHide ),
		QMF_NOTIFY
	);

	AddButton(
		L( "Cancel" ),
		L( "Discard this menu" ),
		PC_CANCEL,
		VoidCb( &CMenuMAGDSettings::Hide ),
		QMF_NOTIFY
	);
}

void CMenuMAGDSettings::_VidInit()
{
	serverUrl.SetRect(
		350,
		225,
		450,
		32
	);

	maxPlayers.SetRect(
		350,
		285,
		200,
		32
	);

	reconnect.SetRect(
		350,
		355,
		450,
		32
	);

	autoConnect.SetRect(
		350,
		410,
		450,
		32
	);

	autoStart.SetRect(
		350,
		465,
		450,
		32
	);

	insecureWs.SetRect(
		350,
		520,
		450,
		32
	);
}

/* ------------------------------------------------------------------------- */
/* MAGD browser init                                                         */
/* ------------------------------------------------------------------------- */

void CMenuMAGDBrowser::_Init()
{
	banner.SetPicture(
		ART_BANNER_MAGD
	);

	AddItem( banner );

	status.szName = L( "Status" );
	status.iFlags |= QMF_INACTIVE;
	status.SetCharSize( QM_SMALLFONT );

	gameList.SetupColumn(
		0,
		"",
		0.06f
	);

	gameList.SetupColumn(
		1,
		L( "Name" ),
		0.27f
	);

	gameList.SetupColumn(
		2,
		L( "Map" ),
		0.17f
	);

	gameList.SetupColumn(
		3,
		L( "Game" ),
		0.13f
	);

	gameList.SetupColumn(
		4,
		L( "Players" ),
		0.11f
	);

	gameList.SetupColumn(
		5,
		L( "Host" ),
		0.16f
	);

	gameList.SetupColumn(
		6,
		L( "Status" ),
		0.10f
	);

	gameList.SetModel(
		&model
	);

	gameList.bFramedHintText = true;
	gameList.bAllowSorting = false;
	gameList.bShowScrollBar = true;

	gameList.onChanged =
		VoidCb(
			&CMenuMAGDBrowser::OnChanged
		);

	AddItem( gameList );
	AddItem( status );

	join = AddButton(
		L( "Join Selected" ),
		L( "Join the selected MAGD room" ),
		PC_JOIN_GAME,
		VoidCb( &CMenuMAGDBrowser::JoinSelected ),
		QMF_NOTIFY
	);

	refresh = AddButton(
		L( "Refresh" ),
		L( "Refresh the MAGD room list" ),
		PC_REFRESH,
		VoidCb( &CMenuMAGDBrowser::RefreshRooms ),
		QMF_NOTIFY
	);

	joinByCode = AddButton(
		L( "Join by Code" ),
		L( "Enter a MAGD room code manually" ),
		PC_CREATE_GAME,
		VoidCb( UI_MAGDJoin_Menu ),
		QMF_NOTIFY
	);

	AddButton(
		L( "Done" ),
		L( "Return to MAGD Online" ),
		PC_DONE,
		VoidCb( &CMenuMAGDBrowser::Hide ),
		QMF_NOTIFY
	);

	join->SetGrayed( true );
}

void CMenuMAGDBrowser::_VidInit()
{
	gameList.SetRect(
		360,
		230,
		-20,
		465
	);

	status.SetRect(
		300,
		705,
		-20,
		40
	);

	joinByCode->SetCoord(
		710,
		705
	);

	refresh->SetCoord(
		560,
		705
	);

	join->SetCoord(
		410,
		705
	);
}

/* ------------------------------------------------------------------------- */
/* Room model -> browser activation                                          */
/* ------------------------------------------------------------------------- */

void CMenuMAGDRoomModel::OnActivateEntry( int line )
{
	if( parent )
		parent->JoinRow( line );
}

/* ------------------------------------------------------------------------- */
/* Menu registrations                                                        */
/* ------------------------------------------------------------------------- */

ADD_MENU(
	menu_magd_online,
	CMenuMAGDOnline,
	UI_MAGDOnline_Menu
);

ADD_MENU(
	menu_magd_create,
	CMenuMAGDCreate,
	UI_MAGDCreate_Menu
);

ADD_MENU(
	menu_magd_browser,
	CMenuMAGDBrowser,
	UI_MAGDBrowser_Menu
);

ADD_MENU(
	menu_magd_join,
	CMenuMAGDJoin,
	UI_MAGDJoin_Menu
);

ADD_MENU(
	menu_magd_settings,
	CMenuMAGDSettings,
	UI_MAGDSettings_Menu
);

/* ------------------------------------------------------------------------- */
/* Cross-menu helpers                                                        */
/* ------------------------------------------------------------------------- */

static void UI_MAGDJoinRoom( const char *code )
{
	if( !menu_magd_join )
		return;

	menu_magd_join->Show();
	menu_magd_join->SetRoomCode( code );
}