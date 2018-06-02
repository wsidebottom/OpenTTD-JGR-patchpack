/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file console_cmds.cpp Implementation of the console hooks. */

#include "stdafx.h"
#include "console_internal.h"
#include "debug.h"
#include "engine_func.h"
#include "train.h"
#include "landscape.h"
#include "saveload/saveload.h"
#include "network/network.h"
#include "network/network_func.h"
#include "network/network_base.h"
#include "network/network_admin.h"
#include "network/network_client.h"
#include "command_func.h"
#include "settings_func.h"
#include "fios.h"
#include "fileio_func.h"
#include "group.h"
#include "core/random_func.hpp"
#include "screenshot.h"
#include "genworld.h"
#include "strings_func.h"
#include "viewport_func.h"
#include "window_func.h"
#include "cargotype.h"
#include "date_func.h"
#include "vehicle_base.h"
#include "vehicle_func.h"
#include "vehicle_gui.h"
#include "vehiclelist.h"
#include "string_func.h"
#include "company_func.h"
#include "gamelog.h"
#include "town.h"
#include "industry.h"
#include "ai/ai.hpp"
#include "ai/ai_config.hpp"
#include "newgrf.h"
#include "console_func.h"
#include "engine_base.h"
#include "game/game.hpp"
#include "table/strings.h"
#include "aircraft.h"
#include "airport.h"
#include "station_base.h"

#include "safeguards.h"

#ifdef UNIX
#include <stdarg.h>
#include <ctype.h>
#define stricmp strcasecmp
#endif

/* scriptfile handling */
static bool _script_running; ///< Script is running (used to abort execution when #ConReturn is encountered).

/** File list storage for the console, for caching the last 'ls' command. */
class ConsoleFileList : public FileList {
public:
	ConsoleFileList() : FileList()
	{
		this->file_list_valid = false;
	}

	/** Declare the file storage cache as being invalid, also clears all stored files. */
	void InvalidateFileList()
	{
		this->Clear();
		this->file_list_valid = false;
	}

	/**
	 * (Re-)validate the file storage cache. Only makes a change if the storage was invalid, or if \a force_reload.
	 * @param Always reload the file storage cache.
	 */
	void ValidateFileList(bool force_reload = false)
	{
		if (force_reload || !this->file_list_valid) {
			this->BuildFileList(FT_SAVEGAME, SLO_LOAD);
			this->file_list_valid = true;
		}
	}

	bool file_list_valid; ///< If set, the file list is valid.
};

static ConsoleFileList _console_file_list; ///< File storage cache for the console.

/* console command defines */
#define DEF_CONSOLE_CMD(function) static bool function(byte argc, char *argv[])
#define DEF_CONSOLE_HOOK(function) static ConsoleHookResult function(bool echo)

/****************
 * command hooks
 ****************/

#ifdef ENABLE_NETWORK

/**
 * Check network availability and inform in console about failure of detection.
 * @return Network availability.
 */
static inline bool NetworkAvailable(bool echo)
{
	if (!_network_available) {
		if (echo) IConsoleError("You cannot use this command because there is no network available.");
		return false;
	}
	return true;
}

/**
 * Check whether we are a server.
 * @return Are we a server? True when yes, false otherwise.
 */
DEF_CONSOLE_HOOK(ConHookServerOnly)
{
	if (!NetworkAvailable(echo)) return CHR_DISALLOW;

	if (!_network_server) {
		if (echo) IConsoleError("This command is only available to a network server.");
		return CHR_DISALLOW;
	}
	return CHR_ALLOW;
}

/**
 * Check whether we are a client in a network game.
 * @return Are we a client in a network game? True when yes, false otherwise.
 */
DEF_CONSOLE_HOOK(ConHookClientOnly)
{
	if (!NetworkAvailable(echo)) return CHR_DISALLOW;

	if (_network_server) {
		if (echo) IConsoleError("This command is not available to a network server.");
		return CHR_DISALLOW;
	}
	return CHR_ALLOW;
}

/**
 * Check whether we are in a multiplayer game.
 * @return True when we are client or server in a network game.
 */
DEF_CONSOLE_HOOK(ConHookNeedNetwork)
{
	if (!NetworkAvailable(echo)) return CHR_DISALLOW;

	if (!_networking || (!_network_server && !MyClient::IsConnected())) {
		if (echo) IConsoleError("Not connected. This command is only available in multiplayer.");
		return CHR_DISALLOW;
	}
	return CHR_ALLOW;
}

/**
 * Check whether we are in single player mode.
 * @return True when no network is active.
 */
DEF_CONSOLE_HOOK(ConHookNoNetwork)
{
	if (_networking) {
		if (echo) IConsoleError("This command is forbidden in multiplayer.");
		return CHR_DISALLOW;
	}
	return CHR_ALLOW;
}

#else
#	define ConHookNoNetwork NULL
#endif /* ENABLE_NETWORK */

DEF_CONSOLE_HOOK(ConHookNewGRFDeveloperTool)
{
	if (_settings_client.gui.newgrf_developer_tools) {
		if (_game_mode == GM_MENU) {
			if (echo) IConsoleError("This command is only available in game and editor.");
			return CHR_DISALLOW;
		}
#ifdef ENABLE_NETWORK
		return ConHookNoNetwork(echo);
#else
		return CHR_ALLOW;
#endif
	}
	return CHR_HIDE;
}

/**
 * Show help for the console.
 * @param str String to print in the console.
 */
static void IConsoleHelp(const char *str)
{
	IConsolePrintF(CC_WARNING, "- %s", str);
}

/** Print string as command help in console, using printf-like formatting */
void CDECL IConsoleHelpF(const char *s, ...)
{
	va_list va;
	char buf[ICON_MAX_STREAMSIZE];

	va_start(va, s);
	//vsnprintf(buf, sizeof(buf), s, va);
	vseprintf(buf, lastof(buf), s, va);
	va_end(va);

	IConsoleHelp(buf);
}

/**
 * Reset status of all engines.
 * @return Will always succeed.
 */
DEF_CONSOLE_CMD(ConResetEngines)
{
	if (argc == 0) {
		IConsoleHelp("Reset status data of all engines. This might solve some issues with 'lost' engines. Usage: 'resetengines'");
		return true;
	}

	StartupEngines();
	return true;
}

/**
 * Reset status of the engine pool.
 * @return Will always return true.
 * @note Resetting the pool only succeeds when there are no vehicles ingame.
 */
DEF_CONSOLE_CMD(ConResetEnginePool)
{
	if (argc == 0) {
		IConsoleHelp("Reset NewGRF allocations of engine slots. This will remove invalid engine definitions, and might make default engines available again.");
		return true;
	}

	if (_game_mode == GM_MENU) {
		IConsoleError("This command is only available in game and editor.");
		return true;
	}

	if (!EngineOverrideManager::ResetToCurrentNewGRFConfig()) {
		IConsoleError("This can only be done when there are no vehicles in the game.");
		return true;
	}

	return true;
}

/**
 * Reset a tile to bare land in debug mode.
 * param tile number.
 * @return True when the tile is reset or the help on usage was printed (0 or two parameters).
 */
DEF_CONSOLE_CMD(ConResetTile)
{
	if (argc == 0) {
		IConsoleHelp("Reset a tile to bare land. Usage: 'resettile <tile>'");
		IConsoleHelp("Tile can be either decimal (34161) or hexadecimal (0x4a5B)");
		return true;
	}

	if (argc == 2) {
		uint32 result;
		if (GetArgumentInteger(&result, argv[1])) {
			DoClearSquare((TileIndex)result);
			return true;
		}
	}

	return false;
}

/**
 * Scroll to a tile on the map.
 * @param arg1 tile tile number or tile x coordinate.
 * @param arg2 optionally tile y coordinate.
 * @note When only one argument is given it is intepreted as the tile number.
 *       When two arguments are given, they are interpreted as the tile's x
 *       and y coordinates.
 * @return True when either console help was shown or a proper amount of parameters given.
 */
DEF_CONSOLE_CMD(ConScrollToTile)
{
	switch (argc) {
		case 0:
			IConsoleHelp("Center the screen on a given tile.");
			IConsoleHelp("Usage: 'scrollto <tile>' or 'scrollto <x> <y>'");
			IConsoleHelp("Numbers can be either decimal (34161) or hexadecimal (0x4a5B).");
			return true;

		case 2: {
			uint32 result;
			if (GetArgumentInteger(&result, argv[1])) {
				if (result >= MapSize()) {
					IConsolePrint(CC_ERROR, "Tile does not exist");
					return true;
				}
				ScrollMainWindowToTile((TileIndex)result);
				return true;
			}
			break;
		}

		case 3: {
			uint32 x, y;
			if (GetArgumentInteger(&x, argv[1]) && GetArgumentInteger(&y, argv[2])) {
				if (x >= MapSizeX() || y >= MapSizeY()) {
					IConsolePrint(CC_ERROR, "Tile does not exist");
					return true;
				}
				ScrollMainWindowToTile(TileXY(x, y));
				return true;
			}
			break;
		}
	}

	return false;
}

/**
 * Save the map to a file.
 * @param filename the filename to save the map to.
 * @return True when help was displayed or the file attempted to be saved.
 */
DEF_CONSOLE_CMD(ConSave)
{
	if (argc == 0) {
		IConsoleHelp("Save the current game. Usage: 'save <filename>'");
		return true;
	}

	if (argc == 2) {
		char *filename = str_fmt("%s.sav", argv[1]);
		IConsolePrint(CC_DEFAULT, "Saving map...");

		if (SaveOrLoad(filename, SLO_SAVE, DFT_GAME_FILE, SAVE_DIR) != SL_OK) {
			IConsolePrint(CC_ERROR, "Saving map failed");
		} else {
			IConsolePrintF(CC_DEFAULT, "Map successfully saved to %s", filename);
		}
		free(filename);
		return true;
	}

	return false;
}

/**
 * Explicitly save the configuration.
 * @return True.
 */
DEF_CONSOLE_CMD(ConSaveConfig)
{
	if (argc == 0) {
		IConsoleHelp("Saves the configuration for new games to the configuration file, typically 'openttd.cfg'.");
		IConsoleHelp("It does not save the configuration of the current game to the configuration file.");
		return true;
	}

	SaveToConfig();
	IConsolePrint(CC_DEFAULT, "Saved config.");
	return true;
}

DEF_CONSOLE_CMD(ConLoad)
{
	if (argc == 0) {
		IConsoleHelp("Load a game by name or index. Usage: 'load <file | number>'");
		return true;
	}

	if (argc != 2) return false;

	const char *file = argv[1];
	_console_file_list.ValidateFileList();
	const FiosItem *item = _console_file_list.FindItem(file);
	if (item != NULL) {
		if (GetAbstractFileType(item->type) == FT_SAVEGAME) {
			_switch_mode = SM_LOAD_GAME;
			_file_to_saveload.SetMode(item->type);
			_file_to_saveload.SetName(FiosBrowseTo(item));
			_file_to_saveload.SetTitle(item->title);
		} else {
			IConsolePrintF(CC_ERROR, "%s: Not a savegame.", file);
		}
	} else {
		IConsolePrintF(CC_ERROR, "%s: No such file or directory.", file);
	}

	return true;
}


DEF_CONSOLE_CMD(ConRemove)
{
	if (argc == 0) {
		IConsoleHelp("Remove a savegame by name or index. Usage: 'rm <file | number>'");
		return true;
	}

	if (argc != 2) return false;

	const char *file = argv[1];
	_console_file_list.ValidateFileList();
	const FiosItem *item = _console_file_list.FindItem(file);
	if (item != NULL) {
		if (!FiosDelete(item->name)) {
			IConsolePrintF(CC_ERROR, "%s: Failed to delete file", file);
		}
	} else {
		IConsolePrintF(CC_ERROR, "%s: No such file or directory.", file);
	}

	_console_file_list.InvalidateFileList();
	return true;
}


/* List all the files in the current dir via console */
DEF_CONSOLE_CMD(ConListFiles)
{
	if (argc == 0) {
		IConsoleHelp("List all loadable savegames and directories in the current dir via console. Usage: 'ls | dir'");
		return true;
	}

	_console_file_list.ValidateFileList(true);
	for (uint i = 0; i < _console_file_list.Length(); i++) {
		IConsolePrintF(CC_DEFAULT, "%d) %s", i, _console_file_list[i].title);
	}

	return true;
}

/* Open the cheat window. */
DEF_CONSOLE_CMD(ConOpenCheats)
{
	if (argc == 0) {
		IConsoleHelp("Open the cheat window. Usage: 'open_cheats'");
		return true;
	}

	extern void ShowCheatWindow();
	ShowCheatWindow();

	return true;
}

/* Change the dir via console */
DEF_CONSOLE_CMD(ConChangeDirectory)
{
	if (argc == 0) {
		IConsoleHelp("Change the dir via console. Usage: 'cd <directory | number>'");
		return true;
	}

	if (argc != 2) return false;

	const char *file = argv[1];
	_console_file_list.ValidateFileList(true);
	const FiosItem *item = _console_file_list.FindItem(file);
	if (item != NULL) {
		switch (item->type) {
			case FIOS_TYPE_DIR: case FIOS_TYPE_DRIVE: case FIOS_TYPE_PARENT:
				FiosBrowseTo(item);
				break;
			default: IConsolePrintF(CC_ERROR, "%s: Not a directory.", file);
		}
	} else {
		IConsolePrintF(CC_ERROR, "%s: No such file or directory.", file);
	}

	_console_file_list.InvalidateFileList();
	return true;
}

DEF_CONSOLE_CMD(ConPrintWorkingDirectory)
{
	const char *path;

	if (argc == 0) {
		IConsoleHelp("Print out the current working directory. Usage: 'pwd'");
		return true;
	}

	/* XXX - Workaround for broken file handling */
	_console_file_list.ValidateFileList(true);
	_console_file_list.InvalidateFileList();

	FiosGetDescText(&path, NULL);
	IConsolePrint(CC_DEFAULT, path);
	return true;
}

DEF_CONSOLE_CMD(ConClearBuffer)
{
	if (argc == 0) {
		IConsoleHelp("Clear the console buffer. Usage: 'clear'");
		return true;
	}

	IConsoleClearBuffer();
	SetWindowDirty(WC_CONSOLE, 0);
	return true;
}


/**********************************
 * Network Core Console Commands
 **********************************/
#ifdef ENABLE_NETWORK

static bool ConKickOrBan(const char *argv, bool ban)
{
	uint n;

	if (strchr(argv, '.') == NULL && strchr(argv, ':') == NULL) { // banning with ID
		ClientID client_id = (ClientID)atoi(argv);

		/* Don't kill the server, or the client doing the rcon. The latter can't be kicked because
		 * kicking frees closes and subsequently free the connection related instances, which we
		 * would be reading from and writing to after returning. So we would read or write data
		 * from freed memory up till the segfault triggers. */
		if (client_id == CLIENT_ID_SERVER || client_id == _redirect_console_to_client) {
			IConsolePrintF(CC_ERROR, "ERROR: Silly boy, you can not %s yourself!", ban ? "ban" : "kick");
			return true;
		}

		NetworkClientInfo *ci = NetworkClientInfo::GetByClientID(client_id);
		if (ci == NULL) {
			IConsoleError("Invalid client");
			return true;
		}

		if (!ban) {
			/* Kick only this client, not all clients with that IP */
			NetworkServerKickClient(client_id);
			return true;
		}

		/* When banning, kick+ban all clients with that IP */
		n = NetworkServerKickOrBanIP(client_id, ban);
	} else {
		n = NetworkServerKickOrBanIP(argv, ban);
	}

	if (n == 0) {
		IConsolePrint(CC_DEFAULT, ban ? "Client not online, address added to banlist" : "Client not found");
	} else {
		IConsolePrintF(CC_DEFAULT, "%sed %u client(s)", ban ? "Bann" : "Kick", n);
	}

	return true;
}

DEF_CONSOLE_CMD(ConKick)
{
	if (argc == 0) {
		IConsoleHelp("Kick a client from a network game. Usage: 'kick <ip | client-id>'");
		IConsoleHelp("For client-id's, see the command 'clients'");
		return true;
	}

	if (argc != 2) return false;

	return ConKickOrBan(argv[1], false);
}

DEF_CONSOLE_CMD(ConBan)
{
	if (argc == 0) {
		IConsoleHelp("Ban a client from a network game. Usage: 'ban <ip | client-id>'");
		IConsoleHelp("For client-id's, see the command 'clients'");
		IConsoleHelp("If the client is no longer online, you can still ban his/her IP");
		return true;
	}

	if (argc != 2) return false;

	return ConKickOrBan(argv[1], true);
}

DEF_CONSOLE_CMD(ConUnBan)
{
	if (argc == 0) {
		IConsoleHelp("Unban a client from a network game. Usage: 'unban <ip | banlist-index>'");
		IConsoleHelp("For a list of banned IP's, see the command 'banlist'");
		return true;
	}

	if (argc != 2) return false;

	/* Try by IP. */
	uint index;
	for (index = 0; index < _network_ban_list.Length(); index++) {
		if (strcmp(_network_ban_list[index], argv[1]) == 0) break;
	}

	/* Try by index. */
	if (index >= _network_ban_list.Length()) {
		index = atoi(argv[1]) - 1U; // let it wrap
	}

	if (index < _network_ban_list.Length()) {
		char msg[64];
		seprintf(msg, lastof(msg), "Unbanned %s", _network_ban_list[index]);
		IConsolePrint(CC_DEFAULT, msg);
		free(_network_ban_list[index]);
		_network_ban_list.Erase(_network_ban_list.Get(index));
	} else {
		IConsolePrint(CC_DEFAULT, "Invalid list index or IP not in ban-list.");
		IConsolePrint(CC_DEFAULT, "For a list of banned IP's, see the command 'banlist'");
	}

	return true;
}

