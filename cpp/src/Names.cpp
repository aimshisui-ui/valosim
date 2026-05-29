#include "Names.h"
#include "NamesData.h"
#include "Common.h"  // rng()

#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_map>

namespace vlr {

namespace {

// Per-country first/last name pools. Kept intentionally small and made up
// of very common given/family names that anyone familiar with the culture
// would recognise. Combined with the country weighting in Country.cpp this
// gives plausible-looking realistic player names without lifting curated
// data lists.
struct NamePool {
    std::vector<std::string> first;
    std::vector<std::string> last;
};

const NamePool& pool_for(std::string_view iso) {
    static const std::unordered_map<std::string, NamePool> table = {
        {"us", {{"Aiden","Brandon","Caleb","Dylan","Ethan","Jordan","Liam","Mason","Noah","Owen","Tyler","Wyatt"},
                {"Brooks","Carter","Dawson","Foster","Hayes","Lane","Miller","Parker","Reed","Sawyer","Wells","Wright"}}},
        {"ca", {{"Adam","Blake","Cole","Eric","Hunter","Logan","Mathieu","Nathan","Ryan","Tristan"},
                {"Bouchard","Carrier","Dubois","Gauthier","Lavoie","Martel","Pelletier","Roy","Tremblay"}}},
        {"br", {{"Bruno","Caio","Diogo","Eduardo","Felipe","Gabriel","Lucas","Mateus","Pedro","Rafael","Thiago","Vinicius"},
                {"Almeida","Cardoso","Costa","Ferreira","Lima","Martins","Oliveira","Pereira","Ribeiro","Santos","Silva","Souza"}}},
        {"mx", {{"Adrian","Cesar","Daniel","Emilio","Hugo","Javier","Luis","Mario","Pablo","Sergio"},
                {"Castro","Diaz","Gomez","Hernandez","Lopez","Mendoza","Morales","Ortiz","Reyes","Vargas"}}},
        {"ar", {{"Agustin","Diego","Esteban","Facundo","Hernan","Joaquin","Lautaro","Matias","Nicolas","Tomas"},
                {"Acuna","Benitez","Fernandez","Gonzalez","Lopez","Martinez","Perez","Rodriguez","Romero","Suarez"}}},
        {"cl", {{"Alonso","Benjamin","Cristobal","Felipe","Joaquin","Maximiliano","Sebastian","Vicente"},
                {"Araya","Espinoza","Fuentes","Gonzalez","Munoz","Rojas","Saavedra","Torres","Vega"}}},

        {"gb", {{"Adam","Ben","Charlie","Daniel","Harry","Jack","James","Lewis","Oliver","Tom","Will"},
                {"Brown","Clarke","Davies","Evans","Hughes","Jones","Smith","Taylor","Walker","Williams"}}},
        {"fr", {{"Antoine","Baptiste","Clement","Hugo","Julien","Lucas","Mathieu","Nathan","Quentin","Theo"},
                {"Bernard","Dubois","Durand","Lambert","Lefevre","Martin","Moreau","Petit","Robert","Roux"}}},
        {"de", {{"Felix","Finn","Jonas","Lars","Leon","Lukas","Maximilian","Niklas","Paul","Tim"},
                {"Bauer","Becker","Fischer","Hoffmann","Klein","Mueller","Schmidt","Schneider","Wagner","Weber"}}},
        {"es", {{"Alejandro","Alvaro","Carlos","Diego","Hugo","Javier","Manuel","Mario","Pablo","Pedro"},
                {"Alonso","Castro","Garcia","Gomez","Lopez","Marin","Martinez","Romero","Sanchez","Torres"}}},
        {"se", {{"Adrian","Albin","Anton","Elias","Hampus","Kasper","Leo","Linus","Oscar","Viktor"},
                {"Andersson","Berg","Eriksson","Holm","Johansson","Karlsson","Larsson","Lindberg","Olsson","Svensson"}}},
        {"tr", {{"Ahmet","Berk","Burak","Can","Emre","Furkan","Kerem","Mert","Murat","Yusuf"},
                {"Aksoy","Aydin","Celik","Demir","Kaya","Koc","Ozturk","Sahin","Yildiz","Yilmaz"}}},
        {"ru", {{"Aleksandr","Alexei","Anton","Daniil","Dmitri","Ivan","Kirill","Maxim","Nikita","Pavel"},
                {"Belov","Fedorov","Ivanov","Kozlov","Kuznetsov","Morozov","Petrov","Smirnov","Sokolov","Volkov"}}},
        {"pl", {{"Adrian","Bartosz","Dawid","Filip","Jakub","Kacper","Kamil","Mateusz","Patryk","Tomasz"},
                {"Adamczyk","Dabrowski","Kaminski","Kowalski","Lewandowski","Mazur","Nowak","Wisniewski","Wojcik","Zielinski"}}},
        {"nl", {{"Bram","Daan","Finn","Jasper","Lars","Luuk","Niels","Sem","Sven","Thijs"},
                {"Bakker","De Boer","De Jong","De Vries","Hendriks","Jansen","Mulder","Smit","Van Dijk","Visser"}}},
        {"it", {{"Alessandro","Andrea","Davide","Federico","Francesco","Gabriele","Lorenzo","Luca","Marco","Matteo"},
                {"Bianchi","Bruno","Conti","Esposito","Ferrari","Galli","Greco","Marino","Romano","Rossi"}}},
        {"dk", {{"Anders","Christian","Frederik","Jonas","Magnus","Mathias","Mikkel","Nikolaj","Oliver","Rasmus"},
                {"Andersen","Christensen","Hansen","Jensen","Jorgensen","Larsen","Madsen","Nielsen","Pedersen","Sorensen"}}},
        {"fi", {{"Aatu","Antti","Eero","Henri","Joona","Mikko","Niko","Onni","Tatu","Veeti"},
                {"Hakkarainen","Heikkinen","Korhonen","Laine","Lehtonen","Makinen","Niemi","Nieminen","Salo","Virtanen"}}},

        {"kr", {{"Doyoon","Hajin","Jaehyun","Jihoon","Jiwon","Joon","Minho","Minjun","Seojun","Seungho","Taehyun","Woojin"},
                {"Bae","Choi","Han","Jang","Jeong","Kang","Kim","Lee","Park","Seo","Shin","Yoon"}}},
        {"jp", {{"Daichi","Haruto","Hayato","Kaito","Kenta","Riku","Sora","Takumi","Yuto","Yuya"},
                {"Fujita","Hayashi","Ito","Kobayashi","Nakamura","Sasaki","Sato","Suzuki","Tanaka","Watanabe"}}},
        {"cn", {{"Bo","Chenxi","Hao","Jian","Lei","Liang","Ming","Tao","Wei","Yong"},
                {"Chen","Huang","Li","Liu","Sun","Wang","Wu","Xu","Yang","Zhang","Zhao","Zhou"}}},
        {"au", {{"Aiden","Bailey","Cooper","Ethan","Harrison","Jackson","Kai","Lachlan","Mitchell","Riley","Tyler"},
                {"Anderson","Brown","Campbell","Davis","Harris","Jones","Mitchell","Nguyen","Smith","Taylor","Walker"}}},
        {"th", {{"Anuchit","Boon","Chai","Kraisorn","Narin","Pichai","Somchai","Tawan","Thanawat","Yothin"},
                {"Boonmee","Charoen","Phongsai","Rattanak","Saetang","Srisuk","Suwannarat","Tanaka","Wong","Yotsri"}}},
        {"vn", {{"Anh","Binh","Cuong","Dat","Duy","Hieu","Khoa","Long","Minh","Phuc","Tuan","Viet"},
                {"Bui","Dang","Dinh","Doan","Duong","Hoang","Le","Nguyen","Pham","Phan","Tran","Vu"}}},
        {"ph", {{"Aaron","Carlo","Daniel","Joshua","Justin","Kenneth","Marc","Miguel","Paolo","Ralph"},
                {"Cruz","Dela Cruz","Fernandez","Garcia","Lim","Mendoza","Reyes","Santos","Tan","Villanueva"}}},
        {"id", {{"Adi","Andi","Bagus","Bayu","Dimas","Eko","Hadi","Joko","Putra","Rizky","Yusuf"},
                {"Hartono","Kusuma","Pratama","Putra","Saputra","Setiawan","Susanto","Wijaya","Wibowo","Widodo"}}},
        {"sg", {{"Aaron","Bryan","Daniel","Glenn","Ian","Jared","Kelvin","Marcus","Ryan","Sean"},
                {"Chan","Lim","Lee","Ng","Ong","Tan","Tay","Wong","Yeo","Yong"}}},

        {"ua", {{"Andriy","Bohdan","Dmytro","Ihor","Maksym","Mykola","Nazar","Oleh","Roman","Taras","Vadym","Yaroslav"},
                {"Bondar","Boyko","Hrytsenko","Kovalchuk","Koval","Melnyk","Oliynyk","Petrenko","Savchenko","Shevchenko","Tkachenko","Voloshyn"}}},
        {"sct",{{"Alasdair","Callum","Duncan","Euan","Fraser","Hamish","Iain","Lachlan","Murray","Rory","Stuart"},
                {"Cameron","Campbell","Ferguson","Fraser","Gordon","MacDonald","MacKenzie","McGregor","Stewart","Sutherland","Wallace"}}},
        {"ie", {{"Aidan","Cian","Conor","Cormac","Darragh","Eoin","Niall","Oisin","Padraig","Ronan","Sean","Tadhg"},
                {"Brennan","Byrne","Donovan","Doyle","Fitzgerald","Gallagher","Kennedy","McCarthy","Murphy","O Brien","O Sullivan","Walsh"}}},
        {"no", {{"Andreas","Eirik","Erlend","Fredrik","Henrik","Jonas","Kristoffer","Magnus","Sigurd","Sondre","Tobias","Vegard"},
                {"Andersen","Berg","Hansen","Haugen","Johansen","Karlsen","Larsen","Moen","Nilsen","Olsen","Pedersen","Solberg"}}},
    };
    static const NamePool default_pool = {
        {"Alex","Chris","Dan","Jake","Mike","Ryan","Sam","Tyler"},
        {"Anderson","Brown","Davis","Garcia","Johnson","Lee","Martin","Smith"}
    };
    auto it = table.find(std::string(iso));
    if (it == table.end()) return default_pool;
    return it->second;
}

// Algorithmic gamertag generation. Output should look like real esports
// handles: short, punchy, occasionally numeric or stylized but not corny.
//
// Five styles, weighted:
//   1. Single root word              "Frost", "Crisp", "Reverb"
//   2. Root + suffix                 "Frostz", "Crispx", "Driftr"
//   3. Root + 1-3 digit number       "Frost7", "Drift99"
//   4. Leet-substituted root         "Fr0st", "Cr1sp"
//   5. Two-syllable composite        "FrostByte", "CrispWolf"
const char* const kRoots[] = {
    "Ace", "Apex", "Arc", "Atlas", "Beam", "Blaze", "Blink", "Bolt",
    "Brink", "Burst", "Byte", "Cipher", "Clash", "Cobalt", "Comet",
    "Cosmo", "Crisp", "Crux", "Cyan", "Daze", "Drift", "Dusk", "Echo",
    "Eden", "Edge", "Ember", "Equal", "Faze", "Flash", "Flux", "Frost",
    "Glass", "Glint", "Glow", "Halo", "Haze", "Hex", "Husk", "Ion",
    "Jolt", "Karma", "Kite", "Krypto", "Lush", "Mach", "Maze",
    "Mecha", "Mist", "Neon", "Nova", "Onyx", "Orbit", "Pixel", "Plume",
    "Polar", "Pulse", "Quake", "Quartz", "Quirk", "Reign", "Reverb",
    "Riot", "Roam", "Rune", "Salt", "Scope", "Shade", "Shock", "Slate",
    "Snare", "Solo", "Spire", "Static", "Storm", "Strike", "Surge",
    "Synth", "Syrup", "Tilt", "Trace", "Trick", "Twilight", "Vibe",
    "Volt", "Vortex", "Wave", "Whisk", "Xenon", "Yonder", "Zephyr",
    "Zero", "Zest", "Zoom"
};

const char* const kSuffixes[] = {
    "x", "z", "r", "y", "ix", "ko", "es", "ah", "io", "ka", "qi", "vy"
};

const char* const kSecondWords[] = {
    "Byte", "Wolf", "Hawk", "Frost", "Edge", "Pulse", "Clip", "Strike",
    "Beam", "Drift", "Pixel", "Saint", "Storm", "Glide"
};

std::string lowercase(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string apply_leet(std::string s) {
    // Sparse leet — pick exactly 1 char to substitute when it makes sense.
    int count = 0;
    int target = rng().irange(0, std::max(1, (int)s.size() - 1));
    for (auto& c : s) {
        if (count++ != target) continue;
        switch (std::tolower((unsigned char)c)) {
            case 'o': c = '0'; return s;
            case 'i': c = '1'; return s;
            case 'e': c = '3'; return s;
            case 'a': c = '4'; return s;
            case 's': c = '5'; return s;
            case 't': c = '7'; return s;
            default: break;
        }
    }
    return s;
}

std::string make_handle() {
    constexpr int kRootCount = static_cast<int>(sizeof(kRoots) / sizeof(*kRoots));
    constexpr int kSuffCount = static_cast<int>(sizeof(kSuffixes) / sizeof(*kSuffixes));
    constexpr int kSecCount  = static_cast<int>(sizeof(kSecondWords) / sizeof(*kSecondWords));

    int style = rng().weighted_index(std::vector<int>{18, 22, 18, 18, 14});
    std::string root = kRoots[rng().irange(0, kRootCount - 1)];

    switch (style) {
        case 0:  // bare root, often lowercase
            return rng().chance(0.5) ? lowercase(root) : root;
        case 1: {  // root + suffix
            std::string s = root + kSuffixes[rng().irange(0, kSuffCount - 1)];
            return rng().chance(0.7) ? lowercase(s) : s;
        }
        case 2: {  // root + number
            int n = rng().chance(0.6) ? rng().irange(1, 9)
                  : rng().chance(0.5) ? rng().irange(10, 99)
                                       : rng().irange(100, 999);
            return root + std::to_string(n);
        }
        case 3:  // leet root
            return apply_leet(rng().chance(0.5) ? lowercase(root) : root);
        case 4:  // two-word composite
        default: {
            std::string second = kSecondWords[rng().irange(0, kSecCount - 1)];
            return root + second;
        }
    }
}

}  // namespace

// Major cities per country — used as the player's hometown in the profile.
// These are well-known major population centers in each country (factual
// public references, not curated data).
const std::vector<std::string>& cities_for(std::string_view iso) {
    static const std::unordered_map<std::string, std::vector<std::string>> table = {
        {"us",  {"Los Angeles","New York","Chicago","Houston","Dallas","Atlanta","Seattle","Miami"}},
        {"ca",  {"Toronto","Montreal","Vancouver","Calgary","Ottawa","Edmonton"}},
        {"br",  {"Sao Paulo","Rio de Janeiro","Brasilia","Salvador","Curitiba","Porto Alegre"}},
        {"mx",  {"Mexico City","Guadalajara","Monterrey","Puebla","Tijuana"}},
        {"ar",  {"Buenos Aires","Cordoba","Rosario","Mendoza","La Plata"}},
        {"cl",  {"Santiago","Valparaiso","Concepcion","Antofagasta"}},
        {"gb",  {"London","Manchester","Birmingham","Liverpool","Leeds","Bristol"}},
        {"sct", {"Edinburgh","Glasgow","Aberdeen","Dundee","Inverness"}},
        {"ie",  {"Dublin","Cork","Galway","Limerick","Waterford"}},
        {"fr",  {"Paris","Marseille","Lyon","Toulouse","Bordeaux","Lille"}},
        {"de",  {"Berlin","Munich","Hamburg","Cologne","Frankfurt","Stuttgart"}},
        {"es",  {"Madrid","Barcelona","Valencia","Seville","Bilbao","Malaga"}},
        {"se",  {"Stockholm","Gothenburg","Malmo","Uppsala","Vasteras"}},
        {"no",  {"Oslo","Bergen","Trondheim","Stavanger","Tromso"}},
        {"tr",  {"Istanbul","Ankara","Izmir","Bursa","Antalya","Adana"}},
        {"ua",  {"Kyiv","Kharkiv","Odesa","Lviv","Dnipro"}},
        {"ru",  {"Moscow","Saint Petersburg","Novosibirsk","Yekaterinburg","Kazan"}},
        {"pl",  {"Warsaw","Krakow","Lodz","Wroclaw","Poznan","Gdansk"}},
        {"nl",  {"Amsterdam","Rotterdam","The Hague","Utrecht","Eindhoven"}},
        {"it",  {"Rome","Milan","Naples","Turin","Bologna","Florence"}},
        {"dk",  {"Copenhagen","Aarhus","Odense","Aalborg"}},
        {"fi",  {"Helsinki","Espoo","Tampere","Vantaa","Oulu"}},
        {"kr",  {"Seoul","Busan","Incheon","Daegu","Daejeon","Gwangju"}},
        {"jp",  {"Tokyo","Osaka","Yokohama","Nagoya","Sapporo","Fukuoka"}},
        {"cn",  {"Shanghai","Beijing","Guangzhou","Shenzhen","Chengdu","Hangzhou"}},
        {"au",  {"Sydney","Melbourne","Brisbane","Perth","Adelaide"}},
        {"th",  {"Bangkok","Chiang Mai","Phuket","Pattaya","Khon Kaen"}},
        {"vn",  {"Hanoi","Ho Chi Minh City","Da Nang","Hai Phong","Can Tho"}},
        {"ph",  {"Manila","Quezon City","Cebu","Davao","Makati"}},
        {"id",  {"Jakarta","Surabaya","Bandung","Medan","Semarang"}},
        {"sg",  {"Singapore"}},
    };
    static const std::vector<std::string> empty{};
    auto it = table.find(std::string(iso));
    return it == table.end() ? empty : it->second;
}

std::string pick_city(std::string_view iso) {
    auto& cs = cities_for(iso);
    if (cs.empty()) return std::string{};
    return cs[static_cast<std::size_t>(rng().irange(0, (int)cs.size() - 1))];
}

// Weighted pick from a generated zengm pool. Each entry has a real-world
// frequency weight; we sample proportionally so common names dominate while
// rare names still appear. Falls back to the front of the vector if total
// underflows (defensive — shouldn't happen with the generator's data).
static std::string weighted_pick(const std::vector<WeightedName>& v) {
    if (v.empty()) return std::string{};
    long long total = 0;
    for (const auto& w : v) total += (w.weight > 0 ? w.weight : 1);
    if (total <= 1) return v.front().name;
    long long r = rng().irange(0, static_cast<int>(total - 1));
    for (const auto& w : v) {
        r -= (w.weight > 0 ? w.weight : 1);
        if (r < 0) return w.name;
    }
    return v.back().name;
}

PlayerIdentity make_identity(const Country& country) {
    PlayerIdentity id;
    // Primary source: zengm's weighted real-world name pool (NamesData.cpp).
    // ~4600 first + ~5000 last names across 27 countries — duplicates between
    // players become vanishingly rare. Fall back to the small hardcoded
    // NamePool (above) for ISOs zengm doesn't ship (kr/sg/cn/cl).
    const auto& gen = generated_name_pools();
    auto wit = gen.find(std::string(country.iso));
    if (wit != gen.end() && !wit->second.first.empty() && !wit->second.last.empty()) {
        id.first = weighted_pick(wit->second.first);
        id.last  = weighted_pick(wit->second.last);
    } else {
        const NamePool& pool = pool_for(country.iso);
        if (!pool.first.empty() && !pool.last.empty()) {
            id.first = pool.first[static_cast<std::size_t>(rng().irange(0, (int)pool.first.size() - 1))];
            id.last  = pool.last [static_cast<std::size_t>(rng().irange(0, (int)pool.last.size()  - 1))];
        }
    }
    id.handle = make_handle();
    id.country = &country;
    return id;
}

std::string generate_handle() { return make_handle(); }

}  // namespace vlr
