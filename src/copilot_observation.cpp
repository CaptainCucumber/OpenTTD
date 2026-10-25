/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License version 2.
 */

/** @file copilot_observation.cpp Bounded runtime observations for OpenTTD Copilot. */

#include "stdafx.h"
#include <random>
#include "copilot_observation.h"

#include "openttd.h"
#include "network/network_internal.h"
#include "vehicle_base.h"
#include "copilot_cmd.h"

#include "safeguards.h"

static std::deque<CopilotObservation> _copilot_observations;
static uint64_t SeedCopilotObservationEpoch()
{
	std::random_device entropy;
	std::uniform_int_distribution<uint64_t> distribution(1, std::numeric_limits<uint64_t>::max() - 1);
	return distribution(entropy);
}

static uint64_t _copilot_observation_epoch = SeedCopilotObservationEpoch();
static uint64_t _copilot_next_observation_sequence = 0;

static void PushCopilotObservation(CopilotObservation observation)
{
	observation.sequence = _copilot_next_observation_sequence++;
	if (_copilot_observations.size() == COPILOT_OBSERVATION_CAPACITY) _copilot_observations.pop_front();
	_copilot_observations.push_back(observation);
}

void ResetCopilotObservations()
{
	_copilot_observations.clear();
	_copilot_next_observation_sequence = 0;
	/* Epoch zero is reserved for malformed/uninitialised responses. */
	if (++_copilot_observation_epoch == 0) ++_copilot_observation_epoch;
}

void RecordCopilotTrainDeparture(const Vehicle *front)
{
	if (front == nullptr || (front->type != VEH_TRAIN && front->type != VEH_ROAD) ||
			front->index == VehicleID::Invalid() ||
			front->owner == OWNER_NONE || front->last_station_visited == StationID::Invalid()) return;

	std::array<uint32_t, NUM_CARGO> amount{};
	std::array<uint32_t, NUM_CARGO> capacity{};
	for (const Vehicle *part = front; part != nullptr; part = part->Next()) {
		if (!IsValidCargoType(part->cargo_type) || part->cargo_cap == 0) continue;
		amount[part->cargo_type] += part->cargo.StoredCount();
		capacity[part->cargo_type] += part->cargo_cap;
	}

	for (CargoType cargo = 0; cargo < NUM_CARGO; cargo++) {
		if (capacity[cargo] == 0 || amount[cargo] == 0) continue;
		PushCopilotObservation({
			.kind = CopilotObservationKind::LoadedDeparture,
			.frame = CopilotCurrentFrame(),
			.vehicle = front->index,
			.company = front->owner,
			.station = front->last_station_visited,
			.cargo = cargo,
			.amount = amount[cargo],
			.capacity = capacity[cargo],
			.industry = IndustryID::Invalid(),
		});
	}
}

void RecordCopilotIndustryDelivery(VehicleID vehicle, StationID station, CargoType cargo, uint32_t amount, IndustryID industry)
{
	const Vehicle *front = Vehicle::GetIfValid(vehicle);
	if (front == nullptr || amount == 0 || !IsValidCargoType(cargo) || station == StationID::Invalid() ||
			(front->type != VEH_TRAIN && front->type != VEH_ROAD)) return;

	uint32_t capacity = 0;
	for (const Vehicle *part = front; part != nullptr; part = part->Next()) {
		if (part->cargo_type == cargo) capacity += part->cargo_cap;
	}
	PushCopilotObservation({
		.kind = CopilotObservationKind::AcceptedIndustryDelivery,
		.frame = CopilotCurrentFrame(),
		.vehicle = front->index,
		.company = front->owner,
		.station = station,
		.cargo = cargo,
		.amount = amount,
		.capacity = capacity,
		.industry = industry,
	});
}

CopilotObservationPage ReadCopilotObservations(CompanyID company, VehicleID vehicle, uint64_t cursor)
{
	CopilotObservationPage page;
	page.runtime_epoch = _copilot_observation_epoch;
	page.oldest_sequence = _copilot_observations.empty() ? _copilot_next_observation_sequence : _copilot_observations.front().sequence;
	page.gap = cursor < page.oldest_sequence;
	uint64_t scan = std::max(cursor, page.oldest_sequence);

	for (const CopilotObservation &event : _copilot_observations) {
		if (event.sequence < scan) continue;
		scan = event.sequence + 1;
		if (event.company != company || event.vehicle != vehicle) continue;
		page.events.push_back(event);
		if (page.events.size() == COPILOT_OBSERVATION_PAGE_SIZE) break;
	}
	page.next_sequence = std::min(scan, _copilot_next_observation_sequence);
	if (page.events.size() < COPILOT_OBSERVATION_PAGE_SIZE) page.next_sequence = _copilot_next_observation_sequence;
	return page;
}
