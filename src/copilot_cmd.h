/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License version 2.
 */

/** @file copilot_cmd.h Bounded commands and durable state for OpenTTD Copilot. */

#ifndef COPILOT_CMD_H
#define COPILOT_CMD_H

#include "command_type.h"
#include "company_type.h"
#include "rail_type.h"
#include "track_type.h"

constexpr size_t COPILOT_MAX_OPERATIONS = 64;

enum class CopilotOperationState : uint8_t {
	Empty = 0,
	Pending = 1,
	Completed = 2,
	Rejected = 3,
	Uncertain = 4,
};

/** Retained operation outcome; sequence is the successful-operation count at its completion. */
struct CopilotOperation {
	uint16_t schema = 2;
	uint8_t state = to_underlying(CopilotOperationState::Empty);
	uint8_t company = 0xFF;
	uint32_t tile = 0;
	uint8_t rail_type = 0;
	uint8_t track = 0;
	uint64_t operation_hi = 0;
	uint64_t operation_lo = 0;
	uint64_t payload_hash_hi = 0;
	uint64_t payload_hash_lo = 0;
	int64_t max_cost = 0;
	int64_t actual_cost = 0;
	uint32_t error_message = 0;
	uint64_t sequence = 0;
	uint32_t completed_frame = 0;
};

extern CopilotOperation _copilot_operation;
extern std::vector<CopilotOperation> _copilot_operation_history;

void ResetCopilotOperation();
const CopilotOperation *FindCopilotOperation(uint64_t operation_hi, uint64_t operation_lo);
const CopilotOperation *GetCopilotHead();

CommandCost CmdCopilotBuildSingleRail(DoCommandFlags flags, TileIndex tile, RailType rail_type, Track track,
		Money max_cost, uint64_t operation_hi, uint64_t operation_lo,
		uint64_t payload_hash_hi, uint64_t payload_hash_lo);

DEF_CMD_TRAIT(CMD_COPILOT_BUILD_SINGLE_RAIL, CmdCopilotBuildSingleRail,
		CommandFlags({CommandFlag::Auto, CommandFlag::NoWater}), CommandType::LandscapeConstruction)

void CcCopilotBuildSingleRail(Commands cmd, const CommandCost &result, TileIndex tile,
		RailType rail_type, Track track, Money max_cost, uint64_t operation_hi,
		uint64_t operation_lo, uint64_t payload_hash_hi, uint64_t payload_hash_lo);

#endif /* COPILOT_CMD_H */
