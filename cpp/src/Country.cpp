#include "Country.h"

#include <algorithm>

namespace vlr {

namespace {

constexpr std::array<const char*, 3> kRegionNames = {"Americas", "EMEA", "Pacific"};

std::vector<Country> g_countries;

void init_countries() {
    if (!g_countries.empty()) return;
    // Region weights are tuned so the "main countries" specified by the
    // designer dominate spawning. Side-countries appear but rarely.
    g_countries = {
        // Americas — North America dominates per design (USA + Canada are
        // the bulk of the regional pro scene). Brazil + Latin America
        // contribute meaningful but secondary depth.
        {"USA",         "us", Region::Americas, 80},
        {"Canada",      "ca", Region::Americas, 35},
        {"Brazil",      "br", Region::Americas, 22},
        {"Mexico",      "mx", Region::Americas, 10},
        {"Chile",       "cl", Region::Americas,  6},
        {"Argentina",   "ar", Region::Americas,  6},

        // EMEA — main contributors first.
        {"Germany",     "de", Region::EMEA,     22},
        {"Sweden",      "se", Region::EMEA,     20},
        {"UK",          "gb", Region::EMEA,     20},
        {"Ukraine",     "ua", Region::EMEA,     18},
        {"France",      "fr", Region::EMEA,     20},
        {"Turkey",      "tr", Region::EMEA,     22},
        {"Finland",     "fi", Region::EMEA,     14},
        {"Scotland",    "sct",Region::EMEA,     10},
        {"Ireland",     "ie", Region::EMEA,     10},
        // Side EMEA countries (smaller scenes).
        {"Spain",       "es", Region::EMEA,      8},
        {"Russia",      "ru", Region::EMEA,      8},
        {"Poland",      "pl", Region::EMEA,      6},
        {"Netherlands", "nl", Region::EMEA,      5},
        {"Italy",       "it", Region::EMEA,      5},
        {"Denmark",     "dk", Region::EMEA,      4},
        {"Norway",      "no", Region::EMEA,      4},

        // Pacific — main contributors first.
        {"Australia",   "au", Region::Pacific,  20},
        {"South Korea", "kr", Region::Pacific,  35},
        {"Japan",       "jp", Region::Pacific,  20},
        {"Singapore",   "sg", Region::Pacific,  12},
        {"Thailand",    "th", Region::Pacific,  14},
        {"Vietnam",     "vn", Region::Pacific,  12},
        {"Philippines", "ph", Region::Pacific,  18},
        // Side Pacific.
        {"China",       "cn", Region::Pacific,   8},
        {"Indonesia",   "id", Region::Pacific,   8},
    };
}

}  // namespace

const char* region_name(Region r) noexcept {
    auto i = static_cast<std::size_t>(r);
    return i < kRegionNames.size() ? kRegionNames[i] : "?";
}

Region region_from_str(std::string_view s) noexcept {
    for (std::size_t i = 0; i < kRegionNames.size(); ++i) {
        if (s == kRegionNames[i]) return static_cast<Region>(i);
    }
    return Region::Americas;
}

const std::vector<Country>& countries() {
    init_countries();
    return g_countries;
}

const Country* find_country(std::string_view name) {
    for (auto& c : countries()) if (c.name == name) return &c;
    return nullptr;
}

const Country* find_country_iso(std::string_view iso) {
    for (auto& c : countries()) if (c.iso == iso) return &c;
    return nullptr;
}

const Country& pick_country_in_region(Region r) {
    auto& cs = countries();
    std::vector<int> w;
    std::vector<int> idx;
    for (std::size_t i = 0; i < cs.size(); ++i) {
        if (cs[i].region == r) {
            w.push_back(cs[i].weight);
            idx.push_back(static_cast<int>(i));
        }
    }
    if (w.empty()) return cs.front();
    int pick = rng().weighted_index(w);
    if (pick < 0) pick = 0;
    return cs[idx[pick]];
}

const Country& pick_country_any() {
    auto& cs = countries();
    std::vector<int> w;
    for (auto& c : cs) w.push_back(c.weight);
    int pick = rng().weighted_index(w);
    if (pick < 0) pick = 0;
    return cs[pick];
}

}  // namespace vlr
