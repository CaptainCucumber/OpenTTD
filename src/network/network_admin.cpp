/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file network_admin.cpp Server part of the admin network protocol. */

#include "../stdafx.h"
#include "../strings_func.h"
#include "../timer/timer_game_calendar.h"
#include "../timer/timer_game_calendar.h"
#include "core/network_game_info.h"
#include "network_admin.h"
#include "network_base.h"
#include "network_server.h"
#include "../command_func.h"
#include "../company_base.h"
#include "../company_func.h"
#include "../engine_base.h"
#include "../engine_func.h"
#include "../industry.h"
#include "../industry_map.h"
#include "../tile_cmd.h"
#include "../newgrf_config.h"
#include "../station_base.h"
#include "../station_func.h"
#include "../depot_base.h"
#include "../depot_map.h"
#include "../vehicle_base.h"
#include "../station_map.h"
#include "../copilot_cmd.h"
#include "../copilot_observation.h"
#include "../console_func.h"
#include "../core/backup_type.hpp"
#include "../core/pool_func.hpp"
#include "../map_func.h"
#include "../openttd.h"
#include "../rail_map.h"
#include "../rail.h"
#include "../rail_type.h"
#include "../rev.h"
#include "../road_map.h"
#include "../road_type.h"
#include "../roadstop_base.h"
#include "../roadveh.h"
#include "../town.h"
#include "../town_map.h"
#include "../articulated_vehicles.h"
#include "../tile_map.h"
#include "../track_func.h"
#include "../game/game.hpp"
#include "../3rdparty/nlohmann/json.hpp"

#include "table/strings.h"

#include "../safeguards.h"


/* This file handles all the admin network commands. */

/** Redirection of the (remote) console to the admin. */
AdminID _redirect_console_to_admin = AdminID::Invalid();

/** The pool with sockets/clients. */
NetworkAdminSocketPool _networkadminsocket_pool("NetworkAdminSocket");
INSTANTIATE_POOL_METHODS(NetworkAdminSocket)

static NetworkAuthenticationDefaultPasswordProvider _admin_password_provider(_settings_client.network.admin_password); ///< Provides the password validation for the game's password.
static NetworkAuthenticationDefaultAuthorizedKeyHandler _admin_authorized_key_handler(_settings_client.network.admin_authorized_keys); ///< Provides the authorized key handling for the game authentication.

/** The timeout for authorisation of the client. */
static const std::chrono::seconds ADMIN_AUTHORISATION_TIMEOUT(10);

/** Version of the deliberately small OpenTTD Copilot Admin extension. */
static constexpr uint16_t COPILOT_ADMIN_VERSION = 3;
static constexpr size_t COPILOT_WORLD_ID_LENGTH = 65;

enum CopilotAdminRequest : uint8_t {
	COPILOT_ADMIN_IDENTITY = 0,
	COPILOT_ADMIN_BUILD_SINGLE_RAIL = 1,
	COPILOT_ADMIN_OPERATION_STATUS = 2,
	COPILOT_ADMIN_OBSERVATIONS = 3,
	COPILOT_ADMIN_TILES = 4,
	COPILOT_ADMIN_INDUSTRIES = 5,
	COPILOT_ADMIN_RAIL_CATALOG = 6,
	COPILOT_ADMIN_RAIL_ASSETS = 7,
	COPILOT_ADMIN_TRAIN_PARTS = 8,
	COPILOT_ADMIN_TRAIN_ORDERS = 9,
	COPILOT_ADMIN_INDUSTRY_DETAILS = 10,
	COPILOT_ADMIN_BUILD_INFRASTRUCTURE = 11,
	COPILOT_ADMIN_INFRASTRUCTURE_STATUS = 12,
	COPILOT_ADMIN_BUILD_TRAIN = 13,
	COPILOT_ADMIN_TRAIN_PURCHASE_STATUS = 14,
	COPILOT_ADMIN_CONFIGURE_TRAIN = 15,
	COPILOT_ADMIN_TRAIN_CONFIGURATION_STATUS = 16,
	COPILOT_ADMIN_CONSTRUCTION_QUOTE = 17,
	COPILOT_ADMIN_BUS = 18,
};

enum CopilotAdminResult : uint8_t {
	COPILOT_ADMIN_OK = 0,
	COPILOT_ADMIN_UNSUPPORTED_VERSION = 1,
	COPILOT_ADMIN_UNSUPPORTED_REQUEST = 2,
	COPILOT_ADMIN_COMPANY_NOT_FOUND = 3,
	COPILOT_ADMIN_LOOPBACK_REQUIRED = 4,
	COPILOT_ADMIN_WORLD_MISMATCH = 5,
	COPILOT_ADMIN_COMPANY_NOT_HUMAN = 6,
	COPILOT_ADMIN_INVALID_ARGUMENT = 7,
	COPILOT_ADMIN_OPERATION_CONFLICT = 8,
	COPILOT_ADMIN_OPERATION_BUSY = 9,
	COPILOT_ADMIN_OPERATION_NOT_FOUND = 10,
	COPILOT_ADMIN_STALE_POSITION = 11,
	COPILOT_ADMIN_JOURNAL_FULL = 12,
};

static constexpr size_t COPILOT_ADMIN_INDUSTRY_NAME_MAX_BYTES = 96;


/** Frequencies, which may be registered for a certain update type. */
static const AdminUpdateFrequencies _admin_update_type_frequencies[] = {
	{AdminUpdateFrequency::Poll, AdminUpdateFrequency::Daily, AdminUpdateFrequency::Weekly, AdminUpdateFrequency::Monthly, AdminUpdateFrequency::Quarterly, AdminUpdateFrequency::Annually}, // ADMIN_UPDATE_DATE
	{AdminUpdateFrequency::Poll, AdminUpdateFrequency::Automatic,                                                                                                                         }, // ADMIN_UPDATE_CLIENT_INFO
	{AdminUpdateFrequency::Poll, AdminUpdateFrequency::Automatic,                                                                                                                         }, // ADMIN_UPDATE_COMPANY_INFO
	{AdminUpdateFrequency::Poll,                              AdminUpdateFrequency::Weekly, AdminUpdateFrequency::Monthly, AdminUpdateFrequency::Quarterly, AdminUpdateFrequency::Annually}, // ADMIN_UPDATE_COMPANY_ECONOMY
	{AdminUpdateFrequency::Poll,                              AdminUpdateFrequency::Weekly, AdminUpdateFrequency::Monthly, AdminUpdateFrequency::Quarterly, AdminUpdateFrequency::Annually}, // ADMIN_UPDATE_COMPANY_STATS
	{                            AdminUpdateFrequency::Automatic,                                                                                                                         }, // ADMIN_UPDATE_CHAT
	{                            AdminUpdateFrequency::Automatic,                                                                                                                         }, // ADMIN_UPDATE_CONSOLE
	{AdminUpdateFrequency::Poll,                                                                                                                                                          }, // ADMIN_UPDATE_CMD_NAMES
	{                            AdminUpdateFrequency::Automatic,                                                                                                                         }, // ADMIN_UPDATE_CMD_LOGGING
	{                            AdminUpdateFrequency::Automatic,                                                                                                                         }, // ADMIN_UPDATE_GAMESCRIPT
};
/** Sanity check. */
static_assert(lengthof(_admin_update_type_frequencies) == ADMIN_UPDATE_END);

/**
 * Create a new socket for the server side of the admin network.
 * @param s The socket to connect with.
 */
ServerNetworkAdminSocketHandler::ServerNetworkAdminSocketHandler(SOCKET s) : NetworkAdminSocketHandler(s)
{
	this->status = ADMIN_STATUS_INACTIVE;
	this->connect_time = std::chrono::steady_clock::now();
}

/**
 * Clear everything related to this admin.
 */
ServerNetworkAdminSocketHandler::~ServerNetworkAdminSocketHandler()
{
	Debug(net, 3, "[admin] '{}' ({}) has disconnected", this->admin_name, this->admin_version);
	if (_redirect_console_to_admin == this->index) _redirect_console_to_admin = AdminID::Invalid();

	if (this->update_frequency[ADMIN_UPDATE_CONSOLE].Test(AdminUpdateFrequency::Automatic)) {
		this->update_frequency[ADMIN_UPDATE_CONSOLE] = {};
		DebugReconsiderSendRemoteMessages();
	}
}

/**
 * Whether a connection is allowed or not at this moment.
 * @return Whether the connection is allowed.
 */
/* static */ bool ServerNetworkAdminSocketHandler::AllowConnection()
{
	return _settings_client.network.AdminAuthenticationConfigured() && ServerNetworkAdminSocketHandler::CanAllocateItem();
}

/** Send the packets for the server sockets. */
/* static */ void ServerNetworkAdminSocketHandler::Send()
{
	for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::Iterate()) {
		if (as->status <= ADMIN_STATUS_AUTHENTICATE && std::chrono::steady_clock::now() > as->connect_time + ADMIN_AUTHORISATION_TIMEOUT) {
			Debug(net, 2, "[admin] Admin did not send its authorisation within {} seconds", std::chrono::duration_cast<std::chrono::seconds>(ADMIN_AUTHORISATION_TIMEOUT).count());
			as->CloseConnection(true);
			continue;
		}
		if (as->writable) {
			as->SendPackets();
		}
	}
}

/**
 * Handle the acceptance of a connection.
 * @param s The socket of the new connection.
 * @param address The address of the peer.
 */
/* static */ void ServerNetworkAdminSocketHandler::AcceptConnection(SOCKET s, const NetworkAddress &address)
{
	ServerNetworkAdminSocketHandler *as = new ServerNetworkAdminSocketHandler(s);
	as->address = address; // Save the IP of the client
}

/***********
 * Sending functions for admin network
 ************/

/**
 * Send an error to the admin.
 * @param error The error to send.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendError(NetworkErrorCode error)
{
	/* Whatever the error might be, authentication (keys) must be released as soon as possible. */
	this->authentication_handler = nullptr;

	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_ERROR);

	p->Send_uint8(error);
	this->SendPacket(std::move(p));

	std::string error_message = GetString(GetNetworkErrorMsg(error));

	Debug(net, 1, "[admin] The admin '{}' ({}) made an error and has been disconnected: '{}'", this->admin_name, this->admin_version, error_message);

	return this->CloseConnection(true);
}

/** Send the protocol version to the admin. */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendProtocol()
{
	this->status = ADMIN_STATUS_ACTIVE;

	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_PROTOCOL);

	/* announce the protocol version */
	p->Send_uint8(NETWORK_GAME_ADMIN_VERSION);

	for (int i = 0; i < ADMIN_UPDATE_END; i++) {
		p->Send_bool  (true);
		p->Send_uint16(i);
		p->Send_uint16(_admin_update_type_frequencies[i].base());
	}

	p->Send_bool(false);
	this->SendPacket(std::move(p));

	return this->SendWelcome();
}

/** Send a welcome message to the admin. */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendWelcome()
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_WELCOME);

	p->Send_string(_settings_client.network.server_name);
	p->Send_string(GetNetworkRevisionString());
	p->Send_bool  (_network_dedicated);

	p->Send_string(""); // Used to be map-name.
	p->Send_uint32(_settings_game.game_creation.generation_seed);
	p->Send_uint8 (to_underlying(_settings_game.game_creation.landscape));
	p->Send_uint32(TimerGameCalendar::ConvertYMDToDate(_settings_game.game_creation.starting_year, 0, 1).base());
	p->Send_uint16(Map::SizeX());
	p->Send_uint16(Map::SizeY());

	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/** Tell the admin we started a new game. */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendNewGame()
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_NEWGAME);
	this->SendPacket(std::move(p));
	return NETWORK_RECV_STATUS_OKAY;
}

/** Tell the admin we're shutting down. */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendShutdown()
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_SHUTDOWN);
	this->SendPacket(std::move(p));
	return NETWORK_RECV_STATUS_OKAY;
}

/** Tell the admin the date. */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendDate()
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_DATE);

	p->Send_uint32(TimerGameCalendar::date.base());
	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Tell the admin that a client joined.
 * @param client_id The client that joined.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendClientJoin(ClientID client_id)
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_CLIENT_JOIN);

	p->Send_uint32(client_id);
	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Send an initial set of data from some client's information.
 * @param cs The socket of the client.
 * @param ci The information about the client.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendClientInfo(const NetworkClientSocket *cs, const NetworkClientInfo *ci)
{
	/* Only send data when we're a proper client, not just someone trying to query the server. */
	if (ci == nullptr) return NETWORK_RECV_STATUS_OKAY;

	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_CLIENT_INFO);

	p->Send_uint32(ci->client_id);
	p->Send_string(cs == nullptr ? "" : const_cast<NetworkAddress &>(cs->client_address).GetHostname());
	p->Send_string(ci->client_name);
	p->Send_uint8 (0); // Used to be language
	p->Send_uint32(ci->join_date.base());
	p->Send_uint8 (ci->client_playas);

	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}


/**
 * Send an update for some client's information.
 * @param ci The information about a client.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendClientUpdate(const NetworkClientInfo *ci)
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_CLIENT_UPDATE);

	p->Send_uint32(ci->client_id);
	p->Send_string(ci->client_name);
	p->Send_uint8 (ci->client_playas);

	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Tell the admin that a client quit.
 * @param client_id The client that quit.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendClientQuit(ClientID client_id)
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_CLIENT_QUIT);

	p->Send_uint32(client_id);
	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Tell the admin that a client made an error.
 * @param client_id The client that made the error.
 * @param error The error that was made.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendClientError(ClientID client_id, NetworkErrorCode error)
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_CLIENT_ERROR);

	p->Send_uint32(client_id);
	p->Send_uint8 (error);
	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Tell the admin that a new company was founded.
 * @param company_id The company that was founded.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendCompanyNew(CompanyID company_id)
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_COMPANY_NEW);
	p->Send_uint8(company_id);

	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Send the admin some information about a company.
 * @param c The company to send the information about.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendCompanyInfo(const Company *c)
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_COMPANY_INFO);

	p->Send_uint8 (c->index);
	p->Send_string(GetString(STR_COMPANY_NAME, c->index));
	p->Send_string(GetString(STR_PRESIDENT_NAME, c->index));
	p->Send_uint8 (c->colour);
	p->Send_bool  (true);
	p->Send_uint32(c->inaugurated_year.base());
	p->Send_bool  (c->is_ai);
	p->Send_uint8 (CeilDiv(c->months_of_bankruptcy, 3)); // send as quarters_of_bankruptcy

	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}


/**
 * Send an update about a company.
 * @param c The company to send the update of.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendCompanyUpdate(const Company *c)
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_COMPANY_UPDATE);

	p->Send_uint8 (c->index);
	p->Send_string(GetString(STR_COMPANY_NAME, c->index));
	p->Send_string(GetString(STR_PRESIDENT_NAME, c->index));
	p->Send_uint8 (c->colour);
	p->Send_bool  (true);
	p->Send_uint8 (CeilDiv(c->months_of_bankruptcy, 3)); // send as quarters_of_bankruptcy

	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Tell the admin that a company got removed.
 * @param company_id The company that got removed.
 * @param acrr The reason for removal, e.g. bankruptcy or merger.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendCompanyRemove(CompanyID company_id, AdminCompanyRemoveReason acrr)
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_COMPANY_REMOVE);

	p->Send_uint8(company_id);
	p->Send_uint8(acrr);

	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/** Send economic information of all companies. */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendCompanyEconomy()
{
	for (const Company *company : Company::Iterate()) {
		/* Get the income. */
		Money income = -std::reduce(std::begin(company->yearly_expenses[0]), std::end(company->yearly_expenses[0]));

		auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_COMPANY_ECONOMY);

		p->Send_uint8(company->index);

		/* Current information. */
		p->Send_uint64(company->money);
		p->Send_uint64(company->current_loan);
		p->Send_uint64(income);
		p->Send_uint16(static_cast<uint16_t>(std::min<uint64_t>(UINT16_MAX, company->cur_economy.delivered_cargo.GetSum<OverflowSafeInt64>())));

		/* Send stats for the last 2 quarters. */
		for (uint i = 0; i < 2; i++) {
			p->Send_uint64(company->old_economy[i].company_value);
			p->Send_uint16(company->old_economy[i].performance_history);
			p->Send_uint16(static_cast<uint16_t>(std::min<uint64_t>(UINT16_MAX, company->old_economy[i].delivered_cargo.GetSum<OverflowSafeInt64>())));
		}

		this->SendPacket(std::move(p));
	}


	return NETWORK_RECV_STATUS_OKAY;
}

