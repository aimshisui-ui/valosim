#pragma once

#include <string>

namespace vlr {

class GameManager;

// === Binary save/load (v1) ================================================
//
// Serializes the ENTIRE world into a single little-endian binary file:
// rng engine state, the name-uniqueness registries, every Player / Coach /
// Scout / Analyst / Team exactly once (cross-referenced by their stable
// uint64 ids), every league tier, every solo-queue ladder, and the
// GameManager's persistent scalars + collections.
//
// v1 gate: saving is refused while a tournament bracket is live
// (gm.active_tournaments non-empty) — Tournament holds mid-bracket Series
// state that v1 intentionally does not serialize. Save between rounds.
//
// Intentionally transient (NOT saved, rebuilt after load): power/community
// rankings (recompute on their weekly tick), the MVP race snapshot, h2h
// season counters, replay recordings (pro/solo match history), and every
// Player*-keyed year-start snapshot map.
class SaveGame {
public:
    struct SlotInfo {
        bool exists = false;
        std::string club;
        int year = 0;
        int day = 0;             // 1-based display day (day_in_year + 1)
        std::string saved_at;    // "YYYY-MM-DD HH:MM" local time
    };

    // Serialize the ENTIRE world. Returns false (and writes nothing) if
    // gm.active_tournaments is non-empty (v1 gate) or on I/O failure.
    static bool save(const GameManager& gm, const std::string& path,
                     std::string* err = nullptr);

    // Restore a world saved by save(). gm is reset first. false on
    // failure/version mismatch.
    static bool load(GameManager& gm, const std::string& path,
                     std::string* err = nullptr);

    // Read just the header (cheap) for slot lists.
    static SlotInfo peek(const std::string& path);
};

}  // namespace vlr
