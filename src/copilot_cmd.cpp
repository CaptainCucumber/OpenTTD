/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License version 2.
 */

/** @file copilot_cmd.cpp Bounded commands and durable state for OpenTTD Copilot. */

#include "stdafx.h"
#include "copilot_cmd.h"

#include "command_func.h"
#include "tile_map.h"
#include "landscape.h"
#include "rail_cmd.h"
#include "station_cmd.h"
#include "station_map.h"
#include "newgrf_station.h"
#include "depot_map.h"
#include "network/network_internal.h"

#include "safeguards.h"

CopilotOperation _copilot_operation;
std::vector<CopilotOperation> _copilot_operation_history;

void ResetCopilotOperation()
{
	_copilot_operation = {};
	_copilot_operation_history.clear();
}

const CopilotOperation *FindCopilotOperation(uint64_t operation_hi, uint64_t operation_lo)
{
	if (_copilot_operation.state != to_underlying(CopilotOperationState::Empty) &&
			_copilot_operation.operation_hi == operation_hi && _copilot_operation.operation_lo == operation_lo) {
		return &_copilot_operation;
	}
	for (const CopilotOperation &operation : _copilot_operation_history) {
		if (operation.operation_hi == operation_hi && operation.operation_lo == operation_lo) return &operation;
	}
	return nullptr;
}

const CopilotOperation *GetCopilotHead()
{
	if (_copilot_operation.state == to_underlying(CopilotOperationState::Completed)) return &_copilot_operation;
	for (auto it = _copilot_operation_history.rbegin(); it != _copilot_operation_history.rend(); ++it) {
		if (it->state == to_underlying(CopilotOperationState::Completed)) return &*it;
	}
	return nullptr;
}

CommandCost CmdCopilotBuildSingleRail(DoCommandFlags flags, TileIndex tile, RailType rail_type, Track track,
		Money max_cost, uint64_t operation_hi, uint64_t operation_lo,
		uint64_t payload_hash_hi, uint64_t payload_hash_lo)
{
	/* These fields are command identity, serialized through the normal command
	 * pipeline. They are deliberately not interpreted by the rail primitive. */
	(void)operation_hi;
	(void)operation_lo;
	(void)payload_hash_hi;
	(void)payload_hash_lo;

	if (max_cost < 0) return CMD_ERROR;
	/* M2b is one flat, clear tile. Broader terrain and infrastructure
	 * policies belong to the route planner/reliability milestones. */
	if (!IsTileType(tile, MP_CLEAR) || GetTileSlope(tile) != SLOPE_FLAT) return CMD_ERROR;

	/* Recheck the cap before the nested command is allowed to execute. The
	 * outer command is tested again at its scheduled network frame. */
	CommandCost quote = Command<CMD_BUILD_SINGLE_RAIL>::Do(
			DoCommandFlags{flags}.Reset(DoCommandFlag::Execute), tile, rail_type, track, false);
	if (quote.Failed()) return quote;
	if (quote.GetCost() > max_cost) return CMD_ERROR;
	if (!flags.Test(DoCommandFlag::Execute)) return quote;
	return Command<CMD_BUILD_SINGLE_RAIL>::Do(flags, tile, rail_type, track, false);
}

void CcCopilotBuildSingleRail(Commands cmd, const CommandCost &result, TileIndex tile,
		RailType rail_type, Track track, Money max_cost, uint64_t operation_hi,
		uint64_t operation_lo, uint64_t payload_hash_hi, uint64_t payload_hash_lo)
{
	if (cmd != CMD_COPILOT_BUILD_SINGLE_RAIL) return;
	if (_copilot_operation.state != to_underlying(CopilotOperationState::Pending)) return;
	if (_copilot_operation.kind != 0) return;
	if (_copilot_operation.operation_hi != operation_hi || _copilot_operation.operation_lo != operation_lo) return;
	if (_copilot_operation.payload_hash_hi != payload_hash_hi || _copilot_operation.payload_hash_lo != payload_hash_lo) return;
	if (_copilot_operation.tile != tile.base() || _copilot_operation.rail_type != to_underlying(rail_type) ||
			_copilot_operation.track != to_underlying(track) || _copilot_operation.max_cost != max_cost.base()) return;

	_copilot_operation.actual_cost = result.GetCost();
	_copilot_operation.error_message = result.Failed() ? result.GetErrorMessage() : 0;
	_copilot_operation.completed_frame = _frame_counter;
	if (result.Succeeded()) {
		_copilot_operation.state = to_underlying(CopilotOperationState::Completed);
		_copilot_operation.sequence++;
	} else {
		_copilot_operation.state = to_underlying(CopilotOperationState::Rejected);
	}
}