/** Send statistics about the companies. */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendCompanyStats()
{
	/* Fetch the latest version of the stats. */
	NetworkCompanyStatsArray company_stats = NetworkGetCompanyStats();

	/* Go through all the companies. */
	for (const Company *company : Company::Iterate()) {
		auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_COMPANY_STATS);

		/* Send the information. */
		p->Send_uint8(company->index);

		for (uint i = 0; i < NETWORK_VEH_END; i++) {
			p->Send_uint16(company_stats[company->index].num_vehicle[i]);
		}

		for (uint i = 0; i < NETWORK_VEH_END; i++) {
			p->Send_uint16(company_stats[company->index].num_station[i]);
		}

		this->SendPacket(std::move(p));
	}

	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Send a chat message.
 * @param action The action associated with the message.
 * @param desttype The destination type.
 * @param client_id The origin of the chat message.
 * @param msg The actual message.
 * @param data Arbitrary extra data.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendChat(NetworkAction action, DestType desttype, ClientID client_id, std::string_view msg, int64_t data)
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_CHAT);

	p->Send_uint8 (action);
	p->Send_uint8 (desttype);
	p->Send_uint32(client_id);
	p->Send_string(msg);
	p->Send_uint64(data);

	this->SendPacket(std::move(p));
	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Send a notification indicating the rcon command has completed.
 * @param command The original command sent.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendRconEnd(std::string_view command)
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_RCON_END);

	p->Send_string(command);
	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Send the reply of an rcon command.
 * @param colour The colour of the text.
 * @param result The result of the command.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendRcon(uint16_t colour, std::string_view result)
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_RCON);

	p->Send_uint16(colour);
	p->Send_string(result);
	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::Receive_ADMIN_RCON(Packet &p)
{
	if (this->status <= ADMIN_STATUS_AUTHENTICATE) return this->SendError(NETWORK_ERROR_NOT_EXPECTED);

	std::string command = p.Recv_string(NETWORK_RCONCOMMAND_LENGTH);

	Debug(net, 3, "[admin] Rcon command from '{}' ({}): {}", this->admin_name, this->admin_version, command);

	_redirect_console_to_admin = this->index;
	IConsoleCmdExec(command);
	_redirect_console_to_admin = AdminID::Invalid();
	return this->SendRconEnd(command);
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::Receive_ADMIN_GAMESCRIPT(Packet &p)
{
	if (this->status <= ADMIN_STATUS_AUTHENTICATE) return this->SendError(NETWORK_ERROR_NOT_EXPECTED);

	std::string json = p.Recv_string(NETWORK_GAMESCRIPT_JSON_LENGTH);

	Debug(net, 6, "[admin] GameScript JSON from '{}' ({}): {}", this->admin_name, this->admin_version, json);

	Game::NewEvent(new ScriptEventAdminPort(json));
	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::Receive_ADMIN_PING(Packet &p)
{
	if (this->status <= ADMIN_STATUS_AUTHENTICATE) return this->SendError(NETWORK_ERROR_NOT_EXPECTED);

	uint32_t d1 = p.Recv_uint32();

	Debug(net, 6, "[admin] Ping from '{}' ({}): {}", this->admin_name, this->admin_version, d1);

	return this->SendPong(d1);
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::SendCopilotIdentity(uint16_t requested_version, uint8_t request_kind, uint32_t request_id, CompanyID company_id)
{
	CopilotAdminResult result = COPILOT_ADMIN_OK;
	if (!this->address.IsInNetmask("127.0.0.0/8") && !this->address.IsInNetmask("::1/128")) {
		result = COPILOT_ADMIN_LOOPBACK_REQUIRED;
	} else if (requested_version != COPILOT_ADMIN_VERSION) {
		result = COPILOT_ADMIN_UNSUPPORTED_VERSION;
	} else if (request_kind != COPILOT_ADMIN_IDENTITY) {
		result = COPILOT_ADMIN_UNSUPPORTED_REQUEST;
	} else if (company_id != CompanyID::Invalid() && !Company::IsValidID(company_id)) {
		result = COPILOT_ADMIN_COMPANY_NOT_FOUND;
	}

	auto response = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_COPILOT);
	response->Send_uint16(COPILOT_ADMIN_VERSION);
	response->Send_uint8(request_kind);
	response->Send_uint32(request_id);
	response->Send_uint8(result);
	if (result != COPILOT_ADMIN_OK) {
		this->SendPacket(std::move(response));
		return NETWORK_RECV_STATUS_OKAY;
	}

	response->Send_string(_game_session_stats.savegame_id);
	response->Send_uint64(_copilot_operation.sequence);
	const CopilotOperation *head = GetCopilotHead();
	response->Send_uint64(head != nullptr ? head->operation_hi : 0);
	response->Send_uint64(head != nullptr ? head->operation_lo : 0);
	response->Send_uint32(_frame_counter);
	response->Send_uint32(_settings_game.game_creation.generation_seed);
	response->Send_uint32(TimerGameCalendar::date.base());
	response->Send_uint16(Map::SizeX());
	response->Send_uint16(Map::SizeY());
	response->Send_uint8(to_underlying(_settings_game.game_creation.landscape));

	uint8_t company_count = 0;
	for (const Company *company : Company::Iterate()) {
		if (company_id == CompanyID::Invalid() || company->index == company_id) company_count++;
	}
	response->Send_uint8(company_count);
	for (const Company *company : Company::Iterate()) {
		if (company_id != CompanyID::Invalid() && company->index != company_id) continue;
		response->Send_uint8(company->index);
		response->Send_bool(company->is_ai);
		response->Send_uint64(static_cast<uint64_t>(company->money));
		response->Send_uint64(static_cast<uint64_t>(company->current_loan));
		response->Send_string(GetString(STR_COMPANY_NAME, company->index));
	}

	this->SendPacket(std::move(response));
	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::SendCopilotOperation(uint8_t request_kind, uint32_t request_id, uint8_t raw_result, const CopilotOperation *operation)
{
	auto response = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_COPILOT);
	response->Send_uint16(COPILOT_ADMIN_VERSION);
	response->Send_uint8(request_kind);
	response->Send_uint32(request_id);
	response->Send_uint8(raw_result);
	if (raw_result != COPILOT_ADMIN_OK) {
		this->SendPacket(std::move(response));
		return NETWORK_RECV_STATUS_OKAY;
	}

	response->Send_string(_game_session_stats.savegame_id);
	response->Send_uint64(_copilot_operation.sequence);
	const CopilotOperation *head = GetCopilotHead();
	response->Send_uint64(head != nullptr ? head->operation_hi : 0);
	response->Send_uint64(head != nullptr ? head->operation_lo : 0);
	if (operation == nullptr) operation = &_copilot_operation;
	response->Send_uint8(operation->state);
	response->Send_uint64(operation->operation_hi);
	response->Send_uint64(operation->operation_lo);
	response->Send_uint64(operation->payload_hash_hi);
	response->Send_uint64(operation->payload_hash_lo);
	response->Send_uint8(operation->company);
	response->Send_uint32(operation->tile);
	response->Send_uint8(operation->rail_type);
	response->Send_uint8(operation->track);
	response->Send_uint64(static_cast<uint64_t>(operation->max_cost));
	response->Send_uint64(static_cast<uint64_t>(operation->actual_cost));
	response->Send_uint32(operation->error_message);
	response->Send_uint32(operation->completed_frame);

	TileIndex tile{operation->tile};
	bool valid_tile = tile != 0 && tile < Map::Size() && IsValidTile(tile);
	bool has_rail = valid_tile && (IsPlainRailTile(tile) || IsLevelCrossingTile(tile));
	TrackBits tracks = TRACK_BIT_NONE;
	RailType rail_type = INVALID_RAILTYPE;
	Owner owner = OWNER_NONE;
	if (has_rail) {
		tracks = IsLevelCrossingTile(tile) ? GetCrossingRailBits(tile) : GetTrackBits(tile);
		rail_type = GetRailType(tile);
		owner = GetTileOwner(tile);
	}
	response->Send_bool(valid_tile);
	response->Send_bool(has_rail);
	response->Send_uint8(owner.base());
	response->Send_uint8(to_underlying(tracks));
	response->Send_uint8(to_underlying(rail_type));

	const Company *company = Company::GetIfValid(CompanyID{operation->company});
	response->Send_bool(company != nullptr);
	response->Send_uint64(company == nullptr ? 0 : static_cast<uint64_t>(company->money));
	response->Send_uint64(company == nullptr ? 0 : static_cast<uint64_t>(company->current_loan));

	if (request_kind == COPILOT_ADMIN_BUILD_INFRASTRUCTURE || request_kind == COPILOT_ADMIN_INFRASTRUCTURE_STATUS ||
			request_kind == COPILOT_ADMIN_BUILD_TRAIN || request_kind == COPILOT_ADMIN_TRAIN_PURCHASE_STATUS ||
			request_kind == COPILOT_ADMIN_CONFIGURE_TRAIN || request_kind == COPILOT_ADMIN_TRAIN_CONFIGURATION_STATUS) {
		response->Send_uint8(operation->kind);
		response->Send_uint8(operation->length);
		response->Send_uint32(operation->created_asset_id);
	}
	if (request_kind == COPILOT_ADMIN_BUILD_TRAIN || request_kind == COPILOT_ADMIN_TRAIN_PURCHASE_STATUS ||
			request_kind == COPILOT_ADMIN_CONFIGURE_TRAIN || request_kind == COPILOT_ADMIN_TRAIN_CONFIGURATION_STATUS) {
		response->Send_uint16(operation->engine_id);
	}
	if (request_kind == COPILOT_ADMIN_CONFIGURE_TRAIN || request_kind == COPILOT_ADMIN_TRAIN_CONFIGURATION_STATUS) {
		response->Send_uint32(operation->vehicle_id);
		response->Send_uint32(operation->target_id);
		response->Send_uint16(operation->order_index);
		response->Send_uint8(operation->policy);
	}

	this->SendPacket(std::move(response));
	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::SendCopilotObservations(uint8_t request_kind, uint32_t request_id,
		uint8_t raw_result, CompanyID company_id, VehicleID vehicle_id, uint64_t cursor)
{
	auto response = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_COPILOT);
	response->Send_uint16(COPILOT_ADMIN_VERSION);
	response->Send_uint8(request_kind);
	response->Send_uint32(request_id);
	response->Send_uint8(raw_result);
	if (raw_result != COPILOT_ADMIN_OK) {
		this->SendPacket(std::move(response));
		return NETWORK_RECV_STATUS_OKAY;
	}

	CopilotObservationPage page = ReadCopilotObservations(company_id, vehicle_id, cursor);
	response->Send_string(_game_session_stats.savegame_id);
	response->Send_uint64(page.runtime_epoch);
	response->Send_uint64(page.oldest_sequence);
	response->Send_uint64(page.next_sequence);
	response->Send_uint32(_frame_counter);
	response->Send_bool(page.gap);
	response->Send_uint8(static_cast<uint8_t>(page.events.size()));
	for (const CopilotObservation &event : page.events) {
		response->Send_uint64(event.sequence);
		response->Send_uint8(to_underlying(event.kind));
		response->Send_uint32(event.frame);
		response->Send_uint32(event.vehicle.base());
		response->Send_uint8(event.company.base());
		response->Send_uint16(event.station.base());
		response->Send_uint8(event.cargo);
		response->Send_uint32(event.amount);
		response->Send_uint32(event.capacity);
		response->Send_uint16(event.industry.base());
	}
	this->SendPacket(std::move(response));
	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::SendCopilotTiles(uint8_t request_kind, uint32_t request_id,
		uint8_t raw_result, uint32_t start_tile, uint8_t count)
{
	auto response = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_COPILOT);
	response->Send_uint16(COPILOT_ADMIN_VERSION);
	response->Send_uint8(request_kind);
	response->Send_uint32(request_id);
	response->Send_uint8(raw_result);
	if (raw_result != COPILOT_ADMIN_OK) {
		this->SendPacket(std::move(response));
		return NETWORK_RECV_STATUS_OKAY;
	}

	response->Send_string(_game_session_stats.savegame_id);
	response->Send_uint32(_frame_counter);
	response->Send_uint32(start_tile);
	response->Send_uint8(count);
	for (uint32_t raw_tile = start_tile; raw_tile < start_tile + count; ++raw_tile) {
		TileIndex tile{raw_tile};
		bool valid = IsValidTile(tile);
		TileType type = valid ? GetTileType(tile) : MP_VOID;
		/* Houses and industries have no company owner; GetTileOwner asserts there. */
		Owner owner = valid && type != MP_HOUSE && type != MP_INDUSTRY ? GetTileOwner(tile) : OWNER_NONE;
		Slope slope = valid ? GetTileSlope(tile) : SLOPE_FLAT;
		uint height = valid ? TileHeight(tile) : 0;
		bool rail = valid && (IsPlainRailTile(tile) || IsLevelCrossingTile(tile));
		TrackBits tracks = rail ? (IsLevelCrossingTile(tile) ? GetCrossingRailBits(tile) : GetTrackBits(tile)) : TRACK_BIT_NONE;
		RailType rail_type = rail ? GetRailType(tile) : INVALID_RAILTYPE;
		response->Send_uint32(raw_tile);
		response->Send_bool(valid);
		response->Send_uint8(to_underlying(type));
		response->Send_uint8(owner.base());
		response->Send_uint8(to_underlying(slope));
		response->Send_uint8(height);
		response->Send_uint8(to_underlying(tracks));
		response->Send_uint8(to_underlying(rail_type));
	}
	this->SendPacket(std::move(response));
	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::SendCopilotIndustries(uint8_t request_kind, uint32_t request_id,
		uint8_t raw_result, uint32_t start_index, uint8_t requested_count)
{
	auto response = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_COPILOT);
	response->Send_uint16(COPILOT_ADMIN_VERSION);
	response->Send_uint8(request_kind);
	response->Send_uint32(request_id);
	response->Send_uint8(raw_result);
	if (raw_result != COPILOT_ADMIN_OK) {
		this->SendPacket(std::move(response));
		return NETWORK_RECV_STATUS_OKAY;
	}

	std::vector<const Industry *> page;
	uint32_t index = 0;
	for (const Industry *industry : Industry::Iterate()) {
		if (index++ < start_index) continue;
		if (page.size() == requested_count) break;
		page.push_back(industry);
	}
	response->Send_string(_game_session_stats.savegame_id);
	response->Send_uint32(_frame_counter);
	response->Send_uint32(start_index);
	response->Send_uint8(static_cast<uint8_t>(page.size()));
	for (const Industry *industry : page) {
		response->Send_uint16(industry->index.base());
		response->Send_uint8(industry->type);
		response->Send_uint32(industry->location.tile.base());
		uint8_t produced_count = 0;
		for (const auto &cargo : industry->produced) if (IsValidCargoType(cargo.cargo)) ++produced_count;
		response->Send_uint8(produced_count);
		for (const auto &cargo : industry->produced) if (IsValidCargoType(cargo.cargo)) response->Send_uint8(cargo.cargo);
		uint8_t accepted_count = 0;
		for (const auto &cargo : industry->accepted) if (IsValidCargoType(cargo.cargo)) ++accepted_count;
		response->Send_uint8(accepted_count);
		for (const auto &cargo : industry->accepted) if (IsValidCargoType(cargo.cargo)) response->Send_uint8(cargo.cargo);
	}
	this->SendPacket(std::move(response));
	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::SendCopilotIndustryDetails(uint8_t request_kind, uint32_t request_id,
		uint8_t raw_result, uint32_t start_index, uint8_t requested_count)
{
	auto response = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_COPILOT);
	response->Send_uint16(COPILOT_ADMIN_VERSION);
	response->Send_uint8(request_kind);
	response->Send_uint32(request_id);
	response->Send_uint8(raw_result);
	if (raw_result != COPILOT_ADMIN_OK) {
		this->SendPacket(std::move(response));
		return NETWORK_RECV_STATUS_OKAY;
	}

	std::vector<const Industry *> page;
	uint32_t index = 0;
	for (const Industry *industry : Industry::Iterate()) {
		if (index++ < start_index) continue;
		if (page.size() == requested_count) break;
		page.push_back(industry);
	}
	response->Send_string(_game_session_stats.savegame_id);
	response->Send_uint32(_frame_counter);
	response->Send_uint32(start_index);
	response->Send_uint8(static_cast<uint8_t>(page.size()));
	for (const Industry *industry : page) {
		const std::string &name = industry->GetCachedName();
		size_t name_length = std::min(name.size(), COPILOT_ADMIN_INDUSTRY_NAME_MAX_BYTES);
		const bool name_truncated = name.size() > COPILOT_ADMIN_INDUSTRY_NAME_MAX_BYTES;
		if (name_truncated) {
			while (name_length > 0 &&
					(static_cast<uint8_t>(name[name_length]) & 0xC0) == 0x80) {
				--name_length;
			}
		}
		response->Send_uint16(industry->index.base());
		response->Send_uint8(industry->type);
		response->Send_uint32(industry->location.tile.base());
		response->Send_uint16(industry->location.w);
		response->Send_uint16(industry->location.h);
		response->Send_string(std::string_view(name).substr(0, name_length));
		response->Send_bool(name_truncated);
		uint8_t produced_count = 0;
		for (const auto &cargo : industry->produced) if (IsValidCargoType(cargo.cargo)) ++produced_count;
		response->Send_uint8(produced_count);
		for (const auto &cargo : industry->produced) if (IsValidCargoType(cargo.cargo)) response->Send_uint8(cargo.cargo);
		uint8_t accepted_count = 0;
		for (const auto &cargo : industry->accepted) if (IsValidCargoType(cargo.cargo)) ++accepted_count;
		response->Send_uint8(accepted_count);
		for (const auto &cargo : industry->accepted) if (IsValidCargoType(cargo.cargo)) response->Send_uint8(cargo.cargo);
	}
	this->SendPacket(std::move(response));
	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::SendCopilotRailCatalog(uint8_t request_kind, uint32_t request_id,
		uint8_t raw_result, CompanyID company_id, uint32_t start_index, uint8_t requested_count)
{
	auto response = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_COPILOT);
	response->Send_uint16(COPILOT_ADMIN_VERSION);
	response->Send_uint8(request_kind);
	response->Send_uint32(request_id);
	response->Send_uint8(raw_result);
	if (raw_result != COPILOT_ADMIN_OK) {
		this->SendPacket(std::move(response));
		return NETWORK_RECV_STATUS_OKAY;
	}

	std::vector<const Engine *> page;
	uint32_t index = 0;
	for (const Engine *engine : Engine::IterateType(VEH_TRAIN)) {
		if (index++ < start_index) continue;
		if (page.size() == requested_count) break;
		page.push_back(engine);
	}
	response->Send_string(_game_session_stats.savegame_id);
	response->Send_uint32(_frame_counter);
	response->Send_uint8(company_id.base());
	response->Send_uint64(GetCompanyRailTypes(company_id, false).base());
	response->Send_uint32(start_index);
	response->Send_uint8(static_cast<uint8_t>(page.size()));
	for (const Engine *engine : page) {
		const RailVehicleInfo &rvi = engine->VehInfo<RailVehicleInfo>();
		response->Send_uint16(engine->index.base());
		response->Send_bool(IsEngineBuildable(engine->index, VEH_TRAIN, company_id));
		response->Send_uint8(rvi.railveh_type);
		response->Send_uint64(rvi.railtypes.base());
		response->Send_uint8(engine->GetDefaultCargoType());
		response->Send_uint32(engine->GetDisplayDefaultCapacity());
		response->Send_uint64(engine->info.refit_mask);
		response->Send_uint64(static_cast<uint64_t>(engine->GetCost()));
		response->Send_uint64(static_cast<uint64_t>(engine->GetRunningCost()));
		response->Send_uint16(engine->GetDisplayMaxSpeed());
		response->Send_uint16(engine->GetPower());
		response->Send_uint16(engine->GetDisplayWeight());
		response->Send_uint8(rvi.shorten_factor);
	}
	this->SendPacket(std::move(response));
	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::SendCopilotRailAssets(uint8_t request_kind, uint32_t request_id,
		uint8_t raw_result, CompanyID company_id, uint8_t asset_kind, uint32_t start_index, uint8_t requested_count)
{
	auto response = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_COPILOT);
	response->Send_uint16(COPILOT_ADMIN_VERSION);
	response->Send_uint8(request_kind);
	response->Send_uint32(request_id);
	response->Send_uint8(raw_result);
	if (raw_result != COPILOT_ADMIN_OK) {
		this->SendPacket(std::move(response));
		return NETWORK_RECV_STATUS_OKAY;
	}

	response->Send_string(_game_session_stats.savegame_id);
	response->Send_uint32(_frame_counter);
	response->Send_uint8(company_id.base());
	response->Send_uint8(asset_kind);
	response->Send_uint32(start_index);
	if (asset_kind == 0) {
		std::vector<const Station *> page;
		uint32_t index = 0;
		for (const Station *station : Station::Iterate()) {
			if (station->owner != company_id || !station->facilities.Test(StationFacility::Train)) continue;
			if (index++ < start_index) continue;
			if (page.size() == requested_count) break;
			page.push_back(station);
		}
		response->Send_uint8(static_cast<uint8_t>(page.size()));
		for (const Station *station : page) {
			response->Send_uint16(station->index.base());
			response->Send_uint32(station->xy.base());
			response->Send_uint32(station->train_station.tile.base());
			response->Send_uint16(station->train_station.w);
			response->Send_uint16(station->train_station.h);
			response->Send_uint64(GetAcceptanceMask(station));
		}
	} else {
		std::vector<const Depot *> page;
		uint32_t index = 0;
		for (const Depot *depot : Depot::Iterate()) {
			if (!IsRailDepotTile(depot->xy) || GetTileOwner(depot->xy) != company_id) continue;
			if (index++ < start_index) continue;
			if (page.size() == requested_count) break;
			page.push_back(depot);
		}
		response->Send_uint8(static_cast<uint8_t>(page.size()));
		for (const Depot *depot : page) {
			Tile tile = depot->xy;
			response->Send_uint16(depot->index.base());
			response->Send_uint32(depot->xy.base());
			response->Send_uint8(GetRailDepotDirection(tile));
			response->Send_uint8(GetRailType(tile));
		}
	}
	this->SendPacket(std::move(response));
	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::SendCopilotTrainParts(uint8_t request_kind, uint32_t request_id,
		uint8_t raw_result, CompanyID company_id, uint32_t after_vehicle_id, uint8_t max_records, uint16_t scan_budget)
{
	auto response = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_COPILOT);
	response->Send_uint16(COPILOT_ADMIN_VERSION);
	response->Send_uint8(request_kind);
	response->Send_uint32(request_id);
	response->Send_uint8(raw_result);
	if (raw_result != COPILOT_ADMIN_OK) {
		this->SendPacket(std::move(response));
		return NETWORK_RECV_STATUS_OKAY;
	}

	const size_t pool_size = Vehicle::GetPoolSize();
	const bool from_start = after_vehicle_id == UINT32_MAX;
	const size_t first_id = from_start ? 0 : static_cast<size_t>(after_vehicle_id) + 1;
	size_t scanned = 0;
	uint32_t next_after_id = from_start ? UINT32_MAX : after_vehicle_id;
	std::vector<const Vehicle *> page;
	page.reserve(max_records);
	for (size_t raw_id = first_id; raw_id < pool_size && scanned < scan_budget && page.size() < max_records; ++raw_id) {
		++scanned;
		next_after_id = static_cast<uint32_t>(raw_id);
		const Vehicle *vehicle = Vehicle::GetIfValid(VehicleID{static_cast<uint32_t>(raw_id)});
		if (vehicle == nullptr || vehicle->type != VEH_TRAIN || vehicle->owner != company_id) continue;
		page.push_back(vehicle);
	}
	const bool complete = first_id + scanned >= pool_size;

	response->Send_string(_game_session_stats.savegame_id);
	response->Send_uint32(_frame_counter);
	response->Send_uint8(company_id.base());
	response->Send_uint32(after_vehicle_id);
	response->Send_uint8(static_cast<uint8_t>(page.size()));
	response->Send_uint32(next_after_id);
	response->Send_bool(complete);
	for (const Vehicle *vehicle : page) {
		uint8_t state = 0;
		if (vehicle->IsPrimaryVehicle()) state |= 1 << 0;
		if (vehicle->vehstatus.Test(VehState::Stopped)) state |= 1 << 1;
		if (vehicle->vehstatus.Test(VehState::Crashed)) state |= 1 << 2;
		if (vehicle->IsInDepot()) state |= 1 << 3;
		response->Send_uint32(vehicle->index.base());
		response->Send_uint8(vehicle->owner.base());
		response->Send_uint32(vehicle->First()->index.base());
		response->Send_uint32(vehicle->Previous() == nullptr ? UINT32_MAX : vehicle->Previous()->index.base());
		response->Send_uint32(vehicle->Next() == nullptr ? UINT32_MAX : vehicle->Next()->index.base());
		response->Send_uint16(vehicle->engine_type.base());
		response->Send_uint16(vehicle->unitnumber);
		response->Send_uint32(vehicle->tile.base());
		response->Send_uint8(state);
		response->Send_uint8(IsValidCargoType(vehicle->cargo_type) ? vehicle->cargo_type : INVALID_CARGO);
		response->Send_uint32(vehicle->cargo.TotalCount());
		response->Send_uint16(vehicle->cargo_cap);
	}
	this->SendPacket(std::move(response));
	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::SendCopilotTrainOrders(uint8_t request_kind, uint32_t request_id,
		uint8_t raw_result, CompanyID company_id, VehicleID vehicle_id, uint16_t start_index,
		uint8_t requested_count)
{
	auto response = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_COPILOT);
	response->Send_uint16(COPILOT_ADMIN_VERSION);
	response->Send_uint8(request_kind);
	response->Send_uint32(request_id);
	response->Send_uint8(raw_result);
	if (raw_result != COPILOT_ADMIN_OK) {
		this->SendPacket(std::move(response));
		return NETWORK_RECV_STATUS_OKAY;
	}

	const Vehicle *vehicle = Vehicle::GetIfValid(vehicle_id);
	const auto orders = vehicle->Orders();
	const uint16_t total_orders = vehicle->GetNumOrders();
	const uint8_t count = static_cast<uint8_t>(
			std::min<size_t>(requested_count, orders.size() - start_index));
	response->Send_string(_game_session_stats.savegame_id);
	response->Send_uint32(_frame_counter);
	response->Send_uint8(company_id.base());
	response->Send_uint32(vehicle_id.base());
	response->Send_uint16(total_orders);
	response->Send_uint16(start_index);
	response->Send_uint8(count);
	for (uint16_t index = start_index; index < start_index + count; ++index) {
		const Order &order = orders[index];
		const bool station_order = order.IsType(OT_GOTO_STATION);
		response->Send_uint8(to_underlying(order.GetType()));
		response->Send_uint32(order.IsGotoOrder() ? order.GetDestination().base() : UINT32_MAX);
		response->Send_uint8(station_order ? to_underlying(order.GetLoadType()) : UINT8_MAX);
		response->Send_uint8(station_order ? to_underlying(order.GetUnloadType()) : UINT8_MAX);
		response->Send_uint8(order.IsGotoOrder() ? order.GetNonStopType().base() : UINT8_MAX);
	}
	this->SendPacket(std::move(response));
	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::SendCopilotBus(uint8_t request_kind, uint32_t request_id,
		uint8_t raw_result, std::string_view world_id, std::string_view json)
{
	auto response = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_COPILOT);
	response->Send_uint16(COPILOT_ADMIN_VERSION);
	response->Send_uint8(request_kind);
	response->Send_uint32(request_id);
	response->Send_uint8(raw_result);
	if (raw_result == COPILOT_ADMIN_OK) {
		response->Send_string(world_id);
		response->Send_string(json);
	}
	this->SendPacket(std::move(response));
	return NETWORK_RECV_STATUS_OKAY;
}

static uint8_t CopilotRoadBitsToWire(RoadBits bits)
{
	return (bits & ROAD_NE ? 0x01 : 0) | (bits & ROAD_SE ? 0x02 : 0) |
			(bits & ROAD_SW ? 0x04 : 0) | (bits & ROAD_NW ? 0x08 : 0);
}

static bool CopilotJSONUint(const nlohmann::json &request, const char *key, uint64_t maximum, uint64_t &value,
		uint64_t default_value, bool required = false)
{
	if (!request.contains(key)) {
		if (required) return false;
		value = default_value;
		return true;
	}
	const auto &field = request[key];
	if (!field.is_number_unsigned()) return false;
	value = field.get<uint64_t>();
	return value <= maximum;
}

static void CopilotBusPosition(nlohmann::json &out)
{
	const CopilotOperation *head = GetCopilotHead();
	out["sequence"] = _copilot_operation.sequence;
	out["head_hi"] = head != nullptr ? head->operation_hi : 0;
	out["head_lo"] = head != nullptr ? head->operation_lo : 0;
	out["head"] = head != nullptr ? nlohmann::json{{"hi", head->operation_hi}, {"lo", head->operation_lo}} :
			nlohmann::json(nullptr);
}

static nlohmann::json CopilotBusOperationJSON(const CopilotOperation &operation)
{
	static constexpr std::array<std::string_view, 5> states = {
		"empty", "pending", "completed", "rejected", "uncertain"
	};
	nlohmann::json out = {
		{"world_id", _game_session_stats.savegame_id},
		{"frame", _frame_counter},
		{"state", operation.state},
		{"state_name", operation.state < states.size() ? states[operation.state] : "invalid"},
		{"operation_hi", operation.operation_hi},
		{"operation_lo", operation.operation_lo},
		{"operation", {{"hi", operation.operation_hi}, {"lo", operation.operation_lo}}},
		{"payload_hash_hi", operation.payload_hash_hi},
		{"payload_hash_lo", operation.payload_hash_lo},
		{"payload_hash", {{"hi", operation.payload_hash_hi}, {"lo", operation.payload_hash_lo}}},
		{"company_id", operation.company},
		{"tile", operation.tile},
		{"kind", operation.kind},
		{"orientation", operation.kind == 7 ? 0 : operation.track},
		{"road_bits", operation.kind == 7 ? operation.track : 0},
		{"max_cost", operation.max_cost},
		{"actual_cost", operation.actual_cost},
		{"error", operation.error_message},
		{"completed_frame", operation.completed_frame},
		{"created_asset_id", operation.created_asset_id},
		{"engine_id", operation.engine_id},
		{"vehicle_id", operation.vehicle_id},
		{"target_id", operation.target_id},
		{"order_index", operation.order_index},
		{"policy", operation.policy},
		{"operation_sequence", operation.sequence},
	};
	CopilotBusPosition(out);
	return out;
}

static bool CopilotBusEngineSupported(const Engine *engine, CompanyID company)
{
	return engine != nullptr && IsEngineBuildable(engine->index, VEH_ROAD, company) &&
			engine->GetDefaultCargoType() == 0 &&
			!RoadTypeIsTram(engine->VehInfo<RoadVehicleInfo>().roadtype) &&
			!IsArticulatedEngine(engine->index);
}

static std::pair<uint32_t, uint32_t> CopilotTownCatchment(TileIndex tile, TownID town_id)
{
	CargoArray produced{};
	CargoArray accepted{};
	int radius = _settings_game.station.modified_catchment ? CA_BUS : CA_UNMODIFIED;
	TileArea area(tile, 1, 1);
	area.Expand(radius);
	for (TileIndex part : area) {
		if (!IsTileType(part, MP_HOUSE) || GetTownIndex(part) != town_id) continue;
		AddProducedCargo(part, produced);
		AddAcceptedCargo(part, accepted, nullptr);
	}
	return {accepted[0], produced[0]};
}

static bool CopilotBusSerialize(const nlohmann::json &out, std::string &encoded)
{
	encoded = out.dump();
	return encoded.size() <= 1400;
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::Receive_ADMIN_COPILOT(Packet &p)
{
	if (this->status <= ADMIN_STATUS_AUTHENTICATE) return this->SendError(NETWORK_ERROR_NOT_EXPECTED);
	if (p.RemainingBytesToTransfer() < sizeof(uint16_t) + sizeof(uint8_t) + sizeof(uint32_t)) {
		return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
	}

	uint16_t version = p.Recv_uint16();
	uint8_t request_kind = p.Recv_uint8();
	uint32_t request_id = p.Recv_uint32();
	if (!this->address.IsInNetmask("127.0.0.0/8") && !this->address.IsInNetmask("::1/128")) {
		return this->SendCopilotOperation(request_kind, request_id, COPILOT_ADMIN_LOOPBACK_REQUIRED);
	}
	if (version != COPILOT_ADMIN_VERSION) {
		return this->SendCopilotOperation(request_kind, request_id, COPILOT_ADMIN_UNSUPPORTED_VERSION);
	}

	if (request_kind == COPILOT_ADMIN_BUS) {
		std::string raw = p.Recv_string(1401);
		if (p.RemainingBytesToTransfer() != 0) return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
		nlohmann::json request;
		try {
			request = nlohmann::json::parse(raw);
		} catch (const nlohmann::json::exception &) {
			return this->SendCopilotBus(request_kind, request_id, COPILOT_ADMIN_INVALID_ARGUMENT);
		}
		if (!request.is_object() || !request.contains("action") || !request["action"].is_string() ||
				!request.contains("world_id") || !request["world_id"].is_string()) {
			return this->SendCopilotBus(request_kind, request_id, COPILOT_ADMIN_INVALID_ARGUMENT);
		}
		std::string action = request["action"].get<std::string>();
		std::string world_id = request["world_id"].get<std::string>();
		if (world_id.empty() || world_id != _game_session_stats.savegame_id) {
			return this->SendCopilotBus(request_kind, request_id, COPILOT_ADMIN_WORLD_MISMATCH);
		}
		uint64_t company_raw;
		if (!CopilotJSONUint(request, "company_id", UINT8_MAX, company_raw, 0)) {
			return this->SendCopilotBus(request_kind, request_id, COPILOT_ADMIN_INVALID_ARGUMENT);
		}
		CompanyID company_id{static_cast<uint8_t>(company_raw)};
		if (!Company::IsValidID(company_id)) {
			return this->SendCopilotBus(request_kind, request_id, COPILOT_ADMIN_COMPANY_NOT_FOUND);
		}
		if (Company::Get(company_id)->is_ai) {
			return this->SendCopilotBus(request_kind, request_id, COPILOT_ADMIN_COMPANY_NOT_HUMAN);
		}
		nlohmann::json out = {{"world_id", world_id}, {"frame", _frame_counter}};
		uint64_t start, count;
		if (action == "towns") {
			if (!CopilotJSONUint(request, "start", UINT32_MAX, start, 0) ||
					!CopilotJSONUint(request, "count", 8, count, 8) || count == 0) {
				return this->SendCopilotBus(request_kind, request_id, COPILOT_ADMIN_INVALID_ARGUMENT);
			}
			nlohmann::json records = nlohmann::json::array();
			uint32_t ordinal = 0;
			bool more = false;
			for (const Town *town : Town::Iterate()) {
				if (ordinal++ < start) continue;
				if (records.size() == count) { more = true; break; }
				records.push_back({{"town_id", town->index.base()}, {"tile", town->xy.base()},
						{"name", town->GetCachedName()}, {"population", town->cache.population}});
			}
			out["towns"] = records;
			out["count"] = records.size();
			out["next_start"] = start + records.size();
			out["complete"] = !more;
		} else if (action == "tiles") {
			if (!CopilotJSONUint(request, "start_tile", UINT32_MAX, start, 0, true) ||
					!CopilotJSONUint(request, "count", 8, count, 8) || count == 0 ||
					start >= Map::Size() || count > Map::Size() - start) {
				return this->SendCopilotBus(request_kind, request_id, COPILOT_ADMIN_INVALID_ARGUMENT);
			}
			nlohmann::json records = nlohmann::json::array();
			for (uint32_t raw_tile = start; raw_tile < start + count; ++raw_tile) {
				TileIndex tile{raw_tile};
				bool valid = IsValidTile(tile);
				TileType type = valid ? GetTileType(tile) : MP_VOID;
				bool normal_road = valid && IsNormalRoadTile(tile);
				bool bus_stop = valid && IsDriveThroughStopTile(tile) && GetRoadStopType(tile) == RoadStopType::Bus;
				Owner owner = valid && type != MP_HOUSE && type != MP_INDUSTRY ? GetTileOwner(tile) : OWNER_NONE;
				nlohmann::json record = {
					{"tile", raw_tile}, {"valid", valid}, {"tile_type", to_underlying(type)},
					{"owner", owner.base()}, {"slope", valid ? to_underlying(GetTileSlope(tile)) : 0},
					{"height", valid ? TileHeight(tile) : 0},
					{"road_bits", normal_road || bus_stop ? CopilotRoadBitsToWire(GetAnyRoadBits(tile, RTT_ROAD)) : 0},
					{"road_type", normal_road || bus_stop ? to_underlying(GetRoadTypeRoad(tile)) : UINT8_MAX},
					{"normal_road", normal_road},
					{"one_way", normal_road ? to_underlying(GetDisallowedRoadDirections(tile)) : 0},
				};
				if (valid && type == MP_HOUSE) record["town_id"] = GetTownIndex(tile).base();
				records.push_back(std::move(record));
			}
			out["tiles"] = records;
			out["count"] = records.size();
		} else if (action == "catalog") {
			if (!CopilotJSONUint(request, "start", UINT32_MAX, start, 0) ||
					!CopilotJSONUint(request, "count", 8, count, 8) || count == 0) {
				return this->SendCopilotBus(request_kind, request_id, COPILOT_ADMIN_INVALID_ARGUMENT);
			}
			nlohmann::json records = nlohmann::json::array();
			uint32_t ordinal = 0;
			bool more = false;
			for (const Engine *engine : Engine::IterateType(VEH_ROAD)) {
				if (ordinal++ < start) continue;
				if (records.size() == count) { more = true; break; }
				const RoadVehicleInfo &rvi = engine->VehInfo<RoadVehicleInfo>();
				records.push_back({
					{"engine_id", engine->index.base()},
					{"buildable", CopilotBusEngineSupported(engine, company_id)},
					{"purchase_cost", engine->GetCost().base()},
					{"running_cost", engine->GetRunningCost().base()},
					{"default_cargo", engine->GetDefaultCargoType()},
					{"default_capacity", engine->GetDisplayDefaultCapacity()},
					{"max_speed", engine->GetDisplayMaxSpeed()},
					{"road_type", to_underlying(rvi.roadtype)},
					{"articulated", IsArticulatedEngine(engine->index)},
				});
			}
			out["engines"] = records;
			out["count"] = records.size();
			out["next_start"] = start + records.size();
			out["complete"] = !more;
		} else if (action == "assets") {
			if (!request.contains("kind") || !request["kind"].is_string() ||
					!CopilotJSONUint(request, "start", UINT32_MAX, start, 0) ||
					!CopilotJSONUint(request, "count", 8, count, 8) || count == 0) {
				return this->SendCopilotBus(request_kind, request_id, COPILOT_ADMIN_INVALID_ARGUMENT);
			}
			std::string kind = request["kind"].get<std::string>();
			if (kind != "vehicles" && kind != "stops" && kind != "depots") {
				return this->SendCopilotBus(request_kind, request_id, COPILOT_ADMIN_INVALID_ARGUMENT);
			}
			nlohmann::json records = nlohmann::json::array();
			uint32_t ordinal = 0;
			bool more = false;
			if (kind == "vehicles") {
				for (const Vehicle *vehicle : Vehicle::Iterate()) {
					if (vehicle->type != VEH_ROAD || vehicle->owner != company_id || !vehicle->IsPrimaryVehicle()) continue;
					if (ordinal++ < start) continue;
					if (records.size() == count) { more = true; break; }
					records.push_back({
						{"vehicle_id", vehicle->index.base()}, {"engine_id", vehicle->engine_type.base()},
						{"tile", vehicle->tile.base()}, {"owner", vehicle->owner.base()},
						{"cargo_type", vehicle->cargo_type}, {"cargo_amount", vehicle->cargo.TotalCount()},
						{"cargo_capacity", vehicle->cargo_cap},
						{"stopped", vehicle->vehstatus.Test(VehState::Stopped)},
						{"crashed", vehicle->vehstatus.Test(VehState::Crashed)},
						{"in_depot", vehicle->IsInDepot()},
						{"orders_shared", vehicle->IsOrderListShared()},
					});
				}
			} else if (kind == "stops") {
				for (const RoadStop *stop : RoadStop::Iterate()) {
					if (!IsDriveThroughStopTile(stop->xy) || GetRoadStopType(stop->xy) != RoadStopType::Bus ||
							GetTileOwner(stop->xy) != company_id) continue;
					if (ordinal++ < start) continue;
					if (records.size() == count) { more = true; break; }
					records.push_back({
						{"station_id", GetStationIndex(stop->xy).base()}, {"stop_id", stop->index.base()},
						{"tile", stop->xy.base()}, {"orientation", to_underlying(GetDriveThroughStopAxis(stop->xy))},
						{"town_id", Station::Get(GetStationIndex(stop->xy))->town->index.base()},
						{"owner", company_id.base()},
					});
				}
			} else {
				for (const Depot *depot : Depot::Iterate()) {
					if (!IsRoadDepotTile(depot->xy) || GetTileOwner(depot->xy) != company_id) continue;
					if (ordinal++ < start) continue;
					if (records.size() == count) { more = true; break; }
					records.push_back({
						{"depot_id", depot->index.base()}, {"tile", depot->xy.base()},
						{"orientation", to_underlying(GetRoadDepotDirection(depot->xy))},
						{"owner", company_id.base()},
					});
				}
			}
			out[kind] = records;
			out["kind"] = kind;
			out["count"] = records.size();
			out["next_start"] = start + records.size();
			out["complete"] = !more;
		} else if (action == "orders") {
			uint64_t vehicle_raw;
			if (!CopilotJSONUint(request, "vehicle_id", UINT32_MAX - 1, vehicle_raw, 0, true)) {
				return this->SendCopilotBus(request_kind, request_id, COPILOT_ADMIN_INVALID_ARGUMENT);
			}
			const Vehicle *vehicle = Vehicle::GetIfValid(VehicleID{static_cast<uint32_t>(vehicle_raw)});
			if (vehicle == nullptr || vehicle->type != VEH_ROAD || vehicle->owner != company_id ||
					!vehicle->IsPrimaryVehicle() || vehicle->GetNumOrders() > COPILOT_MAX_BUS_ORDERS) {
				return this->SendCopilotBus(request_kind, request_id, COPILOT_ADMIN_INVALID_ARGUMENT);
			}
			nlohmann::json orders = nlohmann::json::array();
			for (const Order &order : vehicle->Orders()) {
				orders.push_back({
					{"order_type", to_underlying(order.GetType())},
					{"destination_id", order.IsGotoOrder() ? order.GetDestination().base() : UINT32_MAX},
					{"load_type", order.IsType(OT_GOTO_STATION) ? to_underlying(order.GetLoadType()) : UINT8_MAX},
					{"unload_type", order.IsType(OT_GOTO_STATION) ? to_underlying(order.GetUnloadType()) : UINT8_MAX},
					{"non_stop", order.IsGotoOrder() ? order.GetNonStopType().base() : UINT8_MAX},
				});
			}
			out["vehicle_id"] = vehicle_raw;
			out["orders"] = orders;
			out["count"] = orders.size();
		} else if (action == "quote") {
			uint64_t kind, tile_raw, orientation, road_bits, town_raw, engine_raw;
			if (!CopilotJSONUint(request, "kind", 10, kind, 0, true) || kind < 7 ||
					!CopilotJSONUint(request, "tile", UINT32_MAX, tile_raw, 0, kind != 10) ||
					!CopilotJSONUint(request, "orientation", 3, orientation, 0) ||
					!CopilotJSONUint(request, "road_bits", 15, road_bits, 0) ||
					!CopilotJSONUint(request, "town_id", UINT16_MAX, town_raw, UINT16_MAX) ||
					!CopilotJSONUint(request, "engine_id", UINT16_MAX, engine_raw, UINT16_MAX)) {
				return this->SendCopilotBus(request_kind, request_id, COPILOT_ADMIN_INVALID_ARGUMENT);
			}
			CommandCost quote = CMD_ERROR;
			if (kind == 10) {
				const Engine *engine = Engine::GetIfValid(EngineID{static_cast<uint16_t>(engine_raw)});
				if (CopilotBusEngineSupported(engine, company_id)) quote = CommandCost(EXPENSES_NEW_VEHICLES, engine->GetCost());
			} else {
				TileIndex tile{static_cast<uint32_t>(tile_raw)};
				if (tile != 0 && tile < Map::Size() && IsValidTile(tile)) {
					Backup<CompanyID> company_scope(_current_company, company_id);
					quote = std::get<0>(Command<CMD_COPILOT_BUS_ACTION>::Do(
							CommandFlagsToDCFlags(GetCommandFlags<CMD_COPILOT_BUS_ACTION>()), tile,
							static_cast<uint8_t>(kind), static_cast<uint8_t>(orientation),
							static_cast<uint8_t>(road_bits), EngineID::Invalid(), VehicleID::Invalid(),
							UINT32_MAX, 0, 0, INT64_MAX, 0, 0, 0, 0));
					company_scope.Restore();
				}
			}
			auto catchment = std::pair<uint32_t, uint32_t>{0, 0};
			if (kind == 8 && Town::IsValidID(TownID{static_cast<uint16_t>(town_raw)})) {
				catchment = CopilotTownCatchment(TileIndex{static_cast<uint32_t>(tile_raw)},
						TownID{static_cast<uint16_t>(town_raw)});
			}
			const CopilotOperation *head = GetCopilotHead();
			out.update({
				{"kind", kind}, {"tile", tile_raw}, {"orientation", orientation}, {"road_bits", road_bits},
				{"quote_success", quote.Succeeded()}, {"cost", quote.GetCost().base()},
				{"error", quote.Failed() ? quote.GetErrorMessage() : 0},
				{"passenger_acceptance", catchment.first}, {"passenger_production", catchment.second},
				{"supported_profile", _grfconfig.empty() && _settings_game.game_creation.landscape == LandscapeType::Temperate},
				{"retained_count", _copilot_operation_history.size() +
						(_copilot_operation.state != to_underlying(CopilotOperationState::Empty) ? 1 : 0)},
				{"capacity", COPILOT_MAX_OPERATIONS}, {"operation_state", _copilot_operation.state},
			});
			(void)head;
			CopilotBusPosition(out);
		} else if (action == "operation") {
			uint64_t operation_hi, operation_lo;
			if (!CopilotJSONUint(request, "operation_hi", UINT64_MAX, operation_hi, 0, true) ||
					!CopilotJSONUint(request, "operation_lo", UINT64_MAX, operation_lo, 0, true)) {
				return this->SendCopilotBus(request_kind, request_id, COPILOT_ADMIN_INVALID_ARGUMENT);
			}
			const CopilotOperation *operation = FindCopilotOperation(operation_hi, operation_lo);
			if (operation == nullptr || operation->kind < 7 || operation->kind > 12) {
				return this->SendCopilotBus(request_kind, request_id, COPILOT_ADMIN_OPERATION_NOT_FOUND);
			}
			out = CopilotBusOperationJSON(*operation);
		} else if (action == "mutate") {
			uint64_t kind, tile_raw, max_cost, expected_sequence, expected_head_hi, expected_head_lo;
			uint64_t operation_hi, operation_lo, hash_hi, hash_lo, orientation, road_bits;
			uint64_t engine_raw, vehicle_raw, target_raw, order_index, policy;
			if (!CopilotJSONUint(request, "kind", 12, kind, 0, true) || kind < 7 ||
					!CopilotJSONUint(request, "tile", UINT32_MAX, tile_raw, 0, true) ||
					!CopilotJSONUint(request, "max_cost", INT64_MAX, max_cost, 0, true) ||
					!CopilotJSONUint(request, "expected_sequence", UINT64_MAX, expected_sequence, 0, true) ||
					!CopilotJSONUint(request, "expected_head_hi", UINT64_MAX, expected_head_hi, 0, true) ||
					!CopilotJSONUint(request, "expected_head_lo", UINT64_MAX, expected_head_lo, 0, true) ||
					!CopilotJSONUint(request, "operation_hi", UINT64_MAX, operation_hi, 0, true) ||
					!CopilotJSONUint(request, "operation_lo", UINT64_MAX, operation_lo, 0, true) ||
					!CopilotJSONUint(request, "payload_hash_hi", UINT64_MAX, hash_hi, 0, true) ||
					!CopilotJSONUint(request, "payload_hash_lo", UINT64_MAX, hash_lo, 0, true) ||
					!CopilotJSONUint(request, "orientation", 3, orientation, 0) ||
					!CopilotJSONUint(request, "road_bits", 15, road_bits, 0) ||
					!CopilotJSONUint(request, "engine_id", UINT16_MAX, engine_raw, UINT16_MAX) ||
					!CopilotJSONUint(request, "vehicle_id", UINT32_MAX, vehicle_raw, UINT32_MAX) ||
					!CopilotJSONUint(request, "target_id", UINT32_MAX, target_raw, UINT32_MAX) ||
					!CopilotJSONUint(request, "order_index", UINT16_MAX, order_index, 0) ||
					!CopilotJSONUint(request, "policy", UINT8_MAX, policy, 0) ||
					(operation_hi == 0 && operation_lo == 0)) {
				return this->SendCopilotBus(request_kind, request_id, COPILOT_ADMIN_INVALID_ARGUMENT);
			}
			bool shape_valid =
					(kind == 7 && orientation == 0 && road_bits > 0 &&
						engine_raw == UINT16_MAX && vehicle_raw == UINT32_MAX && target_raw == UINT32_MAX &&
						order_index == 0 && policy == 0) ||
					(kind == 8 && orientation <= 1 && road_bits == 0 &&
						engine_raw == UINT16_MAX && vehicle_raw == UINT32_MAX && target_raw == UINT32_MAX &&
						order_index == 0 && policy == 0) ||
					(kind == 9 && road_bits == 0 &&
						engine_raw == UINT16_MAX && vehicle_raw == UINT32_MAX && target_raw == UINT32_MAX &&
						order_index == 0 && policy == 0) ||
					(kind == 10 && orientation == 0 && road_bits == 0 &&
						engine_raw != UINT16_MAX && vehicle_raw == UINT32_MAX && target_raw == UINT32_MAX &&
						order_index == 0 && policy == 0) ||
					(kind == 11 && orientation == 0 && road_bits == 0 &&
						engine_raw == UINT16_MAX && vehicle_raw != UINT32_MAX && target_raw < UINT16_MAX &&
						order_index < COPILOT_MAX_BUS_ORDERS && policy == 0) ||
					(kind == 12 && orientation == 0 && road_bits == 0 &&
						engine_raw == UINT16_MAX && vehicle_raw != UINT32_MAX && target_raw == UINT32_MAX &&
						order_index == 0 && policy <= 1);
			if (!shape_valid) {
				return this->SendCopilotBus(request_kind, request_id, COPILOT_ADMIN_INVALID_ARGUMENT);
			}
			uint8_t stored_track = kind == 7 ? road_bits : orientation;
			const CopilotOperation *existing = FindCopilotOperation(operation_hi, operation_lo);
			if (existing != nullptr) {
				bool same = existing->payload_hash_hi == hash_hi && existing->payload_hash_lo == hash_lo &&
						existing->company == company_id.base() && existing->tile == tile_raw &&
						existing->kind == kind && existing->track == stored_track &&
						existing->max_cost == static_cast<int64_t>(max_cost) &&
						existing->engine_id == engine_raw && existing->vehicle_id == vehicle_raw &&
						existing->target_id == target_raw && existing->order_index == order_index &&
						existing->policy == policy;
				if (!same) return this->SendCopilotBus(request_kind, request_id, COPILOT_ADMIN_OPERATION_CONFLICT);
				out = CopilotBusOperationJSON(*existing);
			} else {
				const CopilotOperation *head = GetCopilotHead();
				uint64_t head_hi = head != nullptr ? head->operation_hi : 0;
				uint64_t head_lo = head != nullptr ? head->operation_lo : 0;
				TileIndex tile{static_cast<uint32_t>(tile_raw)};
				if (expected_sequence != _copilot_operation.sequence || expected_head_hi != head_hi || expected_head_lo != head_lo) {
					return this->SendCopilotBus(request_kind, request_id, COPILOT_ADMIN_STALE_POSITION);
				}
				if (tile == 0 || tile >= Map::Size() || !IsValidTile(tile) ||
						(kind == 7 && road_bits == 0) || (kind == 8 && orientation > 1) ||
						_copilot_operation.state == to_underlying(CopilotOperationState::Pending) ||
						_copilot_operation.state == to_underlying(CopilotOperationState::Uncertain)) {
					return this->SendCopilotBus(request_kind, request_id,
							_copilot_operation.state >= to_underlying(CopilotOperationState::Pending) &&
							_copilot_operation.state <= to_underlying(CopilotOperationState::Uncertain) ?
							COPILOT_ADMIN_OPERATION_BUSY : COPILOT_ADMIN_INVALID_ARGUMENT);
				}
				if (_copilot_operation_history.size() + 1 >= COPILOT_MAX_OPERATIONS &&
						_copilot_operation.state != to_underlying(CopilotOperationState::Empty)) {
					return this->SendCopilotBus(request_kind, request_id, COPILOT_ADMIN_JOURNAL_FULL);
				}
				if (_copilot_operation.state != to_underlying(CopilotOperationState::Empty)) {
					_copilot_operation_history.push_back(_copilot_operation);
				}
				uint64_t sequence = _copilot_operation.sequence;
				_copilot_operation = {};
				_copilot_operation.schema = 5;
				_copilot_operation.sequence = sequence;
				_copilot_operation.state = to_underlying(CopilotOperationState::Pending);
				_copilot_operation.company = company_id.base();
				_copilot_operation.tile = tile_raw;
				_copilot_operation.rail_type = INVALID_RAILTYPE;
				_copilot_operation.track = stored_track;
				_copilot_operation.operation_hi = operation_hi;
				_copilot_operation.operation_lo = operation_lo;
				_copilot_operation.payload_hash_hi = hash_hi;
				_copilot_operation.payload_hash_lo = hash_lo;
				_copilot_operation.max_cost = max_cost;
				_copilot_operation.kind = kind;
				_copilot_operation.engine_id = engine_raw;
				_copilot_operation.vehicle_id = vehicle_raw;
				_copilot_operation.target_id = target_raw;
				_copilot_operation.order_index = order_index;
				_copilot_operation.policy = policy;
				VehicleID vehicle_id = vehicle_raw == UINT32_MAX ?
						VehicleID::Invalid() : VehicleID{static_cast<uint32_t>(vehicle_raw)};
				Backup<CompanyID> company_scope(_current_company, company_id);
				bool accepted = Command<CMD_COPILOT_BUS_ACTION>::Post(&CcCopilotBusAction, tile,
						static_cast<uint8_t>(kind), static_cast<uint8_t>(orientation),
						static_cast<uint8_t>(road_bits), EngineID{static_cast<uint16_t>(engine_raw)},
						vehicle_id, static_cast<uint32_t>(target_raw),
						static_cast<uint16_t>(order_index), static_cast<uint8_t>(policy), Money{static_cast<int64_t>(max_cost)},
						operation_hi, operation_lo, hash_hi, hash_lo);
				company_scope.Restore();
				if (!accepted) {
					_copilot_operation.state = to_underlying(CopilotOperationState::Rejected);
					_copilot_operation.completed_frame = _frame_counter;
				}
				out = CopilotBusOperationJSON(_copilot_operation);
			}
		} else {
			return this->SendCopilotBus(request_kind, request_id, COPILOT_ADMIN_UNSUPPORTED_REQUEST);
		}
		std::string encoded;
		const char *page_key = action == "towns" ? "towns" : action == "catalog" ? "engines" :
				action == "assets" && request["kind"] == "vehicles" ? "vehicles" :
				action == "assets" && request["kind"] == "stops" ? "stops" :
				action == "assets" && request["kind"] == "depots" ? "depots" : nullptr;
		if (page_key != nullptr) {
			while (!CopilotBusSerialize(out, encoded) && out[page_key].size() > 1) {
				out[page_key].erase(out[page_key].end() - 1);
				out["count"] = out[page_key].size();
				out["next_start"] = start + out[page_key].size();
				out["complete"] = false;
			}
		}
		if (!CopilotBusSerialize(out, encoded)) {
			return this->SendCopilotBus(request_kind, request_id, COPILOT_ADMIN_INVALID_ARGUMENT);
		}
		return this->SendCopilotBus(request_kind, request_id, COPILOT_ADMIN_OK, world_id, encoded);
	}

	if (request_kind == COPILOT_ADMIN_IDENTITY) {
		if (p.RemainingBytesToTransfer() != sizeof(uint8_t)) return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
		return this->SendCopilotIdentity(version, request_kind, request_id, CompanyID{p.Recv_uint8()});
	}

	if (request_kind == COPILOT_ADMIN_CONSTRUCTION_QUOTE) {
		if (p.RemainingBytesToTransfer() < 13) return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
		CompanyID company_id{p.Recv_uint8()};
		TileIndex tile{p.Recv_uint32()};
		uint8_t rail_type = p.Recv_uint8();
		uint8_t orientation = p.Recv_uint8();
		uint8_t kind = p.Recv_uint8();
		uint8_t length = p.Recv_uint8();
		IndustryID industry_id{p.Recv_uint16()};
		CargoType cargo = p.Recv_uint8();
		std::string world_id = p.Recv_string(COPILOT_WORLD_ID_LENGTH);
		if (p.RemainingBytesToTransfer() != 0) return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
		if (world_id.empty() || world_id != _game_session_stats.savegame_id) {
			return this->SendCopilotOperation(request_kind, request_id, COPILOT_ADMIN_WORLD_MISMATCH);
		}
		if (!Company::IsValidID(company_id)) {
			return this->SendCopilotOperation(request_kind, request_id, COPILOT_ADMIN_COMPANY_NOT_FOUND);
		}
		if (Company::Get(company_id)->is_ai) {
			return this->SendCopilotOperation(request_kind, request_id, COPILOT_ADMIN_COMPANY_NOT_HUMAN);
		}
		if (tile == 0 || tile >= Map::Size() || !IsValidTile(tile) || rail_type >= RAILTYPE_END ||
				!IsValidCargoType(cargo) || kind > 2 ||
				(kind == 0 && (orientation >= TRACK_END || length != 0)) ||
				(kind == 1 && (orientation > 1 || length < 1 || length > 7)) ||
				(kind == 2 && (orientation > 3 || length != 1))) {
			return this->SendCopilotOperation(request_kind, request_id, COPILOT_ADMIN_INVALID_ARGUMENT);
		}
		Backup<CompanyID> company_scope(_current_company, company_id);
		CommandCost quote = kind == 0 ?
				Command<CMD_COPILOT_BUILD_SINGLE_RAIL>::Do({}, tile, static_cast<RailType>(rail_type),
						static_cast<Track>(orientation), INT64_MAX, 0, 0, 0, 0) :
				Command<CMD_COPILOT_BUILD_INFRASTRUCTURE>::Do({}, tile, static_cast<RailType>(rail_type),
						orientation, kind, length, INT64_MAX, 0, 0, 0, 0);
		company_scope.Restore();
		bool produces = false;
		CargoArray accepted{};
		const Industry *industry = Industry::GetIfValid(industry_id);
		if (quote.Succeeded() && kind == 1 && industry != nullptr && industry->neutral_station == nullptr) {
			TileArea area(tile, orientation == 0 ? length : 1, orientation == 1 ? length : 1);
			for (TileIndex part : area.Expand(CA_TRAIN)) {
				if (!IsTileType(part, MP_INDUSTRY) || GetIndustryIndex(part) != industry_id) continue;
				produces |= industry->IsCargoProduced(cargo);
				AddAcceptedCargo(part, accepted, nullptr);
			}
		}
		const CopilotOperation *head = GetCopilotHead();
		auto response = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_COPILOT);
		response->Send_uint16(COPILOT_ADMIN_VERSION);
		response->Send_uint8(request_kind);
		response->Send_uint32(request_id);
		response->Send_uint8(COPILOT_ADMIN_OK);
		response->Send_string(world_id);
		response->Send_uint64(_copilot_operation.sequence);
		response->Send_uint64(head != nullptr ? head->operation_hi : 0);
		response->Send_uint64(head != nullptr ? head->operation_lo : 0);
		response->Send_uint32(_frame_counter);
		response->Send_uint8(company_id.base());
		response->Send_uint32(tile.base());
		response->Send_uint8(rail_type);
		response->Send_uint8(orientation);
		response->Send_uint8(kind);
		response->Send_uint8(length);
		response->Send_uint16(industry_id.base());
		response->Send_uint8(cargo);
		response->Send_bool(quote.Succeeded());
		response->Send_uint64(static_cast<uint64_t>(quote.GetCost()));
		response->Send_uint32(quote.Failed() ? quote.GetErrorMessage() : 0);
		response->Send_bool(produces);
		response->Send_uint32(accepted[cargo]);
		response->Send_uint16(static_cast<uint16_t>(_copilot_operation_history.size() +
				(_copilot_operation.state != to_underlying(CopilotOperationState::Empty) ? 1 : 0)));
		response->Send_uint16(COPILOT_MAX_OPERATIONS);
		response->Send_uint8(_copilot_operation.state);
		response->Send_bool(_grfconfig.empty() && _settings_game.game_creation.landscape == LandscapeType::Temperate);
		this->SendPacket(std::move(response));
		return NETWORK_RECV_STATUS_OKAY;
	}

	if (request_kind == COPILOT_ADMIN_OPERATION_STATUS || request_kind == COPILOT_ADMIN_INFRASTRUCTURE_STATUS ||
			request_kind == COPILOT_ADMIN_TRAIN_PURCHASE_STATUS || request_kind == COPILOT_ADMIN_TRAIN_CONFIGURATION_STATUS) {
		if (p.RemainingBytesToTransfer() < sizeof(uint64_t) * 2 + 1) return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
		uint64_t operation_hi = p.Recv_uint64();
		uint64_t operation_lo = p.Recv_uint64();
		std::string world_id = p.Recv_string(COPILOT_WORLD_ID_LENGTH);
		if (p.RemainingBytesToTransfer() != 0) return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
		if (world_id != _game_session_stats.savegame_id) {
			return this->SendCopilotOperation(request_kind, request_id, COPILOT_ADMIN_WORLD_MISMATCH);
		}
		const CopilotOperation *operation = FindCopilotOperation(operation_hi, operation_lo);
		if (operation == nullptr) {
			return this->SendCopilotOperation(request_kind, request_id, COPILOT_ADMIN_OPERATION_NOT_FOUND);
		}
		if (request_kind == COPILOT_ADMIN_OPERATION_STATUS && operation->kind != 0) {
			return this->SendCopilotOperation(request_kind, request_id, COPILOT_ADMIN_UNSUPPORTED_REQUEST);
		}
		if ((request_kind == COPILOT_ADMIN_INFRASTRUCTURE_STATUS && operation->kind >= 3) ||
				(request_kind == COPILOT_ADMIN_TRAIN_PURCHASE_STATUS && operation->kind >= 4)) {
			return this->SendCopilotOperation(request_kind, request_id, COPILOT_ADMIN_UNSUPPORTED_REQUEST);
		}
		return this->SendCopilotOperation(request_kind, request_id, COPILOT_ADMIN_OK, operation);
	}

	if (request_kind == COPILOT_ADMIN_OBSERVATIONS) {
		constexpr size_t fixed_bytes = sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint64_t);
		if (p.RemainingBytesToTransfer() < fixed_bytes + 1) return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
		CompanyID company_id{p.Recv_uint8()};
		VehicleID vehicle_id{p.Recv_uint32()};
		uint64_t cursor = p.Recv_uint64();
		std::string world_id = p.Recv_string(COPILOT_WORLD_ID_LENGTH);
		if (p.RemainingBytesToTransfer() != 0) return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
		if (world_id.empty() || world_id != _game_session_stats.savegame_id) {
			return this->SendCopilotObservations(request_kind, request_id, COPILOT_ADMIN_WORLD_MISMATCH, company_id, vehicle_id, cursor);
		}
		if (!Company::IsValidID(company_id)) {
			return this->SendCopilotObservations(request_kind, request_id, COPILOT_ADMIN_COMPANY_NOT_FOUND, company_id, vehicle_id, cursor);
		}
		if (Company::Get(company_id)->is_ai) {
			return this->SendCopilotObservations(request_kind, request_id, COPILOT_ADMIN_COMPANY_NOT_HUMAN, company_id, vehicle_id, cursor);
		}
		if (vehicle_id == VehicleID::Invalid()) {
			return this->SendCopilotObservations(request_kind, request_id, COPILOT_ADMIN_INVALID_ARGUMENT, company_id, vehicle_id, cursor);
		}
		return this->SendCopilotObservations(request_kind, request_id, COPILOT_ADMIN_OK, company_id, vehicle_id, cursor);
	}

	if (request_kind == COPILOT_ADMIN_TILES) {
		if (p.RemainingBytesToTransfer() < sizeof(uint32_t) + sizeof(uint8_t) + 1) return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
		uint32_t start_tile = p.Recv_uint32();
		uint8_t count = p.Recv_uint8();
		std::string world_id = p.Recv_string(COPILOT_WORLD_ID_LENGTH);
		if (p.RemainingBytesToTransfer() != 0) return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
		if (world_id.empty() || world_id != _game_session_stats.savegame_id) {
			return this->SendCopilotTiles(request_kind, request_id, COPILOT_ADMIN_WORLD_MISMATCH, start_tile, count);
		}
		if (count == 0 || count > 32 || start_tile >= Map::Size() || count > Map::Size() - start_tile) {
			return this->SendCopilotTiles(request_kind, request_id, COPILOT_ADMIN_INVALID_ARGUMENT, start_tile, count);
		}
		return this->SendCopilotTiles(request_kind, request_id, COPILOT_ADMIN_OK, start_tile, count);
	}

	if (request_kind == COPILOT_ADMIN_INDUSTRIES) {
		if (p.RemainingBytesToTransfer() < sizeof(uint32_t) + sizeof(uint8_t) + 1) return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
		uint32_t start_index = p.Recv_uint32();
		uint8_t count = p.Recv_uint8();
		std::string world_id = p.Recv_string(COPILOT_WORLD_ID_LENGTH);
		if (p.RemainingBytesToTransfer() != 0) return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
		if (world_id.empty() || world_id != _game_session_stats.savegame_id) {
			return this->SendCopilotIndustries(request_kind, request_id, COPILOT_ADMIN_WORLD_MISMATCH, start_index, count);
		}
		if (count == 0 || count > 16 || start_index > Industry::GetPoolSize()) {
			return this->SendCopilotIndustries(request_kind, request_id, COPILOT_ADMIN_INVALID_ARGUMENT, start_index, count);
		}
		return this->SendCopilotIndustries(request_kind, request_id, COPILOT_ADMIN_OK, start_index, count);
	}

	if (request_kind == COPILOT_ADMIN_INDUSTRY_DETAILS) {
		if (p.RemainingBytesToTransfer() < sizeof(uint32_t) + sizeof(uint8_t) + 1) return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
		uint32_t start_index = p.Recv_uint32();
		uint8_t count = p.Recv_uint8();
		std::string world_id = p.Recv_string(COPILOT_WORLD_ID_LENGTH);
		if (p.RemainingBytesToTransfer() != 0) return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
		if (world_id.empty() || world_id != _game_session_stats.savegame_id) {
			return this->SendCopilotIndustryDetails(request_kind, request_id, COPILOT_ADMIN_WORLD_MISMATCH, start_index, count);
		}
		if (count == 0 || count > 8 || start_index > Industry::GetPoolSize()) {
			return this->SendCopilotIndustryDetails(request_kind, request_id, COPILOT_ADMIN_INVALID_ARGUMENT, start_index, count);
		}
		return this->SendCopilotIndustryDetails(request_kind, request_id, COPILOT_ADMIN_OK, start_index, count);
	}

	if (request_kind == COPILOT_ADMIN_RAIL_CATALOG) {
		if (p.RemainingBytesToTransfer() < sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint8_t) + 1) return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
		CompanyID company_id{p.Recv_uint8()};
		uint32_t start_index = p.Recv_uint32();
		uint8_t count = p.Recv_uint8();
		std::string world_id = p.Recv_string(COPILOT_WORLD_ID_LENGTH);
		if (p.RemainingBytesToTransfer() != 0) return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
		if (world_id.empty() || world_id != _game_session_stats.savegame_id) {
			return this->SendCopilotRailCatalog(request_kind, request_id, COPILOT_ADMIN_WORLD_MISMATCH, company_id, start_index, count);
		}
		if (!Company::IsValidID(company_id)) {
			return this->SendCopilotRailCatalog(request_kind, request_id, COPILOT_ADMIN_COMPANY_NOT_FOUND, company_id, start_index, count);
		}
		if (Company::Get(company_id)->is_ai) {
			return this->SendCopilotRailCatalog(request_kind, request_id, COPILOT_ADMIN_COMPANY_NOT_HUMAN, company_id, start_index, count);
		}
		if (count == 0 || count > 16 || start_index > Engine::GetPoolSize()) {
			return this->SendCopilotRailCatalog(request_kind, request_id, COPILOT_ADMIN_INVALID_ARGUMENT, company_id, start_index, count);
		}
		return this->SendCopilotRailCatalog(request_kind, request_id, COPILOT_ADMIN_OK, company_id, start_index, count);
	}

	if (request_kind == COPILOT_ADMIN_RAIL_ASSETS) {
		if (p.RemainingBytesToTransfer() < sizeof(uint8_t) * 2 + sizeof(uint32_t) + sizeof(uint8_t) + 1) return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
		CompanyID company_id{p.Recv_uint8()};
		uint8_t asset_kind = p.Recv_uint8();
		uint32_t start_index = p.Recv_uint32();
		uint8_t count = p.Recv_uint8();
		std::string world_id = p.Recv_string(COPILOT_WORLD_ID_LENGTH);
		if (p.RemainingBytesToTransfer() != 0) return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
		if (world_id.empty() || world_id != _game_session_stats.savegame_id) {
			return this->SendCopilotRailAssets(request_kind, request_id, COPILOT_ADMIN_WORLD_MISMATCH, company_id, asset_kind, start_index, count);
		}
		if (!Company::IsValidID(company_id)) {
			return this->SendCopilotRailAssets(request_kind, request_id, COPILOT_ADMIN_COMPANY_NOT_FOUND, company_id, asset_kind, start_index, count);
		}
		if (Company::Get(company_id)->is_ai) {
			return this->SendCopilotRailAssets(request_kind, request_id, COPILOT_ADMIN_COMPANY_NOT_HUMAN, company_id, asset_kind, start_index, count);
		}
		if (asset_kind > 1 || count == 0 || count > 16 || start_index > (asset_kind == 0 ? Station::GetPoolSize() : Depot::GetPoolSize())) {
			return this->SendCopilotRailAssets(request_kind, request_id, COPILOT_ADMIN_INVALID_ARGUMENT, company_id, asset_kind, start_index, count);
		}
		return this->SendCopilotRailAssets(request_kind, request_id, COPILOT_ADMIN_OK, company_id, asset_kind, start_index, count);
	}

	if (request_kind == COPILOT_ADMIN_TRAIN_PARTS) {
		constexpr size_t fixed_bytes = sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint16_t);
		if (p.RemainingBytesToTransfer() < fixed_bytes + 1) return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
		CompanyID company_id{p.Recv_uint8()};
		uint32_t after_vehicle_id = p.Recv_uint32();
		uint8_t max_records = p.Recv_uint8();
		uint16_t scan_budget = p.Recv_uint16();
		std::string world_id = p.Recv_string(COPILOT_WORLD_ID_LENGTH);
		if (p.RemainingBytesToTransfer() != 0) return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
		if (world_id.empty() || world_id != _game_session_stats.savegame_id) {
			return this->SendCopilotTrainParts(request_kind, request_id, COPILOT_ADMIN_WORLD_MISMATCH, company_id, after_vehicle_id, max_records, scan_budget);
		}
		if (!Company::IsValidID(company_id)) {
			return this->SendCopilotTrainParts(request_kind, request_id, COPILOT_ADMIN_COMPANY_NOT_FOUND, company_id, after_vehicle_id, max_records, scan_budget);
		}
		if (Company::Get(company_id)->is_ai) {
			return this->SendCopilotTrainParts(request_kind, request_id, COPILOT_ADMIN_COMPANY_NOT_HUMAN, company_id, after_vehicle_id, max_records, scan_budget);
		}
		if (max_records == 0 || max_records > 16 || scan_budget == 0 || scan_budget > 256 ||
				(after_vehicle_id != UINT32_MAX && after_vehicle_id >= Vehicle::GetPoolSize())) {
			return this->SendCopilotTrainParts(request_kind, request_id, COPILOT_ADMIN_INVALID_ARGUMENT, company_id, after_vehicle_id, max_records, scan_budget);
		}
		return this->SendCopilotTrainParts(request_kind, request_id, COPILOT_ADMIN_OK, company_id, after_vehicle_id, max_records, scan_budget);
	}

	if (request_kind == COPILOT_ADMIN_TRAIN_ORDERS) {
		constexpr size_t fixed_bytes = sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint8_t);
		if (p.RemainingBytesToTransfer() < fixed_bytes + 1) return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
		CompanyID company_id{p.Recv_uint8()};
		VehicleID vehicle_id{p.Recv_uint32()};
		uint16_t start_index = p.Recv_uint16();
		uint8_t count = p.Recv_uint8();
		std::string world_id = p.Recv_string(COPILOT_WORLD_ID_LENGTH);
		if (p.RemainingBytesToTransfer() != 0) return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
		if (world_id.empty() || world_id != _game_session_stats.savegame_id) {
			return this->SendCopilotTrainOrders(request_kind, request_id, COPILOT_ADMIN_WORLD_MISMATCH,
					company_id, vehicle_id, start_index, count);
		}
		if (!Company::IsValidID(company_id)) {
			return this->SendCopilotTrainOrders(request_kind, request_id, COPILOT_ADMIN_COMPANY_NOT_FOUND,
					company_id, vehicle_id, start_index, count);
		}
		if (Company::Get(company_id)->is_ai) {
			return this->SendCopilotTrainOrders(request_kind, request_id, COPILOT_ADMIN_COMPANY_NOT_HUMAN,
					company_id, vehicle_id, start_index, count);
		}
		const Vehicle *vehicle = Vehicle::GetIfValid(vehicle_id);
		if (vehicle == nullptr || vehicle->type != VEH_TRAIN || vehicle->owner != company_id ||
				!vehicle->IsPrimaryVehicle()) {
			return this->SendCopilotTrainOrders(request_kind, request_id, COPILOT_ADMIN_INVALID_ARGUMENT,
					company_id, vehicle_id, start_index, count);
		}
		if (count == 0 || count > 16 || start_index > vehicle->GetNumOrders()) {
			return this->SendCopilotTrainOrders(request_kind, request_id, COPILOT_ADMIN_INVALID_ARGUMENT,
					company_id, vehicle_id, start_index, count);
		}
		return this->SendCopilotTrainOrders(request_kind, request_id, COPILOT_ADMIN_OK,
				company_id, vehicle_id, start_index, count);
	}

	bool infrastructure = request_kind == COPILOT_ADMIN_BUILD_INFRASTRUCTURE;
	bool purchase = request_kind == COPILOT_ADMIN_BUILD_TRAIN;
	bool configure = request_kind == COPILOT_ADMIN_CONFIGURE_TRAIN;
	if (request_kind != COPILOT_ADMIN_BUILD_SINGLE_RAIL && !infrastructure && !purchase && !configure) {
		return this->SendCopilotOperation(request_kind, request_id, COPILOT_ADMIN_UNSUPPORTED_REQUEST);
	}
	constexpr size_t fixed_bytes = sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint8_t) * 2 + sizeof(uint64_t) * 8;
	if (p.RemainingBytesToTransfer() < fixed_bytes + (configure ? 10 : infrastructure ? 2 : 0) + 1) return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
	CompanyID company_id{p.Recv_uint8()};
	uint32_t raw_tile = p.Recv_uint32();
	uint8_t raw_rail_type = purchase || configure ? INVALID_RAILTYPE : p.Recv_uint8();
	uint8_t raw_track = purchase || configure ? 0 : p.Recv_uint8();
	uint16_t engine_id = purchase ? p.Recv_uint16() : UINT16_MAX;
	uint8_t kind = configure ? p.Recv_uint8() : purchase ? 3 : infrastructure ? p.Recv_uint8() : 0;
	uint8_t length = infrastructure ? p.Recv_uint8() : 0;
	uint32_t vehicle_id = configure ? p.Recv_uint32() : UINT32_MAX;
	uint32_t target_id = configure ? p.Recv_uint32() : UINT32_MAX;
	uint16_t order_index = configure ? p.Recv_uint16() : 0;
	uint8_t policy = configure ? p.Recv_uint8() : 0;
	int64_t max_cost_value = static_cast<int64_t>(p.Recv_uint64());
	uint64_t expected_sequence = p.Recv_uint64();
	uint64_t expected_head_hi = p.Recv_uint64();
	uint64_t expected_head_lo = p.Recv_uint64();
	uint64_t operation_hi = p.Recv_uint64();
	uint64_t operation_lo = p.Recv_uint64();
	uint64_t payload_hash_hi = p.Recv_uint64();
	uint64_t payload_hash_lo = p.Recv_uint64();
	std::string world_id = p.Recv_string(COPILOT_WORLD_ID_LENGTH);
	if (p.RemainingBytesToTransfer() != 0) return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);

	if (world_id.empty() || world_id != _game_session_stats.savegame_id) {
		return this->SendCopilotOperation(request_kind, request_id, COPILOT_ADMIN_WORLD_MISMATCH);
	}
	const CopilotOperation *existing = FindCopilotOperation(operation_hi, operation_lo);
	if (existing != nullptr) {
		bool same = existing->payload_hash_hi == payload_hash_hi &&
				existing->payload_hash_lo == payload_hash_lo && existing->company == company_id.base() &&
				existing->tile == raw_tile && existing->rail_type == raw_rail_type &&
				existing->track == raw_track && existing->max_cost == max_cost_value &&
				existing->kind == kind && existing->length == length && existing->engine_id == engine_id &&
				existing->vehicle_id == vehicle_id && existing->target_id == target_id &&
				existing->order_index == order_index && existing->policy == policy;
		return this->SendCopilotOperation(request_kind, request_id,
				same ? COPILOT_ADMIN_OK : COPILOT_ADMIN_OPERATION_CONFLICT, existing);
	}
	const CopilotOperation *head = GetCopilotHead();
	uint64_t current_head_hi = head != nullptr ? head->operation_hi : 0;
	uint64_t current_head_lo = head != nullptr ? head->operation_lo : 0;
	if (expected_sequence != _copilot_operation.sequence || expected_head_hi != current_head_hi || expected_head_lo != current_head_lo) {
		return this->SendCopilotOperation(request_kind, request_id, COPILOT_ADMIN_STALE_POSITION);
	}
	if (!Company::IsValidID(company_id)) {
		return this->SendCopilotOperation(request_kind, request_id, COPILOT_ADMIN_COMPANY_NOT_FOUND);
	}
	if (Company::Get(company_id)->is_ai) {
		return this->SendCopilotOperation(request_kind, request_id, COPILOT_ADMIN_COMPANY_NOT_HUMAN);
	}
	TileIndex tile{raw_tile};
	bool config_valid = vehicle_id != UINT32_MAX &&
			((kind == 4 && target_id != UINT32_MAX && order_index == 0 && policy == 0) ||
			(kind == 5 && target_id < UINT16_MAX && order_index <= 1 && policy <= 1) ||
			(kind == 6 && target_id == UINT32_MAX && order_index == 0 && policy <= 1));
	bool geometry_valid = configure ? config_valid : purchase ? engine_id != UINT16_MAX : !infrastructure ? raw_track < TRACK_END :
			(kind == 1 && raw_track <= 1 && length >= 1 && length <= 7) ||
			(kind == 2 && raw_track <= 3 && length == 1);
	if (tile == 0 || tile >= Map::Size() || !IsValidTile(tile) || (!purchase && !configure && raw_rail_type >= RAILTYPE_END) ||
			!geometry_valid || max_cost_value < 0 || (operation_hi == 0 && operation_lo == 0)) {
		return this->SendCopilotOperation(request_kind, request_id, COPILOT_ADMIN_INVALID_ARGUMENT);
	}

	if (_copilot_operation.state == to_underlying(CopilotOperationState::Pending) ||
			_copilot_operation.state == to_underlying(CopilotOperationState::Uncertain)) {
		return this->SendCopilotOperation(request_kind, request_id, COPILOT_ADMIN_OPERATION_BUSY);
	}
	if (_copilot_operation_history.size() + 1 >= COPILOT_MAX_OPERATIONS &&
			_copilot_operation.state != to_underlying(CopilotOperationState::Empty)) {
		return this->SendCopilotOperation(request_kind, request_id, COPILOT_ADMIN_JOURNAL_FULL);
	}
	if (_copilot_operation.state != to_underlying(CopilotOperationState::Empty)) {
		_copilot_operation_history.push_back(_copilot_operation);
	}

	_copilot_operation.state = to_underlying(CopilotOperationState::Pending);
	_copilot_operation.company = company_id.base();
	_copilot_operation.tile = raw_tile;
	_copilot_operation.rail_type = raw_rail_type;
	_copilot_operation.track = raw_track;
	_copilot_operation.operation_hi = operation_hi;
	_copilot_operation.operation_lo = operation_lo;
	_copilot_operation.payload_hash_hi = payload_hash_hi;
	_copilot_operation.payload_hash_lo = payload_hash_lo;
	_copilot_operation.max_cost = max_cost_value;
	_copilot_operation.actual_cost = 0;
	_copilot_operation.error_message = 0;
	_copilot_operation.completed_frame = 0;
	_copilot_operation.kind = kind;
	_copilot_operation.length = length;
	_copilot_operation.created_asset_id = UINT32_MAX;
	_copilot_operation.engine_id = engine_id;
	_copilot_operation.vehicle_id = vehicle_id;
	_copilot_operation.target_id = target_id;
	_copilot_operation.order_index = order_index;
	_copilot_operation.policy = policy;

	Backup<CompanyID> company_scope(_current_company, company_id);
	Money max_cost = max_cost_value;
	bool accepted = configure ?
			Command<CMD_COPILOT_CONFIGURE_TRAIN>::Post(&CcCopilotConfigureTrain, tile, kind, VehicleID{vehicle_id},
					target_id, order_index, policy, max_cost, operation_hi, operation_lo, payload_hash_hi, payload_hash_lo) : purchase ?
			Command<CMD_COPILOT_BUILD_TRAIN>::Post(&CcCopilotBuildTrain, tile, EngineID{engine_id}, max_cost,
					operation_hi, operation_lo, payload_hash_hi, payload_hash_lo) : infrastructure ?
			Command<CMD_COPILOT_BUILD_INFRASTRUCTURE>::Post(&CcCopilotBuildInfrastructure, tile,
					static_cast<RailType>(raw_rail_type), raw_track, kind, length, max_cost,
					operation_hi, operation_lo, payload_hash_hi, payload_hash_lo) :
			Command<CMD_COPILOT_BUILD_SINGLE_RAIL>::Post(&CcCopilotBuildSingleRail, tile,
					static_cast<RailType>(raw_rail_type), static_cast<Track>(raw_track), max_cost,
					operation_hi, operation_lo, payload_hash_hi, payload_hash_lo);
	company_scope.Restore();
	if (!accepted) {
		_copilot_operation.state = to_underlying(CopilotOperationState::Rejected);
		_copilot_operation.completed_frame = _frame_counter;
	}
	return this->SendCopilotOperation(request_kind, request_id, COPILOT_ADMIN_OK);
}

