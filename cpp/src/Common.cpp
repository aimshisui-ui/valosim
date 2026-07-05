#include "Common.h"
#include "Names.h"   // reset_handle_cache (unique-gamertag registry)

#include <algorithm>
#include <atomic>
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
            // Curated org names — distinct from real-world esports brands but
            // tuned to read like modern competitive orgs. Mix of single-word
            // punchy brands (~70%) and short two-word combos (~30%). Big
            // enough that two seasons of expansion teams + roster shuffles
            // never run dry before falling through to the procedural pool.
            // Compiler-deduced size — explicit `std::array<.., 120>` would
            // leave any unfilled trailing slots as nullptr, and
            // `emplace_back(nullptr)` into a string vector is UB. With a
            // C-style array of deduced extent, slot count == initializer
            // count, so this stays safe even as entries are added/removed.
            const char* const fallback[] = {
                // Single-word brand-modern (read like "Sentinels", "Cloud9").
                "Aurora",      "Arcus",       "Bastion",     "Cinder",
                "Cobalt",      "Eclipse",     "Forge",       "Halcyon",
                "Helios",      "Kestrel",     "Liminal",     "Lumen",
                "Mantis",      "Nimbus",      "Onyx",        "Quasar",
                "Rift",        "Solis",       "Strix",       "Umbra",
                "Verdant",     "Vesper",      "Wraith",      "Zenith",
                "Zephyr",      "Crimson",     "Tempest",     "Veyra",
                "Tundra",      "Surge",       "Apogee",      "Pyre",
                "Vortex",      "Catalyst",    "Citadel",     "Inferno",
                "Specter",     "Talos",       "Aegis",       "Sentry",
                "Cipher",      "Mirage",      "Saga",        "Karma",
                "Reverie",     "Helix",       "Cascade",     "Drift",
                "Riptide",     "Avalanche",   "Beacon",      "Origin",
                // Mythic / classical — short, brand-able.
                "Atlas",       "Apex",        "Nyx",         "Erebus",
                "Boreas",      "Hekate",      "Orpheus",     "Andromeda",
                "Cassiopeia",  "Theseus",     "Calypso",     "Caelum",
                "Nocturna",    "Aurelius",    "Stellaris",   "Solaris",
                // Punchy one-syllable brands.
                "Pulse",       "Echo",        "Bolt",        "Halo",
                "Sable",       "Sigma",       "Omega",       "Vex",
                "Flux",        "Glyph",       "Spire",       "Crux",
                "Veil",        "Reign",       "Wrath",       "Frost",
                // Two-word esport stylings — punchy, no fluff.
                "Iron Tide",   "North Star",  "Pale Horse",  "Sable Reign",
                "Final Hour",  "Last Light",  "True North",  "Cold Mirror",
                "Burning Sky", "Silent Edge", "Apex Forge",  "Stormcrest",
                "Black Crown", "Golden Hand", "Crimson Reign","Ironclaws",
                "Stormhounds", "Nightwatch",  "Daybreak",    "Starfall",
                "Wildfire",    "Skywalkers",  "Deepfreeze",  "Hightide",
                // Region-flavored brand-modern (geographic but still punchy).
                "Pacific Heat","Atlantic Storm","Southern Cross","Highland",
                "Frontier",    "Coastal Surge","Boreal","Equinox"
            };
            g_teams.reserve(sizeof(fallback) / sizeof(fallback[0]));
            for (const char* n : fallback) g_teams.emplace_back(n);
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
            case 4: {  // brand-modern short tag — pronounceable letters,
                       // not keyboard-mash. Heavy on consonant-vowel patterns
                       // and brand-style digit prefixes / suffixes that read
                       // like a competitive org rather than a random TLA.
                static const char kCons[] = "BCDFGHJKLMNPQRSTVWXZ";
                static const char kVow[]  = "AEIOU";
                static const int  kNc = static_cast<int>(sizeof(kCons) - 1);
                static const int  kNv = static_cast<int>(sizeof(kVow)  - 1);
                double r = g_rng.uniform();
                if (r < 0.30) {
                    // CVC: "RIX", "NOX", "JAG", "VEX"
                    out.push_back(kCons[g_rng.irange(0, kNc - 1)]);
                    out.push_back(kVow [g_rng.irange(0, kNv - 1)]);
                    out.push_back(kCons[g_rng.irange(0, kNc - 1)]);
                } else if (r < 0.55) {
                    // CVCC: "VEXT", "RAFT", "JAXX" feel
                    out.push_back(kCons[g_rng.irange(0, kNc - 1)]);
                    out.push_back(kVow [g_rng.irange(0, kNv - 1)]);
                    out.push_back(kCons[g_rng.irange(0, kNc - 1)]);
                    out.push_back(kCons[g_rng.irange(0, kNc - 1)]);
                } else if (r < 0.75) {
                    // Digit + word: "9 Echo", "1 Apex", "100 Surge"
                    int d = g_rng.irange(0, 4);
                    const char* prefix = (d == 0) ? "9" : (d == 1) ? "1"
                                       : (d == 2) ? "100" : (d == 3) ? "7"
                                       : "3";
                    out = std::string(prefix) + " " + pick(kSoloNames);
                } else {
                    // Word + Esports/GG: punchy brand suffix.
                    out = std::string(pick(kSoloNames)) + " "
                        + (g_rng.chance(0.5) ? "GG" : "Esports");
                }
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
    reset_handle_cache();   // unique-gamertag registry (Names.cpp)
    // Reshuffle so the new world doesn't hand out the exact same names in
    // the exact same order — feels slightly fresher on consecutive boots.
    if (!g_names.empty()) g_rng.shuffle(g_names);
    if (!g_teams.empty()) g_rng.shuffle(g_teams);
}