DEF_CONSOLE_CMD(ConBanList)
{
	if (argc == 0) {
		IConsoleHelp("List the IP's of banned clients: Usage 'banlist'");
		return true;
	}

	IConsolePrint(CC_DEFAULT, "Banlist: ");

	uint i = 1;
	for (char **iter = _network_ban_list.Begin(); iter != _network_ban_list.End(); iter++, i++) IConsolePrintF(CC_DEFAULT, "  %d) %s", i, *iter);

	return true;
}

DEF_CONSOLE_CMD(ConPauseGame)
{
	if (argc == 0) {
		IConsoleHelp("Pause a network game. Usage: 'pause'");
		return true;
	}

	if ((_pause_mode & PM_PAUSED_NORMAL) == PM_UNPAUSED) {
		DoCommandP(0, PM_PAUSED_NORMAL, 1, CMD_PAUSE);
		if (!_networking) IConsolePrint(CC_DEFAULT, "Game paused.");
	} else {
		IConsolePrint(CC_DEFAULT, "Game is already paused.");
	}

	return true;
}

DEF_CONSOLE_CMD(ConUnpauseGame)
{
	if (argc == 0) {
		IConsoleHelp("Unpause a network game. Usage: 'unpause'");
		return true;
	}

	if ((_pause_mode & PM_PAUSED_NORMAL) != PM_UNPAUSED) {
		DoCommandP(0, PM_PAUSED_NORMAL, 0, CMD_PAUSE);
		if (!_networking) IConsolePrint(CC_DEFAULT, "Game unpaused.");
	} else if ((_pause_mode & PM_PAUSED_ERROR) != PM_UNPAUSED) {
		IConsolePrint(CC_DEFAULT, "Game is in error state and cannot be unpaused via console.");
	} else if (_pause_mode != PM_UNPAUSED) {
		IConsolePrint(CC_DEFAULT, "Game cannot be unpaused manually; disable pause_on_join/min_active_clients.");
	} else {
		IConsolePrint(CC_DEFAULT, "Game is already unpaused.");
	}

	return true;
}

DEF_CONSOLE_CMD(ConRcon)
{
	if (argc == 0) {
		IConsoleHelp("Remote control the server from another client. Usage: 'rcon <password> <command>'");
		IConsoleHelp("Remember to enclose the command in quotes, otherwise only the first parameter is sent");
		return true;
	}

	if (argc < 3) return false;

	if (_network_server) {
		IConsoleCmdExec(argv[2]);
	} else {
		NetworkClientSendRcon(argv[1], argv[2]);
	}
	return true;
}

DEF_CONSOLE_CMD(ConStatus)
{
	if (argc == 0) {
		IConsoleHelp("List the status of all clients connected to the server. Usage 'status'");
		return true;
	}

	NetworkServerShowStatusToConsole();
	return true;
}

DEF_CONSOLE_CMD(ConServerInfo)
{
	if (argc == 0) {
		IConsoleHelp("List current and maximum client/company limits. Usage 'server_info'");
		IConsoleHelp("You can change these values by modifying settings 'network.max_clients', 'network.max_companies' and 'network.max_spectators'");
		return true;
	}

	IConsolePrintF(CC_DEFAULT, "Current/maximum clients:    %2d/%2d", _network_game_info.clients_on, _settings_client.network.max_clients);
	IConsolePrintF(CC_DEFAULT, "Current/maximum companies:  %2d/%2d", (int)Company::GetNumItems(), _settings_client.network.max_companies);
	IConsolePrintF(CC_DEFAULT, "Current/maximum spectators: %2d/%2d", NetworkSpectatorCount(), _settings_client.network.max_spectators);

	return true;
}

DEF_CONSOLE_CMD(ConClientNickChange)
{
	if (argc != 3) {
		IConsoleHelp("Change the nickname of a connected client. Usage: 'client_name <client-id> <new-name>'");
		IConsoleHelp("For client-id's, see the command 'clients'");
		return true;
	}

	ClientID client_id = (ClientID)atoi(argv[1]);

	if (client_id == CLIENT_ID_SERVER) {
		IConsoleError("Please use the command 'name' to change your own name!");
		return true;
	}

	if (NetworkClientInfo::GetByClientID(client_id) == NULL) {
		IConsoleError("Invalid client");
		return true;
	}

	if (!NetworkServerChangeClientName(client_id, argv[2])) {
		IConsoleError("Cannot give a client a duplicate name");
	}

	return true;
}

DEF_CONSOLE_CMD(ConJoinCompany)
{
	if (argc < 2) {
		IConsoleHelp("Request joining another company. Usage: join <company-id> [<password>]");
		IConsoleHelp("For valid company-id see company list, use 255 for spectator");
		return true;
	}

	CompanyID company_id = (CompanyID)(atoi(argv[1]) <= MAX_COMPANIES ? atoi(argv[1]) - 1 : atoi(argv[1]));

	/* Check we have a valid company id! */
	if (!Company::IsValidID(company_id) && company_id != COMPANY_SPECTATOR) {
		IConsolePrintF(CC_ERROR, "Company does not exist. Company-id must be between 1 and %d.", MAX_COMPANIES);
		return true;
	}

	if (NetworkClientInfo::GetByClientID(_network_own_client_id)->client_playas == company_id) {
		IConsoleError("You are already there!");
		return true;
	}

	if (company_id == COMPANY_SPECTATOR && NetworkMaxSpectatorsReached()) {
		IConsoleError("Cannot join spectators, maximum number of spectators reached.");
		return true;
	}

	if (company_id != COMPANY_SPECTATOR && !Company::IsHumanID(company_id)) {
		IConsoleError("Cannot join AI company.");
		return true;
	}

	/* Check if the company requires a password */
	if (NetworkCompanyIsPassworded(company_id) && argc < 3) {
		IConsolePrintF(CC_ERROR, "Company %d requires a password to join.", company_id + 1);
		return true;
	}

	/* non-dedicated server may just do the move! */
	if (_network_server) {
		NetworkServerDoMove(CLIENT_ID_SERVER, company_id);
	} else {
		NetworkClientRequestMove(company_id, NetworkCompanyIsPassworded(company_id) ? argv[2] : "");
	}

	return true;
}

DEF_CONSOLE_CMD(ConMoveClient)
{
	if (argc < 3) {
		IConsoleHelp("Move a client to another company. Usage: move <client-id> <company-id>");
		IConsoleHelp("For valid client-id see 'clients', for valid company-id see 'companies', use 255 for moving to spectators");
		return true;
	}

	const NetworkClientInfo *ci = NetworkClientInfo::GetByClientID((ClientID)atoi(argv[1]));
	CompanyID company_id = (CompanyID)(atoi(argv[2]) <= MAX_COMPANIES ? atoi(argv[2]) - 1 : atoi(argv[2]));

	/* check the client exists */
	if (ci == NULL) {
		IConsoleError("Invalid client-id, check the command 'clients' for valid client-id's.");
		return true;
	}

	if (!Company::IsValidID(company_id) && company_id != COMPANY_SPECTATOR) {
		IConsolePrintF(CC_ERROR, "Company does not exist. Company-id must be between 1 and %d.", MAX_COMPANIES);
		return true;
	}

	if (company_id != COMPANY_SPECTATOR && !Company::IsHumanID(company_id)) {
		IConsoleError("You cannot move clients to AI companies.");
		return true;
	}

	if (ci->client_id == CLIENT_ID_SERVER && _network_dedicated) {
		IConsoleError("Silly boy, you cannot move the server!");
		return true;
	}

	if (ci->client_playas == company_id) {
		IConsoleError("You cannot move someone to where he/she already is!");
		return true;
	}

	/* we are the server, so force the update */
	NetworkServerDoMove(ci->client_id, company_id);

	return true;
}

DEF_CONSOLE_CMD(ConResetCompany)
{
	if (argc == 0) {
		IConsoleHelp("Remove an idle company from the game. Usage: 'reset_company <company-id>'");
		IConsoleHelp("For company-id's, see the list of companies from the dropdown menu. Company 1 is 1, etc.");
		return true;
	}

	if (argc != 2) return false;

	CompanyID index = (CompanyID)(atoi(argv[1]) - 1);

	/* Check valid range */
	if (!Company::IsValidID(index)) {
		IConsolePrintF(CC_ERROR, "Company does not exist. Company-id must be between 1 and %d.", MAX_COMPANIES);
		return true;
	}

	if (!Company::IsHumanID(index)) {
		IConsoleError("Company is owned by an AI.");
		return true;
	}

	if (NetworkCompanyHasClients(index)) {
		IConsoleError("Cannot remove company: a client is connected to that company.");
		return false;
	}
	const NetworkClientInfo *ci = NetworkClientInfo::GetByClientID(CLIENT_ID_SERVER);
	if (ci->client_playas == index) {
		IConsoleError("Cannot remove company: the server is connected to that company.");
		return true;
	}

	/* It is safe to remove this company */
	DoCommandP(0, 2 | index << 16, CRR_MANUAL, CMD_COMPANY_CTRL);
	IConsolePrint(CC_DEFAULT, "Company deleted.");

	return true;
}

DEF_CONSOLE_CMD(ConNetworkClients)
{
	if (argc == 0) {
		IConsoleHelp("Get a list of connected clients including their ID, name, company-id, and IP. Usage: 'clients'");
		return true;
	}

	NetworkPrintClients();

	return true;
}

DEF_CONSOLE_CMD(ConNetworkReconnect)
{
	if (argc == 0) {
		IConsoleHelp("Reconnect to server to which you were connected last time. Usage: 'reconnect [<company>]'");
		IConsoleHelp("Company 255 is spectator (default, if not specified), 0 means creating new company.");
		IConsoleHelp("All others are a certain company with Company 1 being #1");
		return true;
	}

	CompanyID playas = (argc >= 2) ? (CompanyID)atoi(argv[1]) : COMPANY_SPECTATOR;
	switch (playas) {
		case 0: playas = COMPANY_NEW_COMPANY; break;
		case COMPANY_SPECTATOR: /* nothing to do */ break;
		default:
			/* From a user pov 0 is a new company, internally it's different and all
			 * companies are offset by one to ease up on users (eg companies 1-8 not 0-7) */
			playas--;
			if (playas < COMPANY_FIRST || playas >= MAX_COMPANIES) return false;
			break;
	}

	if (StrEmpty(_settings_client.network.last_host)) {
		IConsolePrint(CC_DEFAULT, "No server for reconnecting.");
		return true;
	}

	/* Don't resolve the address first, just print it directly as it comes from the config file. */
	IConsolePrintF(CC_DEFAULT, "Reconnecting to %s:%d...", _settings_client.network.last_host, _settings_client.network.last_port);

	NetworkClientConnectGame(NetworkAddress(_settings_client.network.last_host, _settings_client.network.last_port), playas);
	return true;
}

DEF_CONSOLE_CMD(ConNetworkConnect)
{
	if (argc == 0) {
		IConsoleHelp("Connect to a remote OTTD server and join the game. Usage: 'connect <ip>'");
		IConsoleHelp("IP can contain port and company: 'IP[:Port][#Company]', eg: 'server.ottd.org:443#2'");
		IConsoleHelp("Company #255 is spectator all others are a certain company with Company 1 being #1");
		return true;
	}

	if (argc < 2) return false;
	if (_networking) NetworkDisconnect(); // we are in network-mode, first close it!

	const char *port = NULL;
	const char *company = NULL;
	char *ip = argv[1];
	/* Default settings: default port and new company */
	uint16 rport = NETWORK_DEFAULT_PORT;
	CompanyID join_as = COMPANY_NEW_COMPANY;

	ParseConnectionString(&company, &port, ip);

	IConsolePrintF(CC_DEFAULT, "Connecting to %s...", ip);
	if (company != NULL) {
		join_as = (CompanyID)atoi(company);
		IConsolePrintF(CC_DEFAULT, "    company-no: %d", join_as);

		/* From a user pov 0 is a new company, internally it's different and all
		 * companies are offset by one to ease up on users (eg companies 1-8 not 0-7) */
		if (join_as != COMPANY_SPECTATOR) {
			if (join_as > MAX_COMPANIES) return false;
			join_as--;
		}
	}
	if (port != NULL) {
		rport = atoi(port);
		IConsolePrintF(CC_DEFAULT, "    port: %s", port);
	}

	NetworkClientConnectGame(NetworkAddress(ip, rport), join_as);

	return true;
}

#endif /* ENABLE_NETWORK */

/*********************************
 *  script file console commands
 *********************************/

DEF_CONSOLE_CMD(ConExec)
{
	if (argc == 0) {
		IConsoleHelp("Execute a local script file. Usage: 'exec <script> <?>'");
		return true;
	}

	if (argc < 2) return false;

	FILE *script_file = FioFOpenFile(argv[1], "r", BASE_DIR);

	if (script_file == NULL) {
		if (argc == 2 || atoi(argv[2]) != 0) IConsoleError("script file not found");
		return true;
	}

	_script_running = true;

	char cmdline[ICON_CMDLN_SIZE];
	while (_script_running && fgets(cmdline, sizeof(cmdline), script_file) != NULL) {
		/* Remove newline characters from the executing script */
		for (char *cmdptr = cmdline; *cmdptr != '\0'; cmdptr++) {
			if (*cmdptr == '\n' || *cmdptr == '\r') {
				*cmdptr = '\0';
				break;
			}
		}
		IConsoleCmdExec(cmdline);
	}

	if (ferror(script_file)) {
		IConsoleError("Encountered error while trying to read from script file");
	}

	_script_running = false;
	FioFCloseFile(script_file);
	return true;
}

DEF_CONSOLE_CMD(ConReturn)
{
	if (argc == 0) {
		IConsoleHelp("Stop executing a running script. Usage: 'return'");
		return true;
	}

	_script_running = false;
	return true;
}

/*****************************
 *  default console commands
 ******************************/
extern bool CloseConsoleLogIfActive();

DEF_CONSOLE_CMD(ConScript)
{
	extern FILE *_iconsole_output_file;

	if (argc == 0) {
		IConsoleHelp("Start or stop logging console output to a file. Usage: 'script <filename>'");
		IConsoleHelp("If filename is omitted, a running log is stopped if it is active");
		return true;
	}

	if (!CloseConsoleLogIfActive()) {
		if (argc < 2) return false;

		IConsolePrintF(CC_DEFAULT, "file output started to: %s", argv[1]);
		_iconsole_output_file = fopen(argv[1], "ab");
		if (_iconsole_output_file == NULL) IConsoleError("could not open file");
	}

	return true;
}


DEF_CONSOLE_CMD(ConEcho)
{
	if (argc == 0) {
		IConsoleHelp("Print back the first argument to the console. Usage: 'echo <arg>'");
		return true;
	}

	if (argc < 2) return false;
	IConsolePrint(CC_DEFAULT, argv[1]);
	return true;
}

DEF_CONSOLE_CMD(ConEchoC)
{
	if (argc == 0) {
		IConsoleHelp("Print back the first argument to the console in a given colour. Usage: 'echoc <colour> <arg2>'");
		return true;
	}

	if (argc < 3) return false;
	IConsolePrint((TextColour)Clamp(atoi(argv[1]), TC_BEGIN, TC_END - 1), argv[2]);
	return true;
}

DEF_CONSOLE_CMD(ConNewGame)
{
	if (argc == 0) {
		IConsoleHelp("Start a new game. Usage: 'newgame [seed]'");
		IConsoleHelp("The server can force a new game using 'newgame'; any client joined will rejoin after the server is done generating the new game.");
		return true;
	}

	StartNewGameWithoutGUI((argc == 2) ? strtoul(argv[1], NULL, 10) : GENERATE_NEW_SEED);
	return true;
}

DEF_CONSOLE_CMD(ConRestart)
{
	if (argc == 0) {
		IConsoleHelp("Restart game. Usage: 'restart'");
		IConsoleHelp("Restarts a game. It tries to reproduce the exact same map as the game started with.");
		IConsoleHelp("However:");
		IConsoleHelp(" * restarting games started in another version might create another map due to difference in map generation");
		IConsoleHelp(" * restarting games based on scenarios, loaded games or heightmaps will start a new game based on the settings stored in the scenario/savegame");
		return true;
	}

	/* Don't copy the _newgame pointers to the real pointers, so call SwitchToMode directly */
	_settings_game.game_creation.map_x = MapLogX();
	_settings_game.game_creation.map_y = FindFirstBit(MapSizeY());
	_switch_mode = SM_RESTARTGAME;
	return true;
}

/**
 * Print a text buffer line by line to the console. Lines are separated by '\n'.
 * @param buf The buffer to print.
 * @note All newlines are replace by '\0' characters.
 */
static void PrintLineByLine(char *buf)
{
	char *p = buf;
	/* Print output line by line */
	for (char *p2 = buf; *p2 != '\0'; p2++) {
		if (*p2 == '\n') {
			*p2 = '\0';
			IConsolePrintF(CC_DEFAULT, "%s", p);
			p = p2 + 1;
		}
	}
}

DEF_CONSOLE_CMD(ConListAILibs)
{
	char buf[4096];
	AI::GetConsoleLibraryList(buf, lastof(buf));

	PrintLineByLine(buf);

	return true;
}

DEF_CONSOLE_CMD(ConListAI)
{
	char buf[4096];
	AI::GetConsoleList(buf, lastof(buf));

	PrintLineByLine(buf);

	return true;
}

