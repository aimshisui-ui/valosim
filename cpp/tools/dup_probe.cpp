// Quick probe: generate N USA players + N Brazil players and count
// unique first/last/full names. Compile + run manually to verify the
// new zengm-sourced weighted pools reduce duplicate rates.
//
// Build (from cpp/build/ after build.bat release):
//   cl /std:c++17 /EHsc /I ../include ../tools/dup_probe.cpp Common.obj Country.obj Names.obj NamesData.obj /Fe:dup_probe.exe
//   dup_probe.exe

#include "Names.h"
#include "Country.h"
#include "Common.h"

#include <cstdio>
#include <set>
#include <string>

int main() {
    using namespace vlr;
    const int N = 500;
    auto run = [&](const char* iso, const char* label) {
        const Country* c = find_country_iso(iso);
        if (!c) { std::printf("missing iso %s\n", iso); return; }
        std::set<std::string> firsts, lasts, fulls;
        for (int i = 0; i < N; ++i) {
            auto id = make_identity(*c);
            firsts.insert(id.first);
            lasts.insert(id.last);
            fulls.insert(id.first + " " + id.last);
        }
        std::printf("%s [%s] N=%d  unique first=%zu  last=%zu  full=%zu  (full-dup rate %.1f%%)\n",
                    label, iso, N, firsts.size(), lasts.size(), fulls.size(),
                    100.0 * (1.0 - (double)fulls.size() / N));
    };
    run("us", "USA");
    run("br", "Brazil");
    run("kr", "Korea (hardcoded fallback)");
    run("sg", "Singapore (hardcoded fallback)");
    return 0;
}
