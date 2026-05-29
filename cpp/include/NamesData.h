#pragma once

// NamesData — generated weighted name pools sourced from zengm's name database.
// See cpp/tools/gen_names.js + cpp/src/NamesData.cpp.
//
// Keyed by 2-letter ISO code (e.g. "us", "br", "kr"). Each pool is a list of
// {name, weight} pairs. Weights are zengm's real-world frequency counts —
// the consumer in Names.cpp does cumulative-weight sampling so picking is
// proportional to the name's real-world commonality.
//
// Countries zengm doesn't ship (Korea has only 6 names there; Singapore +
// China absent) are NOT in this table — Names.cpp keeps its hardcoded
// pools for those ISOs and consults this table for everything else.

#include <string>
#include <unordered_map>
#include <vector>

namespace vlr {

struct WeightedName {
    std::string name;
    int         weight = 1;
};

struct WeightedNamePool {
    std::vector<WeightedName> first;
    std::vector<WeightedName> last;
};

const std::unordered_map<std::string, WeightedNamePool>& generated_name_pools();

}  // namespace vlr