// === Save/load registry snapshot + restore ================================
// The save system persists the two "already handed out" sets so a loaded
// world never hands a restored player's/org's name to a fresh entity. Only
// the SETS travel — the cursors are left wherever the post-reset state put
// them, because both take_* draws re-check the set on every candidate (a
// stale cursor merely skips names that are already taken).
std::vector<std::string> snapshot_used_names() {
    return std::vector<std::string>(g_used_names.begin(), g_used_names.end());
}

std::vector<std::string> snapshot_taken_team_names() {
    return std::vector<std::string>(g_taken_team_names.begin(),
                                    g_taken_team_names.end());
}

void restore_name_registries(std::vector<std::string> used,
                             std::vector<std::string> taken) {
    g_used_names.clear();
    for (auto& s : used) g_used_names.insert(std::move(s));
    g_taken_team_names.clear();
    for (auto& s : taken) g_taken_team_names.insert(std::move(s));
}

// === Stable entity IDs ===================================================
// One global monotonic counter shared by Player/Team/Coach. Atomic so it stays
// correct even though day-sim runs on a worker thread (entity construction is
// single-threaded today, but atomic costs nothing and future-proofs it). Starts
// at 1 so id==0 reads as "unset" (default-initialized / pre-id legacy data).
namespace {
std::atomic<std::uint64_t> g_next_entity_id{1};
}
std::uint64_t next_entity_id() {
    return g_next_entity_id.fetch_add(1, std::memory_order_relaxed);
}
void reset_entity_id_counter(std::uint64_t at_least) {
    std::uint64_t cur = g_next_entity_id.load(std::memory_order_relaxed);
    while (cur < at_least &&
           !g_next_entity_id.compare_exchange_weak(cur, at_least,
                                                   std::memory_order_relaxed)) {
        // cur is reloaded by compare_exchange_weak on failure; loop until set.
    }
}

// === WS-B region meta (B3) ===============================================
RegionId region_id_from_name(const std::string& region) noexcept {
    if (region == "EMEA")    return RegionId::EMEA;
    if (region == "Pacific") return RegionId::Pacific;
    return RegionId::Americas;   // default, incl. any unknown region
}
const RegionMeta& region_meta(const std::string& region) noexcept {
    return kRegionMeta[static_cast<std::size_t>(region_id_from_name(region))];
}
std::string region_clash_note(const std::string& rA, const std::string& rB) {
    if (rA == rB) return "";
    return rA + "'s " + region_meta(rA).brand + " vs " +
           rB + "'s " + region_meta(rB).brand;
}

}  // namespace vlr
