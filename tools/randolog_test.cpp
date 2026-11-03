// Standalone test harness for RandoLog parsing.
// Build on Linux:  g++ -std=c++23 -I../EROverlay/deps/nlohmann_json/include -o randolog_test tools/randolog_test.cpp
// (The include below pulls in randolog.cpp directly; on Windows the file is
// compiled as part of the Boss overlay plugin instead.)
#include "../EROverlay/src/boss/randolog.cpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace er::bosses;

static void testSynthetic() {
    std::string log =
        "Options and seed: enemy item seed:123 --preset Custom\n"
        "Version: v0.11.4\n"
        "\n"
        "-- Hints for key items:\n"
        "Replacing starting gift Golden Seed: Telescope\n"
        "\n"
        "-- Boss placements\n"
        "Replacing Margit, the Fell Omen (#10000850) in Stormveil Castle: Messmer the Impaler (#21010800) from Shadow Keep (scaling 10->10)\n"
        "Replacing Godrick the Grafted (#10000800) in Stormveil Castle: Margit, the Fell Omen (#10000850) from Stormveil Castle\n"
        "Replacing Tree Sentinel (#1042360800) in Church of Elleh: Malenia, Blade of Miquella (#15000800) from Elphael, Brace of the Haligtree\n"
        "Replacing missing #1042370800: Foo (#123456) from Somewhere\n"
        "Replacing Radahn, Consort of Miquella (#20010800) in Enir-Ilim: Godfrey, First Elden Lord (#11000850) from Leyndell, Ashen Capital\n"
        "\n"
        "-- Miniboss placements\n"
        "Replacing Night's Cavalry (#1043370340) in Agheel Lake: Ulcerated Tree Spirit (#18000800) from Limgrave\n"
        "Replacing Runebear (#31010800) in Earthbore Cave: Runebear (#31010800) from Earthbore Cave\n"
        "\n"
        "-- Basic placements\n"
        "Replacing Troll (#1012021) in Limgrave: Bat (#1050200) from Limgrave\n";
    RandoLog rl;
    assert(rl.parse(log));
    printf("parsed placements: %zu\n", rl.size());

    // Without enemy data: entity IDs are used as flags directly.
    const auto *p = rl.find(1042360800);
    assert(p != nullptr);
    assert(p->size() == 1);
    assert(p->front().name == "Malenia, Blade of Miquella");
    assert(p->front().sourceId == 15000800);
    assert(p->front().section == RandoSection::Boss);
    printf("Tree Sentinel location -> %s (from %s)\n", p->front().name.c_str(), p->front().sourcePlace.c_str());

    p = rl.find(10000850);
    assert(p != nullptr && p->front().name == "Messmer the Impaler" && p->front().sourcePlace == "Shadow Keep");

    p = rl.find(31010800);
    assert(p != nullptr && p->front().name == "Runebear" && p->front().section == RandoSection::Miniboss);

    assert(rl.find(1012021) != nullptr); // basic section entry still parses
    assert(rl.find(999999999) == nullptr); // unknown flag

    // With enemy data (loaded before the log, as in BossDataSet::loadRandoLog):
    // Night's Cavalry entity 1043370340 -> defeat flag 1043370800.
    RandoLog rlFlag;
    assert(!rlFlag.loadEnemyData(std::wstring())); // empty path fails cleanly
    assert(!rlFlag.enemyDataLoaded());
    std::string enemyData =
        "Classes:\r\n"
        "- Class: Boss\r\n"
        "  Parent: Basic\r\n"
        "- ID: 10000850\r\n"
        "  Map: m10_00_00_00\r\n"
        "  Class: Boss\r\n"
        "  DefeatFlag: 10000850\r\n"
        "- ID: 1043370340\r\n"
        "  Map: m60_43_37_00\r\n"
        "  Class: NightMiniboss\r\n"
        "  DefeatFlag: 1043370800\r\n"
        "- ID: 1052380800\r\n"
        "  Class: Boss\r\n"
        "  DefeatFlag: 1252380800\r\n";
    {
        FILE *f = fopen("/tmp/randolog_enemy.txt", "wb");
        assert(f != nullptr);
        fwrite(enemyData.data(), 1, enemyData.size(), f);
        fclose(f);
    }
    assert(rlFlag.loadEnemyData(L"/tmp/randolog_enemy.txt"));
    assert(rlFlag.enemyDataLoaded());
    assert(rlFlag.parse(log));
    p = rlFlag.find(1043370800); // flag differs from entity ID
    assert(p != nullptr && p->front().name == "Ulcerated Tree Spirit");
    p = rlFlag.find(10000850); // identity mapping still works
    assert(p != nullptr && p->front().name == "Messmer the Impaler");

    // Boss section beats Miniboss section for the same ID.
    std::string log2 =
        "-- Miniboss placements\n"
        "Replacing A (#123) in X: MinibossOne (#456) from Y\n"
        "\n"
        "-- Boss placements\n"
        "Replacing A (#123) in X: BossOne (#789) from Z\n";
    RandoLog rl2;
    assert(rl2.parse(log2));
    assert(rl2.find(123)->front().name == "BossOne");

    // No boss randomization: "(not randomized)" sections.
    std::string log3 =
        "-- Boss placements\n"
        "(not randomized)\n"
        "\n"
        "-- Miniboss placements\n"
        "(not randomized)\n";
    RandoLog rl3;
    assert(!rl3.parse(log3));
    assert(!rl3.loaded());

    printf("testSynthetic OK\n");
}

