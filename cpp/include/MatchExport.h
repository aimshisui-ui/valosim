#pragma once

// MatchExport - JSON serializer for a finished series.
//
// Schema version 1.
// Reserved for v2+: round_history (per-round events), economy_log,
// utility_impact, heatmap_kills, ability_usage, timeline_events.
// Consumers should treat unknown fields as forward-compat extensions and
// ignore them rather than failing.
//
// This module is OUTPUT-ONLY. It does not mutate any engine state and
// has no dependencies beyond the C++17 standard library.

#include "Player.h"   // for RecordedMatchPtr

#include <string>
#include <vector>

namespace vlr {

// Pretty-printed JSON dump of a complete series. Returns "" if recordings
// is empty. Caller is responsible for whether the series is "finished" -
// this just serializes whatever recordings are passed in.
//
// Parameters:
//   recordings   - every map of the series, in play order. Typically
//                  Series::all_recordings(). Each entry's history_record
//                  + match_stats + round_history are the source of truth.
//   event_name   - tournament/event label (e.g. "Americas Regionals 2026"
//                  or "Champions Tour 2026: Masters Madrid"). Used as
//                  "event" field in JSON.
//   best_of      - 1, 2, 3, or 5. Used to derive series_type.
//   year         - world year at time of export.
//   day_in_year  - 0..199 day index for timestamp.
std::string export_series_to_json(
    const std::vector<RecordedMatchPtr>& recordings,
    const std::string& event_name,
    int best_of,
    int year,
    int day_in_year);

}  // namespace vlr