DEF_CONSOLE_CMD(ConListGameLibs)
{
	char buf[4096];
	Game::GetConsoleLibraryList(buf, lastof(buf));

	PrintLineByLine(buf);

	return true;
}

DEF_CONSOLE_CMD(ConListGame)
{
	char buf[4096];
	Game::GetConsoleList(buf, lastof(buf));

	PrintLineByLine(buf);

	return true;
}

DEF_CONSOLE_CMD(ConStartAI)
{
	if (argc == 0 || argc > 3) {
		IConsoleHelp("Start a new AI. Usage: 'start_ai [<AI>] [<settings>]'");
		IConsoleHelp("Start a new AI. If <AI> is given, it starts that specific AI (if found).");
		IConsoleHelp("If <settings> is given, it is parsed and the AI settings are set to that.");
		return true;
	}

	if (_game_mode != GM_NORMAL) {
		IConsoleWarning("AIs can only be managed in a game.");
		return true;
	}

	if (Company::GetNumItems() == CompanyPool::MAX_SIZE) {
		IConsoleWarning("Can't start a new AI (no more free slots).");
		return true;
	}
	if (_networking && !_network_server) {
		IConsoleWarning("Only the server can start a new AI.");
		return true;
	}
	if (_networking && !_settings_game.ai.ai_in_multiplayer) {
		IConsoleWarning("AIs are not allowed in multiplayer by configuration.");
		IConsoleWarning("Switch AI -> AI in multiplayer to True.");
		return true;
	}
	if (!AI::CanStartNew()) {
		IConsoleWarning("Can't start a new AI.");
		return true;
	}

	int n = 0;
	Company *c;
	/* Find the next free slot */
	FOR_ALL_COMPANIES(c) {
		if (c->index != n) break;
		n++;
	}

	AIConfig *config = AIConfig::GetConfig((CompanyID)n);
	if (argc >= 2) {
		config->Change(argv[1], -1, true);
		if (!config->HasScript()) {
			IConsoleWarning("Failed to load the specified AI");
			return true;
		}
		if (argc == 3) {
			config->StringToSettings(argv[2]);
		}
	}

	/* Start a new AI company */
	DoCommandP(0, 1 | INVALID_COMPANY << 16, 0, CMD_COMPANY_CTRL);

	return true;
}

DEF_CONSOLE_CMD(ConReloadAI)
{
	if (argc != 2) {
		IConsoleHelp("Reload an AI. Usage: 'reload_ai <company-id>'");
		IConsoleHelp("Reload the AI with the given company id. For company-id's, see the list of companies from the dropdown menu. Company 1 is 1, etc.");
		return true;
	}

	if (_game_mode != GM_NORMAL) {
		IConsoleWarning("AIs can only be managed in a game.");
		return true;
	}

	if (_networking && !_network_server) {
		IConsoleWarning("Only the server can reload an AI.");
		return true;
	}

	CompanyID company_id = (CompanyID)(atoi(argv[1]) - 1);
	if (!Company::IsValidID(company_id)) {
		IConsolePrintF(CC_DEFAULT, "Unknown company. Company range is between 1 and %d.", MAX_COMPANIES);
		return true;
	}

	if (Company::IsHumanID(company_id)) {
		IConsoleWarning("Company is not controlled by an AI.");
		return true;
	}

	/* First kill the company of the AI, then start a new one. This should start the current AI again */
	DoCommandP(0, 2 | company_id << 16, CRR_MANUAL, CMD_COMPANY_CTRL);
	DoCommandP(0, 1 | company_id << 16, 0, CMD_COMPANY_CTRL);
	IConsolePrint(CC_DEFAULT, "AI reloaded.");

	return true;
}

DEF_CONSOLE_CMD(ConStopAI)
{
	if (argc != 2) {
		IConsoleHelp("Stop an AI. Usage: 'stop_ai <company-id>'");
		IConsoleHelp("Stop the AI with the given company id. For company-id's, see the list of companies from the dropdown menu. Company 1 is 1, etc.");
		return true;
	}

	if (_game_mode != GM_NORMAL) {
		IConsoleWarning("AIs can only be managed in a game.");
		return true;
	}

	if (_networking && !_network_server) {
		IConsoleWarning("Only the server can stop an AI.");
		return true;
	}

	CompanyID company_id = (CompanyID)(atoi(argv[1]) - 1);
	if (!Company::IsValidID(company_id)) {
		IConsolePrintF(CC_DEFAULT, "Unknown company. Company range is between 1 and %d.", MAX_COMPANIES);
		return true;
	}

	if (Company::IsHumanID(company_id) || company_id == _local_company) {
		IConsoleWarning("Company is not controlled by an AI.");
		return true;
	}

	/* Now kill the company of the AI. */
	DoCommandP(0, 2 | company_id << 16, CRR_MANUAL, CMD_COMPANY_CTRL);
	IConsolePrint(CC_DEFAULT, "AI stopped, company deleted.");

	return true;
}

DEF_CONSOLE_CMD(ConRescanAI)
{
	if (argc == 0) {
		IConsoleHelp("Rescan the AI dir for scripts. Usage: 'rescan_ai'");
		return true;
	}

	if (_networking && !_network_server) {
		IConsoleWarning("Only the server can rescan the AI dir for scripts.");
		return true;
	}

	AI::Rescan();

	return true;
}

DEF_CONSOLE_CMD(ConRescanGame)
{
	if (argc == 0) {
		IConsoleHelp("Rescan the Game Script dir for scripts. Usage: 'rescan_game'");
		return true;
	}

	if (_networking && !_network_server) {
		IConsoleWarning("Only the server can rescan the Game Script dir for scripts.");
		return true;
	}

	Game::Rescan();

	return true;
}

DEF_CONSOLE_CMD(ConRescanNewGRF)
{
	if (argc == 0) {
		IConsoleHelp("Rescan the data dir for NewGRFs. Usage: 'rescan_newgrf'");
		return true;
	}

	ScanNewGRFFiles(NULL);

	return true;
}

DEF_CONSOLE_CMD(ConGetSeed)
{
	if (argc == 0) {
		IConsoleHelp("Returns the seed used to create this game. Usage: 'getseed'");
		IConsoleHelp("The seed can be used to reproduce the exact same map as the game started with.");
		return true;
	}

	IConsolePrintF(CC_DEFAULT, "Generation Seed: %u", _settings_game.game_creation.generation_seed);
	return true;
}

DEF_CONSOLE_CMD(ConGetDate)
{
	if (argc == 0) {
		IConsoleHelp("Returns the current date (day-month-year) of the game. Usage: 'getdate'");
		return true;
	}

	YearMonthDay ymd;
	ConvertDateToYMD(_date, &ymd);
	IConsolePrintF(CC_DEFAULT, "Date: %d-%d-%d", ymd.day, ymd.month + 1, ymd.year);
	return true;
}


DEF_CONSOLE_CMD(ConAlias)
{
	IConsoleAlias *alias;

	if (argc == 0) {
		IConsoleHelp("Add a new alias, or redefine the behaviour of an existing alias . Usage: 'alias <name> <command>'");
		return true;
	}

	if (argc < 3) return false;

	alias = IConsoleAliasGet(argv[1]);
	if (alias == NULL) {
		IConsoleAliasRegister(argv[1], argv[2]);
	} else {
		free(alias->cmdline);
		alias->cmdline = stredup(argv[2]);
	}
	return true;
}

DEF_CONSOLE_CMD(ConScreenShot)
{
	if (argc == 0) {
		IConsoleHelp("Create a screenshot of the game. Usage: 'screenshot [big | giant | no_con] [file name]'");
		IConsoleHelp("'big' makes a zoomed-in screenshot of the visible area, 'giant' makes a screenshot of the "
				"whole map, 'no_con' hides the console to create the screenshot. 'big' or 'giant' "
				"screenshots are always drawn without console");
		return true;
	}

	if (argc > 3) return false;

	ScreenshotType type = SC_VIEWPORT;
	const char *name = NULL;

	if (argc > 1) {
		if (strcmp(argv[1], "big") == 0) {
			/* screenshot big [filename] */
			type = SC_ZOOMEDIN;
			if (argc > 2) name = argv[2];
		} else if (strcmp(argv[1], "giant") == 0) {
			/* screenshot giant [filename] */
			type = SC_WORLD;
			if (argc > 2) name = argv[2];
		} else if (strcmp(argv[1], "no_con") == 0) {
			/* screenshot no_con [filename] */
			IConsoleClose();
			if (argc > 2) name = argv[2];
		} else if (argc == 2) {
			/* screenshot filename */
			name = argv[1];
		} else {
			/* screenshot argv[1] argv[2] - invalid */
			return false;
		}
	}

	MakeScreenshot(type, name);
	return true;
}

DEF_CONSOLE_CMD(ConMinimap)
{
	if (argc == 0) {
		IConsoleHelp("Create a flat image of the game minimap. Usage: 'minimap [owner] [file name]'");
		IConsoleHelp("'owner' uses the tile owner to colour the minimap image, this is the only mode at present");
		return true;
	}

	const char *name = NULL;
	if (argc > 1) {
		if (strcmp(argv[1], "owner") != 0) {
			/* invalid mode */
			return false;
		}
	}
	if (argc > 2) {
		name = argv[2];
	}

	SaveMinimap(name);
	return true;
}

DEF_CONSOLE_CMD(ConInfoCmd)
{
	if (argc == 0) {
		IConsoleHelp("Print out debugging information about a command. Usage: 'info_cmd <cmd>'");
		return true;
	}

	if (argc < 2) return false;

	const IConsoleCmd *cmd = IConsoleCmdGet(argv[1]);
	if (cmd == NULL) {
		IConsoleError("the given command was not found");
		return true;
	}

	IConsolePrintF(CC_DEFAULT, "command name: %s", cmd->name);
	IConsolePrintF(CC_DEFAULT, "command proc: %p", cmd->proc);

	if (cmd->hook != NULL) IConsoleWarning("command is hooked");

	return true;
}

DEF_CONSOLE_CMD(ConDebugLevel)
{
	if (argc == 0) {
		IConsoleHelp("Get/set the default debugging level for the game. Usage: 'debug_level [<level>]'");
		IConsoleHelp("Level can be any combination of names, levels. Eg 'net=5 ms=4'. Remember to enclose it in \"'s");
		return true;
	}

	if (argc > 2) return false;

	if (argc == 1) {
		IConsolePrintF(CC_DEFAULT, "Current debug-level: '%s'", GetDebugString());
	} else {
		SetDebugString(argv[1]);
	}

	return true;
}

DEF_CONSOLE_CMD(ConExit)
{
	if (argc == 0) {
		IConsoleHelp("Exit the game. Usage: 'exit'");
		return true;
	}

	if (_game_mode == GM_NORMAL && _settings_client.gui.autosave_on_exit) DoExitSave();

	_exit_game = true;
	return true;
}

DEF_CONSOLE_CMD(ConPart)
{
	if (argc == 0) {
		IConsoleHelp("Leave the currently joined/running game (only ingame). Usage: 'part'");
		return true;
	}

	if (_game_mode != GM_NORMAL) return false;

	_switch_mode = SM_MENU;
	return true;
}

DEF_CONSOLE_CMD(ConHelp)
{
	if (argc == 2) {
		const IConsoleCmd *cmd;
		const IConsoleAlias *alias;

		RemoveUnderscores(argv[1]);
		cmd = IConsoleCmdGet(argv[1]);
		if (cmd != NULL) {
			cmd->proc(0, NULL);
			return true;
		}

		alias = IConsoleAliasGet(argv[1]);
		if (alias != NULL) {
			cmd = IConsoleCmdGet(alias->cmdline);
			if (cmd != NULL) {
				cmd->proc(0, NULL);
				return true;
			}
			IConsolePrintF(CC_ERROR, "ERROR: alias is of special type, please see its execution-line: '%s'", alias->cmdline);
			return true;
		}

		IConsoleError("command not found");
		return true;
	}

	IConsolePrint(CC_WARNING, " ---- OpenTTD Console Help ---- ");
	IConsolePrint(CC_DEFAULT, " - commands: [command to list all commands: list_cmds]");
	IConsolePrint(CC_DEFAULT, " call commands with '<command> <arg2> <arg3>...'");
	IConsolePrint(CC_DEFAULT, " - to assign strings, or use them as arguments, enclose it within quotes");
	IConsolePrint(CC_DEFAULT, " like this: '<command> \"string argument with spaces\"'");
	IConsolePrint(CC_DEFAULT, " - use 'help <command>' to get specific information");
	IConsolePrint(CC_DEFAULT, " - scroll console output with shift + (up | down | pageup | pagedown)");
	IConsolePrint(CC_DEFAULT, " - scroll console input history with the up or down arrows");
	IConsolePrint(CC_DEFAULT, "");
	return true;
}

DEF_CONSOLE_CMD(ConListCommands)
{
	if (argc == 0) {
		IConsoleHelp("List all registered commands. Usage: 'list_cmds [<pre-filter>]'");
		return true;
	}

	for (const IConsoleCmd *cmd = _iconsole_cmds; cmd != NULL; cmd = cmd->next) {
		if (argv[1] == NULL || strstr(cmd->name, argv[1]) != NULL) {
			if (cmd->unlisted == false && (cmd->hook == NULL || cmd->hook(false) != CHR_HIDE)) IConsolePrintF(CC_DEFAULT, "%s", cmd->name);
		}
	}

	return true;
}

DEF_CONSOLE_CMD(ConListAliases)
{
	if (argc == 0) {
		IConsoleHelp("List all registered aliases. Usage: 'list_aliases [<pre-filter>]'");
		return true;
	}

	for (const IConsoleAlias *alias = _iconsole_aliases; alias != NULL; alias = alias->next) {
		if (argv[1] == NULL || strstr(alias->name, argv[1]) != NULL) {
			IConsolePrintF(CC_DEFAULT, "%s => %s", alias->name, alias->cmdline);
		}
	}

	return true;
}

DEF_CONSOLE_CMD(ConCompanies)
{
	if (argc == 0) {
		IConsoleHelp("List the details of all companies in the game. Usage 'companies'");
		return true;
	}

	Company *c;
	FOR_ALL_COMPANIES(c) {
		/* Grab the company name */
		char company_name[512];
		SetDParam(0, c->index);
		GetString(company_name, STR_COMPANY_NAME, lastof(company_name));

		const char *password_state = "";
		if (c->is_ai) {
			password_state = "AI";
		}
#ifdef ENABLE_NETWORK
		else if (_network_server) {
				password_state = StrEmpty(_network_company_states[c->index].password) ? "unprotected" : "protected";
		}
#endif

		char colour[512];
		GetString(colour, STR_COLOUR_DARK_BLUE + _company_colours[c->index], lastof(colour));
		IConsolePrintF(CC_INFO, "#:%d(%s) Company Name: '%s'  Year Founded: %d  Money: " OTTD_PRINTF64 "  Loan: " OTTD_PRINTF64 "  Value: " OTTD_PRINTF64 "  (T:%d, R:%d, P:%d, S:%d) %s",
			c->index + 1, colour, company_name,
			c->inaugurated_year, (int64)c->money, (int64)c->current_loan, (int64)CalculateCompanyValue(c),
			c->group_all[VEH_TRAIN].num_vehicle,
			c->group_all[VEH_ROAD].num_vehicle,
			c->group_all[VEH_AIRCRAFT].num_vehicle,
			c->group_all[VEH_SHIP].num_vehicle,
			password_state);
	}

	return true;
}

#ifdef ENABLE_NETWORK

DEF_CONSOLE_CMD(ConSay)
{
	if (argc == 0) {
		IConsoleHelp("Chat to your fellow players in a multiplayer game. Usage: 'say \"<msg>\"'");
		return true;
	}

	if (argc != 2) return false;

	if (!_network_server) {
		NetworkClientSendChat(NETWORK_ACTION_CHAT, DESTTYPE_BROADCAST, 0 /* param does not matter */, argv[1]);
	} else {
		bool from_admin = (_redirect_console_to_admin < INVALID_ADMIN_ID);
		NetworkServerSendChat(NETWORK_ACTION_CHAT, DESTTYPE_BROADCAST, 0, argv[1], CLIENT_ID_SERVER, from_admin);
	}

	return true;
}

DEF_CONSOLE_CMD(ConSayCompany)
{
	if (argc == 0) {
		IConsoleHelp("Chat to a certain company in a multiplayer game. Usage: 'say_company <company-no> \"<msg>\"'");
		IConsoleHelp("CompanyNo is the company that plays as company <companyno>, 1 through max_companies");
		return true;
	}

	if (argc != 3) return false;

	CompanyID company_id = (CompanyID)(atoi(argv[1]) - 1);
	if (!Company::IsValidID(company_id)) {
		IConsolePrintF(CC_DEFAULT, "Unknown company. Company range is between 1 and %d.", MAX_COMPANIES);
		return true;
	}

	if (!_network_server) {
		NetworkClientSendChat(NETWORK_ACTION_CHAT_COMPANY, DESTTYPE_TEAM, company_id, argv[2]);
	} else {
		bool from_admin = (_redirect_console_to_admin < INVALID_ADMIN_ID);
		NetworkServerSendChat(NETWORK_ACTION_CHAT_COMPANY, DESTTYPE_TEAM, company_id, argv[2], CLIENT_ID_SERVER, from_admin);
	}

	return true;
}

