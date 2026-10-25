/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file copilot_bridge.h Transport-neutral OpenTTD Copilot request dispatcher. */

#ifndef COPILOT_BRIDGE_H
#define COPILOT_BRIDGE_H

static constexpr uint8_t COPILOT_BRIDGE_LOOPBACK_REQUIRED = 4;

enum class CopilotBridgeMode {
	Admin,
	LocalSinglePlayer,
};

struct CopilotBridgeResponse {
	std::vector<uint8_t> payload;
	bool malformed = false;

	static CopilotBridgeResponse Malformed() { return {{}, true}; }
};

CopilotBridgeResponse DispatchCopilotBridgeRequest(std::span<const uint8_t> request, CopilotBridgeMode mode);
CopilotBridgeResponse MakeCopilotBridgeErrorResponse(uint8_t request_kind, uint32_t request_id, uint8_t result);

#endif /* COPILOT_BRIDGE_H */