CommandCost CmdCopilotBuildInfrastructure(DoCommandFlags flags, TileIndex tile, RailType rail_type,
		uint8_t orientation, uint8_t kind, uint8_t length, Money max_cost,
		uint64_t operation_hi, uint64_t operation_lo, uint64_t payload_hash_hi, uint64_t payload_hash_lo)
{
	(void)operation_hi;
	(void)operation_lo;
	(void)payload_hash_hi;
	(void)payload_hash_lo;
	if (max_cost < 0 || (kind != 1 && kind != 2)) return CMD_ERROR;
	if (kind == 1 && (orientation > 1 || length < 1 || length > 7)) return CMD_ERROR;
	if (kind == 2 && (orientation > 3 || length != 1)) return CMD_ERROR;

	/* Restrict this first slice to new construction on level clear land,
	 * never replacement, station joining, or automatic terrain preparation. */
	for (uint8_t i = 0; i < length; i++) {
		uint x = TileX(tile) + (kind == 1 && orientation == 0 ? i : 0);
		uint y = TileY(tile) + (kind == 1 && orientation == 1 ? i : 0);
		if (x >= Map::SizeX() || y >= Map::SizeY()) return CMD_ERROR;
		TileIndex part = TileXY(x, y);
		if (!IsValidTile(part) || !IsTileType(part, MP_CLEAR) || GetTileSlope(part) != SLOPE_FLAT ||
				GetTileZ(part) != GetTileZ(tile)) return CMD_ERROR;
	}
	auto command = [&](DoCommandFlags command_flags) {
		if (kind == 1) {
			return Command<CMD_BUILD_RAIL_STATION>::Do(command_flags, tile, rail_type,
					static_cast<Axis>(orientation), 1, length, STAT_CLASS_DFLT, 0, NEW_STATION, true);
		}
		return Command<CMD_BUILD_TRAIN_DEPOT>::Do(command_flags, tile, rail_type,
				static_cast<DiagDirection>(orientation));
	};
	CommandCost quote = command(DoCommandFlags{flags}.Reset(DoCommandFlag::Execute));
	if (quote.Failed()) return quote;
	if (quote.GetCost() > max_cost) return CMD_ERROR;
	if (!flags.Test(DoCommandFlag::Execute)) return quote;
	return command(flags);
}

void CcCopilotBuildInfrastructure(Commands cmd, const CommandCost &result, TileIndex tile,
		RailType rail_type, uint8_t orientation, uint8_t kind, uint8_t length, Money max_cost,
		uint64_t operation_hi, uint64_t operation_lo, uint64_t payload_hash_hi, uint64_t payload_hash_lo)
{
	if (cmd != CMD_COPILOT_BUILD_INFRASTRUCTURE) return;
	if (_copilot_operation.state != to_underlying(CopilotOperationState::Pending)) return;
	if (_copilot_operation.operation_hi != operation_hi || _copilot_operation.operation_lo != operation_lo ||
			_copilot_operation.payload_hash_hi != payload_hash_hi || _copilot_operation.payload_hash_lo != payload_hash_lo ||
			_copilot_operation.tile != tile.base() || _copilot_operation.rail_type != to_underlying(rail_type) ||
			_copilot_operation.track != orientation || _copilot_operation.kind != kind ||
			_copilot_operation.length != length || _copilot_operation.max_cost != max_cost.base()) return;
	_copilot_operation.actual_cost = result.GetCost();
	_copilot_operation.error_message = result.Failed() ? result.GetErrorMessage() : 0;
	_copilot_operation.completed_frame = _frame_counter;
	if (result.Succeeded()) {
		_copilot_operation.state = to_underlying(CopilotOperationState::Completed);
		if (kind == 1 && IsRailStationTile(tile) && GetTileOwner(tile).base() == _copilot_operation.company) {
			_copilot_operation.created_asset_id = GetStationIndex(tile).base();
		} else if (kind == 2 && IsRailDepotTile(tile) && GetTileOwner(tile).base() == _copilot_operation.company) {
			_copilot_operation.created_asset_id = GetDepotIndex(tile).base();
		} else {
			_copilot_operation.state = to_underlying(CopilotOperationState::Uncertain);
		}
		if (_copilot_operation.state == to_underlying(CopilotOperationState::Completed)) _copilot_operation.sequence++;
	} else {
		_copilot_operation.state = to_underlying(CopilotOperationState::Rejected);
	}
}
