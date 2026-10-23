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
};

struct COPLChunkHandler : ChunkHandler {
	COPLChunkHandler() : ChunkHandler('COPL', CH_TABLE) {}

	void Save() const override
	{
		SlTableHeader(_copilot_operation_desc);
		SlSetArrayIndex(0);
		SlObject(&_copilot_operation, _copilot_operation_desc);
	}

	void Load() const override
	{
		const std::vector<SaveLoad> slt = SlTableHeader(_copilot_operation_desc);
		if (SlIterateArray() == -1) return;
		SlObject(&_copilot_operation, slt);
		if (SlIterateArray() != -1) SlErrorCorrupt("Too many COPL entries");
		if (_copilot_operation.schema != 1) SlErrorCorrupt("Unsupported COPL schema");
		if (_copilot_operation.state > to_underlying(CopilotOperationState::Uncertain)) {
			SlErrorCorrupt("Invalid COPL operation state");
		}
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
