#include "Common.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <numeric>
#include <unordered_set>

namespace vlr {

namespace {

constexpr std::array<const char*, kAttrCount> kAttrNames = {
    "aim",            "headshot",        "entry",          "utility",
    "game_sense",     "clutch",          "decision_making",
    "intelligence",   "aggressiveness",  "positioning",
    "communication",  "adaptability",    "leadership",
    "reaction",       "spike_handle",    "anchor",
    "movement",       "crosshair_placement", "lurking",    "awping",
    "stamina",        "mid_round_calling",   "economy_mgmt", "anti_strat"
};

SimConfig g_config{};
Rng g_rng;

std::vector<std::string> g_names;
std::vector<std::string> g_teams;
std::unordered_set<std::string> g_used_names;
std::size_t g_name_cursor = 0;
std::size_t g_team_cursor = 0;

void ensure_loaded() {
    if (g_names.empty()) {
        g_names = load_lines("xato-net-10-million-usernames.txt", 12000);
        if (g_names.empty()) {
            g_names.reserve(15000);
            for (int i = 0; i < 15000; ++i) {
                g_names.emplace_back("Player" + std::to_string(i));
            }
        }
        g_rng.shuffle(g_names);
    }
    if (g_teams.empty()) {
        g_teams = load_lines("CLan names.txt", 4000);
        if (g_teams.empty()) {
            // Original team names (not real-world esports orgs). Aiming for
            // single-word punchy brands that read like a pro team without
            // veering into "fluffy esports nicknames" territory.
            const std::array<const char*, 36> fallback = {
                "Aurora",      "Arcus",       "Bastion",     "Cinder",
                "Cobalt",      "Eclipse",     "Forge",       "Halcyon",
                "Helios",      "Kestrel",     "Liminal",     "Lumen",
                "Mantis",      "Nimbus",      "Onyx",        "Quasar",
                "Rift",        "Solis",       "Strix",       "Umbra",
                "Verdant",     "Vesper",      "Wraith",      "Zenith",
                "Zephyr",      "Crimson",     "Stormcrest",  "Tempest",
                "Veyra",       "Tundra",      "Surge",       "Apex Forge",
                "Iron Tide",   "North Star",  "Pale Horse",  "Sable Reign"
            };
            for (auto* n : fallback) g_teams.emplace_back(n);
        }
        g_rng.shuffle(g_teams);
    }
}

}  // namespace

const char* attr_name(Attr a) noexcept {
    auto idx = static_cast<std::size_t>(a);
    return idx < kAttrCount ? kAttrNames[idx] : "?";
}

Attr attr_from_str(std::string_view s) noexcept {
    for (std::size_t i = 0; i < kAttrCount; ++i) {
        if (s == kAttrNames[i]) return static_cast<Attr>(i);
    }
    return Attr::Count;
}

Rng::Rng() {
    auto t = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    engine_.seed(static_cast<std::uint64_t>(t));
}
Rng::Rng(std::uint64_t s) { engine_.seed(s); }
void Rng::seed(std::uint64_t s) { engine_.seed(s); }

int Rng::irange(int lo, int hi) {
    if (hi <= lo) return lo;
    std::uniform_int_distribution<int> d(lo, hi);
    return d(engine_);
}
double Rng::drange(double lo, double hi) {
    if (hi <= lo) return lo;
    std::uniform_real_distribution<double> d(lo, hi);
    return d(engine_);
}
double Rng::uniform() {
    std::uniform_real_distribution<double> d(0.0, 1.0);
    return d(engine_);
}
bool Rng::chance(double p) {
    if (p <= 0) return false;
    if (p >= 1) return true;
    return uniform() < p;
}

int Rng::weighted_index(const std::vector<int>& weights) {
    if (weights.empty()) return -1;
    long long total = 0;
    for (int w : weights) total += w > 0 ? w : 0;
    if (total <= 0) return irange(0, static_cast<int>(weights.size()) - 1);
    std::uniform_int_distribution<long long> d(0, total - 1);
    long long r = d(engine_);
    long long acc = 0;
    for (std::size_t i = 0; i < weights.size(); ++i) {
        acc += weights[i] > 0 ? weights[i] : 0;
        if (r < acc) return static_cast<int>(i);
    }
    return static_cast<int>(weights.size() - 1);
}

int Rng::weighted_index(const std::vector<double>& weights) {
    if (weights.empty()) return -1;
    double total = std::accumulate(weights.begin(), weights.end(), 0.0,
                                   [](double s, double w) { return s + (w > 0 ? w : 0); });
    if (total <= 0) return irange(0, static_cast<int>(weights.size()) - 1);
    std::uniform_real_distribution<double> d(0.0, total);
    double r = d(engine_);
    double acc = 0.0;
    for (std::size_t i = 0; i < weights.size(); ++i) {
        acc += weights[i] > 0 ? weights[i] : 0;
        if (r < acc) return static_cast<int>(i);
    }
    return static_cast<int>(weights.size() - 1);
}

Rng& rng() { return g_rng; }
const SimConfig& config() { return g_config; }
SimConfig& mutable_config() { return g_config; }

std::string get_rank_from_mmr(int mmr) {
    if (mmr < 1100) return "Iron 1";
    if (mmr < 1200) return "Iron 2";
    if (mmr < 1300) return "Iron 3";
    if (mmr < 1400) return "Bronze 1";
    if (mmr < 1500) return "Bronze 2";
    if (mmr < 1600) return "Bronze 3";
    if (mmr < 1700) return "Silver 1";
    if (mmr < 1800) return "Silver 2";
    if (mmr < 1900) return "Silver 3";
    if (mmr < 2000) return "Gold 1";
    if (mmr < 2100) return "Gold 2";
    if (mmr < 2200) return "Gold 3";
    if (mmr < 2350) return "Platinum 1";
    if (mmr < 2500) return "Platinum 2";
    if (mmr < 2650) return "Platinum 3";
    if (mmr < 2800) return "Diamond 1";
    if (mmr < 2950) return "Diamond 2";
    if (mmr < 3100) return "Diamond 3";
    if (mmr < 3250) return "Ascendant 1";
    if (mmr < 3400) return "Ascendant 2";
    if (mmr < 3550) return "Ascendant 3";
    if (mmr < 3700) return "Immortal 1";
    if (mmr < 3850) return "Immortal 2";
    if (mmr < 4000) return "Immortal 3";
    return "Radiant";
}

std::vector<std::string> load_lines(const std::string& path, std::size_t cap) {
    std::vector<std::string> out;
    std::ifstream in(path);
    if (!in.is_open()) return out;
    std::string line;
    out.reserve(cap < 4096 ? cap : 4096);
    while (std::getline(in, line) && out.size() < cap) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        if (line.empty()) continue;
        if (line.rfind("[source", 0) == 0) continue;
        auto pos = line.find(". ");
        if (pos != std::string::npos && pos < 6) {
            bool all_digits = true;
            for (std::size_t i = 0; i < pos; ++i) {
                if (line[i] < '0' || line[i] > '9') { all_digits = false; break; }
            }
            if (all_digits) line = line.substr(pos + 2);
        }
        if (line.empty()) continue;
        out.emplace_back(std::move(line));
    }
    std::unordered_set<std::string> seen;
    std::vector<std::string> uniq;
    uniq.reserve(out.size());
    for (auto& s : out) {
        if (seen.insert(s).second) uniq.emplace_back(std::move(s));
    }
    return uniq;
}

