/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License version 2.
 */

/** @file copilot_observation.h Bounded runtime observations for OpenTTD Copilot. */

#ifndef COPILOT_OBSERVATION_H
#define COPILOT_OBSERVATION_H

#include "cargo_type.h"
#include "company_type.h"
#include "industry_type.h"
#include "station_type.h"
#include "vehicle_type.h"

static constexpr size_t COPILOT_OBSERVATION_CAPACITY = 256;
static constexpr uint8_t COPILOT_OBSERVATION_PAGE_SIZE = 16;

enum class CopilotObservationKind : uint8_t {
	LoadedDeparture = 1,
	AcceptedIndustryDelivery = 2,
};

struct CopilotObservation {
	uint64_t sequence = 0;
	CopilotObservationKind kind = CopilotObservationKind::LoadedDeparture;
	uint32_t frame = 0;
	VehicleID vehicle = VehicleID::Invalid();
	CompanyID company = CompanyID::Invalid();
	StationID station = StationID::Invalid();
	CargoType cargo = INVALID_CARGO;
	uint32_t amount = 0;
	uint32_t capacity = 0;
	IndustryID industry = IndustryID::Invalid();
};

struct CopilotObservationPage {
	uint64_t runtime_epoch = 0;
	uint64_t oldest_sequence = 0;
	uint64_t next_sequence = 0;
	bool gap = false;
	std::vector<CopilotObservation> events;
};

void ResetCopilotObservations();
void RecordCopilotTrainDeparture(const Vehicle *front);
void RecordCopilotIndustryDelivery(VehicleID vehicle, StationID station, CargoType cargo, uint32_t amount, IndustryID industry);
CopilotObservationPage ReadCopilotObservations(CompanyID company, VehicleID vehicle, uint64_t cursor);

#endif /* COPILOT_OBSERVATION_H */
