/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License version 2.
 */

/** @file copilot_sl.cpp Save/load the bounded OpenTTD Copilot operation journal. */

#include "../stdafx.h"
#include "saveload.h"

#include "../copilot_cmd.h"

#include "../safeguards.h"

static const SaveLoad _copilot_operation_desc[] = {
	SLE_VAR(CopilotOperation, schema, SLE_UINT16),
	SLE_VAR(CopilotOperation, state, SLE_UINT8),
	SLE_VAR(CopilotOperation, company, SLE_UINT8),
	SLE_VAR(CopilotOperation, tile, SLE_UINT32),
	SLE_VAR(CopilotOperation, rail_type, SLE_UINT8),
	SLE_VAR(CopilotOperation, track, SLE_UINT8),
	SLE_VAR(CopilotOperation, operation_hi, SLE_UINT64),
	SLE_VAR(CopilotOperation, operation_lo, SLE_UINT64),
	SLE_VAR(CopilotOperation, payload_hash_hi, SLE_UINT64),
	SLE_VAR(CopilotOperation, payload_hash_lo, SLE_UINT64),
	SLE_VAR(CopilotOperation, max_cost, SLE_INT64),
	SLE_VAR(CopilotOperation, actual_cost, SLE_INT64),
	SLE_VAR(CopilotOperation, error_message, SLE_UINT32),
	SLE_VAR(CopilotOperation, sequence, SLE_UINT64),
	SLE_VAR(CopilotOperation, completed_frame, SLE_UINT32),
	SLE_VAR(CopilotOperation, kind, SLE_UINT8),
	SLE_VAR(CopilotOperation, length, SLE_UINT8),
	SLE_VAR(CopilotOperation, created_asset_id, SLE_UINT32),
	SLE_VAR(CopilotOperation, engine_id, SLE_UINT16),
	SLE_VAR(CopilotOperation, vehicle_id, SLE_UINT32),
	SLE_VAR(CopilotOperation, target_id, SLE_UINT32),
	SLE_VAR(CopilotOperation, order_index, SLE_UINT16),
	SLE_VAR(CopilotOperation, policy, SLE_UINT8),
};

struct COPLChunkHandler : ChunkHandler {
	COPLChunkHandler() : ChunkHandler('COPL', CH_TABLE) {}

	void Save() const override
	{
		SlTableHeader(_copilot_operation_desc);
		SlSetArrayIndex(0);
		SlObject(&_copilot_operation, _copilot_operation_desc);
		for (size_t i = 0; i < _copilot_operation_history.size(); i++) {
			SlSetArrayIndex(i + 1);
			SlObject(&_copilot_operation_history[i], _copilot_operation_desc);
		}
	}