/**
 * Send console output of other clients.
 * @param origin The origin of the string.
 * @param string The string that's put on the console.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendConsole(std::string_view origin, std::string_view string)
{
	/* If the length of both strings, plus the 2 '\0' terminations and 3 bytes of the packet
	 * are bigger than the MTU, just ignore the message. Better safe than sorry. It should
	 * never occur though as the longest strings are chat messages, which are still 30%
	 * smaller than COMPAT_MTU. */
	if (origin.size() + string.size() + 2 + 3 >= COMPAT_MTU) return NETWORK_RECV_STATUS_OKAY;

	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_CONSOLE);

	p->Send_string(origin);
	p->Send_string(string);
	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Send GameScript JSON output.
 * @param json The JSON string.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendGameScript(std::string_view json)
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_GAMESCRIPT);

	p->Send_string(json);
	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/** Send ping-reply (pong) to admin **/
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendPong(uint32_t d1)
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_PONG);

	p->Send_uint32(d1);
	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/** Send the names of the commands. */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendCmdNames()
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_CMD_NAMES);

	for (uint16_t i = 0; i < CMD_END; i++) {
		std::string_view cmdname = GetCommandName(static_cast<Commands>(i));

		/* Should COMPAT_MTU be exceeded, start a new packet
		 * (magic 5: 1 bool "more data" and one uint16_t "command id", one
		 * byte for string '\0' termination and 1 bool "no more data" */
		if (!p->CanWriteToPacket(cmdname.size() + 5)) {
			p->Send_bool(false);
			this->SendPacket(std::move(p));

			p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_CMD_NAMES);
		}

		p->Send_bool(true);
		p->Send_uint16(i);
		p->Send_string(cmdname);
	}

	/* Marker to notify the end of the packet has been reached. */
	p->Send_bool(false);
	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * Send a command for logging purposes.
 * @param client_id The client executing the command.
 * @param cp The command that would be executed.
 */
