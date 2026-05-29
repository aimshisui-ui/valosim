// Console-only fallback build target. The real game ships as vlrgui.exe.
// Kept around for headless / CI smoke testing on machines without DX11.

#include "GameManager.h"

#include <cstdio>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string s(argv[i]);
        if (s == "--seed" && i + 1 < argc) {
            vlr::rng().seed(static_cast<std::uint64_t>(std::stoull(argv[i + 1])));
            ++i;
        }
    }

    vlr::GameManager gm;
    std::cout << "Initializing world...\n";
    gm.initialize_world();
    std::cout << "User team: " << gm.user_team->name << " (" << gm.user_team->region << ")\n";

    std::vector<std::string> log;
    gm.simulate_full_season(log);
    std::cout << "Season simulated. Year is now " << gm.year << ".\n";
    std::cout << "Last 10 log lines:\n";
    std::size_t start = log.size() > 10 ? log.size() - 10 : 0;
    for (std::size_t i = start; i < log.size(); ++i) std::cout << "  " << log[i] << "\n";
    return 0;
}
