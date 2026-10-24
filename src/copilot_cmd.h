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
#include "direction_type.h"
#include "engine_type.h"
#include "vehicle_type.h"

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
	uint16_t schema = 5;
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
	uint8_t kind = 0; ///< 0: rail, 1: station, 2: depot, 3: purchase, 4: attach, 5: append order, 6: set running.
	uint8_t length = 0;
	uint32_t created_asset_id = UINT32_MAX;
	uint16_t engine_id = UINT16_MAX;
	uint32_t vehicle_id = UINT32_MAX;
	uint32_t target_id = UINT32_MAX;
	uint16_t order_index = 0;
	uint8_t policy = 0;
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

CommandCost CmdCopilotBuildInfrastructure(DoCommandFlags flags, TileIndex tile, RailType rail_type,
		uint8_t orientation, uint8_t kind, uint8_t length, Money max_cost,
		uint64_t operation_hi, uint64_t operation_lo, uint64_t payload_hash_hi, uint64_t payload_hash_lo);

DEF_CMD_TRAIT(CMD_COPILOT_BUILD_INFRASTRUCTURE, CmdCopilotBuildInfrastructure,
		CommandFlags({CommandFlag::Auto, CommandFlag::NoWater}), CommandType::LandscapeConstruction)

void CcCopilotBuildInfrastructure(Commands cmd, const CommandCost &result, TileIndex tile,
		RailType rail_type, uint8_t orientation, uint8_t kind, uint8_t length, Money max_cost,
		uint64_t operation_hi, uint64_t operation_lo, uint64_t payload_hash_hi, uint64_t payload_hash_lo);

std::tuple<CommandCost, VehicleID> CmdCopilotBuildTrain(DoCommandFlags flags, TileIndex tile,
		EngineID engine_id, Money max_cost, uint64_t operation_hi, uint64_t operation_lo,
		uint64_t payload_hash_hi, uint64_t payload_hash_lo);

DEF_CMD_TRAIT(CMD_COPILOT_BUILD_TRAIN, CmdCopilotBuildTrain, {}, CommandType::VehicleConstruction)

void CcCopilotBuildTrain(Commands cmd, const CommandCost &result, VehicleID vehicle_id, TileIndex tile,
		EngineID engine_id, Money max_cost, uint64_t operation_hi, uint64_t operation_lo,
		uint64_t payload_hash_hi, uint64_t payload_hash_lo);

CommandCost CmdCopilotConfigureTrain(DoCommandFlags flags, TileIndex tile, uint8_t action,
		VehicleID vehicle_id, uint32_t target_id, uint16_t order_index, uint8_t policy,
		Money max_cost, uint64_t operation_hi, uint64_t operation_lo,
		uint64_t payload_hash_hi, uint64_t payload_hash_lo);

DEF_CMD_TRAIT(CMD_COPILOT_CONFIGURE_TRAIN, CmdCopilotConfigureTrain, {}, CommandType::VehicleManagement)

void CcCopilotConfigureTrain(Commands cmd, const CommandCost &result, TileIndex tile, uint8_t action,
		VehicleID vehicle_id, uint32_t target_id, uint16_t order_index, uint8_t policy,
		Money max_cost, uint64_t operation_hi, uint64_t operation_lo,
		uint64_t payload_hash_hi, uint64_t payload_hash_lo);

#endif /* COPILOT_CMD_H */
