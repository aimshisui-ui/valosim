#pragma once

#include "Country.h"

#include <string>
#include <vector>

namespace vlr {

// Generated player identity. `first` and `last` give the legal name
// associated with the player's birth country; `handle` is the in-game
// gamertag they go by professionally and is the primary display name in
// most UI surfaces.
struct PlayerIdentity {
    std::string first;
    std::string last;
    std::string handle;
    const Country* country = nullptr;
};

// Build a full identity for a player born in `country`.
PlayerIdentity make_identity(const Country& country);

// Just the gamertag, in case you want to refresh it. The generator mixes
// short root words with optional leet substitutions and numeric suffixes
// to produce names that read as plausible esports handles. Handles are
// world-unique (a used-registry dedupes; numbered fallback when exhausted).
std::string generate_handle();

// Clear the unique-gamertag registry for a fresh world (reset_name_caches calls it).
void reset_handle_cache();

// Save/load: snapshot + restore the unique-gamertag registry so a loaded
// world never re-issues a restored player's handle to a fresh rookie.
std::vector<std::string> snapshot_used_handles();
void restore_used_handles(std::vector<std::string> v);

// Pick a hometown city within the given country's ISO. Returns the country
// name as a fallback if no city pool is registered for that ISO.
std::string pick_city(std::string_view iso);

}  // namespace vlr
