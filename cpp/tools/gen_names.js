// gen_names.js — generates cpp/src/NamesData.cpp from zengm's data/names.json.
//
// Run once:
//   cd cpp/tools
//   node gen_names.js
//
// Output: cpp/src/NamesData.cpp with weighted name pools per ISO code.
// Top-N cap per pool keeps binary size manageable while preserving the
// most-weighted (most realistic) names. Countries that zengm doesn't ship
// (South Korea barely, Singapore, China) are intentionally skipped here;
// Names.cpp keeps its existing hardcoded pools for those ISOs.

'use strict';
const fs = require('fs');
const path = require('path');

const ZENGM = 'C:\\Users\\fulls\\Downloads\\zengm-master\\data\\names.json';
const OUT  = path.resolve(__dirname, '..', 'src', 'NamesData.cpp');

const TOP_N = 250;  // cap per pool (first + last separately)

// zengm country name -> our ISO code. Only ISOs zengm has meaningful data
// for (>= 30 names each). Korea (6/5 in zengm) stays hardcoded.
const ISO_MAP = {
    'USA':         'us',
    'Canada':      'ca',
    'Brazil':      'br',
    'Mexico':      'mx',
    'Chile':       'cl',
    'Argentina':   'ar',
    'Germany':     'de',
    'Sweden':      'se',
    'England':     'gb',
    'Ukraine':     'ua',
    'France':      'fr',
    'Turkey':      'tr',
    'Finland':     'fi',
    'Scotland':    'sct',
    'Ireland':     'ie',
    'Spain':       'es',
    'Russia':      'ru',
    'Poland':      'pl',
    'Netherlands': 'nl',
    'Italy':       'it',
    'Denmark':     'dk',
    'Norway':      'no',
    'Australia':   'au',
    'Japan':       'jp',
    'Thailand':    'th',
    'Vietnam':     'vn',
    'Philippines': 'ph',
    'Indonesia':   'id',
    // Korea, Singapore, China missing or too sparse — Names.cpp covers them.
};

function topN(weights, n) {
    const arr = Object.entries(weights);          // [[name, w], ...]
    arr.sort((a, b) => b[1] - a[1]);
    return arr.slice(0, n);
}

function escCpp(s) {
    return s.replace(/\\/g, '\\\\').replace(/"/g, '\\"');
}

function emitPool(iso, pool) {
    const first = topN(pool.first, TOP_N);
    const last  = topN(pool.last,  TOP_N);
    const parts = [];
    parts.push(`    {"${iso}", {`);
    parts.push(`        {`);  // first names array
    for (const [name, w] of first) {
        parts.push(`            {"${escCpp(name)}", ${w}},`);
    }
    parts.push(`        },`);
    parts.push(`        {`);  // last names array
    for (const [name, w] of last) {
        parts.push(`            {"${escCpp(name)}", ${w}},`);
    }
    parts.push(`        }`);
    parts.push(`    }},`);
    return parts.join('\n');
}

function main() {
    const raw = fs.readFileSync(ZENGM, 'utf-8');
    const data = JSON.parse(raw);
    const countries = data.countries;

    const blocks = [];
    let totalFirst = 0, totalLast = 0, countryCount = 0;
    for (const [zengmName, iso] of Object.entries(ISO_MAP)) {
        const pool = countries[zengmName];
        if (!pool) {
            console.warn(`! missing zengm country: ${zengmName} (-> ${iso})`);
            continue;
        }
        blocks.push(emitPool(iso, pool));
        totalFirst += Math.min(TOP_N, Object.keys(pool.first).length);
        totalLast  += Math.min(TOP_N, Object.keys(pool.last).length);
        countryCount++;
    }

    const banner = [
        '// ===== GENERATED FILE — DO NOT EDIT BY HAND =====',
        '// Run cpp/tools/gen_names.js to regenerate.',
        '// Source: zengm-master/data/names.json (Apache 2.0)',
        '//',
        `// ${countryCount} countries, ~${totalFirst} first + ${totalLast} last names total.`,
        `// Top-${TOP_N} per pool (sorted by zengm\'s real-world weights).`,
        '// Korea / Singapore / China missing from zengm; Names.cpp keeps',
        '// hardcoded pools for those ISOs.',
        '',
        '#include "NamesData.h"',
        '',
        'namespace vlr {',
        '',
        'const std::unordered_map<std::string, WeightedNamePool>& generated_name_pools() {',
        '    static const std::unordered_map<std::string, WeightedNamePool> table = {',
    ];
    const footer = [
        '    };',
        '    return table;',
        '}',
        '',
        '}  // namespace vlr',
        '',
    ];

    const out = banner.concat(blocks, footer).join('\n');
    fs.writeFileSync(OUT, out);
    console.log(`wrote ${OUT}`);
    console.log(`${countryCount} countries, ~${totalFirst} first + ${totalLast} last names`);
}

main();