DEF_CONSOLE_CMD(ConSayClient)
{
	if (argc == 0) {
		IConsoleHelp("Chat to a certain client in a multiplayer game. Usage: 'say_client <client-no> \"<msg>\"'");
		IConsoleHelp("For client-id's, see the command 'clients'");
		return true;
	}

	if (argc != 3) return false;

	if (!_network_server) {
		NetworkClientSendChat(NETWORK_ACTION_CHAT_CLIENT, DESTTYPE_CLIENT, atoi(argv[1]), argv[2]);
	} else {
		bool from_admin = (_redirect_console_to_admin < INVALID_ADMIN_ID);
		NetworkServerSendChat(NETWORK_ACTION_CHAT_CLIENT, DESTTYPE_CLIENT, atoi(argv[1]), argv[2], CLIENT_ID_SERVER, from_admin);
	}

	return true;
}

DEF_CONSOLE_CMD(ConCompanyPassword)
{
	if (argc == 0) {
		const char *helpmsg;

		if (_network_dedicated) {
			helpmsg = "Change the password of a company. Usage: 'company_pw <company-no> \"<password>\"";
		} else if (_network_server) {
			helpmsg = "Change the password of your or any other company. Usage: 'company_pw [<company-no>] \"<password>\"'";
		} else {
			helpmsg = "Change the password of your company. Usage: 'company_pw \"<password>\"'";
		}

		IConsoleHelp(helpmsg);
		IConsoleHelp("Use \"*\" to disable the password.");
		return true;
	}

	CompanyID company_id;
	const char *password;
	const char *errormsg;

	if (argc == 2) {
		company_id = _local_company;
		password = argv[1];
		errormsg = "You have to own a company to make use of this command.";
	} else if (argc == 3 && _network_server) {
		company_id = (CompanyID)(atoi(argv[1]) - 1);
		password = argv[2];
		errormsg = "You have to specify the ID of a valid human controlled company.";
	} else {
		return false;
	}

	if (!Company::IsValidHumanID(company_id)) {
		IConsoleError(errormsg);
		return false;
	}

	password = NetworkChangeCompanyPassword(company_id, password);

	if (StrEmpty(password)) {
		IConsolePrintF(CC_WARNING, "Company password cleared");
	} else {
		IConsolePrintF(CC_WARNING, "Company password changed to: %s", password);
	}

	return true;
}

/* Content downloading only is available with ZLIB */
#if defined(WITH_ZLIB)
#include "network/network_content.h"

/** Resolve a string to a content type. */
static ContentType StringToContentType(const char *str)
{
	static const char * const inv_lookup[] = { "", "base", "newgrf", "ai", "ailib", "scenario", "heightmap" };
	for (uint i = 1 /* there is no type 0 */; i < lengthof(inv_lookup); i++) {
		if (strcasecmp(str, inv_lookup[i]) == 0) return (ContentType)i;
	}
	return CONTENT_TYPE_END;
}

/** Asynchronous callback */
struct ConsoleContentCallback : public ContentCallback {
	void OnConnect(bool success)
	{
		IConsolePrintF(CC_DEFAULT, "Content server connection %s", success ? "established" : "failed");
	}

	void OnDisconnect()
	{
		IConsolePrintF(CC_DEFAULT, "Content server connection closed");
	}

	void OnDownloadComplete(ContentID cid)
	{
		IConsolePrintF(CC_DEFAULT, "Completed download of %d", cid);
	}
};

/**
 * Outputs content state information to console
 * @param ci the content info
 */
static void OutputContentState(const ContentInfo *const ci)
{
	static const char * const types[] = { "Base graphics", "NewGRF", "AI", "AI library", "Scenario", "Heightmap", "Base sound", "Base music", "Game script", "GS library" };
	assert_compile(lengthof(types) == CONTENT_TYPE_END - CONTENT_TYPE_BEGIN);
	static const char * const states[] = { "Not selected", "Selected", "Dep Selected", "Installed", "Unknown" };
	static const TextColour state_to_colour[] = { CC_COMMAND, CC_INFO, CC_INFO, CC_WHITE, CC_ERROR };

	char buf[sizeof(ci->md5sum) * 2 + 1];
	md5sumToString(buf, lastof(buf), ci->md5sum);
	IConsolePrintF(state_to_colour[ci->state], "%d, %s, %s, %s, %08X, %s", ci->id, types[ci->type - 1], states[ci->state], ci->name, ci->unique_id, buf);
}

DEF_CONSOLE_CMD(ConContent)
{
	static ContentCallback *cb = NULL;
	if (cb == NULL) {
		cb = new ConsoleContentCallback();
		_network_content_client.AddCallback(cb);
	}

	if (argc <= 1) {
		IConsoleHelp("Query, select and download content. Usage: 'content update|upgrade|select [all|id]|unselect [all|id]|state [filter]|download'");
		IConsoleHelp("  update: get a new list of downloadable content; must be run first");
		IConsoleHelp("  upgrade: select all items that are upgrades");
		IConsoleHelp("  select: select a specific item given by its id or 'all' to select all. If no parameter is given, all selected content will be listed");
		IConsoleHelp("  unselect: unselect a specific item given by its id or 'all' to unselect all");
		IConsoleHelp("  state: show the download/select state of all downloadable content. Optionally give a filter string");
		IConsoleHelp("  download: download all content you've selected");
		return true;
	}

	if (strcasecmp(argv[1], "update") == 0) {
		_network_content_client.RequestContentList((argc > 2) ? StringToContentType(argv[2]) : CONTENT_TYPE_END);
		return true;
	}

	if (strcasecmp(argv[1], "upgrade") == 0) {
		_network_content_client.SelectUpgrade();
		return true;
	}

	if (strcasecmp(argv[1], "select") == 0) {
		if (argc <= 2) {
			/* List selected content */
			IConsolePrintF(CC_WHITE, "id, type, state, name");
			for (ConstContentIterator iter = _network_content_client.Begin(); iter != _network_content_client.End(); iter++) {
				if ((*iter)->state != ContentInfo::SELECTED && (*iter)->state != ContentInfo::AUTOSELECTED) continue;
				OutputContentState(*iter);
			}
		} else if (strcasecmp(argv[2], "all") == 0) {
			_network_content_client.SelectAll();
		} else {
			_network_content_client.Select((ContentID)atoi(argv[2]));
		}
		return true;
	}

	if (strcasecmp(argv[1], "unselect") == 0) {
		if (argc <= 2) {
			IConsoleError("You must enter the id.");
			return false;
		}
		if (strcasecmp(argv[2], "all") == 0) {
			_network_content_client.UnselectAll();
		} else {
			_network_content_client.Unselect((ContentID)atoi(argv[2]));
		}
		return true;
	}

	if (strcasecmp(argv[1], "state") == 0) {
		IConsolePrintF(CC_WHITE, "id, type, state, name");
		for (ConstContentIterator iter = _network_content_client.Begin(); iter != _network_content_client.End(); iter++) {
			if (argc > 2 && strcasestr((*iter)->name, argv[2]) == NULL) continue;
			OutputContentState(*iter);
		}
		return true;
	}

	if (strcasecmp(argv[1], "download") == 0) {
		uint files;
		uint bytes;
		_network_content_client.DownloadSelectedContent(files, bytes);
		IConsolePrintF(CC_DEFAULT, "Downloading %d file(s) (%d bytes)", files, bytes);
		return true;
	}

	return false;
}
#endif /* defined(WITH_ZLIB) */
#endif /* ENABLE_NETWORK */

DEF_CONSOLE_CMD(ConSetting)
{
	if (argc == 0) {
		IConsoleHelp("Change setting for all clients. Usage: 'setting <name> [<value>]'");
		IConsoleHelp("Omitting <value> will print out the current value of the setting.");
		return true;
	}

	if (argc == 1 || argc > 3) return false;

	if (argc == 2) {
		IConsoleGetSetting(argv[1]);
	} else {
		IConsoleSetSetting(argv[1], argv[2]);
	}

	return true;
}

DEF_CONSOLE_CMD(ConSettingNewgame)
{
	if (argc == 0) {
		IConsoleHelp("Change setting for the next game. Usage: 'setting_newgame <name> [<value>]'");
		IConsoleHelp("Omitting <value> will print out the current value of the setting.");
		return true;
	}

	if (argc == 1 || argc > 3) return false;

	if (argc == 2) {
		IConsoleGetSetting(argv[1], true);
	} else {
		IConsoleSetSetting(argv[1], argv[2], true);
	}

	return true;
}

/** Identifier of alias for matches and commands*/
const int LIST_ALIAS = -1;

/**
 Vehicle command ID
*/
enum VehicleCommand {
	VEHICLE_COMMAND_ALIAS = LIST_ALIAS,
	VEHICLE_INVALID_COMMAND = 0,
	VEHICLE_CENTER,
	VEHICLE_CLONE,
	VEHICLE_CLONE_SHARED,
	VEHICLE_DEPOT,
	TRAIN_IGNORE,
	TRAIN_WAGON_INFO,
	TRAIN_SELL_WAGON,
	VEHICLE_INFO,
	VEHICLE_LEAVE_STATION,
	VEHICLE_OPEN,
	VEHICLE_SELL,
	VEHICLE_SERVICE,
	VEHICLE_SKIP_ORDER,
	VEHICLE_START,
	VEHICLE_STOP,
	VEHICLE_TURN,
	VEHICLE_INTERVAL,
	VEHICLE_UNDEPOT,
	VEHICLE_UNSERVICE,
	VEHICLE_COUNT
};

/**
 Town command ID
*/
enum TownCommand {
	TOWN_COMMAND_ALIAS = LIST_ALIAS,
	TOWN_INVALID_COMMAND = 0,
	TOWN_CENTER,
	TOWN_INFO,
	TOWN_PRINT,
	TOWN_OPEN,
	TOWN_OPEN_AUTH,
	TOWN_ACTION_AD_SMALL,
	TOWN_ACTION_AD_MEDIUM,
	TOWN_ACTION_AD_LARGE,
	TOWN_ACTION_ROAD,
	TOWN_ACTION_STATUE,
	TOWN_ACTION_FUND,
	TOWN_ACTION_EXCLUSIVE,
	TOWN_ACTION_BRIBE,
	TOWN_EXPAND,
	TOWN_DELETE,
	TOWN_COUNT
};

/** First available town action */
const TownCommand TOWN_ACTION_0 = TOWN_ACTION_AD_SMALL;

/**
 Industry command ID
*/
enum IndustryCommand {
	INDUSTRY_COMMAND_ALIAS = LIST_ALIAS,
	INDUSTRY_INVALID_COMMAND = 0,
	INDUSTRY_CENTER,
	INDUSTRY_INFO,
	INDUSTRY_OPEN,
	INDUSTRY_COUNT,
	INDUSTRY_DELETE
};

/**
 Type of match for vehicle, town and industry commands
*/
enum MatchType {
	//Generic
	MATCH_ALIAS = LIST_ALIAS,
	MATCH_INVALID = 0,
	MATCH_GENERIC,
	MATCH_ALL,

	//Vehicles
	MATCH_GROUP,
	MATCH_CRASHED,
	MATCH_LENGTH,
	MATCH_WAGONS,
	MATCH_ORDERS,
	MATCH_SPEED,
	MATCH_AGE,
	MATCH_BREAKDOWNS,
	MATCH_MAXSPEED,
	MATCH_PROFIT,
	MATCH_PROFIT_THIS,
	MATCH_PROFIT_LAST,
	MATCH_SERVICE,
	MATCH_IN_DEPOT,
	MATCH_BROKEN,

	//Towns
	MATCH_TOWN_POPULATION,
	MATCH_TOWN_HOUSES,
	MATCH_TOWN_RATING,
	MATCH_TOWN_STATUE,
	MATCH_TOWN_NO_STATUE,
	MATCH_TOWN_FUNDING,
	MATCH_TOWN_ROADWORKS,
	MATCH_TOWN_EXCLUSIVE_COMPANY,
	MATCH_TOWN_EXCLUSIVE_MONTHS,
	MATCH_TOWN_EXCLUSIVE_MY_MONTHS,
	MATCH_TOWN_EXCLUSIVE_OTHERS_MONTHS,
	MATCH_TOWN_UNWANTED_MONTHS,
	MATCH_TOWN_NOISE,
	MATCH_TOWN_NOISE_REMAIN,
	MATCH_TOWN_NOISE_MAX,

	//Industries
	MATCH_INDUSTRY_PRODUCTION,
	MATCH_INDUSTRY_PRODUCTION_THIS,
	MATCH_INDUSTRY_PERCENT,
	MATCH_INDUSTRY_PERCENT_THIS,

};

/**
 Subtype of match for numeric matches
*/
enum MatchSubtype {
	MATCH_NONE,
	MATCH_NOT_EQUAL,
	MATCH_EQUAL,
	MATCH_LESS,
	MATCH_LESS_OR_EQUAL,
	MATCH_GREATER_OR_EQUAL,
	MATCH_GREATER
};

/**
 Structure with match information
*/
class MatchInfo {
public:
	/** Type of match */
	MatchType type;
	/** Subtype of match */
	MatchSubtype subtype;
	/** Parameter of match */
	const char *id;
	/** Next match in chain */
	MatchInfo *next;
	/** Constructor */
	MatchInfo(): type(MATCH_GENERIC), subtype(MATCH_NONE), id(NULL), next(NULL) {
	}
	/** Constructor */
	MatchInfo(MatchType t, MatchSubtype st, const char *ix): next(NULL) {
		type = t;
		subtype = st;
		id = ix;
	}
	~MatchInfo() {
		if (next) delete next;
	}
};

// Bitmask for StringInfo<T>.req
//Vehicles
const int FOR_TRAIN     = 0x01; //Command for train
const int FOR_ROAD      = 0x02; //Command for road vehicle
const int FOR_SHIP      = 0x04; //Command for ship
const int FOR_AIRCRAFT  = 0x08; //Command for plane
const int NOT_CRASHED   = 0x10; //Target vehicle must not be crashed
const int IN_DEPOT      = 0x20; //Target vehicle must be in depot
const int STOPPED       = 0x40; //Target vehicle must be stopped
const int IS_ALIAS      = 0x80; //Internal flag for command alias
const int FOR_VEHICLE   = FOR_TRAIN | FOR_ROAD | FOR_SHIP | FOR_AIRCRAFT; //Command for any vehicle
//Towns
const int FOR_TOWN      = 0x100; //Command for town
//Industries
const int FOR_INDUSTRY  = 0x200; //Command for industry
//All types
const int USE_PRINTF    = 0x400; //Help text contains one %s to be replaced by name of target object type
const int IN_EDITOR     = 0x800; //Command usable only in editor

/**
 Structure mapping one command or match type to it's ID
*/
template<typename T> struct StringInfo {
 /** ID of command or match */
 T id;
 /** Name of command or match */
 const char *name;
 /** Number of required parameters */
 int params;
 /** Requirements for target of command */
 int req;
 /** Help text */
 const char *help;
};

/**
 List of all command names for vehicle commands.
 All aliases must be listed right before their commands
 */
const StringInfo<VehicleCommand> veh_commands[] = {
	{ VEHICLE_COMMAND_ALIAS, "centre",              0, 0, "" },
	{ VEHICLE_CENTER,        "center",              0, FOR_VEHICLE,
	                         "Center main view on vehicle's location" },
	{ VEHICLE_CLONE,         "clone",               0, FOR_VEHICLE | IN_DEPOT,
	                         "Clone vehicle, if it is in depot. Parameter specifies number of created clones (default 1)" },
	{ VEHICLE_CLONE_SHARED,  "clone_shared",        0, FOR_VEHICLE | IN_DEPOT,
	                         "Same as clone, but with shared orders"  },
	{ VEHICLE_COUNT,         "count",               0, FOR_VEHICLE,
	                         "Count vehicles matching given criteria" },
	{ VEHICLE_DEPOT,         "depot",               0, FOR_VEHICLE | NOT_CRASHED,
	                         "Send to depot" },
	{ TRAIN_IGNORE,          "ignore",              0, FOR_TRAIN | NOT_CRASHED,
	                         "Ignore signals" },
	{ VEHICLE_INFO,          "info",                0, FOR_VEHICLE,
	                         "Show vehicle info in console" },
	{ VEHICLE_INTERVAL,      "interval",            1, FOR_VEHICLE | NOT_CRASHED,
	                         "Set servicing interval. Parameter specifies new interval in days/percent" },
	{ VEHICLE_LEAVE_STATION, "leave",               0, FOR_VEHICLE | NOT_CRASHED,
	                         "Leave station by skipping to next order" },
	{ VEHICLE_COMMAND_ALIAS, "show",                0, 0, "" },
	{ VEHICLE_OPEN,          "open",                0, FOR_VEHICLE,
	                         "Open vehicle window" },
	{ VEHICLE_SELL,          "sell",                0, FOR_VEHICLE | STOPPED | IN_DEPOT,
	                         "Sell vehicle, if it is stopped in depot" },
	{ VEHICLE_SERVICE,       "service",             0, FOR_VEHICLE | NOT_CRASHED,
	                         "Send for servicing" },
	{ VEHICLE_SKIP_ORDER,    "skip",                0, FOR_VEHICLE | NOT_CRASHED,
	                         "Skip to next order. Optional parameter specifies how many orders to skip ('r' = skip to random order, default is 1)" },
	{ VEHICLE_COMMAND_ALIAS, "go",                  0, 0, ""},
	{ VEHICLE_START,         "start",               0, FOR_VEHICLE | NOT_CRASHED,
	                         "Start vehicle" },
	{ VEHICLE_STOP,          "stop",                0, FOR_VEHICLE | NOT_CRASHED,
	                         "Stop vehicle" },
	{ VEHICLE_COMMAND_ALIAS, "reverse",             0, 0, "" },
	{ VEHICLE_TURN,          "turn",                0, FOR_TRAIN | FOR_ROAD | NOT_CRASHED,
	                         "Turn around" },
	{ VEHICLE_UNSERVICE,     "unservice",           0, FOR_VEHICLE | NOT_CRASHED,
	                         "Cancel order to be sent for servicing" },
	{ VEHICLE_UNDEPOT,       "undepot",             0, FOR_VEHICLE | NOT_CRASHED,
	                         "Cancel order to be sent to depot" },
	{ TRAIN_WAGON_INFO,      "winfo",               0, FOR_TRAIN,
	                         "Show info about train wagons in console" },
	{ TRAIN_SELL_WAGON,      "wsell",               1, FOR_TRAIN | STOPPED | IN_DEPOT,
	                         "Sell train wagons(s). If one parameter is given, single wagon will be sold. If two parameters are given, they will specify range of wagons to sell." },
};