static void testUserLog(const char *path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        printf("user log not found: %s\n", path);
        return;
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    RandoLog rl;
    bool ok = rl.parse(ss.str());
    printf("user log parsed=%d placements=%zu (expected 0 for items-only run)\n", ok ? 1 : 0, rl.size());
    assert(!ok && rl.size() == 0);
    printf("testUserLog OK\n");
}

static void testEdgeCases() {
    // CRLF line endings and UTF-8 BOM.
    std::string log = "\xEF\xBB\xBF-- Boss placements\r\n"
                      "Replacing Godrick the Grafted (#10000800) in Stormveil Castle: Rennala, Queen of the Full Moon (#14000800) from Raya Lucaria Academy\r\n"
                      "(not randomized)\r\n";
    RandoLog rl;
    assert(rl.parse(log));
    assert(rl.find(10000800) != nullptr && rl.find(10000800)->front().name == "Rennala, Queen of the Full Moon");

    // Missing source id -> skipped; malformed lines -> skipped without crashing.
    std::string log2 =
        "-- Boss placements\n"
        "Replacing Foo (#123) in X: NoIdHere from Y\n"
        "Replacing Foo (#124) in X: Bad (#abc) from Y\n"
        "Replacing Foo (#125) in X\n"
        "Replacing Bar (#126) in X: Good (#777) from Y\n";
    RandoLog rl2;
    assert(rl2.parse(log2));
    assert(rl2.find(123) == nullptr);
    assert(rl2.find(124) == nullptr);
    assert(rl2.find(125) == nullptr);
    assert(rl2.find(126) != nullptr && rl2.find(126)->front().sourceId == 777);

    // Duo/trio companions: the randomizer's enemy data maps companion entity IDs
    // (flag+1, +2) to the same defeat flag, so both land under one flag key and
    // the label builder collapses repeated names.
    std::string enemyData =
        "- ID: 1051360800\r\n"
        "  Class: Boss\r\n"
        "  DefeatFlag: 1051360800\r\n"
        "- ID: 1051360801\r\n"
        "  Class: Boss\r\n"
        "  DefeatFlag: 1051360800\r\n"
        "- ID: 1041510800\r\n"
        "  Class: Miniboss\r\n"
        "  DefeatFlag: 1041510800\r\n"
        "- ID: 1041510801\r\n"
        "  Class: Miniboss\r\n"
        "  DefeatFlag: 1041510800\r\n";
    {
        FILE *f = fopen("/tmp/randolog_enemy2.txt", "wb");
        assert(f != nullptr);
        fwrite(enemyData.data(), 1, enemyData.size(), f);
        fclose(f);
    }
    std::string log3 =
        "-- Boss placements\n"
        "Replacing Crucible Knight (#1051360800) in Redmane Castle: Messmer the Impaler (#21010800) from Shadow Keep\n"
        "Replacing Misbegotten Warrior (#1051360801) in Redmane Castle: Rellana, Twin Moon Knight (#2048440800) from Castle Ensis\n"
        "\n"
        "-- Miniboss placements\n"
        "Replacing Tree Sentinel A (#1041510800) in Altus Plateau: Malenia, Blade of Miquella (#15000800) from Elphael\n"
        "Replacing Tree Sentinel B (#1041510801) in Altus Plateau: Malenia, Blade of Miquella (#15000800) from Elphael\n";
    RandoLog rl3;
    assert(rl3.loadEnemyData(L"/tmp/randolog_enemy2.txt"));
    assert(rl3.parse(log3));
    auto duo = rl3.find(1051360800);
    assert(duo != nullptr && duo->size() == 2 && duo->front().name == "Messmer the Impaler" && duo->back().name == "Rellana, Twin Moon Knight");
    auto twins = rl3.find(1041510800);
    assert(twins != nullptr && twins->size() == 2 && twins->front().name == "Malenia, Blade of Miquella" && twins->back().name == "Malenia, Blade of Miquella");

    printf("testEdgeCases OK\n");
}

int main() {
    testSynthetic();
    testUserLog("/workspaces/ERTracker/spoiler_logs.txt");
    testEdgeCases();
    printf("ALL TESTS PASSED\n");
    return 0;
}