NetworkRecvStatus ServerNetworkAdminSocketHandler::SendCmdLogging(ClientID client_id, const CommandPacket &cp)
{
	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_CMD_LOGGING);

	p->Send_uint32(client_id);
	p->Send_uint8 (cp.company);
	p->Send_uint16(cp.cmd);
	p->Send_buffer(cp.data);
	p->Send_uint32(cp.frame);

	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

/***********
 * Receiving functions
 ************/

NetworkRecvStatus ServerNetworkAdminSocketHandler::Receive_ADMIN_JOIN(Packet &p)
{
	if (this->status != ADMIN_STATUS_INACTIVE) return this->SendError(NETWORK_ERROR_NOT_EXPECTED);

	if (!_settings_client.network.allow_insecure_admin_login) {
		/* You're not authorized to login using this method. */
		return this->SendError(NETWORK_ERROR_NOT_AUTHORIZED);
	}

	std::string password = p.Recv_string(NETWORK_PASSWORD_LENGTH);

	if (_settings_client.network.admin_password.empty() || _settings_client.network.admin_password != password) {
		/* Password is invalid */
		return this->SendError(NETWORK_ERROR_WRONG_PASSWORD);
	}

	this->admin_name = p.Recv_string(NETWORK_CLIENT_NAME_LENGTH);
	this->admin_version = p.Recv_string(NETWORK_REVISION_LENGTH);

	if (this->admin_name.empty() || this->admin_version.empty()) {
		/* no name or version supplied */
		return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
	}

	Debug(net, 3, "[admin] '{}' ({}) has connected", this->admin_name, this->admin_version);

	return this->SendProtocol();
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::Receive_ADMIN_QUIT(Packet &)
{
	/* The admin is leaving nothing else to do */
	return this->CloseConnection();
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::Receive_ADMIN_UPDATE_FREQUENCY(Packet &p)
{
	if (this->status <= ADMIN_STATUS_AUTHENTICATE) return this->SendError(NETWORK_ERROR_NOT_EXPECTED);

	AdminUpdateType type = (AdminUpdateType)p.Recv_uint16();
	AdminUpdateFrequencies freq = static_cast<AdminUpdateFrequencies>(p.Recv_uint16());

	if (type >= ADMIN_UPDATE_END || !_admin_update_type_frequencies[type].All(freq)) {
		/* The server does not know of this UpdateType. */
		Debug(net, 1, "[admin] Not supported update frequency {} ({}) from '{}' ({})", type, freq, this->admin_name, this->admin_version);
		return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
	}

	this->update_frequency[type] = freq;

	if (type == ADMIN_UPDATE_CONSOLE) DebugReconsiderSendRemoteMessages();

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::Receive_ADMIN_POLL(Packet &p)
{
	if (this->status <= ADMIN_STATUS_AUTHENTICATE) return this->SendError(NETWORK_ERROR_NOT_EXPECTED);

	AdminUpdateType type = (AdminUpdateType)p.Recv_uint8();
	uint32_t d1 = p.Recv_uint32();

	switch (type) {
		case ADMIN_UPDATE_DATE:
			/* The admin is requesting the current date. */
			this->SendDate();
			break;

		case ADMIN_UPDATE_CLIENT_INFO:
			/* The admin is requesting client info. */
			if (d1 == UINT32_MAX) {
				this->SendClientInfo(nullptr, NetworkClientInfo::GetByClientID(CLIENT_ID_SERVER));
				for (const NetworkClientSocket *cs : NetworkClientSocket::Iterate()) {
					this->SendClientInfo(cs, cs->GetInfo());
				}
			} else {
				if (d1 == CLIENT_ID_SERVER) {
					this->SendClientInfo(nullptr, NetworkClientInfo::GetByClientID(CLIENT_ID_SERVER));
				} else {
					const NetworkClientSocket *cs = NetworkClientSocket::GetByClientID((ClientID)d1);
					if (cs != nullptr) this->SendClientInfo(cs, cs->GetInfo());
				}
			}
			break;

		case ADMIN_UPDATE_COMPANY_INFO:
			/* The admin is asking for company info. */
			if (d1 == UINT32_MAX) {
				for (const Company *company : Company::Iterate()) {
					this->SendCompanyInfo(company);
				}
			} else {
				const Company *company = Company::GetIfValid(d1);
				if (company != nullptr) this->SendCompanyInfo(company);
			}
			break;

		case ADMIN_UPDATE_COMPANY_ECONOMY:
			/* The admin is requesting economy info. */
			this->SendCompanyEconomy();
			break;

		case ADMIN_UPDATE_COMPANY_STATS:
			/* the admin is requesting company stats. */
			this->SendCompanyStats();
			break;

		case ADMIN_UPDATE_CMD_NAMES:
			/* The admin is requesting the names of DoCommands. */
			this->SendCmdNames();
			break;

		default:
			/* An unsupported "poll" update type. */
			Debug(net, 1, "[admin] Not supported poll {} ({}) from '{}' ({}).", type, d1, this->admin_name, this->admin_version);
			return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
	}

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::Receive_ADMIN_CHAT(Packet &p)
{
	if (this->status <= ADMIN_STATUS_AUTHENTICATE) return this->SendError(NETWORK_ERROR_NOT_EXPECTED);

	NetworkAction action = (NetworkAction)p.Recv_uint8();
	DestType desttype = (DestType)p.Recv_uint8();
	int dest = p.Recv_uint32();

	std::string msg = p.Recv_string(NETWORK_CHAT_LENGTH);

	switch (action) {
		case NETWORK_ACTION_CHAT:
		case NETWORK_ACTION_CHAT_CLIENT:
		case NETWORK_ACTION_CHAT_COMPANY:
		case NETWORK_ACTION_SERVER_MESSAGE:
			NetworkServerSendChat(action, desttype, dest, msg, _network_own_client_id, 0, true);
			break;

		default:
			Debug(net, 1, "[admin] Invalid chat action {} from admin '{}' ({}).", action, this->admin_name, this->admin_version);
			return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
	}

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::Receive_ADMIN_EXTERNAL_CHAT(Packet &p)
{
	if (this->status <= ADMIN_STATUS_AUTHENTICATE) return this->SendError(NETWORK_ERROR_NOT_EXPECTED);

	std::string source = p.Recv_string(NETWORK_CHAT_LENGTH);
	TextColour colour = (TextColour)p.Recv_uint16();
	std::string user = p.Recv_string(NETWORK_CHAT_LENGTH);
	std::string msg = p.Recv_string(NETWORK_CHAT_LENGTH);

	if (!IsValidConsoleColour(colour)) {
		Debug(net, 1, "[admin] Not supported chat colour {} ({}, {}, {}) from '{}' ({}).", (uint16_t)colour, source, user, msg, this->admin_name, this->admin_version);
		return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
	}

	NetworkServerSendExternalChat(source, colour, user, msg);

	return NETWORK_RECV_STATUS_OKAY;
}

/*
 * Secure authentication send and receive methods.
 */

NetworkRecvStatus ServerNetworkAdminSocketHandler::Receive_ADMIN_JOIN_SECURE(Packet &p)
{
	if (this->status != ADMIN_STATUS_INACTIVE) return this->SendError(NETWORK_ERROR_NOT_EXPECTED);

	this->admin_name = p.Recv_string(NETWORK_CLIENT_NAME_LENGTH);
	this->admin_version = p.Recv_string(NETWORK_REVISION_LENGTH);
	NetworkAuthenticationMethodMask method_mask{p.Recv_uint16()};

	/* Always exclude key exchange only, as that provides no credential checking. */
	method_mask.Reset(NetworkAuthenticationMethod::X25519_KeyExchangeOnly);

	if (this->admin_name.empty() || this->admin_version.empty()) {
		/* No name or version supplied. */
		return this->SendError(NETWORK_ERROR_ILLEGAL_PACKET);
	}

	auto handler = NetworkAuthenticationServerHandler::Create(&_admin_password_provider, &_admin_authorized_key_handler, method_mask);
	if (!handler->CanBeUsed()) return this->SendError(NETWORK_ERROR_NO_AUTHENTICATION_METHOD_AVAILABLE);

	this->authentication_handler = std::move(handler);
	Debug(net, 3, "[admin] '{}' ({}) has connected", this->admin_name, this->admin_version);

	return this->SendAuthRequest();
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::SendAuthRequest()
{
	this->status = ADMIN_STATUS_AUTHENTICATE;

	Debug(net, 6, "[admin] '{}' ({}) authenticating using {}", this->admin_name, this->admin_version, this->authentication_handler->GetName());

	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_AUTH_REQUEST);
	this->authentication_handler->SendRequest(*p);

	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::SendEnableEncryption()
{
	if (this->status != ADMIN_STATUS_AUTHENTICATE) return this->SendError(NETWORK_ERROR_NOT_EXPECTED);

	auto p = std::make_unique<Packet>(this, ADMIN_PACKET_SERVER_ENABLE_ENCRYPTION);
	this->authentication_handler->SendEnableEncryption(*p);
	this->SendPacket(std::move(p));

	return NETWORK_RECV_STATUS_OKAY;
}

NetworkRecvStatus ServerNetworkAdminSocketHandler::Receive_ADMIN_AUTH_RESPONSE(Packet &p)
{
	if (this->status != ADMIN_STATUS_AUTHENTICATE) return this->SendError(NETWORK_ERROR_NOT_EXPECTED);

	switch (this->authentication_handler->ReceiveResponse(p)) {
		case NetworkAuthenticationServerHandler::ResponseResult::Authenticated:
			Debug(net, 3, "[admin] '{}' ({}) authenticated", this->admin_name, this->admin_version);

			this->SendEnableEncryption();

			this->receive_encryption_handler = this->authentication_handler->CreateClientToServerEncryptionHandler();
			this->send_encryption_handler = this->authentication_handler->CreateServerToClientEncryptionHandler();
			this->authentication_handler = nullptr;
			return this->SendProtocol();

		case NetworkAuthenticationServerHandler::ResponseResult::RetryNextMethod:
			Debug(net, 6, "[admin] '{}' ({}) authentication failed, trying next method", this->admin_name, this->admin_version);
			return this->SendAuthRequest();

		case NetworkAuthenticationServerHandler::ResponseResult::NotAuthenticated:
		default:
			Debug(net, 3, "[admin] '{}' ({}) authentication failed", this->admin_name, this->admin_version);
			return this->SendError(NETWORK_ERROR_WRONG_PASSWORD);
	}
}

/*
 * Useful wrapper functions
 */

/**
 * Notify the admin network of a new client (if they did opt in for the respective update).
 * @param cs the client info.
 * @param new_client if this is a new client, send the respective packet too.
 */
void NetworkAdminClientInfo(const NetworkClientSocket *cs, bool new_client)
{
	for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::IterateActive()) {
		if (as->update_frequency[ADMIN_UPDATE_CLIENT_INFO].Test(AdminUpdateFrequency::Automatic)) {
			as->SendClientInfo(cs, cs->GetInfo());
			if (new_client) {
				as->SendClientJoin(cs->client_id);
			}
		}
	}
}

/**
 * Notify the admin network of a client update (if they did opt in for the respective update).
 * @param ci the client info.
 */
void NetworkAdminClientUpdate(const NetworkClientInfo *ci)
{
	for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::IterateActive()) {
		if (as->update_frequency[ADMIN_UPDATE_CLIENT_INFO].Test(AdminUpdateFrequency::Automatic)) {
			as->SendClientUpdate(ci);
		}
	}
}

/**
 * Notify the admin network that a client quit (if they have opt in for the respective update).
 * @param client_id of the client that quit.
 */
void NetworkAdminClientQuit(ClientID client_id)
{
	for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::IterateActive()) {
		if (as->update_frequency[ADMIN_UPDATE_CLIENT_INFO].Test(AdminUpdateFrequency::Automatic)) {
			as->SendClientQuit(client_id);
		}
	}
}

/**
 * Notify the admin network of a client error (if they have opt in for the respective update).
 * @param client_id the client that made the error.
 * @param error_code the error that was caused.
 */
void NetworkAdminClientError(ClientID client_id, NetworkErrorCode error_code)
{
	for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::IterateActive()) {
		if (as->update_frequency[ADMIN_UPDATE_CLIENT_INFO].Test(AdminUpdateFrequency::Automatic)) {
			as->SendClientError(client_id, error_code);
		}
	}
}

/**
 * Notify the admin network of a new company.
 * @param company the company of which details will be sent into the admin network.
 */
void NetworkAdminCompanyNew(const Company *company)
{
	if (company == nullptr) {
		Debug(net, 1, "[admin] Empty company given for update");
		return;
	}

	for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::IterateActive()) {
		if (as->update_frequency[ADMIN_UPDATE_COMPANY_INFO] != AdminUpdateFrequency::Automatic) continue;

		as->SendCompanyNew(company->index);
		as->SendCompanyInfo(company);
	}
}

/**
 * Notify the admin network of company updates.
 * @param company company of which updates are going to be sent into the admin network.
 */
void NetworkAdminCompanyUpdate(const Company *company)
{
	if (company == nullptr) return;

	for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::IterateActive()) {
		if (as->update_frequency[ADMIN_UPDATE_COMPANY_INFO] != AdminUpdateFrequency::Automatic) continue;

		as->SendCompanyUpdate(company);
	}
}