/**
 List of all command names for town commands.
 All aliases must be listed right before their commands
 */
const StringInfo<TownCommand> town_commands[] = {
	{ TOWN_COMMAND_ALIAS,    "centre",              0, 0, "" },
	{ TOWN_CENTER,           "center",              0, FOR_TOWN,
	                         "Center main view on town location" },
	{ TOWN_COUNT,            "count",               0, FOR_TOWN,
	                         "Count towns matching given criteria" },
	{ TOWN_INFO,             "info",                0, FOR_TOWN,
	                         "Show town info in console" },
	{ TOWN_PRINT,            "print",               0, FOR_TOWN,
	                         "Print town name in console" },
	{ TOWN_COMMAND_ALIAS,    "show",                0, 0, "" },
	{ TOWN_OPEN,             "open",                0, FOR_TOWN,
	                         "Open town window" },
	{ TOWN_OPEN_AUTH,        "auth",                0, FOR_TOWN,
	                         "Open town authority window" },
	{ TOWN_COMMAND_ALIAS,    "small_ad",            0, 0, "" },
	{ TOWN_ACTION_AD_SMALL,  "ad_small",            0, FOR_TOWN,
	                         "Launch small advertising campaign in the town" },
	{ TOWN_COMMAND_ALIAS,    "medium_ad",           0, 0, "" },
	{ TOWN_ACTION_AD_MEDIUM, "ad_medium",           0, FOR_TOWN,
	                         "Launch medium advertising campaign in the town" },
	{ TOWN_COMMAND_ALIAS,    "large_ad",            0, 0, "" },
	{ TOWN_ACTION_AD_LARGE,  "ad_large",            0, FOR_TOWN,
	                         "Launch large advertising campaign in the town" },
	{ TOWN_COMMAND_ALIAS,    "reconstruction",      0, 0, "" },
	{ TOWN_ACTION_ROAD,      "road",                0, FOR_TOWN,
	                         "Fund road reconstruction in town" },
	{ TOWN_ACTION_STATUE,    "statue",              0, FOR_TOWN,
	                         "Build statue in town" },
	{ TOWN_COMMAND_ALIAS,    "building",            0, 0, "" },
	{ TOWN_ACTION_FUND,      "fund",                0, FOR_TOWN,
	                         "Fund construction of new buildings" },
	{ TOWN_ACTION_EXCLUSIVE, "exclusive",           0, FOR_TOWN,
	                         "Buy exclusive rights in town" },
	{ TOWN_ACTION_BRIBE,     "bribe",               0, FOR_TOWN,
	                         "Bribe town authority" },
	{ TOWN_EXPAND,           "expand",              0, FOR_TOWN | IN_EDITOR,
	                         "Expand town (scenario editor only) Parameter specifies number of repetitions (default 1)" },
	{ TOWN_DELETE,           "delete",              0, FOR_TOWN | IN_EDITOR,
	                         "Delete the town (scenario editor only)" },
};

/**
 List of all command names for industry commands.
 All aliases must be listed right before their commands
 */
const StringInfo<IndustryCommand> ind_commands[] = {
	{ INDUSTRY_COMMAND_ALIAS, "centre",              0, 0, "" },
	{ INDUSTRY_CENTER,        "center",              0, FOR_INDUSTRY,
	                          "Center main view on industry location" },
	{ INDUSTRY_COUNT,         "count",               0, FOR_INDUSTRY,
	                          "Count industries matching given criteria" },
	{ INDUSTRY_INFO,          "info",                0, FOR_INDUSTRY,
	                          "Show industry info in console" },
	{ INDUSTRY_COMMAND_ALIAS, "show",                0, 0, "" },
	{ INDUSTRY_OPEN,          "open",                0, FOR_INDUSTRY,
	                          "Open industry window" },
	{ INDUSTRY_DELETE,        "delete",                0, FOR_INDUSTRY,
	                          "Delete the industry" },
};

/**
 List of all non-numeric match names.
 */
const StringInfo<MatchType> match_nn_info[] = {
	{ MATCH_ALL,              "all",         0, FOR_VEHICLE | FOR_INDUSTRY | FOR_TOWN | USE_PRINTF,
	                          " for all %ss" },
	{ MATCH_ALL,              "*",           0, FOR_VEHICLE | FOR_INDUSTRY | FOR_TOWN | USE_PRINTF,
	                          " for all %ss" },
	{ MATCH_BROKEN,           "broken",      0, FOR_VEHICLE | USE_PRINTF,
	                          " for all broken down %ss" },
	{ MATCH_CRASHED,          "crashed",     0, FOR_VEHICLE | USE_PRINTF,
	                          " for all crashed %ss" },
	{ MATCH_IN_DEPOT,         "depot",       0, FOR_VEHICLE | USE_PRINTF,
	                          " for all %ss in depot" },
	{ MATCH_TOWN_STATUE,      "statue",      0, FOR_TOWN,
	                          " for all towns where you have a statue" },
	{ MATCH_TOWN_NO_STATUE,   "no_statue",   0, FOR_TOWN,
	                          " for all towns where you don't have a statue" },
};

/**
 List of all numeric match names.
 */
const StringInfo<MatchType> match_info[] = {
	//Vehicles
	{ MATCH_AGE,         "age",         0, FOR_VEHICLE,
	                     "=[value] for matching age (in years)" },
	{ MATCH_BREAKDOWNS,  "breakdowns",  0, FOR_VEHICLE,
	                     "=[value] for matching breakdowns since last service" },
	{ MATCH_LENGTH,      "len",         0, FOR_TRAIN,
	                     "=[value] for matching train length (in tiles)" },
	{ MATCH_MAXSPEED,    "maxspeed",    0, FOR_VEHICLE,
	                     "=[value] for matching maximum speed (in km/h)" },
	{ MATCH_ORDERS,      "orders",      0, FOR_VEHICLE,
	                     "=[value] for matching number of orders" },
	{ MATCH_GROUP,       "group",       0, FOR_VEHICLE,
	                     "=[name] for matching group by name" },
	{ MATCH_PROFIT,      "profit",      0, FOR_VEHICLE,
	                     "=[value] for matching sum of this and last year's profit (in pounds)" },
	{ MATCH_PROFIT_THIS, "profit_this", 0, FOR_VEHICLE,
	                     "=[value] for matching this year's profit (in pounds)" },
	{ MATCH_PROFIT_LAST, "profit_last", 0, FOR_VEHICLE,
	                     "=[value] for matching last year's profit (in pounds)" },
	{ MATCH_SERVICE,     "service",     0, FOR_VEHICLE,
	                     "=[value] for matching service interval (in days/percent)" },
	{ MATCH_SPEED,       "speed",       0, FOR_VEHICLE,
	                     "=[value] for matching current speed (in km/h)" },
	{ MATCH_WAGONS,      "wagons",      0, FOR_TRAIN,
	                     "=[value] for matching number of train wagons" },

	//Towns
	{ MATCH_TOWN_POPULATION,              "population",      0, FOR_TOWN,
	                                      "=[value] for matching town population" },
	{ MATCH_TOWN_HOUSES,                  "houses",          0, FOR_TOWN,
	                                      "=[value] for matching number of town houses" },
	{ MATCH_TOWN_RATING,                  "rating",          0, FOR_TOWN,
	                                      "=[value] for matching your rating in town" },
	{ MATCH_TOWN_NOISE,                   "currnoise",       0, FOR_TOWN,
	                                      "=[value] for matching currently used noise level" },
	{ MATCH_TOWN_NOISE_REMAIN,            "noise",           0, FOR_TOWN,
	                                      "=[value] for matching remaining (usable by you) noise level" },
	{ MATCH_TOWN_NOISE_MAX,               "maxnoise",        0, FOR_TOWN,
	                                      "=[value] for matching maximal noise level" },
	{ MATCH_TOWN_FUNDING,                 "fund",            0, FOR_TOWN,
	                                      "=[value] for matching months remaining in building funding" },
	{ MATCH_TOWN_ROADWORKS,               "roadworks",       0, FOR_TOWN,
	                                      "=[value] for matching months remaining in road reconstructions" },
	{ MATCH_TOWN_EXCLUSIVE_COMPANY,       "exclusive",       0, FOR_TOWN,
	                                      "=[value] for matching company having exclusive rights" },
	{ MATCH_TOWN_EXCLUSIVE_MONTHS,        "any_exclusive",   0, FOR_TOWN,
	                                      "=[value] for matching months of remaining exclusive rights for any company" },
	{ MATCH_TOWN_EXCLUSIVE_MY_MONTHS,     "my_exclusive",    0, FOR_TOWN,
	                                      "=[value] for matching months of remaining exclusive rights for your company" },
	{ MATCH_TOWN_EXCLUSIVE_OTHERS_MONTHS, "other_exclusive", 0, FOR_TOWN,
	                                      "=[value] for matching months of remaining exclusive rights for any competitor company" },
	{ MATCH_TOWN_UNWANTED_MONTHS,         "unwanted",        0, FOR_TOWN,
	                                      "=[value] for matching months you are unwanted in town due to bribe" },

	//Industries
	{ MATCH_INDUSTRY_PRODUCTION,          "production",       0, FOR_INDUSTRY,
	                                      "=[value] for matching industry production last month" },
	{ MATCH_INDUSTRY_PRODUCTION_THIS,     "thisproduction",   0, FOR_INDUSTRY,
	                                      "=[value] for matching industry production this month" },
	{ MATCH_INDUSTRY_PERCENT,             "percent",          0, FOR_INDUSTRY,
	                                      "=[value] for percent transported last month" },
	{ MATCH_INDUSTRY_PERCENT_THIS,        "thispercent",      0, FOR_INDUSTRY,
	                                      "=[value] for percent transported this month" },

};

/** Invalid commands */
const StringInfo<VehicleCommand>  INVALID_COMMAND_VEHICLE  = { VEHICLE_INVALID_COMMAND, "", 0, 0, "" };
const StringInfo<TownCommand>     INVALID_COMMAND_TOWN     = { TOWN_INVALID_COMMAND, "", 0, 0, "" };
const StringInfo<IndustryCommand> INVALID_COMMAND_INDUSTRY = { INDUSTRY_INVALID_COMMAND, "", 0, 0, "" };

/** Invalid match */
const StringInfo<MatchType> INVALID_MATCH = { MATCH_INVALID, "", 0, 0, "" };

/**
 * Change a string into its monetary representation.
 * @param *value the variable a successfull conversion will be put in
 * @param *arg the string to be converted
 * @return Return true on success or false on failure
 */
bool GetArgumentMoney(Money *value, const char *arg)
{
	//TODO: perform conversion between currencies
	char *endptr;

	*value = strtoull(arg, &endptr, 0);
	return arg != endptr;
}

/**
 Generic numeric match template subroutine,
 compare original value with target value using given compare type
 @param value Original value
 @param subtype Type of match
 @param target_value Value to compare with
 @return true in succesful match, false otherwise
*/
template<class X> bool NumericMatch (const X value, MatchSubtype subtype, const X target_value)
{
	switch (subtype) {
		case MATCH_EQUAL: return (value == target_value);
		case MATCH_NOT_EQUAL: return (value != target_value);
		case MATCH_LESS: return (value < target_value);
		case MATCH_LESS_OR_EQUAL: return (value <= target_value);
		case MATCH_GREATER_OR_EQUAL: return (value >= target_value);
		case MATCH_GREATER: return (value > target_value);
		default: NOT_REACHED(); break;
	}
	return false;
}

/**
 Perform numeric match, compare original value with target value using given compare type
 @param value Original value
 @param subtype Type of match
 @param target_value_str Value to compare with, in string form
*/
bool NumericValueSubMatch (uint32 value, MatchSubtype subtype, const char *target_value_str)
{
	uint32 target_value;
	if (!GetArgumentInteger(&target_value, target_value_str)) return false;
	return NumericMatch(value, subtype, target_value);
}

/**
 Perform money match, compare original value with target value using given compare type
 @param value Original value
 @param subtype Type of match
 @param target_value_str Value to compare with, in string form
*/
bool MoneyValueSubMatch (Money value, MatchSubtype subtype, const char *target_value_str)
{
	Money target_value;
	if (!GetArgumentMoney(&target_value, target_value_str)) return false;
	return NumericMatch(value, subtype, target_value);
}

/**
 Perform lexicographical case insensitive string match,
 compare original value with target value using given compare type
 @param value Original value
 @param subtype Type of match
 @param target_value_str Value to compare with
*/
bool StringValueSubMatch (const char *value, MatchSubtype subtype, const char *target_value)
{
	int res = strcasecmp(value, target_value);
	return NumericMatch(res, subtype, 0);
}

/**
 Return number of wagons in train.
 Engine is algo counted as wagon and for multi-part wagons or engines, each part is counted.
 @param v Train to examine
*/
int CountWagons(const Vehicle *v) {
	int num = 0;
	while (v) {
		num++;
		v = v->Next();
	}
	return num;
}

/**
 Check if given vehicle matches, considering given match type, subtype and ID
 @param v Vehicle to check
 @param m MatchInfo to match
*/
bool VehicleMatches(const Vehicle *v, MatchInfo *m)
{
	if (m->next) {
		//Next match in chain
		if (!VehicleMatches(v, m->next)) return false;
	}
	switch (m->type) {
		case MATCH_ALL: return true;
		case MATCH_CRASHED: return (v->vehstatus & VS_CRASHED) == VS_CRASHED;
		case MATCH_BROKEN: return (v->breakdown_ctr != 0);
		case MATCH_IN_DEPOT: return v->IsInDepot();
		case MATCH_SERVICE: return NumericValueSubMatch(v->service_interval, m->subtype, m->id);
		case MATCH_SPEED: return NumericValueSubMatch(v->cur_speed, m->subtype, m->id);
		case MATCH_ORDERS: return NumericValueSubMatch(v->GetNumOrders(), m->subtype, m->id);
		case MATCH_AGE: return NumericValueSubMatch(v->age / 365, m->subtype, m->id);
		case MATCH_BREAKDOWNS: return NumericValueSubMatch(v->breakdowns_since_last_service, m->subtype, m->id);
		case MATCH_MAXSPEED: {
			if (v->type == VEH_TRAIN) return NumericValueSubMatch(Train::From(v)->vcache.cached_max_speed, m->subtype, m->id);
			else return NumericValueSubMatch(v->vcache.cached_max_speed, m->subtype, m->id);
		}
		case MATCH_LENGTH: return NumericValueSubMatch((Train::From(v)->gcache.cached_total_length + 15) / 16, m->subtype, m->id);
		case MATCH_WAGONS: {
			assert(v->type == VEH_TRAIN);
			int num_wagons = CountWagons(v);
			return NumericValueSubMatch(num_wagons, m->subtype, m->id);
		}
		case MATCH_GENERIC: return NumericValueSubMatch(v->unitnumber, MATCH_EQUAL, m->id);
		case MATCH_PROFIT: return MoneyValueSubMatch(v->profit_this_year + v->profit_last_year, m->subtype, m->id);
		case MATCH_PROFIT_THIS: return MoneyValueSubMatch(v->profit_this_year, m->subtype, m->id);
		case MATCH_PROFIT_LAST: return MoneyValueSubMatch(v->profit_last_year, m->subtype, m->id);
		case MATCH_GROUP: {
			if (!Group::IsValidID(v->group_id)) return false; //No group
			char buf[ICON_MAX_STREAMSIZE];

			//Get string (name) from group
			const Group *g = Group::Get(v->group_id);
			assert(g);
			SetDParam(0, g->index);
			GetString(buf, STR_GROUP_NAME, lastof(buf));

			return StringValueSubMatch(buf, m->subtype, m->id);
		}
		default: NOT_REACHED(); break;
	}
	return false;
}

/**
 Get name of given town
 @param t town to query
*/
const char *TownName(const Town *t) {
	static const int len = 64;
	static char town_name[len];

	SetDParam(0, t->index);
	GetString(town_name, STR_TOWN_NAME, &town_name[len - 1]);
	return town_name;
}

