#pragma once

#include "Common.h"

#include <string>
#include <vector>

namespace vlr {

// Each country lives in exactly one esports region, used for league
// affiliation. Mirrors the way pro Valorant scopes regional play.
enum class Region : std::uint8_t { Americas = 0, EMEA = 1, Pacific = 2 };

const char* region_name(Region r) noexcept;
Region      region_from_str(std::string_view s) noexcept;

struct Country {
    std::string name;     // English country name
    std::string iso;      // 2-letter ISO 3166-1 code, lowercase
    Region      region;
    int         weight;   // relative spawn weight within its region
};

const std::vector<Country>& countries();
const Country* find_country(std::string_view name);
const Country* find_country_iso(std::string_view iso);

// Sub-regional NEIGHBOR clusters (grounded geographic adjacency, not fabricated).
// The scouting knowledge model uses these as the tightest coverage ring: a scout's
// home country + its neighbors are known best, then the wider region, then abroad.
const std::vector<std::string>& country_neighbors(std::string_view iso);
bool are_neighbors(std::string_view a, std::string_view b);

// Pick a random country biased to the given region (or any region for
// imports). Weight-aware so e.g. South Korea spawns more often than Vietnam
// in Pacific.
const Country& pick_country_in_region(Region r);
const Country& pick_country_any();

}  // namespace vlr
