// Validate a bosses.json-like file against the Boss overlay loader expectations.
// Mirrors BossDataSet::load() field handling and the randomizer flag matching.
#include "../EROverlay/src/boss/randolog.cpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

using namespace er::bosses;

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: %s <bosses.json>\n", argv[0]);
        return 1;
    }
    std::ifstream ifs(argv[1]);
    if (!ifs) {
        printf("cannot open %s\n", argv[0]);
        return 1;
    }
    std::stringstream ss;
    ss << ifs.rdbuf();
    std::string err;
    auto j = nlohmann::ordered_json::parse(ss.str(), nullptr, false);
    if (!j.is_array()) {
        printf("INVALID: not a JSON array\n");
        return 1;
    }
    size_t bosses = 0, withFlag = 0, withPlace = 0, emptyPlace = 0;
    for (auto &region: j.items()) {
        if (!region.value().contains("region_name") || !region.value().contains("bosses")) {
            printf("INVALID: region missing region_name/bosses\n");
            return 1;
        }
        for (auto &b: region.value()["bosses"].items()) {
            bosses++;
            auto &bd = b.value();
            if (!bd.contains("boss") || !bd.contains("flag_id")) {
                printf("INVALID: boss entry missing boss/flag_id\n");
                return 1;
            }
            if (!bd["flag_id"].is_number_integer()) {
                printf("INVALID: flag_id not an integer for %s\n", std::string(bd["boss"]).c_str());
                return 1;
            }
            auto flag = bd["flag_id"].get<long long>();
            if (flag <= 0) {
                printf("INVALID: bad flag_id %lld for %s\n", flag, std::string(bd["boss"]).c_str());
                return 1;
            }
            if (bd.contains("place") && !std::string(bd["place"]).empty()) withPlace++; else emptyPlace++;
            withFlag++;
        }
    }
    printf("VALID: %zu regions, %zu bosses (%zu with place, %zu with empty place)\n", j.size(), bosses, withPlace, emptyPlace);
    return 0;
}