/**
 Check if given town matches, considering given match type, subtype and ID
 @param v Town to check
 @param m MatchInfo to match
*/
bool TownMatches(const Town *t, MatchInfo *m)
{
	if (m->next) {
		//Next match in chain
		if (!TownMatches(t, m->next)) return false;
	}
	bool have_company = Company::IsValidID(_local_company);
	switch (m->type) {
		case MATCH_TOWN_POPULATION: return NumericValueSubMatch(t->cache.population, m->subtype, m->id);
		case MATCH_TOWN_HOUSES: return NumericValueSubMatch(t->cache.num_houses, m->subtype, m->id);
		case MATCH_TOWN_RATING: {
			//Does make sense only if own company exists
			if (!Company::IsValidID(_local_company)) return false;
			return NumericValueSubMatch(t->ratings[_local_company], m->subtype, m->id);
		}
		case MATCH_TOWN_NOISE: return NumericValueSubMatch(t->noise_reached, m->subtype, m->id);
		case MATCH_TOWN_NOISE_REMAIN: return NumericValueSubMatch(t->MaxTownNoise()-t->noise_reached, m->subtype, m->id);
		case MATCH_TOWN_NOISE_MAX: return NumericValueSubMatch(t->MaxTownNoise(), m->subtype, m->id);
		case MATCH_TOWN_FUNDING: return NumericValueSubMatch(t->fund_buildings_months, m->subtype, m->id);
		case MATCH_TOWN_ROADWORKS: return NumericValueSubMatch(t->road_build_months, m->subtype, m->id);
		case MATCH_TOWN_EXCLUSIVE_COMPANY: {
			if (!t->exclusive_counter) return false;
			return NumericValueSubMatch(t->exclusivity, m->subtype, m->id);
		}
		case MATCH_TOWN_EXCLUSIVE_MONTHS: {
			return NumericValueSubMatch(t->exclusive_counter, m->subtype, m->id);
		}
		case MATCH_TOWN_EXCLUSIVE_MY_MONTHS: {
			if (!have_company) return false;
			if (t->exclusivity != _local_company) return false;
			return NumericValueSubMatch(t->exclusive_counter, m->subtype, m->id);
		}
		case MATCH_TOWN_EXCLUSIVE_OTHERS_MONTHS: {
			if (!have_company) return false;
			if (t->exclusivity == _local_company) return false;
			if (t->exclusivity == INVALID_COMPANY) return false;
			return NumericValueSubMatch(t->exclusive_counter, m->subtype, m->id);
		}
		case MATCH_TOWN_STATUE: {
			if (!have_company) return false;
			return HasBit(t->statues, _local_company);
		}
		case MATCH_TOWN_NO_STATUE: {
			if (!have_company) return false;
			return !(HasBit(t->statues, _local_company));
		}
		case MATCH_TOWN_UNWANTED_MONTHS: {
			if (!have_company) return false;
			return NumericValueSubMatch(t->unwanted[_local_company], m->subtype, m->id);
		}
		case MATCH_GENERIC: {
			int n_id = atoi(m->id);
			return (stricmp(TownName(t), m->id) == 0) || (t->index == n_id);
		}
		case MATCH_ALL: return true;
		default: NOT_REACHED(); break;
	}
	return false;
}

/**
 Check if given industry matches, considering given match type, subtype and ID
 @param i Industry to check
 @param m MatchInfo to match
*/
bool IndustryMatches(const Industry *i, MatchInfo *m)
{
	if (m->next) {
		//Next match in chain
		if (!IndustryMatches(i, m->next)) return false;
	}
	assert(i->town);
	switch (m->type) {
		case MATCH_GENERIC: {
			int n_id = atoi(m->id);
			return (stricmp(TownName(i->town), m->id) == 0) || (i->index == n_id);
		}
		case MATCH_ALL: return true;
		case MATCH_INDUSTRY_PRODUCTION: {
			int production = i->last_month_production[0] + i->last_month_production[1];
			return NumericValueSubMatch(production, m->subtype, m->id);
		}
		case MATCH_INDUSTRY_PERCENT: {
			int production = i->last_month_production[0] + i->last_month_production[1];
			int transport = i->last_month_transported[0] + i->last_month_transported[1];
			return NumericValueSubMatch(production?(transport*100/production):0, m->subtype, m->id);
		}
		case MATCH_INDUSTRY_PRODUCTION_THIS: {
			int production = i->this_month_production[0] + i->this_month_production[1];
			return NumericValueSubMatch(production, m->subtype, m->id);
		}
		case MATCH_INDUSTRY_PERCENT_THIS: {
			int production = i->this_month_production[0] + i->this_month_production[1];
			int transport = i->this_month_transported[0] + i->this_month_transported[1];
			return NumericValueSubMatch(production?(transport*100/production):0, m->subtype, m->id);
		}
		default: NOT_REACHED(); break;
	}
	return false;
}

/**
 Perform command on given town
 @param v Target town of command
 @param command Command to make
 @param argc Number of extra parameters to command
 @param argv Extra command parameters, if any
*/
int DoTownCommand(Town *t, TownCommand command, int argc, char **argv)
{
	switch (command) {
		// Count towns
		case TOWN_COUNT: return 1;
		case TOWN_CENTER: {
			ScrollMainWindowToTile(t->xy);
			return 1;
		}
		case TOWN_PRINT: {
			IConsolePrintF(CC_DEFAULT, "%-20s  (%d)", TownName(t), t->cache.population);
			return 1;
		}
		case TOWN_INFO: {
			IConsolePrintF(CC_DEFAULT, "ID: %4d %-20s, population: %4d houses: %4d%s", t->index, TownName(t), t->cache.population, t->cache.num_houses, t->larger_town?" (Larger town)":"");
			const char *layout_str = "?";
			switch(t->layout) {
				case TL_ORIGINAL: layout_str = "original"; break;
				case TL_BETTER_ROADS: layout_str = "better roads"; break;
				case TL_2X2_GRID: layout_str = "2x2"; break;
				case TL_3X3_GRID: layout_str = "3x3"; break;
				case TL_RANDOM: layout_str = "random"; break;
				default: break; //Unknown
			};
			IConsolePrintF(CC_DEFAULT, "  Noise: %d/%d, Road layout: %s", t->noise_reached, t->MaxTownNoise(), layout_str);
			if (t->fund_buildings_months) IConsolePrintF(CC_DEFAULT, "  Fund buildings : %d months.", t->fund_buildings_months);
			if (t->road_build_months) IConsolePrintF(CC_DEFAULT, " Road reconstruction : %d months.", t->road_build_months);

			const Company *c;
			FOR_ALL_COMPANIES(c) {
				int i = c->index;
				if ((HasBit(t->have_ratings, i) || t->exclusivity == i || HasBit(t->statues, i))) {
					IConsolePrintF(CC_DEFAULT, " Company %2d : rating %d%s%s%s", i, t->ratings[i],
					               (t->exclusivity == i)?" (EXCLUSIVE)":"", t->unwanted[i]?" (UNWANTED)":"", HasBit(t->statues, i)?" (STATUE)":"");
					if (t->exclusivity == i) IConsolePrintF(CC_DEFAULT, "  Exclusivity expires in %d months", t->exclusive_counter);
					if (t->unwanted[i]) IConsolePrintF(CC_DEFAULT, "  Unwanted for %d months", t->unwanted[i]);
				}
			}
			return 1;
		}
		case TOWN_OPEN: {
			ShowTownViewWindow(t->index);
			return 1;
		}
		case TOWN_OPEN_AUTH: {
			ShowTownAuthorityWindow(t->index);
			return 1;
		}
		case TOWN_EXPAND: {
			//assert(_game_mode == GM_EDITOR);
			int rep = 1;
			if (argc) rep=atoi(argv[0]);
			for (int i = 0;i < rep; i++) GrowTown(t);
			return 1;
		}
		case TOWN_DELETE: {
			//assert(_game_mode == GM_EDITOR);
			delete t;
			return 1;
		}
		case TOWN_ACTION_AD_SMALL:
		case TOWN_ACTION_AD_MEDIUM:
		case TOWN_ACTION_AD_LARGE:
		case TOWN_ACTION_ROAD:
		case TOWN_ACTION_STATUE:
		case TOWN_ACTION_FUND:
		case TOWN_ACTION_EXCLUSIVE:
		case TOWN_ACTION_BRIBE:
		{
			DoCommandP(t->xy, t->index, command - TOWN_ACTION_0, CMD_DO_TOWN_ACTION | CMD_MSG(STR_ERROR_CAN_T_DO_THIS));
			return 1;
		}
		default: NOT_REACHED(); break;
	}
	return 0;
}

void ShowIndustryViewWindow(int industry);

/**
 Perform command on given industry
 @param v Target industry of command
 @param command Command to make
 @param argc Number of extra parameters to command
 @param argv Extra command parameters, if any
*/
int DoIndustryCommand(const Industry *i, IndustryCommand command, int argc, char **argv)
{
	switch (command) {
		// Count industries
		case INDUSTRY_COUNT: return 1;
		// Center view on vehicle
		case INDUSTRY_CENTER: {
			ScrollMainWindowToTile(i->location.tile);
			return 1;
		}
		case INDUSTRY_INFO: {
			//General information
			IConsolePrintF(CC_DEFAULT, "ID: %d Town: %-20s", i->index, TownName(i->town));
			IConsolePrintF(CC_DEFAULT, "  Size: %d x %d", i->location.w, i->location.h);
			char cargo_name[512];
			//Produced cargo details
			for (int cp = 0; cp < 2; cp++) {
				if (i->produced_cargo[cp] == CT_INVALID) continue;
				const CargoSpec *cs = CargoSpec::Get(i->produced_cargo[cp]);
				GetString(cargo_name, cs->name, lastof(cargo_name));
				IConsolePrintF(CC_DEFAULT, "  Cargo produced: %s (%d per month, %d waiting)", cargo_name, i->production_rate[cp], i->produced_cargo_waiting[cp]);
				int this_prod = i->this_month_production[cp];
				int this_tran = i->this_month_transported[cp];
				int last_prod = i->last_month_production[cp];
				int last_tran = i->last_month_transported[cp];
				IConsolePrintF(CC_DEFAULT, "    This month transported/produced: %d/%d (%d%%)", this_tran, this_prod, this_prod?(this_tran*100/this_prod):0);
				IConsolePrintF(CC_DEFAULT, "    Last month transported/produced: %d/%d (%d%%)", last_tran, last_prod, last_prod?(last_tran*100/last_prod):0);
			}
			//Accepted cargo details
			IConsolePrintF(CC_DEFAULT, "  General production level: %d", i->prod_level);
			for (int ca = 0; ca < 3; ca++) {
				if (i->accepts_cargo[ca] == CT_INVALID) continue;
				const CargoSpec *cs = CargoSpec::Get(i->accepts_cargo[ca]);
				GetString(cargo_name, cs->name, lastof(cargo_name));
				IConsolePrintF(CC_DEFAULT, "  Cargo accepted: %s (waiting %d)", cargo_name, i->incoming_cargo_waiting[ca]);
			}
			return 1;
		}
		//Open window with industry
		case INDUSTRY_OPEN: {
			ShowIndustryViewWindow(i->index);
			return 1;
		}
		// Delete the industry
		case INDUSTRY_DELETE: {
			//General information
			IConsolePrintF(CC_DEFAULT, "ID: %d Town: %-20s", i->index, TownName(i->town));
			IConsolePrintF(CC_DEFAULT, "  Size: %d x %d", i->location.w, i->location.h);
			delete i;
			return 1;
		}
		default: NOT_REACHED(); break;
	}
	return 0;
}

/**
 Perform command on given vehicle
 @param v Target vehicle of command
 @param command Command to make
 @param argc Number of extra parameters to command
 @param argv Extra command parameters, if any
*/
int DoVehicleCommand(const Vehicle *v, VehicleCommand command, int argc, char **argv)
{
	int cmd_code = 0;
	int32 num_orders = 1;
	switch (command) {
		// Count vehicles
		case VEHICLE_COUNT: return 1;
		// Open vehicle window
		case VEHICLE_OPEN: {
			ShowVehicleViewWindow(v);
			return 1;
		}
		// Set service interval
		case VEHICLE_INTERVAL: {
			int32 new_interval;
			assert(argc);
			if (!GetArgumentSignedInteger(&new_interval, argv[0])) return 0;
			new_interval = GetServiceIntervalClamped(new_interval, v->owner);
			if (new_interval == v->service_interval) return 0; // No change
			DoCommandP(v->tile, v->index, new_interval, CMD_CHANGE_SERVICE_INT | CMD_MSG(STR_ERROR_CAN_T_CHANGE_SERVICING));
			return 1;
		}
		// Center view on vehicle
		case VEHICLE_CENTER: {
			ScrollMainWindowTo(v->x_pos, v->y_pos);
			return 1;
		}
		// Print train wagon info in console
		case TRAIN_WAGON_INFO: {
			assert(v->type == VEH_TRAIN);
			IConsolePrintF(CC_DEFAULT, "Train #%4d wagons", v->unitnumber);
			const Train *w = Train::From(v);
			int i = 0;
			while (w) {
				int cargo = w->cargo_type;
				char cargo_name[512];
				i++;
				const CargoSpec *cs = CargoSpec::Get(cargo);
				GetString(cargo_name, cs->name, lastof(cargo_name));

				IConsolePrintF(CC_DEFAULT, "%2d,  Cargo capacity: %d (%s),  Max speed: %d km/h %s", i, w->cargo_cap, cargo_name, w->vcache.cached_max_speed, w->IsWagon() ? "" : " (engine)");
				w = w->Next();
			}
			return 1;
		}
		// Print vehicle info in console
		case VEHICLE_INFO: {
			IConsolePrintF(CC_DEFAULT, "#%4d, Location: [%d, %d, %d]%s%s%s%s", v->unitnumber, v->x_pos, v->y_pos, v->z_pos,
					(v->vehstatus & VS_STOPPED) ? " (STOPPED)" : "",
					(v->vehstatus & VS_CRASHED) ? " (CRASHED)" : "",
					(v->breakdown_ctr != 0) ? " (BROKEN)" : "",
					v->IsInDepot() ? " (IN DEPOT)" : "");
			IConsolePrintF(CC_DEFAULT, "      Age: %d/%d years", v->age / 365, v->max_age / 365);
			if (v->type == VEH_TRAIN) {
				const Train *tr_v = Train::From(v);
				IConsolePrintF(CC_DEFAULT, "      Speed: %d/%d km/h, Orders: %d", v->cur_speed, tr_v->vcache.cached_max_speed, v->GetNumOrders());
				IConsolePrintF(CC_DEFAULT, "      Length: %d tiles, Power: %d hp,  Weight: %d t", (tr_v->gcache.cached_total_length+15)/16, tr_v->gcache.cached_power, tr_v->gcache.cached_weight);
			} else {
				int speed_factor = 1;
				if (v->type != VEH_AIRCRAFT) speed_factor = 2;
				IConsolePrintF(CC_DEFAULT, "      Speed: %d/%d km/h, Orders: %d", v->cur_speed / speed_factor, v->vcache.cached_max_speed / speed_factor, v->GetNumOrders());
			}
			IConsolePrintF(CC_DEFAULT, "      Service interval: %d days/%%, Breakdowns: %d (reliability %d%%)", v->service_interval, v->breakdowns_since_last_service, (100 * (v->reliability>>8) >> 8));
			return 1;
		}
		// Skip to next order(s)
		case VEHICLE_SKIP_ORDER: {
			if (argc) {
				if (tolower(argv[0][0]) == 'r') num_orders = InteractiveRandom(); // Modulo later will correct this
				else if (!GetArgumentSignedInteger(&num_orders, argv[0])) num_orders = 1;
			}
			//No break or return here, fall through
		}
		// Skip to next order if vehicle is stopped in station
		// (Or fall through from generic skip order)
		case VEHICLE_LEAVE_STATION: {
			if (command == VEHICLE_LEAVE_STATION && v->current_order.GetType() != OT_LOADING) return 0;
			if (num_orders == 0) return 0; // Skip 0 orders
			int new_order = (v->current_order.index + num_orders) % v->GetNumOrders();
			if (new_order < 0) new_order = v->GetNumOrders(); //If skipped before first, go to last
			assert (new_order >= 0);
			DoCommandP(v->tile, v->index, new_order, CMD_SKIP_TO_ORDER | CMD_MSG(STR_ERROR_CAN_T_SKIP_ORDER));
			return 1;
		}
		// Ignore signals
		case TRAIN_IGNORE: {
			DoCommandP(v->tile, v->index, 0, CMD_FORCE_TRAIN_PROCEED | CMD_MSG(STR_ERROR_CAN_T_MAKE_TRAIN_PASS_SIGNAL));
			return 1;
		}
		// Turn vehicle around
		case VEHICLE_TURN: {
			switch (v->type) {
				case VEH_TRAIN:    cmd_code = CMD_REVERSE_TRAIN_DIRECTION | CMD_MSG(STR_ERROR_CAN_T_REVERSE_DIRECTION_TRAIN); break;
				case VEH_ROAD:     cmd_code = CMD_TURN_ROADVEH | CMD_MSG(STR_ERROR_CAN_T_MAKE_ROAD_VEHICLE_TURN); break;
				default: NOT_REACHED(); break;
			}
			DoCommandP(v->tile, v->index, 0, cmd_code);
			return 1;
		}
		// Stop vehicle
		case VEHICLE_STOP:
		// Start vehicle
		case VEHICLE_START: {
			if ((command == VEHICLE_STOP) && (v->vehstatus & VS_STOPPED)) return 0;
			if ((command == VEHICLE_START) && !(v->vehstatus & VS_STOPPED)) return 0;
			DoCommandP(v->tile, v->index, 0, CMD_START_STOP_VEHICLE);
			return 1;
		}
		// Send vehicle to depot
		case VEHICLE_DEPOT:
		// Send vehicle for servicing
		case VEHICLE_SERVICE:
		// Cancel sending vehicle to depot
		case VEHICLE_UNDEPOT:
		// Cancel sending vehicle for servicing
		case VEHICLE_UNSERVICE: {
			if ((v->vehstatus & VS_STOPPED) && v->IsInDepot()) return 0; //Already in depot
			if (v->current_order.IsType(OT_GOTO_DEPOT)) {
				//Already heading to a depot (either for service or for stopping)
				bool halt_in_depot = v->current_order.GetDepotActionType() & ODATFB_HALT;
				if (halt_in_depot) {
					if (command == VEHICLE_DEPOT) return 0;
					if (command == VEHICLE_UNSERVICE) return 0;
				} else {
					if (command == VEHICLE_UNDEPOT) return 0;
					if (command == VEHICLE_SERVICE) return 0;
				}
			} else {
				//Not heading to a depot at all - nothing to cancel
				if ((command == VEHICLE_UNDEPOT) || (command == VEHICLE_UNSERVICE)) {
					return 0;
				}
			}
			cmd_code = GetCmdSendToDepot(v);
			DoCommandP(v->tile, v->index, (command == VEHICLE_SERVICE || command == VEHICLE_UNSERVICE) ? DEPOT_SERVICE : 0, cmd_code);
			return 1;
		}
		// Clone vehicle
		case VEHICLE_CLONE:
		// Clone vehicle with shared orders
		case VEHICLE_CLONE_SHARED: {
			uint32 num_clones = 1;
			if (argc) if (!GetArgumentInteger(&num_clones, argv[0])) num_clones = 1;
			for (uint32 i = 0; i < num_clones; i++) DoCommandP(v->tile, v->index, (command == VEHICLE_CLONE_SHARED) ? 1 : 0, CMD_CLONE_VEHICLE);
			return 1;
		}
		// Sell one or more train wagons. Wagons are indexed from 1 (0 is head engine). Articulated parts are not counted
		case TRAIN_SELL_WAGON: {
			assert(v->type == VEH_TRAIN);
			assert(argc);
			uint32 min;
			if (!GetArgumentInteger(&min, argv[0])) return 0;
			uint32 max = min;
			if (argc >= 2) {
				if (!GetArgumentInteger(&max, argv[1])) return 0;
				if (max < min) return 0;
			}

			const Vehicle *w = v;

			// Note: if maximal number of wagons in train is raised, this should be raised too.
			// If neglecting, crashes will not happen, but it will be impossible to sell more
			// than 100 wagons at once
			VehicleID to_be_sold[100];
			uint num_to_sell = 0;

			const Train *tr_v = Train::From(v);
			for (uint i = 0; i <= max; i++) {
				// Skip articulated parts
				while (tr_v && tr_v->IsArticulatedPart()) tr_v = tr_v->Next();
				// End of train
				if (!tr_v) break;
				// Check if this is one to sell
				if (i >= min) {
					// Add to sell list
					to_be_sold[num_to_sell] = tr_v->index;
					num_to_sell;
					if (num_to_sell >= lengthof(to_be_sold)) break;
				}
				tr_v = tr_v->Next();
			}

			// Sell all vehicles in sell list
			for (uint i = 0; i < num_to_sell; i++) DoCommandP(w->tile, to_be_sold[i], 0, GetCmdSellVeh(VEH_TRAIN));
			return 1;
		}
		// Sell vehicle
		case VEHICLE_SELL: {
			cmd_code = GetCmdSellVeh(v);
			DoCommandP(v->tile, v->index, (v->type == VEH_TRAIN) ? 1 : 0, cmd_code);
			return 1;
		}
		default: NOT_REACHED(); break;
	}
	return 0;
}