	void Load() const override
	{
		const std::vector<SaveLoad> slt = SlTableHeader(_copilot_operation_desc);
		int index = SlIterateArray();
		if (index == -1) return;
		if (index != 0) SlErrorCorrupt("Missing COPL active entry");
		SlObject(&_copilot_operation, slt);
		bool legacy = _copilot_operation.schema == 1;
		if (_copilot_operation.schema < 1 || _copilot_operation.schema > 5) SlErrorCorrupt("Unsupported COPL schema");
		_copilot_operation_history.clear();
		while ((index = SlIterateArray()) != -1) {
			if (legacy || _copilot_operation_history.size() + 1 >= COPILOT_MAX_OPERATIONS ||
					index != static_cast<int>(_copilot_operation_history.size() + 1)) {
				SlErrorCorrupt("Invalid COPL history size or index");
			}
			CopilotOperation operation;
			SlObject(&operation, slt);
			if (operation.schema < 2 || operation.schema > 5) SlErrorCorrupt("Unsupported COPL history schema");
			_copilot_operation_history.push_back(operation);
		}

		uint64_t sequence = 0;
		auto validate = [&](CopilotOperation &operation, bool active) {
			if (operation.schema < 3) {
				operation.kind = 0;
				operation.length = 0;
				operation.created_asset_id = UINT32_MAX;
				operation.schema = 3;
			}
			if (operation.schema < 4) {
				operation.engine_id = UINT16_MAX;
				operation.schema = 4;
			}
			if (operation.schema < 5) {
				operation.vehicle_id = UINT32_MAX;
				operation.target_id = UINT32_MAX;
				operation.order_index = 0;
				operation.policy = 0;
				operation.schema = 5;
			}
			auto state = static_cast<CopilotOperationState>(operation.state);
			if (state == CopilotOperationState::Empty) {
				if (!active || !_copilot_operation_history.empty() || operation.sequence != 0) {
					SlErrorCorrupt("Invalid empty COPL operation");
				}
				return;
			}
			if (operation.state > to_underlying(CopilotOperationState::Uncertain) ||
					(!active && state != CopilotOperationState::Completed && state != CopilotOperationState::Rejected)) {
				SlErrorCorrupt("Invalid COPL operation state");
			}
			if (operation.operation_hi == 0 && operation.operation_lo == 0) SlErrorCorrupt("Invalid COPL operation ID");
			bool geometry_valid = (operation.kind == 0 && operation.track < TRACK_END && operation.length == 0) ||
					(operation.kind == 1 && operation.track <= 1 && operation.length >= 1 && operation.length <= 7) ||
					(operation.kind == 2 && operation.track <= 3 && operation.length == 1) ||
					(operation.kind >= 3 && operation.kind <= 6 && operation.track == 0 && operation.length == 0) ||
					(operation.kind == 7 && operation.track > 0 && operation.track <= 0x0F && operation.length == 0) ||
					(operation.kind >= 8 && operation.kind <= 9 && operation.track <= 3 && operation.length == 0) ||
					(operation.kind >= 10 && operation.kind <= 12 && operation.track == 0 && operation.length == 0);
			bool engine_valid = operation.kind == 3 ?
					(operation.engine_id != UINT16_MAX && operation.rail_type == INVALID_RAILTYPE) :
					operation.kind == 10 ?
					(operation.engine_id != UINT16_MAX && operation.rail_type == INVALID_RAILTYPE) :
					(operation.engine_id == UINT16_MAX &&
					(operation.kind >= 4 ? operation.rail_type == INVALID_RAILTYPE : operation.rail_type < RAILTYPE_END));
			bool config_valid =
					((operation.kind < 4 || (operation.kind >= 7 && operation.kind <= 10)) &&
						operation.vehicle_id == UINT32_MAX && operation.target_id == UINT32_MAX &&
						operation.order_index == 0 && operation.policy == 0) ||
					(operation.vehicle_id != UINT32_MAX &&
						((operation.kind == 4 && operation.target_id != UINT32_MAX &&
							operation.order_index == 0 && operation.policy == 0) ||
						(operation.kind == 5 && operation.target_id < UINT16_MAX &&
							operation.order_index <= 1 && operation.policy <= 1) ||
						(operation.kind == 6 && operation.target_id == UINT32_MAX &&
							operation.order_index == 0 && operation.policy <= 1) ||
						(operation.kind == 11 && operation.target_id < UINT16_MAX &&
							operation.order_index < COPILOT_MAX_BUS_ORDERS && operation.policy == 0) ||
						(operation.kind == 12 && operation.target_id == UINT32_MAX &&
							operation.order_index == 0 && operation.policy <= 1)));
			if (!geometry_valid || !engine_valid || !config_valid || operation.max_cost < 0) {
				SlErrorCorrupt("Invalid COPL command geometry");
			}
			bool has_created_id = operation.created_asset_id != UINT32_MAX;
			if (has_created_id != (((operation.kind >= 1 && operation.kind <= 3) ||
					(operation.kind >= 8 && operation.kind <= 10)) && state == CopilotOperationState::Completed)) {
				SlErrorCorrupt("Invalid COPL created asset identity");
			}
			if (FindCopilotOperation(operation.operation_hi, operation.operation_lo) != &operation) {
				SlErrorCorrupt("Duplicate COPL operation ID");
			}
			if (state == CopilotOperationState::Completed) sequence++;
			if (operation.sequence != sequence) SlErrorCorrupt("Invalid COPL sequence");
		};
		for (CopilotOperation &operation : _copilot_operation_history) validate(operation, false);
		validate(_copilot_operation, true);
		_copilot_operation.schema = 5;
		/* The network command queue is not reconstructed as an operation retry.
		 * A saved pending intent has an unknown outcome after load. */
		if (_copilot_operation.state == to_underlying(CopilotOperationState::Pending)) {
			_copilot_operation.state = to_underlying(CopilotOperationState::Uncertain);
		}
	}
};

static const COPLChunkHandler COPL;
static const ChunkHandlerRef copilot_chunk_handlers[] = {
	COPL,
};

extern const ChunkHandlerTable _copilot_chunk_handlers(copilot_chunk_handlers);