const std::vector<std::string>& global_names() { ensure_loaded(); return g_names; }
const std::vector<std::string>& global_teams() { ensure_loaded(); return g_teams; }

std::string take_unused_name() {
    ensure_loaded();
    while (g_name_cursor < g_names.size()) {
        std::string& candidate = g_names[g_name_cursor++];
        if (g_used_names.insert(candidate).second) return candidate;
    }
    std::string fallback = "Solo_" + std::to_string(g_rng.irange(1000, 999999));
    while (!g_used_names.insert(fallback).second) {
        fallback = "Solo_" + std::to_string(g_rng.irange(1000, 999999));
    }
    return fallback;
}

// Procedural org name generation. No "Team_NNNN" placeholders ever. Pulls
// from curated word lists across many esports-org-flavored styles. Originally
// 5 styles; expanded to 12 styles with deeper component pools so the
// combinatorial space per region is now ~thousands of distinct names — making
// duplicates across the 36 default teams vanishingly rare.
//
// Styles (uniform pick across them, then attempt-loop on de-dup):
//   0.  single bold noun           -> "Vortex", "Apex", "Forge", "Karma"
//   1.  modifier + noun (animals)  -> "Crimson Wolves", "Iron Hawks"
//   2.  compound mythic/animal     -> "Stormhounds", "Ironclaws"
//   3.  solo + suffix              -> "Sentinel Esports", "Atlas Gaming"
//   4.  acronym style              -> "AFK", "OG", "G2", "FNX"
//   5.  animal + descriptor        -> "Phoenix Rising", "Solar Wolves"
//   6.  region-flavored noun       -> "Pacific Heat", "Atlantic Storm"
//   7.  Greek/Latin/mythic         -> "Aetheria", "Velox", "Erebus", "Nyx"
//   8.  military / tactical        -> "Vanguard Six", "Battalion", "Strike Team"
//   9.  color + animal/noun        -> "Black Hornet", "Onyx Order"
//   10. two-word esport stylings   -> "Final Light", "Last Hope", "True North"
//   11. number-noun                -> "Nine Sigma", "Seven Order"
//
// Combined with a global de-dup set (`is_taken`) so no name repeats. The
// underlying pools are large enough (~600-1000 combinatorial candidates per
// style; tens of thousands overall) that the 36 teams generated per default
// world rarely collide.
namespace {
const char* const kTeamModifiers[] = {
    "Crimson", "Iron", "Storm", "Shadow", "Royal", "Phoenix", "Northern",
    "Black", "Silver", "Golden", "Apex", "Wild", "Frozen", "Sacred",
    "Ancient", "Hidden", "Final", "Last", "Lone", "Twin", "Nine", "Mad",
    "Risen", "Fallen", "Untamed", "Sovereign", "Ember", "Vector", "Solar",
    "Lunar", "Cobalt", "Onyx", "Velvet", "Steel", "Pale", "Burning",
    "Echo", "Bright", "Quiet", "Liminal", "Distant", "Hollow", "Forsaken",
    "Verdant", "Glass", "Stone", "Brass", "Copper", "Crystal", "Gilded",
    "Tidal", "Boreal", "Astral", "Nordic", "Eastern", "Western", "Southern",
    "Sapphire", "Emerald", "Ruby", "Pearl", "Obsidian", "Amber", "Ivory",
    "Azure", "Scarlet", "Jade", "Bronze", "Platinum", "Titan", "Vortex",
    "Eternal", "Endless", "Silent", "Roaring", "Restless", "Wandering",
    "Hidden", "Open", "True", "Cold", "Hot", "Eldritch", "Stellar"
};
const char* const kTeamNouns[] = {
    "Wolves", "Hawks", "Tigers", "Lions", "Dragons", "Vipers", "Ravens",
    "Eagles", "Bears", "Falcons", "Foxes", "Sharks", "Jaguars", "Panthers",
    "Owls", "Cobras", "Griffins", "Wraiths", "Sentinels", "Knights",
    "Vanguards", "Champions", "Kings", "Royals", "Saints", "Pirates",
    "Reapers", "Ghosts", "Spectres", "Phantoms", "Riders", "Outlaws",
    "Renegades", "Heralds", "Marshals", "Rebels", "Crusaders", "Legends",
    "Titans", "Giants", "Comets", "Stars", "Suns", "Moons", "Tides",
    "Waves", "Storms", "Winds", "Echoes", "Embers", "Flames", "Blades",
    "Spears", "Arrows", "Daggers", "Crowns", "Anvils", "Hammers", "Bolts",
    "Pythons", "Lynx", "Bulls", "Stags", "Drakes", "Serpents", "Hounds",
    "Mammoths", "Hornets", "Mantises", "Scarabs", "Beasts", "Hunters",
    "Watchers", "Keepers", "Wanderers", "Pilgrims", "Nomads", "Marauders"
};
const char* const kSoloNames[] = {
    "Vortex", "Eclipse", "Halo", "Tempest", "Zenith", "Apex", "Bastion",
    "Cinder", "Cobalt", "Forge", "Halcyon", "Helios", "Kestrel", "Lumen",
    "Mantis", "Nimbus", "Onyx", "Quasar", "Rift", "Solis", "Strix", "Umbra",
    "Vesper", "Wraith", "Aurora", "Arcus", "Solace", "Sable", "Veyra",
    "Tundra", "Surge", "Beacon", "Tactic", "Method", "Lattice", "Static",
    "Pulse", "Origin", "Vertex", "Pyre", "Ascend", "Reverie", "Helix",
    "Mirage", "Karma", "Reign", "Empire", "Nemesis", "Nova", "Echo",
    "Saga", "Specter", "Riptide", "Avalanche", "Tundra", "Cipher", "Caliber",
    "Ember", "Bolt", "Arsenal", "Catalyst", "Inferno", "Citadel", "Lumen",
    "Quartz", "Sigma", "Omega", "Delta", "Theta", "Kairos", "Talos",
    "Lyric", "Cascade", "Aegis", "Sentry", "Apogee", "Ravine", "Drift"
};
const char* const kCompoundLeft[] = {
    "Storm", "Iron", "Steel", "Night", "Ghost", "Dragon", "Phantom",
    "Flame", "Frost", "Shadow", "Blood", "Star", "Moon", "Sun", "Sky",
    "Stone", "Wolf", "Hawk", "Snow", "Salt", "Pine", "Oak", "Ash",
    "Bone", "Iron", "Thunder", "Lightning", "Cloud", "Spirit", "Wind",
    "Rain", "Hail", "Glass", "Mirror", "Black", "White", "Red", "Gold"
};
const char* const kCompoundRight[] = {
    "claw", "fang", "shade", "wing", "fall", "fire", "born", "watch",
    "guard", "blade", "spire", "haven", "song", "host", "grip", "keeper",
    "rider", "bound", "kin", "hold", "veil", "binder", "mark",
    "strike", "burn", "step", "stride", "crown", "blade", "shard",
    "forge", "tide", "weaver", "breaker", "lord", "warden", "drift",
    "hunter", "stalker", "seer", "smith"
};
const char* const kSuffixes[] = {
    "Esports", "Gaming", "GG", "Squad", "Collective", "Faction", "Crew",
    "Group", "Project", "Studio", "Athletic", "Club", "Society", "Union",
    "League", "Order", "Syndicate"
};

// Animal pool — used for descriptor styles ("Phoenix Rising") and color+animal
// combos ("Black Hornet"). Smaller / more iconic than the full kTeamNouns
// list so the resulting names land cleaner.
const char* const kAnimals[] = {
    "Phoenix", "Wolf", "Hawk", "Eagle", "Bear", "Tiger", "Lion", "Falcon",
    "Viper", "Cobra", "Dragon", "Stag", "Drake", "Hornet", "Jaguar",
    "Panther", "Mantis", "Raven", "Owl", "Shark", "Lynx", "Bull"
};
// Plural form used in "Solar Wolves" style — separate so we don't
// over-pluralize collectives ("Phoenixes Rising" is wrong; "Solar Wolves" ok).
const char* const kAnimalsPlural[] = {
    "Wolves", "Hawks", "Eagles", "Bears", "Tigers", "Lions", "Falcons",
    "Vipers", "Cobras", "Dragons", "Stags", "Drakes", "Hornets", "Jaguars",
    "Panthers", "Ravens", "Owls", "Sharks", "Lynx", "Bulls", "Foxes",
    "Pythons", "Reapers"
};
const char* const kAnimalDescriptors[] = {
    "Rising", "Ascending", "Striking", "Hunting", "Soaring", "Burning",
    "Howling", "Roaring", "Watching", "Stalking", "Wandering", "Awakened",
    "Eternal", "Unbound", "Untamed", "Reborn"
};
// Color-class prefix for color+noun style. Skews darker / esport-clean.
const char* const kColors[] = {
    "Black", "Crimson", "Onyx", "Iron", "Golden", "Silver", "Cobalt",
    "Obsidian", "Scarlet", "Ivory", "Amber", "Emerald", "Sapphire",
    "Ruby", "Pearl", "Bronze", "Platinum", "Azure", "Jade", "Copper",
    "Shadow", "Pale", "Burnt"
};
// Standalone "noun of consequence" — used after a color, and in the
// "<Color> <Noun>" combos ("Onyx Order", "Iron Veil", "Golden Vanguard").
// Skews mythic + tactical so it sounds like an esport org rather than a
// fantasy guild.
const char* const kColorNouns[] = {
    "Order", "Veil", "Vanguard", "Reign", "Court", "Empire", "Hand",
    "Crown", "Banner", "Throne", "Edict", "Cipher", "Doctrine", "Mandate",
    "Heir", "Pact", "Accord", "Chapter", "Bloc", "Faction", "Phantom",
    "Tide", "Front", "Echo", "Mirror", "Veil"
};
// Geographic / region-flavored adjective + noun. Pairs are interesting
// even at random — "Pacific Heat", "Atlantic Storm", "Coastal Surge".
const char* const kRegionAdjectives[] = {
    "Pacific", "Atlantic", "Northern", "Southern", "Eastern", "Western",
    "Coastal", "Desert", "Alpine", "Boreal", "Equatorial", "Polar",
    "Tropic", "Highland", "Lowland", "Tundra", "Arctic", "Mediterranean",
    "Continental", "Riverside", "Mountain", "Valley", "Frontier"
};
const char* const kRegionNouns[] = {
    "Heat", "Storm", "Lights", "Mirage", "Surge", "Frost", "Drift",
    "Current", "Wave", "Tide", "Reach", "Front", "Crest", "Horizon",
    "Bloom", "Flare", "Wind", "Span", "Edge", "Skyline"
};
// Greek / Latin / mythic standalone names. Each is intentionally complete
// — they read like proper-noun orgs ("Aetheria", "Velox", "Erebus", "Nyx").
const char* const kMythicNames[] = {
    "Aetheria", "Velox", "Erebus", "Nyx", "Sentinel", "Lethe", "Hyperion",
    "Selene", "Eos", "Helios", "Lumen", "Atlas", "Prometheus", "Tartarus",
    "Pyrrha", "Astraea", "Boreas", "Zephyrus", "Notos", "Eurus", "Hekate",
    "Daedalus", "Orpheus", "Cerberus", "Hydra", "Chimera", "Achilles",
    "Andromeda", "Cassiopeia", "Perseus", "Theseus", "Ariadne", "Icarus",
    "Phaedra", "Calypso", "Ulysses", "Prima", "Tertia", "Ultima",
    "Caelum", "Nocturna", "Diurna", "Aetas", "Forma", "Magna", "Nova",
    "Aria", "Aevum", "Solis", "Lux", "Umbra", "Veritas", "Aurora",
    "Stellaris", "Lunaris", "Solaris", "Caligo", "Vespera"
};
// Military / tactical pool. Two sub-pools so we can produce "Vanguard Six",
// "Recon Unit", "Strike Team", "Tactical Reserve" combos and also single-
// word units like "Battalion".
const char* const kMilitaryHeads[] = {
    "Vanguard", "Recon", "Strike", "Tactical", "Combat", "Special", "Echo",
    "Foxtrot", "Sigma", "Bravo", "Delta", "Alpha", "Charlie", "Recon",
    "Shock", "Phantom", "Iron", "Steel", "Storm", "Sniper", "Ranger",
    "Marine", "Outrider", "Spearhead", "Forward", "Black", "Crimson"
};
const char* const kMilitaryTails[] = {
    "Six", "Nine", "Twelve", "One", "Zero", "Unit", "Team", "Reserve",
    "Force", "Squad", "Brigade", "Regiment", "Detachment", "Cadre",
    "Command", "Company", "Section", "Platoon", "Element", "Group"
};
// Standalone military words (used alone, no compounding) — read as
// stoic single-word org names.
const char* const kMilitarySolo[] = {
    "Battalion", "Brigade", "Regiment", "Sentinel", "Vanguard", "Bulwark",
    "Garrison", "Reserve", "Patrol", "Outpost", "Bastion", "Vanguard",
    "Cohort"
};
// Two-word "esport styling" pool. Designed so the cross product reads
// idiomatic ("Final Light", "Last Hope", "True North", "Cold Mirror",
// "Burning Skies").
const char* const kTwoWordA[] = {
    "Final", "Last", "True", "Cold", "Burning", "Silent", "Hidden", "Open",
    "Empty", "Endless", "First", "Lone", "Wild", "Distant", "Quiet", "Lost",
    "Bright", "Dark", "Solar", "Lunar", "Cold", "Pale", "Sacred",
    "Forgotten", "Reborn", "Risen"
};
const char* const kTwoWordB[] = {
    "Light", "Hope", "North", "Mirror", "Skies", "Hours", "Echo", "Voice",
    "Truth", "Storm", "Reign", "Spark", "Dawn", "Hour", "Path", "Pulse",
    "Rite", "Edge", "Mark", "Hand", "Order", "Crown", "Verse", "Ash",
    "Sigil", "Watchers"
};
// Number-noun esport style ("Nine Sigma", "Seven Order"). Numbers as
// English words so they read like an org tag rather than a serial.
const char* const kNumberWords[] = {
    "One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight",
    "Nine", "Ten", "Eleven", "Twelve", "Hundred", "Thousand", "Zero"
};

// Module-static so `reset_name_caches()` can wipe it. Tracks every team
// name we've handed out (whether from `g_teams` or generated) so a second
// call to `take_team_name` never collides.
std::unordered_set<std::string> g_taken_team_names;

bool is_taken(const std::string& s) {
    if (g_taken_team_names.count(s)) return true;
    g_taken_team_names.insert(s);
    return false;
}

template <std::size_t N>
const char* pick(const char* const (&arr)[N]) {
    return arr[g_rng.irange(0, static_cast<int>(N) - 1)];
}

std::string generate_org_name() {
    constexpr int kMax = 64;  // try this many times before falling back
    // 12 styles total, weighted uniformly (each gets weight 10). Acronyms
    // and number-prefixed forms get a slight nerf so the pool feels more
    // like an esports league than a phonebook of TLAs.
    static const std::vector<int> kStyleWeights = {
        10, 12, 10, 8,  6,   // 0-4 original
        12, 10, 10, 10, 12, 12, 6   // 5-11 new
    };
    for (int attempt = 0; attempt < kMax; ++attempt) {
        int style = g_rng.weighted_index(kStyleWeights);
        std::string out;
        switch (style) {
            case 0: {  // single bold noun
                out = pick(kSoloNames);
                break;
            }
            case 1: {  // modifier + noun (animals)
                out = std::string(pick(kTeamModifiers)) + " " + pick(kTeamNouns);
                break;
            }
            case 2: {  // compound
                out = std::string(pick(kCompoundLeft)) + pick(kCompoundRight);
                break;
            }
            case 3: {  // solo + suffix
                out = std::string(pick(kSoloNames)) + " " + pick(kSuffixes);
                break;
            }
            case 4: {  // acronym style
                int len = g_rng.irange(2, 4);
                for (int i = 0; i < len; ++i) {
                    out += static_cast<char>('A' + g_rng.irange(0, 25));
                }
                if (g_rng.chance(0.3)) out += std::to_string(g_rng.irange(1, 9));
                break;
            }
            case 5: {  // animal + descriptor / descriptor + animal-plural
                if (g_rng.chance(0.55)) {
                    // "Phoenix Rising", "Wolf Hunting"
                    out = std::string(pick(kAnimals)) + " " + pick(kAnimalDescriptors);
                } else {
                    // "Solar Wolves", "Crimson Hawks", "Iron Bears"
                    out = std::string(pick(kTeamModifiers)) + " " + pick(kAnimalsPlural);
                }
                break;
            }
            case 6: {  // region-flavored noun
                out = std::string(pick(kRegionAdjectives)) + " " + pick(kRegionNouns);
                break;
            }
            case 7: {  // Greek / Latin / mythic
                out = pick(kMythicNames);
                break;
            }
            case 8: {  // military / tactical
                double r = g_rng.uniform();
                if (r < 0.45) {
                    out = std::string(pick(kMilitaryHeads)) + " " + pick(kMilitaryTails);
                } else if (r < 0.75) {
                    out = pick(kMilitarySolo);
                } else {
                    // "Sigma-Six" hyphenated callsign feel
                    out = std::string(pick(kMilitaryHeads)) + "-" + pick(kMilitaryTails);
                }
                break;
            }
            case 9: {  // color + animal/noun
                if (g_rng.chance(0.5)) {
                    out = std::string(pick(kColors)) + " " + pick(kAnimals);
                } else {
                    out = std::string(pick(kColors)) + " " + pick(kColorNouns);
                }
                break;
            }
            case 10: {  // two-word esport stylings
                out = std::string(pick(kTwoWordA)) + " " + pick(kTwoWordB);
                break;
            }
            case 11: {  // number-noun
                out = std::string(pick(kNumberWords)) + " " + pick(kColorNouns);
                break;
            }
            default: {
                out = pick(kSoloNames);
                break;
            }
        }
        if (!is_taken(out)) return out;
    }
    // Last-resort: append a digit. Still no "Team_" prefix ever.
    std::string base = pick(kSoloNames);
    for (int n = 1; n < 999; ++n) {
        std::string candidate = base + std::to_string(n);
        if (!is_taken(candidate)) return candidate;
    }
    return base + "X";  // final-final fallback
}

// 3-letter ALL-CAPS abbreviation. Pulls initials from spaces/casings, or
// falls back to first 3 letters of the LAST word for two-word names that
// wouldn't produce a sane 3-letter tag (e.g. "Final Light" -> "LIG" not
// "FL"). Single-word names take the first 3 alphabetic chars.
std::string derive_team_tag(const std::string& name) {
    // Collect word boundaries (skip spaces / hyphens).
    std::vector<std::string> words;
    std::string cur;
    for (char c : name) {
        if (c == ' ' || c == '-') {
            if (!cur.empty()) { words.push_back(cur); cur.clear(); }
        } else if (std::isalpha((unsigned char)c) || std::isdigit((unsigned char)c)) {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) words.push_back(cur);

    // Path 1: 3+ words -> stack initials.
    if (words.size() >= 3) {
        std::string tag;
        for (auto& w : words) {
            if (tag.size() >= 3) break;
            if (!w.empty()) tag += (char)std::toupper((unsigned char)w[0]);
        }
        if (tag.size() >= 3) return tag.substr(0, 3);
    }

    // Path 2: 2 words -> prefer first letter + first 2 of second word for
    // a more readable tag ("Crimson Wolves" -> "CWO", "Final Light" -> "FLI").
    // If that produces a weak/duplicate-prone shape fall back to first 3 of
    // last word per spec.
    if (words.size() == 2) {
        std::string tag;
        if (!words[0].empty()) tag += (char)std::toupper((unsigned char)words[0][0]);
        for (char c : words[1]) {
            if (tag.size() >= 3) break;
            if (std::isalpha((unsigned char)c)) tag += (char)std::toupper((unsigned char)c);
        }
        if (tag.size() >= 3) return tag.substr(0, 3);
        // Fallback: first 3 letters of LAST word.
        std::string lw;
        for (char c : words[1]) {
            if (lw.size() >= 3) break;
            if (std::isalpha((unsigned char)c)) lw += (char)std::toupper((unsigned char)c);
        }
        if (lw.size() >= 3) return lw;
    }

    // Path 3: 1 word -> first 3 alphabetic chars. Works for "Vortex"->"VOR",
    // "TSM"->"TSM", "Aetheria"->"AET".
    if (!words.empty()) {
        std::string tag;
        for (char c : words[0]) {
            if (tag.size() >= 3) break;
            if (std::isalpha((unsigned char)c)) tag += (char)std::toupper((unsigned char)c);
        }
        if (tag.size() >= 3) return tag.substr(0, 3);
        // Acronym-only names like "G2" produce "G" + digit -> pad to 3.
        for (char c : words[0]) {
            if (tag.size() >= 3) break;
            if (std::isdigit((unsigned char)c)) tag += c;
        }
        while (tag.size() < 3) tag += (char)('A' + g_rng.irange(0, 25));
        return tag;
    }

    // Empty name (defensive) — random 3 letters.
    std::string tag;
    while (tag.size() < 3) tag += (char)('A' + g_rng.irange(0, 25));
    return tag;
}

}  // namespace

std::string take_team_name() {
    ensure_loaded();
    // Use the curated team list if anything's left and not already taken.
    while (g_team_cursor < g_teams.size()) {
        std::string candidate = g_teams[g_team_cursor++];
        if (!is_taken(candidate)) return candidate;
    }
    // Otherwise generate a procedural org name. Never returns "Team_NNNN".
    return generate_org_name();
}

std::string make_team_tag(const std::string& name) {
    return derive_team_tag(name);
}

void reset_name_caches() {
    // Wipe cursors + de-dup sets so a fresh world can be booted in-process.
    // Underlying pool vectors (g_names, g_teams) stay loaded; only the
    // "handed out" bookkeeping resets.
    g_name_cursor = 0;
    g_team_cursor = 0;
    g_used_names.clear();
    g_taken_team_names.clear();
    // Reshuffle so the new world doesn't hand out the exact same names in
    // the exact same order — feels slightly fresher on consecutive boots.
    if (!g_names.empty()) g_rng.shuffle(g_names);
    if (!g_teams.empty()) g_rng.shuffle(g_teams);
}

}  // namespace vlr