/**
 Return true, if first string is a prefix (case insensitive) of second string
 @param s1 first string
 @param s2 second string
*/
bool str_isprefix(const char *s1, const char *s2)
{
	int len = 0;
	while (s1[len] && s2[len] && (tolower(s1[len]) == tolower(s2[len]))) len++;

	if (!s2[len] && s1[len]) return false; //Second string is prefix for first
	if (len>0 && !s1[len]) return true;    //First string is prefix for (or equal to) second
	return false;
}

/**
 Get command or match type based on it's ID or copy of invalid_value if ID not recognized
 @param id string to look for
 @param invalid_value what to return if match not found
 @param string_array where to look
 @param array_length length of array
*/
template<typename T> const StringInfo<T> GetStringInfo(const char *id,
 const StringInfo<T> &invalid_value, const StringInfo<T> *string_array, size_t array_length)
{
	StringInfo<T> cmd = invalid_value;
	bool unique_prefix = true;
	for (uint i = 0; i < array_length; i++) {
		int real_i = i;
		//Skip through alias(es) until reaching the command
		while (string_array[real_i].id == LIST_ALIAS) real_i++;
		if (stricmp(id, string_array[i].name) == 0) return string_array[real_i];
		/*
		 * If same command as already found, skip match
		 * (to avoid 'disambiguating' between 'center' and 'centre')
		 */
		if (str_isprefix(id, string_array[i].name) && string_array[real_i].id != cmd.id) {
			// Case-insensitive prefix match
			if (cmd.id) unique_prefix = false; //Ambiguous case-insensitive prefix match
			cmd = string_array[real_i];
			continue;
		}
	}
	if (cmd.id && unique_prefix) return cmd;
	return invalid_value;
}

/**
 Get vehicle command based on it's ID or INVALID_COMMAND_VEHICLE if ID not recognized
 @param id string to look for
*/
const StringInfo<VehicleCommand> GetVehicleCommand(const char *id) {
 return GetStringInfo<VehicleCommand>(id, INVALID_COMMAND_VEHICLE, veh_commands, lengthof(veh_commands));
}

/**
 Get town command based on it's ID or INVALID_COMMAND_TOWN if ID not recognized
 @param id string to look for
*/
const StringInfo<TownCommand> GetTownCommand(const char *id) {
 return GetStringInfo<TownCommand>(id, INVALID_COMMAND_TOWN, town_commands, lengthof(town_commands));
}

/**
 Get industry command based on it's ID or INVALID_COMMAND_INDUSTRY if ID not recognized
 @param id string to look for
*/
const StringInfo<IndustryCommand> GetIndustryCommand(const char *id) {
 return GetStringInfo<IndustryCommand>(id, INVALID_COMMAND_INDUSTRY, ind_commands, lengthof(ind_commands));
}

/**
 Get match type based on it's ID or INVALID_MATCH if ID not recognized
 @param id string to look for
*/
const StringInfo<MatchType> GetMatchType(const char *id) {
 return GetStringInfo<MatchType>(id, INVALID_MATCH, match_info, lengthof(match_info));
}

/**
 Given name of the group, return pointer to it, or return NULL if group is not found or is owned by someone else.
 First it tries to match case sensitively, if it fails,
 it tries case-insensitive match and return it only if the match is unambigous.
 For example, matching of 'XYZ' against group 'xyz' and 'Xyz' will fail.
 @param name Name of the group
*/
const Group* GetGroupByName(const char *name)
{
	const Group *g;
	const Group *nocase_g = NULL;
	const Group *prefix_g = NULL;
	bool unique_nocase = true;
	bool unique_prefix = true;
	char buf[512];

	FOR_ALL_GROUPS(g) {
		if (g->owner == _local_company) {
			SetDParam(0, g->index);
			GetString(buf, STR_GROUP_NAME, lastof(buf));
		}
		if (strcmp(buf, name) == 0) return g;           // Case-sensitive match
		if (stricmp(buf, name) == 0) {                  // Case-insensitive match
			if (nocase_g) unique_nocase = false;    // Ambiguous case-insensitive match
			nocase_g = g;
			continue;
		}
		if (str_isprefix(name, buf)) {                  // Case-insensitive prefix match
			if (prefix_g) unique_prefix = false;    //Ambiguous case-insensitive prefix match
			prefix_g = g;
			continue;
		}
	}

	if (nocase_g && unique_nocase) return nocase_g;
	if (prefix_g && unique_prefix) return prefix_g;
	return NULL;
}

/**
 Print generic help for type of matches in commands
 @param m_info Pointer to MatchInfo array
 @param m_len Number of matches in array
 @param target_type Name of the target for these matches
 @param mask Mask for filtering matches
*/
void ConMatchTypeHelp(const StringInfo<MatchType> *m_info, size_t m_len, const char *target_type, int mask) {
	for (uint mi = 0; mi < m_len; mi++) {
		if (!(m_info[mi].req & mask)) {
			// Not for this object type.
			continue;
		}
		if (m_info[mi].req & USE_PRINTF) {
			char buf[ICON_MAX_STREAMSIZE];
			seprintf(buf, lastof(buf), m_info[mi].help, target_type);
			IConsoleHelpF("  %s%s", m_info[mi].name, buf);
		} else {
			IConsoleHelpF("  %s%s", m_info[mi].name, m_info[mi].help);
		}
	}
}

/**
 Print generic help for town/industry/vehicle commands
 @param target_type Name of the target for these commands
 @param argv0 name of command for which the help is
 @param t_commands Pointer to array with Stringinfo<T> records for commandes
 @param num_commands Number of commands in array
 @param mask Mask for filtering commands
*/
template<typename T> void ConCommandsHelp(const char *target_type, const char *argv0,
 const StringInfo<T> *t_commands, size_t num_commands, int mask) {
	IConsoleHelpF("Invoke command on specified %s(s). Usage: '%s <identifier> <command> [<optional command parameters...>]'", target_type, argv0);
	IConsoleHelp("Command can be:");

	// Help for commands
	char alias[ICON_MAX_STREAMSIZE];
	alias[0] = 0;
	for (uint i = 0; i < num_commands; i++) {

		if (t_commands[i].id == LIST_ALIAS) {
			if (!alias[0]) strecpy(alias, " (Aliases: ", lastof(alias));
			else strecat(alias, ", ", lastof(alias));
			strecat(alias, t_commands[i].name, lastof(alias));
		} else {
			if (!(t_commands[i].req & mask)) {
				// Not for this object type. Reset list of aliases
				alias[0] = 0;
				continue;
			}
			if (alias[0]) {
				strecat(alias, ")", lastof(alias));
			}
			IConsoleHelpF("  %-15s %s%s", t_commands[i].name, t_commands[i].help, alias[0] ? alias : "");
			alias[0] = 0;
		}
	}

	IConsoleHelp ("Identifier can be:");
	// Help for non-numeric match types
	ConMatchTypeHelp(match_nn_info, lengthof(match_nn_info), target_type, mask);

	IConsoleHelp ("Operators < > <= >= and <> can be also used instead of = for following matches:");
	// Help for numeric match types
	ConMatchTypeHelp(match_info, lengthof(match_info), target_type, mask);

	IConsoleHelp ("You can specify multiple match conditions before the command.");
	IConsoleHelp ("If you use more than one match condition, you have to separate them by 'and' or '&' parameter. Number of match conditions is not limited.");
}

/**
 Checks match_id for known match types and subtypes.
 @param match_id match string
 @param mask match mask
 @return NULL, if invalid match is specified, pointer to MatchInfo if valid match found, or match is considered to be generic match
*/
MatchInfo* CheckMatch(const char *match_id, int mask) {

	//Default values
	MatchType match_type = MATCH_GENERIC;
	MatchSubtype match_subtype = MATCH_NONE;
	const char *id = match_id;

	// Check for criteria in form of key=value, key<value, key>=value, etc ...
	size_t keylen = strcspn(match_id, "<>=");
	if (match_id[keylen] == '=') {
		// Key=value
		id = match_id + keylen + 1;
		match_subtype = MATCH_EQUAL;
	} else if (match_id[keylen] == '<') {
		// Key<value or Key<=value
		id = match_id + keylen + 1;
		if (match_id[keylen+1] == '=') {
			match_subtype = MATCH_LESS_OR_EQUAL;
			id++;
		} else if (match_id[keylen+1] == '>') {
			match_subtype = MATCH_NOT_EQUAL;
			id++;
		} else {
			match_subtype = MATCH_LESS;
		}
	} else if (match_id[keylen] == '>') {
		// Key>value or Key>=value
		id = match_id + keylen + 1;
		if (match_id[keylen+1] == '=') {
			match_subtype = MATCH_GREATER_OR_EQUAL;
			id++;
		} else {
			match_subtype = MATCH_GREATER;
		}
	}

	if (keylen) {
		// Criteria in form of key=value, key<value, key>=value .... was specified
		char match_key[ICON_MAX_STREAMSIZE];
		strncpy(match_key, match_id, keylen);
		match_key[keylen] = 0;

		StringInfo<MatchType> match = GetMatchType(match_key);

		// Found some match
		if (match.id) {
			// Safety check for correct object type
			if (!(match.req & mask)) {
				IConsoleError("You have specified invalid match type for this query.");
				return NULL;
			}
			match_type = match.id;
		}
	}

	//Check for special match
	for (size_t i = 0;i < lengthof(match_nn_info);i++) {
		if (match_nn_info[i].req & mask) {
			if (stricmp(match_id, match_nn_info[i].name) == 0) {
				match_type = match_nn_info[i].id;
				break;
			}
		}
	}
	//If no special match is found, keep default generic match
	return new MatchInfo(match_type, match_subtype, id);
}

/**
 Checks argc+argv for known match types and subtypes.
 @param argc Argument count
 @param argv Arguments
 @param mask Match mask
 @return NULL, if invalid match is specified, pointer to MatchInfo if valid match found, or match is considered to be generic match
*/
MatchInfo* CheckMatch(byte &argc, char** &argv, int mask) {

	//Need at least <name> <match> <command>
	if (argc < 3) return NULL;

	//Skip name of command
	argv++;
	argc--;
	MatchInfo *m = NULL;
	while (argc) {
		MatchInfo *tm = CheckMatch(argv[0], mask);
		if (!tm) {
			if (m) delete m;
			return NULL;
		}
		tm->next = m;
		m = tm;
		argv++;
		argc--;
		if (!argc) break; //MAtch not followed by command
		if (!(stricmp(argv[0], "and") == 0 || stricmp(argv[0], "&") == 0)) break;
		//Match followed by "and"
		argv++;
		argc--;
	}
	return m;
}

/**
 Perform a town command
 @param argc Number of arguments
 @param argv Arguments
*/
DEF_CONSOLE_CMD(ConTown)
{
	int mask = FOR_TOWN;
	if (argc == 0) {
		ConCommandsHelp("town", "town", town_commands, lengthof(town_commands), mask);
		IConsoleHelpF("You can also use:");
		IConsoleHelpF(" name of town or ID of town");
		return true;
	}
	if (argc < 3) return false;

	MatchInfo *m = CheckMatch(argc, argv, mask);

	if (!m) return true;

	if (argc == 0) {
		//Missing command (CMD <match> and <match>)
		delete m;
		return false;
	}

	//Parse command string and get command identifier
	StringInfo<TownCommand> cmd = GetTownCommand(argv[0]);

	if (!cmd.id) {
		IConsoleError("You have specified invalid command.");
		return false;
	}

	if (argc < 1 + cmd.params) {
		IConsoleError("This command requires additional parameter(s).");
		return true;
	}

	if (_game_mode != GM_EDITOR && (cmd.req & IN_EDITOR)) {
		IConsoleError("This command can be used only in scenario editor");
		//return true;
	}

	assert(!(cmd.req & IS_ALIAS));

	int affected = 0;
	int matched = 0;

	//Loop through all towns
	Town *t;
	FOR_ALL_TOWNS(t) {
		if (TownMatches(t, m)) {
			// Town matches criteria
			matched++;
			// Pass rest of parameters to command
			affected += DoTownCommand(t, cmd.id, argc - 1, argv + 1);
		}
	}

	IConsolePrintF(CC_DEFAULT, "Number of towns matched: %d, affected: %d", matched, affected);
	delete m;

	return true;
}

/**
 Perform an industry command
 @param argc Number of arguments
 @param argv Arguments
*/
DEF_CONSOLE_CMD(ConIndustry)
{
	int mask = FOR_INDUSTRY;
	if (argc == 0) {
		ConCommandsHelp("industry", "industry", ind_commands, lengthof(ind_commands), mask);
		IConsoleHelpF("You can also use:");
		IConsoleHelpF(" name of town, to which the industry belongs, or ID of industry");
		return true;
	}
	if (argc < 3) return false;

	MatchInfo *m = CheckMatch(argc, argv, mask);

	if (!m) return true;

	if (argc == 0) {
		//Missing command (CMD <match> and <match>)
		delete m;
		return false;
	}

	//Parse command string and get command identifier
	StringInfo<IndustryCommand> cmd = GetIndustryCommand(argv[0]);

	if (!cmd.id) {
		IConsoleError("You have specified invalid command.");
		return false;
	}

	if (argc < 1 + cmd.params) {
		IConsoleError("This command requires additional parameter(s).");
		return true;
	}

	if (_game_mode != GM_EDITOR && (cmd.req & IN_EDITOR)) {
		IConsoleError("This command can be used only in scenario editor");
		//return true;
	}

	assert(!(cmd.req & IS_ALIAS));

	int affected = 0;
	int matched = 0;

	//Loop through all industries
	Industry *i;
	FOR_ALL_INDUSTRIES(i) {
		if (IndustryMatches(i, m)) {
			// Industry matches criteria
			matched++;
			// Pass rest of parameters to command
			affected = DoIndustryCommand(i, cmd.id, argc - 1, argv + 1);
		}
	}

	IConsolePrintF(CC_DEFAULT, "Number of industries matched: %d, affected: %d", matched, affected);

	return true;
}

