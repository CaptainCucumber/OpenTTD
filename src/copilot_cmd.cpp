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
