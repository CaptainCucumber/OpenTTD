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
#include "vehicle_cmd.h"
#include "train.h"
#include "engine_base.h"
#include "engine_func.h"
#include "train_cmd.h"
#include "order_cmd.h"
#include "station_base.h"
#include "road_cmd.h"
#include "road_map.h"
#include "roadveh.h"
#include "articulated_vehicles.h"
#include "newgrf_roadstop.h"
#include "network/network_internal.h"
#include "timer/timer_game_tick.h"

#include "safeguards.h"

CopilotOperation _copilot_operation;
std::vector<CopilotOperation> _copilot_operation_history;

void ResetCopilotOperation()
{
	_copilot_operation = {};
	_copilot_operation_history.clear();
}

uint32_t CopilotCurrentFrame()
{
	/* Preserve protocol-v3 Admin frames; local single-player uses the saved game tick. */
	return _networking ? _frame_counter : static_cast<uint32_t>(TimerGameTick::counter);
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
	_copilot_operation.completed_frame = CopilotCurrentFrame();
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
	_copilot_operation.completed_frame = CopilotCurrentFrame();
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

std::tuple<CommandCost, VehicleID> CmdCopilotBuildTrain(DoCommandFlags flags, TileIndex tile,
		EngineID engine_id, Money max_cost, uint64_t operation_hi, uint64_t operation_lo,
		uint64_t payload_hash_hi, uint64_t payload_hash_lo)
{
	(void)operation_hi;
	(void)operation_lo;
	(void)payload_hash_hi;
	(void)payload_hash_lo;
	if (max_cost < 0 || !IsRailDepotTile(tile) || !IsTileOwner(tile, _current_company) ||
			!IsEngineBuildable(engine_id, VEH_TRAIN, _current_company)) return {CMD_ERROR, VehicleID::Invalid()};

	/* Stock wagon construction can join a same-type free wagon chain. Refuse
	 * that implicit reuse; engine purchase also disables free-wagon adoption. */
	if (Engine::Get(engine_id)->VehInfo<RailVehicleInfo>().railveh_type == RAILVEH_WAGON) {
		for (const Train *train : Train::Iterate()) {
			if (train->tile == tile && train->owner == _current_company && train->IsInDepot() &&
					train->IsFreeWagon() && train->engine_type == engine_id) return {CMD_ERROR, VehicleID::Invalid()};
		}
	}
	auto quote = Command<CMD_BUILD_VEHICLE>::Do(
			DoCommandFlags{flags}.Reset(DoCommandFlag::Execute), tile, engine_id, false, INVALID_CARGO, INVALID_CLIENT_ID);
	const CommandCost &cost = std::get<0>(quote);
	if (cost.Failed()) return {cost, VehicleID::Invalid()};
	if (cost.GetCost() > max_cost) return {CMD_ERROR, VehicleID::Invalid()};
	if (!flags.Test(DoCommandFlag::Execute)) return {cost, VehicleID::Invalid()};
	auto result = Command<CMD_BUILD_VEHICLE>::Do(flags, tile, engine_id, false, INVALID_CARGO, INVALID_CLIENT_ID);
	return {std::get<0>(result), std::get<1>(result)};
}

void CcCopilotBuildTrain(Commands cmd, const CommandCost &result, VehicleID vehicle_id, TileIndex tile,
		EngineID engine_id, Money max_cost, uint64_t operation_hi, uint64_t operation_lo,
		uint64_t payload_hash_hi, uint64_t payload_hash_lo)
{
	if (cmd != CMD_COPILOT_BUILD_TRAIN || _copilot_operation.kind != 3 ||
			_copilot_operation.state != to_underlying(CopilotOperationState::Pending)) return;
	if (_copilot_operation.operation_hi != operation_hi || _copilot_operation.operation_lo != operation_lo ||
			_copilot_operation.payload_hash_hi != payload_hash_hi || _copilot_operation.payload_hash_lo != payload_hash_lo ||
			_copilot_operation.tile != tile.base() || _copilot_operation.engine_id != engine_id.base() ||
			_copilot_operation.max_cost != max_cost.base()) return;
	_copilot_operation.actual_cost = result.GetCost();
	_copilot_operation.error_message = result.Failed() ? result.GetErrorMessage() : 0;
	_copilot_operation.completed_frame = CopilotCurrentFrame();
	if (result.Failed()) {
		_copilot_operation.state = to_underlying(CopilotOperationState::Rejected);
		return;
	}
	const Train *train = Train::GetIfValid(vehicle_id);
	if (train == nullptr || train->owner.base() != _copilot_operation.company ||
			train->engine_type != engine_id || train->tile != tile || !train->IsInDepot()) {
		_copilot_operation.state = to_underlying(CopilotOperationState::Uncertain);
		return;
	}
	_copilot_operation.created_asset_id = vehicle_id.base();
	_copilot_operation.state = to_underlying(CopilotOperationState::Completed);
	_copilot_operation.sequence++;
}

static Order CopilotStationOrder(uint32_t station_id, uint8_t policy)
{
	Order order;
	order.MakeGoToStation(StationID{static_cast<uint16_t>(station_id)});
	order.SetLoadType(policy == 0 ? OrderLoadType::FullLoad : OrderLoadType::NoLoad);
	order.SetUnloadType(policy == 0 ? OrderUnloadType::NoUnload : OrderUnloadType::UnloadIfPossible);
	return order;
}

CommandCost CmdCopilotConfigureTrain(DoCommandFlags flags, TileIndex tile, uint8_t action,
		VehicleID vehicle_id, uint32_t target_id, uint16_t order_index, uint8_t policy,
		Money max_cost, uint64_t operation_hi, uint64_t operation_lo,
		uint64_t payload_hash_hi, uint64_t payload_hash_lo)
{
	(void)operation_hi;
	(void)operation_lo;
	(void)payload_hash_hi;
	(void)payload_hash_lo;
	Train *train = Train::GetIfValid(vehicle_id);
	if (max_cost < 0 || train == nullptr || train->owner != _current_company ||
			train->tile != tile || train->vehstatus.Test(VehState::Crashed)) return CMD_ERROR;
	auto command = [&](DoCommandFlags command_flags) -> CommandCost {
		switch (action) {
			case 4: {
				Train *destination = Train::GetIfValid(VehicleID{target_id});
				if (policy != 0 || order_index != 0 || !train->IsFreeWagon() || train->First() != train ||
						!train->IsStoppedInDepot() || destination == nullptr || !destination->IsPrimaryVehicle() ||
						destination->owner != _current_company || destination->tile != tile ||
						!destination->IsStoppedInDepot() || destination->vehstatus.Test(VehState::Crashed)) return CMD_ERROR;
				return Command<CMD_MOVE_RAIL_VEHICLE>::Do(command_flags, vehicle_id, destination->Last()->index, true);
			}
			case 5: {
				if (policy > 1 || order_index > 1 || train->GetNumOrders() != order_index ||
						!train->IsPrimaryVehicle() || !train->IsStoppedInDepot() || train->IsOrderListShared() ||
						target_id >= UINT16_MAX) return CMD_ERROR;
				const Station *station = Station::GetIfValid(StationID{static_cast<uint16_t>(target_id)});
				if (station == nullptr || station->owner != _current_company ||
						!station->facilities.Test(StationFacility::Train)) return CMD_ERROR;
				return Command<CMD_INSERT_ORDER>::Do(command_flags, vehicle_id, order_index, CopilotStationOrder(target_id, policy));
			}
			case 6: {
				if (policy > 1 || order_index != 0 || target_id != UINT32_MAX || !train->IsPrimaryVehicle()) return CMD_ERROR;
				bool running = !train->vehstatus.Test(VehState::Stopped);
				if (running == (policy == 1)) return CommandCost();
				if (policy == 1 && (!train->IsStoppedInDepot() || train->GetNumOrders() != 2)) return CMD_ERROR;
				return Command<CMD_START_STOP_VEHICLE>::Do(command_flags, vehicle_id, true);
			}
			default: return CMD_ERROR;
		}
	};
	CommandCost quote = command(DoCommandFlags{flags}.Reset(DoCommandFlag::Execute));
	if (quote.Failed()) return quote;
	if (quote.GetCost() > max_cost) return CMD_ERROR;
	if (!flags.Test(DoCommandFlag::Execute)) return quote;
	return command(flags);
}

void CcCopilotConfigureTrain(Commands cmd, const CommandCost &result, TileIndex tile, uint8_t action,
		VehicleID vehicle_id, uint32_t target_id, uint16_t order_index, uint8_t policy,
		Money max_cost, uint64_t operation_hi, uint64_t operation_lo,
		uint64_t payload_hash_hi, uint64_t payload_hash_lo)
{
	if (cmd != CMD_COPILOT_CONFIGURE_TRAIN ||
			_copilot_operation.state != to_underlying(CopilotOperationState::Pending)) return;
	if (_copilot_operation.operation_hi != operation_hi || _copilot_operation.operation_lo != operation_lo ||
			_copilot_operation.payload_hash_hi != payload_hash_hi || _copilot_operation.payload_hash_lo != payload_hash_lo ||
			_copilot_operation.tile != tile.base() || _copilot_operation.kind != action ||
			_copilot_operation.vehicle_id != vehicle_id.base() || _copilot_operation.target_id != target_id ||
			_copilot_operation.order_index != order_index || _copilot_operation.policy != policy ||
			_copilot_operation.max_cost != max_cost.base()) return;
	_copilot_operation.actual_cost = result.GetCost();
	_copilot_operation.error_message = result.Failed() ? result.GetErrorMessage() : 0;
	_copilot_operation.completed_frame = CopilotCurrentFrame();
	if (result.Failed()) {
		_copilot_operation.state = to_underlying(CopilotOperationState::Rejected);
		return;
	}
	const Train *train = Train::GetIfValid(vehicle_id);
	bool verified = result.GetCost() <= max_cost && train != nullptr &&
			train->owner.base() == _copilot_operation.company;
	if (verified && action == 4) {
		verified = train->First()->index == VehicleID{target_id};
	} else if (verified && action == 5) {
		verified = train->GetNumOrders() == order_index + 1;
		if (verified) {
			const Order &order = train->Orders()[order_index];
			Order expected = CopilotStationOrder(target_id, policy);
			verified = order.IsType(OT_GOTO_STATION) && order.GetDestination() == expected.GetDestination() &&
					order.GetLoadType() == expected.GetLoadType() && order.GetUnloadType() == expected.GetUnloadType();
		}
	} else if (verified && action == 6) {
		verified = train->vehstatus.Test(VehState::Stopped) == (policy == 0);
	}
	_copilot_operation.state = to_underlying(verified ? CopilotOperationState::Completed : CopilotOperationState::Uncertain);
	if (verified) _copilot_operation.sequence++;
}

static Order CopilotBusStationOrder(uint32_t station_id)
{
	Order order;
	order.MakeGoToStation(StationID{static_cast<uint16_t>(station_id)});
	order.SetLoadType(OrderLoadType::LoadIfPossible);
	order.SetUnloadType(OrderUnloadType::UnloadIfPossible);
	order.SetStopLocation(OrderStopLocation::FarEnd);
	return order;
}

static RoadBits CopilotWireRoadBits(uint8_t bits)
{
	return static_cast<RoadBits>(
			(bits & 0x01 ? ROAD_NE : ROAD_NONE) |
			(bits & 0x02 ? ROAD_SE : ROAD_NONE) |
			(bits & 0x04 ? ROAD_SW : ROAD_NONE) |
			(bits & 0x08 ? ROAD_NW : ROAD_NONE));
}

std::tuple<CommandCost, VehicleID> CmdCopilotBusAction(DoCommandFlags flags, TileIndex tile,
		uint8_t action, uint8_t orientation, uint8_t road_bits, EngineID engine_id,
		VehicleID vehicle_id, uint32_t target_id, uint16_t order_index, uint8_t policy,
		Money max_cost, uint64_t operation_hi, uint64_t operation_lo,
		uint64_t payload_hash_hi, uint64_t payload_hash_lo)
{
	(void)operation_hi;
	(void)operation_lo;
	(void)payload_hash_hi;
	(void)payload_hash_lo;
	if (max_cost < 0 || action < 7 || action > 12 || orientation > 3 || road_bits > 0x0F ||
			(action == 8 && orientation > 1)) {
		return {CMD_ERROR, VehicleID::Invalid()};
	}
	auto command = [&](DoCommandFlags command_flags) -> std::tuple<CommandCost, VehicleID> {
		switch (action) {
			case 7: {
				if (road_bits == 0 || engine_id != EngineID::Invalid() || vehicle_id != VehicleID::Invalid() ||
						target_id != UINT32_MAX || order_index != 0 || policy != 0) return {CMD_ERROR, VehicleID::Invalid()};
				if (IsTileType(tile, MP_ROAD)) {
					if (!IsNormalRoadTile(tile) || HasRoadWorks(tile) ||
							GetDisallowedRoadDirections(tile) != DRD_NONE ||
							(GetRoadTypeRoad(tile) != ROADTYPE_ROAD && GetRoadTypeRoad(tile) != INVALID_ROADTYPE)) {
						return {CMD_ERROR, VehicleID::Invalid()};
					}
				} else if (!IsTileType(tile, MP_CLEAR) && !IsTileType(tile, MP_TREES)) {
					return {CMD_ERROR, VehicleID::Invalid()};
				}
				return {Command<CMD_BUILD_ROAD>::Do(command_flags, tile, CopilotWireRoadBits(road_bits),
						ROADTYPE_ROAD, DRD_NONE, TownID::Invalid()), VehicleID::Invalid()};
			}
			case 8: {
				if (road_bits != 0 || engine_id != EngineID::Invalid() || vehicle_id != VehicleID::Invalid() ||
						target_id != UINT32_MAX || order_index != 0 || policy != 0 ||
						!IsNormalRoadTile(tile) || GetRoadTypeRoad(tile) != ROADTYPE_ROAD ||
						GetDisallowedRoadDirections(tile) != DRD_NONE ||
						!IsStraightRoad(GetRoadBits(tile, RTT_ROAD)) ||
						GetRoadBits(tile, RTT_ROAD) != (orientation == 0 ? ROAD_X : ROAD_Y)) {
					return {CMD_ERROR, VehicleID::Invalid()};
				}
				return {Command<CMD_BUILD_ROAD_STOP>::Do(command_flags, tile, 1, 1, RoadStopType::Bus, true,
						orientation == 0 ? DIAGDIR_NE : DIAGDIR_SE, ROADTYPE_ROAD, ROADSTOP_CLASS_DFLT, 0,
						NEW_STATION, true), VehicleID::Invalid()};
			}
			case 9: {
				if (road_bits != 0 || engine_id != EngineID::Invalid() || vehicle_id != VehicleID::Invalid() ||
						target_id != UINT32_MAX || order_index != 0 || policy != 0 ||
						(!IsTileType(tile, MP_CLEAR) && !IsTileType(tile, MP_TREES))) {
					return {CMD_ERROR, VehicleID::Invalid()};
				}
				return {Command<CMD_BUILD_ROAD_DEPOT>::Do(command_flags, tile, ROADTYPE_ROAD,
						static_cast<DiagDirection>(orientation)), VehicleID::Invalid()};
			}
			case 10: {
				const Engine *engine = Engine::GetIfValid(engine_id);
				if (orientation != 0 || road_bits != 0 || vehicle_id != VehicleID::Invalid() ||
						target_id != UINT32_MAX || order_index != 0 || policy != 0 ||
						!IsRoadDepotTile(tile) || !IsTileOwner(tile, _current_company) ||
						engine == nullptr || !IsEngineBuildable(engine_id, VEH_ROAD, _current_company) ||
						engine->GetDefaultCargoType() != 0 ||
						RoadTypeIsTram(engine->VehInfo<RoadVehicleInfo>().roadtype) ||
						IsArticulatedEngine(engine_id)) return {CMD_ERROR, VehicleID::Invalid()};
				auto result = Command<CMD_BUILD_VEHICLE>::Do(
						command_flags, tile, engine_id, false, INVALID_CARGO, INVALID_CLIENT_ID);
				return {std::get<0>(result), std::get<1>(result)};
			}
			case 11: {
				Vehicle *bus = Vehicle::GetIfValid(vehicle_id);
				const Station *station = target_id < UINT16_MAX ?
						Station::GetIfValid(StationID{static_cast<uint16_t>(target_id)}) : nullptr;
				if (orientation != 0 || road_bits != 0 || engine_id != EngineID::Invalid() || policy != 0 ||
						bus == nullptr || bus->type != VEH_ROAD || bus->owner != _current_company ||
						!bus->IsPrimaryVehicle() || !bus->vehstatus.Test(VehState::Stopped) ||
						bus->vehstatus.Test(VehState::Crashed) || bus->IsOrderListShared() ||
						order_index > bus->GetNumOrders() || bus->GetNumOrders() >= COPILOT_MAX_BUS_ORDERS ||
						station == nullptr || station->owner != _current_company ||
						!station->facilities.Test(StationFacility::BusStop)) return {CMD_ERROR, VehicleID::Invalid()};
				return {Command<CMD_INSERT_ORDER>::Do(command_flags, vehicle_id, order_index,
						CopilotBusStationOrder(target_id)), VehicleID::Invalid()};
			}
			case 12: {
				Vehicle *bus = Vehicle::GetIfValid(vehicle_id);
				if (orientation != 0 || road_bits != 0 || engine_id != EngineID::Invalid() ||
						target_id != UINT32_MAX || order_index != 0 || policy > 1 ||
						bus == nullptr || bus->type != VEH_ROAD || bus->owner != _current_company ||
						!bus->IsPrimaryVehicle()) return {CMD_ERROR, VehicleID::Invalid()};
				bool running = !bus->vehstatus.Test(VehState::Stopped);
				if (running == (policy == 1)) return {CommandCost(), VehicleID::Invalid()};
				if (policy == 1 && (bus->GetNumOrders() < 2 || bus->GetNumOrders() > COPILOT_MAX_BUS_ORDERS ||
						bus->vehstatus.Test(VehState::Crashed))) {
					return {CMD_ERROR, VehicleID::Invalid()};
				}
				return {Command<CMD_START_STOP_VEHICLE>::Do(command_flags, vehicle_id, true), VehicleID::Invalid()};
			}
			default: return {CMD_ERROR, VehicleID::Invalid()};
		}
	};
	auto quote = command(DoCommandFlags{flags}.Reset(DoCommandFlag::Execute));
	if (std::get<0>(quote).Failed()) return quote;
	if (std::get<0>(quote).GetCost() > max_cost) return {CMD_ERROR, VehicleID::Invalid()};
	if (!flags.Test(DoCommandFlag::Execute)) return quote;
	return command(flags);
}

void CcCopilotBusAction(Commands cmd, const CommandCost &result, VehicleID created_vehicle,
		TileIndex tile, uint8_t action, uint8_t orientation, uint8_t road_bits,
		EngineID engine_id, VehicleID vehicle_id, uint32_t target_id, uint16_t order_index,
		uint8_t policy, Money max_cost, uint64_t operation_hi, uint64_t operation_lo,
		uint64_t payload_hash_hi, uint64_t payload_hash_lo)
{
	uint32_t wire_vehicle_id = vehicle_id == VehicleID::Invalid() ? UINT32_MAX : vehicle_id.base();
	if (cmd != CMD_COPILOT_BUS_ACTION ||
			_copilot_operation.state != to_underlying(CopilotOperationState::Pending) ||
			_copilot_operation.operation_hi != operation_hi || _copilot_operation.operation_lo != operation_lo ||
			_copilot_operation.payload_hash_hi != payload_hash_hi || _copilot_operation.payload_hash_lo != payload_hash_lo ||
			_copilot_operation.tile != tile.base() || _copilot_operation.kind != action ||
			_copilot_operation.track != (action == 7 ? road_bits : orientation) ||
			_copilot_operation.engine_id != engine_id.base() ||
			_copilot_operation.vehicle_id != wire_vehicle_id || _copilot_operation.target_id != target_id ||
			_copilot_operation.order_index != order_index || _copilot_operation.policy != policy ||
			_copilot_operation.max_cost != max_cost.base()) return;
	_copilot_operation.actual_cost = result.GetCost();
	_copilot_operation.error_message = result.Failed() ? result.GetErrorMessage() : 0;
	_copilot_operation.completed_frame = CopilotCurrentFrame();
	if (result.Failed()) {
		_copilot_operation.state = to_underlying(CopilotOperationState::Rejected);
		return;
	}
	bool verified = result.GetCost() <= max_cost;
	switch (action) {
		case 7:
			verified &= IsNormalRoadTile(tile) &&
					(GetRoadBits(tile, RTT_ROAD) & CopilotWireRoadBits(road_bits)) == CopilotWireRoadBits(road_bits);
			break;
		case 8:
			verified &= IsDriveThroughStopTile(tile) && GetRoadStopType(tile) == RoadStopType::Bus;
			if (verified) _copilot_operation.created_asset_id = GetStationIndex(tile).base();
			break;
		case 9:
			verified &= IsRoadDepotTile(tile) && IsTileOwner(tile, CompanyID{_copilot_operation.company});
			if (verified) _copilot_operation.created_asset_id = GetDepotIndex(tile).base();
			break;
		case 10: {
			const Vehicle *bus = Vehicle::GetIfValid(created_vehicle);
			verified &= bus != nullptr && bus->type == VEH_ROAD && bus->owner.base() == _copilot_operation.company &&
					bus->engine_type == engine_id && bus->tile == tile && bus->IsStoppedInDepot();
			if (verified) _copilot_operation.created_asset_id = created_vehicle.base();
			break;
		}
		case 11: {
			const Vehicle *bus = Vehicle::GetIfValid(vehicle_id);
			verified &= bus != nullptr && order_index < bus->GetNumOrders() &&
					bus->GetNumOrders() <= COPILOT_MAX_BUS_ORDERS;
			if (verified) {
				const Order &order = bus->Orders()[order_index];
				verified = order.IsType(OT_GOTO_STATION) && order.GetDestination().base() == target_id &&
						order.GetLoadType() == OrderLoadType::LoadIfPossible &&
						order.GetUnloadType() == OrderUnloadType::UnloadIfPossible;
			}
			break;
		}
		case 12: {
			const Vehicle *bus = Vehicle::GetIfValid(vehicle_id);
			verified &= bus != nullptr && bus->vehstatus.Test(VehState::Stopped) == (policy == 0);
			break;
		}
	}
	_copilot_operation.state = to_underlying(verified ? CopilotOperationState::Completed : CopilotOperationState::Uncertain);
	if (verified) _copilot_operation.sequence++;
}