/**
 * Notify the admin network of a company to be removed (including the reason why).
 * @param company_id ID of the company that got removed.
 * @param bcrr the reason why the company got removed (e.g. bankruptcy).
 */
void NetworkAdminCompanyRemove(CompanyID company_id, AdminCompanyRemoveReason bcrr)
{
	for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::IterateActive()) {
		as->SendCompanyRemove(company_id, bcrr);
	}
}


/**
 * Send chat to the admin network (if they did opt in for the respective update).
 */
void NetworkAdminChat(NetworkAction action, DestType desttype, ClientID client_id, std::string_view msg, int64_t data, bool from_admin)
{
	if (from_admin) return;

	for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::IterateActive()) {
		if (as->update_frequency[ADMIN_UPDATE_CHAT].Test(AdminUpdateFrequency::Automatic)) {
			as->SendChat(action, desttype, client_id, msg, data);
		}
	}
}

/**
 * Pass the rcon reply to the admin.
 * @param admin_index The admin to give the reply.
 * @param colour_code The colour of the string.
 * @param string      The string to show.
 */
void NetworkServerSendAdminRcon(AdminID admin_index, TextColour colour_code, std::string_view string)
{
	ServerNetworkAdminSocketHandler::Get(admin_index)->SendRcon(colour_code, string);
}