/**
 Perform a vehicle command
 @param argc Number of arguments
 @param argv Arguments
 @param vtype Vehicle type to run this command against
 @param argv0 Name of the command (selfreference for printing help, etc ..)
*/
bool ConVehicleCommand(byte argc, char **argv, VehicleType vtype, const char *argv0)
{
	int mask;
	const char *vehicle_name;
	switch (vtype) {
		case VEH_TRAIN:    mask = FOR_TRAIN;    vehicle_name = "train";        break;
		case VEH_ROAD:     mask = FOR_ROAD;     vehicle_name = "road vehicle"; break;
		case VEH_SHIP:     mask = FOR_SHIP;     vehicle_name = "ship";         break;
		case VEH_AIRCRAFT: mask = FOR_AIRCRAFT; vehicle_name = "aircraft";     break;
		case VEH_INVALID:  mask = FOR_VEHICLE;  vehicle_name = "vehicle";      break;
		default: NOT_REACHED(); return false;
	}

	if (argc == 0) {
		ConCommandsHelp(vehicle_name, argv0, veh_commands, lengthof(veh_commands), mask);

		IConsoleHelpF("You can also use:");
		IConsoleHelpF(" name of group for all %ss from specified group. Can accept unique prefix of group name", vehicle_name);
		IConsoleHelpF(" %s number for specific %s", vehicle_name, vehicle_name);

		return true;
	}

	if (!Company::IsValidID(_local_company)) {
		IConsoleError("You have to own a company to make use of this command.");
		return true;
	}

	if (argc < 3) return false;

	MatchInfo *m = CheckMatch(argc, argv, mask);

	if (!m) return true;

	if (argc == 0) {
		//Missing command (CMD <match> and <match>)
		delete m;
		return false;
	}

	//Parse command string and get command identifier
	StringInfo<VehicleCommand> cmd = GetVehicleCommand(argv[0]);

	if (!cmd.id) {
		IConsoleError("You have specified invalid command.");
		return false;
	}

	if (argc < 1 + cmd.params) {
		IConsoleError("This command requires additional parameter(s).");
		return true;
	}

	// Safety check for correct vehicle type
	if (!(cmd.req & mask)) {
		IConsolePrintF(CC_ERROR, " ERROR: The command you have specified cannot be applied to %s.", vehicle_name);
		return true;
	}

	assert(!(cmd.req & IS_ALIAS));

	int affected = 0;
	int matched = 0;

	VehicleListIdentifier vli;
	VehicleList sort_list;

	int object_id = 0;
	int list_type = VL_STANDARD;

	//Check for generic matches and convert them to group matches if appropriate
	MatchInfo *mx = m;
	while (mx) {
		// Try matching group name for generic match
		if (mx->type == MATCH_GENERIC) {
			const Group *g = GetGroupByName(mx->id);
			if (g) {
				if (list_type == VL_STANDARD) {
					list_type = VL_GROUP_LIST;
					object_id = g->index;
					// Toggle to match all, as the input is already filtered by this group
					mx->type = MATCH_ALL;
				} else {
					mx->type = MATCH_GROUP;
				}
			}
		}
		mx = mx->next;
	}

	// Generate list of vehicles
	GenerateVehicleSortList(&sort_list, vli);
	//GenerateVehicleSortList(&sort_list, vtype, _local_company, object_id, list_type);
	uint list_len = sort_list.Length();

	for (uint vi = 0; vi < list_len; vi++) {  // For each vehicle in list
		const Vehicle *v = sort_list[vi];
		if (VehicleMatches(v, m)) {
			// Vehicle matches criteria
			matched++;

			// Check specific command requirements if necessary:

			// Check for "not crashed"
			if ((cmd.req & NOT_CRASHED) && (v->vehstatus & VS_CRASHED)) continue;
			// Check for "is stopped"
			if ((cmd.req & STOPPED) && !(v->vehstatus & VS_STOPPED)) continue;
			// Check for "is in depot"
			if ((cmd.req & IN_DEPOT) && !v->IsInDepot()) continue;

			 //Check vehicle type in case of commands for multiple vehicle types
			switch (v->type) {
				case VEH_TRAIN: if (!(cmd.req & FOR_TRAIN)) continue; break;
				case VEH_ROAD: if (!(cmd.req & FOR_ROAD)) continue; break;
				case VEH_SHIP: if (!(cmd.req & FOR_SHIP)) continue; break;
				case VEH_AIRCRAFT: if (!(cmd.req & FOR_AIRCRAFT)) continue; break;
				default: NOT_REACHED(); break;
			}
			// Pass rest of parameters to command
			affected = DoVehicleCommand(v, cmd.id, argc - 1, argv + 1);
		}
	}

	IConsolePrintF(CC_DEFAULT, "Number of %ss matched: %d, affected: %d", vehicle_name, matched, affected);

	return true;
}

DEF_CONSOLE_CMD(ConTrain)
{
	// Call generic vehicle command with "train" specialization
	return ConVehicleCommand(argc, argv, VEH_TRAIN, "train");
}

DEF_CONSOLE_CMD(ConRoad)
{
	// Call generic vehicle command with "road vehicle" specialization
	return ConVehicleCommand(argc, argv, VEH_ROAD, "road");
}

DEF_CONSOLE_CMD(ConShip)
{
	// Call generic vehicle command with "ship" specialization
	return ConVehicleCommand(argc, argv, VEH_SHIP, "ship");
}

DEF_CONSOLE_CMD(ConAircraft)
{
	// Call generic vehicle command with "aircraft" specialization
	return ConVehicleCommand(argc, argv, VEH_AIRCRAFT, "aircraft");
}

DEF_CONSOLE_CMD(ConVehicle)
{
	// Call generic vehicle command with "all vehicles" specialization
	return ConVehicleCommand(argc, argv, VEH_INVALID, "vehicle");
}

DEF_CONSOLE_CMD(ConListSettings)
{
	if (argc == 0) {
		IConsoleHelp("List settings. Usage: 'list_settings [<pre-filter>]'");
		return true;
	}

	if (argc > 2) return false;

	IConsoleListSettings((argc == 2) ? argv[1] : NULL);
	return true;
}

DEF_CONSOLE_CMD(ConGamelogPrint)
{
	GamelogPrintConsole();
	return true;
}

DEF_CONSOLE_CMD(ConNewGRFReload)
{
	if (argc == 0) {
		IConsoleHelp("Reloads all active NewGRFs from disk. Equivalent to reapplying NewGRFs via the settings, but without asking for confirmation. This might crash OpenTTD!");
		return true;
	}

	ReloadNewGRFData();

	extern void PostCheckNewGRFLoadWarnings();
	PostCheckNewGRFLoadWarnings();
	return true;
}

DEF_CONSOLE_CMD(ConResetBlockedHeliports)
{
	if (argc == 0) {
		IConsoleHelp("Resets heliports blocked by the improved breakdowns bug, for single-player use only.");
		return true;
	}

	unsigned int count = 0;
	Station *st;
	FOR_ALL_STATIONS(st) {
		if (st->airport.tile == INVALID_TILE) continue;
		if (st->airport.HasHangar()) continue;
		if (!st->airport.flags) continue;

		bool occupied = false;
		const Aircraft *a;
		FOR_ALL_AIRCRAFT(a) {
			if (a->targetairport == st->index && a->state != FLYING) {
				occupied = true;
				break;
			}
		}
		if (!occupied) {
			st->airport.flags = 0;
			count++;
			char buffer[256];
			SetDParam(0, st->index);
			GetString(buffer, STR_STATION_NAME, lastof(buffer));
			IConsolePrintF(CC_DEFAULT, "Unblocked: %s", buffer);
		}
	}

	IConsolePrintF(CC_DEFAULT, "Unblocked %u heliports", count);
	return true;
}

DEF_CONSOLE_CMD(ConDumpCommandLog)
{
	if (argc == 0) {
		IConsoleHelp("Dump log of recently executed commands.");
		return true;
	}

	char buffer[32768];
	DumpCommandLog(buffer, lastof(buffer));
	PrintLineByLine(buffer);
	return true;
}

DEF_CONSOLE_CMD(ConCheckCaches)
{
	if (argc == 0) {
		IConsoleHelp("Debug: Check caches");
		return true;
	}

	if (argc > 2) return false;

	bool broadcast = (argc == 2 && atoi(argv[1]) > 0 && (!_networking || _network_server));
	if (broadcast) {
		DoCommandP(0, 0, 0, CMD_DESYNC_CHECK);
	} else {
		extern void CheckCaches(bool force_check);
		CheckCaches(true);
	}

	return true;
}

/******************
 *  debug commands
 ******************/

static void IConsoleDebugLibRegister()
{
	IConsoleCmdRegister("resettile",        ConResetTile);
	IConsoleAliasRegister("dbg_echo",       "echo %A; echo %B");
	IConsoleAliasRegister("dbg_echo2",      "echo %!");
}

/*******************************
 * console command registration
 *******************************/

void IConsoleStdLibRegister()
{
	IConsoleCmdRegister("debug_level",  ConDebugLevel);
	IConsoleCmdRegister("echo",         ConEcho);
	IConsoleCmdRegister("echoc",        ConEchoC);
	IConsoleCmdRegister("exec",         ConExec);
	IConsoleCmdRegister("exit",         ConExit);
	IConsoleCmdRegister("part",         ConPart);
	IConsoleCmdRegister("help",         ConHelp);
	IConsoleCmdRegister("info_cmd",     ConInfoCmd);
	IConsoleCmdRegister("list_cmds",    ConListCommands);
	IConsoleCmdRegister("list_aliases", ConListAliases);
	IConsoleCmdRegister("newgame",      ConNewGame);
	IConsoleCmdRegister("restart",      ConRestart);
	IConsoleCmdRegister("getseed",      ConGetSeed);
	IConsoleCmdRegister("getdate",      ConGetDate);
	IConsoleCmdRegister("quit",         ConExit);
	IConsoleCmdRegister("resetengines", ConResetEngines, ConHookNoNetwork);
	IConsoleCmdRegister("reset_enginepool", ConResetEnginePool, ConHookNoNetwork);
	IConsoleCmdRegister("return",       ConReturn);
	IConsoleCmdRegister("screenshot",   ConScreenShot);
	IConsoleCmdRegister("minimap",      ConMinimap);
	IConsoleCmdRegister("script",       ConScript);
	IConsoleCmdRegister("scrollto",     ConScrollToTile);
	IConsoleCmdRegister("alias",        ConAlias);
	IConsoleCmdRegister("load",         ConLoad);
	IConsoleCmdRegister("rm",           ConRemove);
	IConsoleCmdRegister("save",         ConSave);
	IConsoleCmdRegister("saveconfig",   ConSaveConfig);
	IConsoleCmdRegister("ls",           ConListFiles);
	IConsoleCmdRegister("open_cheats",  ConOpenCheats);
	IConsoleCmdRegister("cheats",       ConOpenCheats);
	IConsoleCmdRegister("cd",           ConChangeDirectory);
	IConsoleCmdRegister("pwd",          ConPrintWorkingDirectory);
	IConsoleCmdRegister("clear",        ConClearBuffer);
	IConsoleCmdRegister("setting",      ConSetting);
	IConsoleCmdRegister("setting_newgame", ConSettingNewgame);
	IConsoleCmdRegister("list_settings",ConListSettings);
	IConsoleCmdRegister("gamelog",      ConGamelogPrint);
	IConsoleCmdRegister("rescan_newgrf", ConRescanNewGRF);
	IConsoleCmdRegister("train",        ConTrain);
	IConsoleCmdRegister("aircraft",     ConAircraft);
	IConsoleCmdRegister("road",         ConRoad);
	IConsoleCmdRegister("ship",         ConShip);
	IConsoleCmdRegister("vehicle",      ConVehicle);
	IConsoleCmdRegister("industry",     ConIndustry);
	IConsoleCmdRegister("town",         ConTown);

	IConsoleAliasRegister("dir",          "ls");
	IConsoleAliasRegister("del",          "rm %+");
	IConsoleAliasRegister("newmap",       "newgame");
	IConsoleAliasRegister("patch",        "setting %+");
	IConsoleAliasRegister("set",          "setting %+");
	IConsoleAliasRegister("set_newgame",  "setting_newgame %+");
	IConsoleAliasRegister("list_patches", "list_settings %+");
	IConsoleAliasRegister("plane",        "aircraft %+");
	IConsoleAliasRegister("developer",    "setting developer %+");

	IConsoleCmdRegister("list_ai_libs", ConListAILibs);
	IConsoleCmdRegister("list_ai",      ConListAI);
	IConsoleCmdRegister("reload_ai",    ConReloadAI);
	IConsoleCmdRegister("rescan_ai",    ConRescanAI);
	IConsoleCmdRegister("start_ai",     ConStartAI);
	IConsoleCmdRegister("stop_ai",      ConStopAI);

	IConsoleCmdRegister("list_game",    ConListGame);
	IConsoleCmdRegister("list_game_libs", ConListGameLibs);
	IConsoleCmdRegister("rescan_game",    ConRescanGame);

	IConsoleCmdRegister("companies",       ConCompanies);
	IConsoleAliasRegister("players",       "companies");

	/* networking functions */
#ifdef ENABLE_NETWORK
/* Content downloading is only available with ZLIB */
#if defined(WITH_ZLIB)
	IConsoleCmdRegister("content",         ConContent);
#endif /* defined(WITH_ZLIB) */

	/*** Networking commands ***/
	IConsoleCmdRegister("say",             ConSay, ConHookNeedNetwork);
	IConsoleCmdRegister("say_company",     ConSayCompany, ConHookNeedNetwork);
	IConsoleAliasRegister("say_player",    "say_company %+");
	IConsoleCmdRegister("say_client",      ConSayClient, ConHookNeedNetwork);

	IConsoleCmdRegister("connect",         ConNetworkConnect, ConHookClientOnly);
	IConsoleCmdRegister("clients",         ConNetworkClients, ConHookNeedNetwork);
	IConsoleCmdRegister("status",          ConStatus, ConHookServerOnly);
	IConsoleCmdRegister("server_info",     ConServerInfo, ConHookServerOnly);
	IConsoleAliasRegister("info",          "server_info");
	IConsoleCmdRegister("reconnect",       ConNetworkReconnect, ConHookClientOnly);
	IConsoleCmdRegister("rcon",            ConRcon, ConHookNeedNetwork);

	IConsoleCmdRegister("join",            ConJoinCompany, ConHookNeedNetwork);
	IConsoleAliasRegister("spectate",      "join 255");
	IConsoleCmdRegister("move",            ConMoveClient, ConHookServerOnly);
	IConsoleCmdRegister("reset_company",   ConResetCompany, ConHookServerOnly);
	IConsoleAliasRegister("clean_company", "reset_company %A");
	IConsoleCmdRegister("client_name",     ConClientNickChange, ConHookServerOnly);
	IConsoleCmdRegister("kick",            ConKick, ConHookServerOnly);
	IConsoleCmdRegister("ban",             ConBan, ConHookServerOnly);
	IConsoleCmdRegister("unban",           ConUnBan, ConHookServerOnly);
	IConsoleCmdRegister("banlist",         ConBanList, ConHookServerOnly);

	IConsoleCmdRegister("pause",           ConPauseGame, ConHookServerOnly);
	IConsoleCmdRegister("unpause",         ConUnpauseGame, ConHookServerOnly);

	IConsoleCmdRegister("company_pw",      ConCompanyPassword, ConHookNeedNetwork);
	IConsoleAliasRegister("company_password",      "company_pw %+");

	IConsoleAliasRegister("net_frame_freq",        "setting frame_freq %+");
	IConsoleAliasRegister("net_sync_freq",         "setting sync_freq %+");
	IConsoleAliasRegister("server_pw",             "setting server_password %+");
	IConsoleAliasRegister("server_password",       "setting server_password %+");
	IConsoleAliasRegister("rcon_pw",               "setting rcon_password %+");
	IConsoleAliasRegister("rcon_password",         "setting rcon_password %+");
	IConsoleAliasRegister("name",                  "setting client_name %+");
	IConsoleAliasRegister("server_name",           "setting server_name %+");
	IConsoleAliasRegister("server_port",           "setting server_port %+");
	IConsoleAliasRegister("server_advertise",      "setting server_advertise %+");
	IConsoleAliasRegister("max_clients",           "setting max_clients %+");
	IConsoleAliasRegister("max_companies",         "setting max_companies %+");
	IConsoleAliasRegister("max_spectators",        "setting max_spectators %+");
	IConsoleAliasRegister("max_join_time",         "setting max_join_time %+");
	IConsoleAliasRegister("pause_on_join",         "setting pause_on_join %+");
	IConsoleAliasRegister("autoclean_companies",   "setting autoclean_companies %+");
	IConsoleAliasRegister("autoclean_protected",   "setting autoclean_protected %+");
	IConsoleAliasRegister("autoclean_unprotected", "setting autoclean_unprotected %+");
	IConsoleAliasRegister("restart_game_year",     "setting restart_game_year %+");
	IConsoleAliasRegister("min_players",           "setting min_active_clients %+");
	IConsoleAliasRegister("reload_cfg",            "setting reload_cfg %+");
#endif /* ENABLE_NETWORK */

	/* debugging stuff */
	IConsoleDebugLibRegister();
	IConsoleCmdRegister("dump_command_log", ConDumpCommandLog, nullptr, true);
	IConsoleCmdRegister("check_caches", ConCheckCaches, nullptr, true);

	/* NewGRF development stuff */
	IConsoleCmdRegister("reload_newgrfs",  ConNewGRFReload, ConHookNewGRFDeveloperTool);

	/* Bug workarounds */
	IConsoleCmdRegister("jgrpp_bug_workaround_unblock_heliports", ConResetBlockedHeliports, ConHookNoNetwork, true);
}