/**
 * Send console to the admin network (if they did opt in for the respective update).
 * @param origin the origin of the message.
 * @param string the message as printed on the console.
 */
void NetworkAdminConsole(std::string_view origin, std::string_view string)
{
	for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::IterateActive()) {
		if (as->update_frequency[ADMIN_UPDATE_CONSOLE].Test(AdminUpdateFrequency::Automatic)) {
			as->SendConsole(origin, string);
		}
	}
}

/**
 * Send GameScript JSON to the admin network (if they did opt in for the respective update).
 * @param json The JSON data as received from the GameScript.
 */
void NetworkAdminGameScript(std::string_view json)
{
	for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::IterateActive()) {
		if (as->update_frequency[ADMIN_UPDATE_GAMESCRIPT].Test(AdminUpdateFrequency::Automatic)) {
			as->SendGameScript(json);
		}
	}
}

/**
 * Distribute CommandPacket details over the admin network for logging purposes.
 * @param owner The owner of the CommandPacket (who sent us the CommandPacket).
 * @param cp    The CommandPacket to be distributed.
 */
void NetworkAdminCmdLogging(const NetworkClientSocket *owner, const CommandPacket &cp)
{
	ClientID client_id = owner == nullptr ? _network_own_client_id : owner->client_id;

	for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::IterateActive()) {
		if (as->update_frequency[ADMIN_UPDATE_CMD_LOGGING].Test(AdminUpdateFrequency::Automatic)) {
			as->SendCmdLogging(client_id, cp);
		}
	}
}

/**
 * Send a Welcome packet to all connected admins
 */
void ServerNetworkAdminSocketHandler::WelcomeAll()
{
	for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::IterateActive()) {
		as->SendWelcome();
	}
}

/**
 * Send (push) updates to the admin network as they have registered for these updates.
 * @param freq the frequency to be processed.
 */
void NetworkAdminUpdate(AdminUpdateFrequency freq)
{
	for (ServerNetworkAdminSocketHandler *as : ServerNetworkAdminSocketHandler::IterateActive()) {
		for (int i = 0; i < ADMIN_UPDATE_END; i++) {
			if (as->update_frequency[i].Test(freq)) {
				/* Update the admin for the required details */
				switch (i) {
					case ADMIN_UPDATE_DATE:
						as->SendDate();
						break;

					case ADMIN_UPDATE_COMPANY_ECONOMY:
						as->SendCompanyEconomy();
						break;

					case ADMIN_UPDATE_COMPANY_STATS:
						as->SendCompanyStats();
						break;

					default: NOT_REACHED();
				}
			}
		}
	}
}
