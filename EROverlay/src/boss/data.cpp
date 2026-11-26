#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <vector>

#include <nlohmann/json.hpp>

#include "api.h"
#include "data.hpp"
#include "params/param.hpp"
#include "util/eventflag.hpp"
#include "util/file.hpp"
#include "util/memory.hpp"

extern EROverlayAPI *api;

namespace er::bosses {

BossDataSet gBossDataSet;

static bool readFlagId(const nlohmann::ordered_json &json, uint32_t &flagId) {
    if (!json.is_number_integer())
        return false;
    if (json.is_number_unsigned()) {
        auto value = json.get<uint64_t>();
        if (value == 0 || value > (std::numeric_limits<uint32_t>::max)())
            return false;
        flagId = static_cast<uint32_t>(value);
    } else {
        auto value = json.get<int64_t>();
        if (value <= 0 || value > (std::numeric_limits<uint32_t>::max)())
            return false;
        flagId = static_cast<uint32_t>(value);
    }
    return true;
}

static bool loadReviveFlags(const nlohmann::ordered_json &json, BossData &boss) {
    auto entry = json.find("revive_flags");
    if (entry == json.end())
        return true;
    if (!entry->is_array())
        return false;
    for (const auto &reset: *entry) {
        BossData::ReviveFlag flag;
        if (reset.is_object()) {
            auto id = reset.find("flag_id");
            auto value = reset.find("value");
            if (id == reset.end() || value == reset.end() || !value->is_boolean() || !readFlagId(*id, flag.flagId))
                return false;
            flag.value = value->get<bool>();
        } else if (!readFlagId(reset, flag.flagId)) {
            return false;
        }
        if (flag.flagId == boss.flagId) {
            if (flag.value)
                return false;
            continue;
        }
        auto duplicate = std::find_if(boss.reviveFlags.begin(), boss.reviveFlags.end(), [&](const auto &other) { return other.flagId == flag.flagId; });
        if (duplicate != boss.reviveFlags.end()) {
            if (duplicate->value != flag.value)
                return false;
            continue;
        }
        boss.reviveFlags.push_back(flag);
    }
    return true;
}

// --- ChallengeState ---

void ChallengeState::loadConfig(const wchar_t *modulePath) {
    if (!enabled)
        return;
    auto filename = std::wstring(modulePath) + L"\\Challenge.txt";
    std::ifstream ifs(filename.c_str());
    if (!ifs)
        return;

    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        auto key = line.substr(0, eq);
        auto val = line.substr(eq + 1);
        if (key == "tries") {
            tries = std::strtol(val.c_str(), nullptr, 10);
        } else if (key == "deaths_on_start") {
            deathsOnStart = std::strtol(val.c_str(), nullptr, 10);
        } else if (key == "best") {
            best = std::strtol(val.c_str(), nullptr, 10);
        } else if (key == "chest_best") {
            chestBest = std::strtol(val.c_str(), nullptr, 10);
        }
    }
    // Only create change notification once (avoid handle leak on reload)
    if (changeEvent == INVALID_HANDLE_VALUE) {
        changeEvent = FindFirstChangeNotificationW(modulePath, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE);
    }
}

void ChallengeState::saveConfig(const wchar_t *modulePath) {
    auto filename = std::wstring(modulePath) + L"\\Challenge.txt";
    std::ofstream ofs(filename.c_str());
    if (!ofs) {
        fwprintf(stderr, L"Unable to open %ls for writing\n", filename.c_str());
        return;
    }
    ofs << "# Total tries\ntries=" << tries << "\n";
    ofs << "# Personal Best\nbest=" << best << "\n";
    ofs << "# Chest Challenge Personal Best\nchest_best=" << chestBest << "\n";
    ofs << "\n# !!!DO NOT EDIT THIS!!!\ndeaths_on_start=" << deathsOnStart << "\n";
    ofs.flush();

    // Drain pending file change notifications to avoid self-triggered reload
    while (WaitForSingleObject(changeEvent, 0) == WAIT_OBJECT_0) {
        FindNextChangeNotification(changeEvent);
    }
}

void ChallengeState::checkForChange(const wchar_t *modulePath) {
    if (changeEvent == INVALID_HANDLE_VALUE)
        return;
    bool needReload = false;
    while (WaitForSingleObject(changeEvent, 0) == WAIT_OBJECT_0) {
        needReload = true;
        FindNextChangeNotification(changeEvent);
    }
    if (needReload)
        loadConfig(modulePath);
}

void ChallengeState::cleanup() noexcept {
    if (changeEvent != INVALID_HANDLE_VALUE) {
        FindCloseChangeNotification(changeEvent);
        changeEvent = INVALID_HANDLE_VALUE;
    }
}

// --- BossDataSet ---

void BossDataSet::load(bool hasDLC) {
    toggleFullModeKey_ = api->configGetVirtualKey("boss.toggle_full_mode", VK_OEM_PLUS);
    allowRevive_ = api->configEnabled("boss.allow_revive");
    std::wstring name = api->configGetString("boss.data_file", L"bosses.json");
    std::wstring lang = api->configGetString("common.language", L"");
    if (lang.empty()) {
        lang = api->getGameLanguage();
    }
    std::wstring filename = std::wstring(api->getModulePath()) + L"\\data\\" + lang + L"\\" + name;
    std::ifstream ifs(filename.c_str());
    if (!ifs) {
        /* fallback to use english */
        lang = L"engUS";
        filename = std::wstring(api->getModulePath()) + L"\\data\\" + lang + L"\\" + name;
        ifs.open(filename.c_str());
        if (!ifs) {
            fwprintf(stderr, L"Unable to open %ls\n", filename.c_str());
            return;
        }
    }
    auto j = nlohmann::ordered_json::parse(ifs, nullptr, false);
    ifs.close();
    if (!j.is_array()) {
        fwprintf(stderr, L"Invalid boss data in %ls\n", filename.c_str());
        return;
    }

    // Recursive loader: handles both the flat dataset (regions + bosses) and the
    // Chest Challenge tree (children + chests + bosses at any depth).
    // Regions are always written through their index — emplace_back during the
    // recursive descent reallocates regions_, so holding a reference across the
    // recursion would be a use-after-free.
    struct Loader {
        BossDataSet &self;
        bool hasDLC;
        std::vector<size_t> loadNodes(const nlohmann::ordered_json &nodes, size_t parent) {
            std::vector<size_t> indices;
            for (auto &p: nodes.items()) {
                auto &v = p.value();
                if (!hasDLC && v.value("dlc", 0) == 1) {
                    continue;
                }
                auto regionIndex = self.regions_.size();
                {
                    auto &rd = self.regions_.emplace_back();
                    rd.name = v["region_name"];
                    rd.parent = parent;
                    rd.dlc = v.value("dlc", 0) == 1;
                }
                indices.push_back(regionIndex);
                for (auto &n: v.value("regions", nlohmann::ordered_json::array())) {
                    self.regionMap_[n.get<uint32_t>()] = static_cast<int>(regionIndex);
                }
                for (auto &n: v.value("bosses", nlohmann::ordered_json::array())) {
                    auto index = self.bosses_.size();
                    auto &bd = self.bosses_.emplace_back();
                    bd.boss = n["boss"];
                    bd.place = n.value("place", "");
                    auto primary = n.find("flag_id");
                    bd.reviveFlagsValid = primary != n.end() && readFlagId(*primary, bd.flagId) && loadReviveFlags(n, bd);
                    if (!bd.reviveFlagsValid) {
                        fprintf(stderr, "Invalid flag_id or revive_flags for boss %u (%s); revival is disabled for this boss\n", bd.flagId, bd.boss.c_str());
                    }
                    bd.index = index;
                    bd.regionIndex = regionIndex;
                    self.regions_[regionIndex].bosses.push_back(index);
                }
                for (auto &n: v.value("chests", nlohmann::ordered_json::array())) {
                    auto index = self.chests_.size();
                    auto &cd = self.chests_.emplace_back();
                    cd.chest = n.value("chest", n.value("name", ""));
                    cd.place = n.value("place", "");
                    auto primary = n.find("flag_id");
                    if (primary != n.end()) {
                        readFlagId(*primary, cd.flagId);
                    }
                    auto lot = n.find("lot_id");
                    if (lot != n.end()) {
                        readFlagId(*lot, cd.lotId);
                    }
                    cd.index = index;
                    cd.regionIndex = regionIndex;
                    self.regions_[regionIndex].chests.push_back(index);
                }
                auto children = v.find("children");
                if (children != v.end() && children->is_array()) {
                    self.regions_[regionIndex].children = loadNodes(*children, regionIndex);
                }
            }
            return indices;
        }
    } loader{*this, hasDLC};
    loader.loadNodes(j, kNoRegion);
    treeMode_ = !chests_.empty();
    if (treeMode_) {
        size_t withFlag = 0;
        for (const auto &c: chests_) {
            if (c.flagId != 0) withFlag++;
        }
        fwprintf(stderr, L"Loaded %zu regions, %zu bosses, %zu chests (%zu with flags)\n",
                 regions_.size(), bosses_.size(), chests_.size(), withFlag);
    }

    regionCounts_.resize(regions_.size());
    dead_.resize(bosses_.size());
    deadSwapBuf_.resize(bosses_.size());
    regionCountSwapBuf_.resize(regions_.size());
    chestsOpened_.resize(chests_.size());
    chestSwapBuf_.resize(chests_.size());

    notificationsEnabled_ = api->configGetString("boss.randomizer_notifications", L"true") == std::wstring(L"true");
    chestDebugWatch_ = api->configEnabled("boss.chest_debug_watch");
    loadRandoLog();

    challenge_.enabled = api->configEnabled("boss.challenge_mode");
    if (!challenge_.enabled)
        return;
    challenge_.maxDeaths = api->configGetInt("boss.challenge_death_count", 0);
    challenge_.loadConfig(api->getModulePath());
}

void BossDataSet::loadRandoLog() {
    // Config may point to a spoiler log file directly, or to a directory of logs.
    std::wstring configured = api->configGetString("boss.randomizer_spoiler_log", L"");
    std::wstring dir = std::move(configured);
    std::wstring path;
    std::error_code ec;
    if (dir.empty()) {
        // Default: the randomizer's spoiler_logs directory beside the loader, when it exists.
        std::wstring defaultDir = std::wstring(api->getModulePath()) + L"\\spoiler_logs";
        if (std::filesystem::is_directory(std::filesystem::path(defaultDir), ec)) {
            dir = std::move(defaultDir);
        }
    }
    if (!dir.empty() && std::filesystem::is_directory(std::filesystem::path(dir), ec)) {
        // A directory was configured or detected: pick the newest log inside it.
        path = RandoLog::findNewest(dir);
        if (path.empty()) {
            fwprintf(stderr, L"No spoiler log found in directory %ls\n", dir.c_str());
            return;
        }
    } else if (!dir.empty()) {
        path = std::move(dir);
    }
    // The randomizer's enemy data maps entity IDs to defeat flags; without it, only
    // bosses whose entity ID equals the defeat flag can be matched (most of them).
    std::wstring enemyData = api->configGetString("boss.randomizer_enemy_data", L"");
    if (enemyData.empty()) {
        // Default: the randomizer install's diste/Base/enemy.txt beside the loader.
        std::wstring defaultEnemyData = std::wstring(api->getModulePath()) + L"\\diste\\Base\\enemy.txt";
        if (std::filesystem::exists(std::filesystem::path(defaultEnemyData), ec)) {
            enemyData = std::move(defaultEnemyData);
        }
    }
    if (!enemyData.empty() && !randoLog_.loadEnemyData(enemyData)) {
        fwprintf(stderr, L"Failed to parse randomizer enemy data %ls; some placements may not be matched\n", enemyData.c_str());
    }
    if (path.empty()) {
        fwprintf(stderr, L"No randomizer spoiler log configured (boss.randomizer_spoiler_log); running in vanilla mode\n");
        return;
    }
    if (!randoLog_.load(path)) {
        fwprintf(stderr, L"Failed to load randomizer placements from %ls\n", path.c_str());
        return;
    }
    for (auto &bd: bosses_) {
        bd.randoPlacements = randoLog_.find(bd.flagId);
        bd.displayLabel.clear();
        if (bd.randoPlacements == nullptr || bd.randoPlacements->empty()) continue;
        // Combine the placed enemies' names: "Messmer the Impaler", or "A & B" for
        // duo/trio fights (repeated names of a same placed boss type are collapsed).
        std::string label;
        for (const auto &placement: *bd.randoPlacements) {
            bool duplicate = false;
            for (const auto &seen: *bd.randoPlacements) {
                if (&seen == &placement) break;
                if (seen.name == placement.name) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;
            if (!label.empty()) label += " & ";
            label += placement.name;
        }
        // Only override the label when it differs from the vanilla boss name.
        if (label != bd.boss) bd.displayLabel = std::move(label);
    }
    fwprintf(stderr, L"Loaded %zu randomizer placements from %ls (%s enemy data)\n", randoLog_.size(), randoLog_.path().c_str(),
             randoLog_.enemyDataLoaded() ? "with" : "without");
}

BossDataSet::~BossDataSet() noexcept { challenge_.cleanup(); }

void BossDataSet::initMemoryAddresses() {
    auto addr = api->getGameAddresses();
    gameDataMan_ = addr.gameDataMan;
    fieldArea_ = addr.fieldArea;
    lotFlagsPending_ = true;
}

// ItemLotParam row (game v1.16 layout, row size 174): getItemFlagId at 0x80 — the flag
// the game writes when any item of the lot is granted; lotItemId01..08 at 0x00..0x1C —
// the granted item IDs. The randomizer repoints chest entities to other lots (the placed
// item's original lot, or its own aux lots) without patching the chest's vanilla row, so
// the vanilla row's getItemFlagId alone misses those chests. Instead we match by granted
// item: the spoiler log tells which item each tracked chest received; rows granting that
// item identify the candidate grant flags.
void BossDataSet::resolveChestLotFlags() {
    // The param table walk touches game-internal structures whose layout may differ
    // between game versions (no plugin used it before this feature); contain any bad
    // dereference instead of taking the game down.
    __try {
        auto table = (const er::params::ParamTable *)api->paramFindTable(L"ItemLotParam");
        if (table == nullptr) {
            return; // params not loaded yet; retried on the next update tick
        }
        // 1. Collect every row: grant flag -> granted item IDs. Rows are dense
        //    (paramId, offset) entries; iterate them all once.
        struct LotRow {
            uint32_t flag = 0;
            uint32_t itemIds[8] = {};
        };
        std::map<uint32_t, LotRow> rows; // row id -> LotRow
        {
            const ::er::params::ParamTable *paramTable = table;
            uint16_t count = paramTable->count;
            for (uint16_t i = 0; i < count; i++) {
                const ::er::params::ParamEntryOffset *entry = &paramTable->entries[i];
                const uint8_t *row = (const uint8_t *)((uintptr_t)table + entry->offset);
                auto &lr = rows[entry->paramId];
                lr.flag = *(const uint32_t *)(row + 0x80);
                for (int k = 0; k < 8; k++) {
                    lr.itemIds[k] = *(const uint32_t *)(row + 4 * k);
                }
            }
        }
        // 2. Item name -> vanilla lot ids (chest_items.json, generated from the live
        //    extraction). Needed to translate the log's item names into item IDs.
        std::map<std::string, std::vector<uint32_t>> itemLots;
        loadChestItemLots(itemLots);
        // 3. For each tracked chest, find its placement in the log (best phrase match),
        //    the placed item's ID, and every row granting that item: their grant flags
        //    are the chest's candidate open flags.
        size_t attributed = 0, ambiguous = 0, unplaced = 0;
        for (auto &c: chests_) {
            // Keep the vanilla row's flag as a fallback signal (may itself be patched).
            if (c.lotId != 0) {
                auto row = rows.find(c.lotId);
                if (row != rows.end()) {
                    c.lotFlagId = row->second.flag;
                    if (c.lotFlagId != 0 && c.lotFlagId != c.flagId) {
                        fwprintf(stderr, L"Chest '%hs': lot %u getItemFlagId=%u (dataset flag %u)\n",
                                 c.chest.c_str(), c.lotId, c.lotFlagId, c.flagId);
                    }
                }
            }
            // Find the log placement describing this chest.
            const ItemPlacement *placement = nullptr;
            int bestScore = 0;
            for (const auto &p: randoLog_.itemPlacements()) {
                int score = matchChestPlacement(c, p);
                if (score > bestScore) {
                    bestScore = score;
                    placement = &p;
                }
            }
            if (placement == nullptr) {
                unplaced++;
                continue;
            }
            // Placed item's IDs: from the item's vanilla lots' granted item IDs.
            std::vector<uint32_t> wantedIds;
            auto lots = itemLots.find(placement->item);
            if (lots != itemLots.end()) {
                for (auto lotId: lots->second) {
                    auto row = rows.find(lotId);
                    if (row == rows.end()) continue;
                    for (auto id: row->second.itemIds) {
                        if (id != 0 && id != 0xFFFFFFFF) {
                            wantedIds.push_back(id);
                        }
                    }
                }
            }
            if (wantedIds.empty()) {
                unplaced++;
                continue;
            }
            // Candidate grant rows: any row granting one of the placed item's IDs whose
            // grant flag is not a tracked chest's dataset/vanilla flag. The chest's own
            // flags are already checked; only NEW flags extend tracking.
            std::vector<uint32_t> candidates;
            for (const auto &[rowId, lr]: rows) {
                if (lr.flag == 0) continue;
                if (lr.flag == c.flagId || lr.flag == c.lotFlagId) continue;
                bool grants = false;
                for (auto id: lr.itemIds) {
                    if (id != 0 && id != 0xFFFFFFFF) {
                        for (auto want: wantedIds) {
                            if (id == want) {
                                grants = true;
                                break;
                            }
                        }
                    }
                    if (grants) break;
                }
                if (grants) {
                    candidates.push_back(lr.flag);
                }
            }
            if (candidates.empty()) {
                unplaced++;
                continue;
            }
            c.extraFlags = std::move(candidates);
            attributed++;
            if (c.extraFlags.size() > 1) ambiguous++;
        }
        lotFlagsPending_ = false;
        fwprintf(stderr, L"Chest flags: %zu attributed (%zu ambiguous), %zu without placement\n",
                 attributed, ambiguous, unplaced);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lotFlagsPending_ = false;
        fwprintf(stderr, L"Chest lot flags: ItemLotParam walk failed (exception %lx); "
                         L"falling back to dataset flags only\n", GetExceptionCode());
    }
}

void BossDataSet::update() {
    if (gameDataMan_ == 0 || fieldArea_ == 0) {
        auto gameAddresses = api->getGameAddresses();
        if (gameDataMan_ == 0)
            gameDataMan_ = gameAddresses.gameDataMan;
        if (fieldArea_ == 0)
            fieldArea_ = gameAddresses.fieldArea;
    }
    challenge_.checkForChange(api->getModulePath());
    if (api->screenState() != 0) {
        return;
    }
    auto igt = readInGameTime();
    if (igt <= 0)
        return;
    // Resolve chest lot flags only once the game is actually running (regulation
    // loaded, stable state): paramFindTable walks the regulation manager, which is
    // still being initialized during boot, and must not be touched before then.
    if (lotFlagsPending_) {
        resolveChestLotFlags();
    }
    updateBosses();
    if (challenge_.enabled)
        updateChallengeMode();
    else
        updateNormalMode();
}

void BossDataSet::updateNormalMode() {
    std::lock_guard lock(mutex_);
    challenge_.playerDeaths = readDeathCount();
}

bool BossDataSet::revive(int index) {
    std::lock_guard lock(mutex_);
    if (!allowRevive_ || index < 0 || static_cast<size_t>(index) >= bosses_.size() || api->screenState() != 0 || readInGameTime() <= 0)
        return false;
    const auto &boss = bosses_[index];
    if (!boss.reviveFlagsValid)
        return false;
    auto managerSlot = api->getGameAddresses().eventFlagMan;
    if (managerSlot == 0)
        return false;
    auto manager = util::MemoryHandle(managerSlot).as<uintptr_t &>();
    if (manager == 0)
        return false;
    struct FlagWrite {
        uintptr_t address;
        uint8_t bits;
        bool value;
    };
    std::vector<FlagWrite> writes;
    writes.reserve(boss.reviveFlags.size() + 1);
    auto resolve = [&](uint32_t id, bool value) {
        uint8_t bits = 0;
        auto address = api->resolveFlagAddress(id, &bits);
        if (address == 0 || bits == 0) {
            fprintf(stderr, "Cannot revive boss %u: flag %u is unavailable; no flags were changed\n", boss.flagId, id);
            return false;
        }
        writes.push_back({address, bits, value});
        return true;
    };
    // Resolve every address against the current save before changing any byte.
    for (const auto &flag: boss.reviveFlags) {
        if (!resolve(flag.flagId, flag.value))
            return false;
    }
    if (!resolve(boss.flagId, false))
        return false;
    if (api->screenState() != 0 || util::MemoryHandle(managerSlot).as<uintptr_t &>() != manager)
        return false;
    if ((std::atomic_ref(util::MemoryHandle(writes.back().address).as<uint8_t &>()).load(std::memory_order_relaxed) & writes.back().bits) == 0)
        return false;
    // Restore arena state first, then expose the boss as alive. Preserve other
    // flags sharing a byte with an atomic bit operation for each write.
    for (const auto &write: writes) {
        std::atomic_ref byte(util::MemoryHandle(write.address).as<uint8_t &>());
        if (write.value)
            byte.fetch_or(write.bits, std::memory_order_relaxed);
        else
            byte.fetch_and(static_cast<uint8_t>(~write.bits), std::memory_order_relaxed);
    }
    if (dead_[index]) {
        dead_[index] = 0;
        --count_;
        --regionCounts_[boss.regionIndex];
    }
    return true;
}

void BossDataSet::updateBosses() {
    bool needSave = false;
    {
        std::lock_guard lock(mutex_);
        int cnt = 0;
        std::fill(deadSwapBuf_.begin(), deadSwapBuf_.end(), 0);
        std::fill(regionCountSwapBuf_.begin(), regionCountSwapBuf_.end(), 0);
        size_t sz = bosses_.size();
        auto now = std::chrono::steady_clock::now();
        for (size_t i = 0; i < sz; i++) {
            auto &b = bosses_[i];
            // Addresses may change when a save is loaded; retry unavailable flags.
            b.offset = api->resolveFlagAddress(b.flagId, &b.bits);
            if (b.offset == 0 || b.bits == 0) {
                continue;
            }
            bool al = (std::atomic_ref(util::MemoryHandle(b.offset).as<uint8_t &>()).load(std::memory_order_relaxed) & b.bits) != 0;
            deadSwapBuf_[i] = al ? 1 : 0;
            if (al) {
                cnt++;
                regionCountSwapBuf_[b.regionIndex]++;
                // Detect a fresh kill (alive->dead transition) for the notification toast.
                // The first pass after load only records the initial save state.
                if (notificationsEnabled_ && !firstUpdate_ && dead_.size() == sz && dead_[i] == 0) {
                    KillNotification note;
                    note.boss = b.boss;
                    note.place = b.place;
                    if (b.randoPlacements != nullptr && !b.randoPlacements->empty()) {
                        note.placed = b.randoPlacements->front().name;
                    }
                    note.time = now;
                    notifications_.push_back(std::move(note));
                    if (notifications_.size() > kMaxKillNotifications) {
                        notifications_.erase(notifications_.begin(), notifications_.end() - kMaxKillNotifications);
                    }
                }
            }
        }

        // Chest open flags (Chest Challenge dataset).
        // Three flag sources per chest: flagId (dataset/event-script flag), lotFlagId
        // (the chest's vanilla ItemLotParam row's live getItemFlagId) and extraFlags
        // (grant flags of the lots granting the item the chest received — the
        // randomizer repoints chest entities to other lots per run). A chest counts
        // as opened when any of them is set.
        int chestCnt = 0;
        std::fill(chestSwapBuf_.begin(), chestSwapBuf_.end(), 0);
        for (size_t i = 0; i < chests_.size(); i++) {
            auto &c = chests_[i];
            bool opened = false;
            if (c.flagId != 0) {
                c.offset = api->resolveFlagAddress(c.flagId, &c.bits);
                opened = c.offset != 0 && c.bits != 0 &&
                         (std::atomic_ref(util::MemoryHandle(c.offset).as<uint8_t &>()).load(std::memory_order_relaxed) & c.bits) != 0;
            }
            if (!opened && c.lotFlagId != 0) {
                c.lotFlagOffset = api->resolveFlagAddress(c.lotFlagId, &c.lotFlagBits);
                opened = c.lotFlagOffset != 0 && c.lotFlagBits != 0 &&
                         (std::atomic_ref(util::MemoryHandle(c.lotFlagOffset).as<uint8_t &>()).load(std::memory_order_relaxed) & c.lotFlagBits) != 0;
            }
            if (!opened && !c.extraFlags.empty()) {
                if (c.extraOffsets.size() != c.extraFlags.size()) {
                    c.extraOffsets.assign(c.extraFlags.size(), 0);
                    c.extraBits.assign(c.extraFlags.size(), 0);
                }
                for (size_t k = 0; k < c.extraFlags.size(); k++) {
                    c.extraOffsets[k] = api->resolveFlagAddress(c.extraFlags[k], &c.extraBits[k]);
                    if (c.extraOffsets[k] != 0 && c.extraBits[k] != 0 &&
                        (std::atomic_ref(util::MemoryHandle(c.extraOffsets[k]).as<uint8_t &>()).load(std::memory_order_relaxed) & c.extraBits[k]) != 0) {
                        opened = true;
                        break;
                    }
                }
            }
            if (!opened) {
                continue; // manual-only or fully-unresolvable chest
            }
            chestSwapBuf_[i] = 1;
            chestCnt++;
        }
        chestsOpened_.swap(chestSwapBuf_);
        chestCount_ = chestCnt;
        if (chestDebugWatch_) {
            updateChestDebugWatch();
        }

        regionCounts_.swap(regionCountSwapBuf_);
        dead_.swap(deadSwapBuf_);
        count_ = cnt;
        firstUpdate_ = false;
        if (fieldArea_ != 0) {
            auto addr1 = *(uintptr_t *)fieldArea_;
            if (addr1 != 0) {
                auto mapIdOffset = api->getGameVersion() < offsets::kVersionThreshold1_12 ? offsets::kMapIdPre1_12 : offsets::kMapIdPost1_12;
                auto mapId = *(uint32_t *)(addr1 + mapIdOffset);
                if (mapId != 0 && mapId != mapId_) {
                    mapId_ = mapId;
                    auto ite = regionMap_.find(mapId / 1000);
                    if (ite != regionMap_.end()) {
                        regionIndex_ = ite->second;
                    }
                }
            }
        }
        if (challenge_.enabled && cnt > challenge_.best && challenge_.deaths() <= challenge_.maxDeaths) {
            challenge_.best = cnt;
            needSave = true;
        }
        if (challenge_.enabled && chestCnt > challenge_.chestBest && challenge_.deaths() <= challenge_.maxDeaths) {
            challenge_.chestBest = chestCnt;
            needSave = true;
        }
    }
    if (needSave) {
        challenge_.saveConfig(api->getModulePath());
    }
}

void BossDataSet::updateChallengeMode() {
    if (challenge_.reachFlagOffset == 0) {
        challenge_.reachFlagOffset = api->resolveFlagAddress(kStrandedGraveyardFlagId, &challenge_.reachFlagBits);
    }
    if (challenge_.reachFlagOffset == 0)
        return;
    auto deathCount = readDeathCount();
    auto reached = (*(uint8_t *)challenge_.reachFlagOffset & challenge_.reachFlagBits) != 0;
    bool needSave = false;
    {
        std::lock_guard lock(mutex_);
        if (reached != challenge_.reachedGraveyard) {
            challenge_.reachedGraveyard = reached;
            if (reached) {
                challenge_.deathsOnStart = deathCount;
            } else {
                challenge_.playerDeaths = 0;
                challenge_.deathsOnStart = 0;
            }
            needSave = true;
        }
        if (deathCount != challenge_.playerDeaths) {
            challenge_.playerDeaths = deathCount;
            if (challenge_.deaths() == challenge_.maxDeaths + 1) {
                challenge_.tries++;
            }
            needSave = true;
        }
    }
    if (needSave)
        challenge_.saveConfig(api->getModulePath());
}

void BossDataSet::fillRenderState(RenderState &out) {
    // IGT reads game memory directly (not our state), no mutex needed
    out.inGameTime = readInGameTime();
    std::lock_guard lock(mutex_);
    out.count = count_;
    out.deaths = challenge_.deaths();
    out.regionIndex = regionIndex_;
    out.challengeMode = challenge_.enabled;
    out.challengeBest = challenge_.best;
    out.challengeTries = challenge_.tries;
    out.dead = dead_;
    out.regionCounts = regionCounts_;
    out.chestsOpened = chestsOpened_;
    out.chestCount = chestCount_;
    out.chestTotal = static_cast<int>(chests_.size());
    out.chestBest = challenge_.chestBest;
    // Drop expired notifications, then hand a copy to the render thread.
    auto now = std::chrono::steady_clock::now();
    notifications_.erase(std::remove_if(notifications_.begin(), notifications_.end(), [&](const KillNotification &n) {
        return std::chrono::duration_cast<std::chrono::seconds>(now - n.time).count() >= kKillNotificationMaxAgeSeconds;
    }), notifications_.end());
    out.notifications = notifications_;
}

int BossDataSet::readInGameTime() const {
    if (gameDataMan_ == 0)
        return -1;
    auto addr = util::MemoryHandle(gameDataMan_).as<uintptr_t &>();
    if (addr == 0)
        return -1;
    return util::MemoryHandle(addr + offsets::kInGameTime).as<int &>();
}

int BossDataSet::readDeathCount() const {
    if (gameDataMan_ == 0)
        return 0;
    auto addr = util::MemoryHandle(gameDataMan_).as<uintptr_t &>();
    if (addr == 0)
        return 0;
    return util::MemoryHandle(addr + offsets::kDeathCount).as<int &>();
}

// Load the item name -> vanilla lot ids table (data/<lang>/chest_items.json, generated
// from a live extraction by tools). Falls back to engUS like the boss data loader.
void BossDataSet::loadChestItemLots(std::map<std::string, std::vector<uint32_t>> &itemLots) {
    std::wstring lang = api->configGetString("common.language", L"");
    if (lang.empty()) {
        lang = api->getGameLanguage();
    }
    std::wstring filename = std::wstring(api->getModulePath()) + L"\\data\\" + lang + L"\\chest_items.json";
    auto read = [&]() -> nlohmann::ordered_json {
        std::ifstream ifs(filename.c_str());
        if (!ifs) return {};
        auto j = nlohmann::ordered_json::parse(ifs, nullptr, false);
        return j;
    };
    auto j = read();
    if (!j.is_object()) {
        lang = L"engUS";
        filename = std::wstring(api->getModulePath()) + L"\\data\\" + lang + L"\\chest_items.json";
        j = read();
        if (!j.is_object()) return;
    }
    for (auto &entry: j.items()) {
        if (!entry.value().is_array()) continue;
        std::vector<uint32_t> lots;
        for (auto &lot: entry.value()) {
            if (lot.is_number_unsigned() && lot.get<uint64_t>() > 0 && lot.get<uint64_t>() <= 0xFFFFFFFFu) {
                lots.push_back(static_cast<uint32_t>(lot.get<uint64_t>()));
            }
        }
        if (!lots.empty()) itemLots[entry.key()] = std::move(lots);
    }
}

// Score how well a log item placement describes a tracked chest: common significant
// words between the chest's name+place and the placement's chest phrase, requiring the
// region's first word to appear as a bonus anchor. Returns 0 when clearly unrelated.
int BossDataSet::matchChestPlacement(const ChestData &chest, const ItemPlacement &placement) const {
    static const char *stopwords[] = {"a", "an", "the", "in", "of", "at", "on", "to", "and",
                                      "by", "near", "from", "with", "for", "one", "two", "top"};
    auto significant = [](const std::string &word) {
        for (auto *stop: stopwords) {
            if (word == stop) return false;
        }
        return word.size() >= 3;
    };
    auto words = [](const std::string &text) {
        std::vector<std::string> out;
        std::string current;
        for (char c: text) {
            if (isalnum(static_cast<unsigned char>(c))) {
                current.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));
            } else if (!current.empty()) {
                out.push_back(std::move(current));
                current.clear();
            }
        }
        if (!current.empty()) out.push_back(std::move(current));
        return out;
    };
    auto haystack = words(chest.chest + " " + chest.place);
    auto needle = words(placement.chestPhrase);
    if (needle.empty()) return 0;
    int score = 0;
    for (const auto &n: needle) {
        if (!significant(n)) continue;
        for (const auto &h: haystack) {
            if (h == n || (n.size() >= 5 && h.size() >= 5 && h.find(n) == 0)) {
                score++;
                break;
            }
        }
    }
    return score;
}

// Chest flag debug watcher (boss.chest_debug_watch=true). On the first pass it
// snapshots every event flag category that holds a tracked chest's flag; on later
// passes it reports each bit that changed in those categories. Because a chest can
// legitimately write a flag we did not map (randomizer aux lots), this observes the
// whole category rather than only the tracked flags.
void BossDataSet::updateChestDebugWatch() {
    // Note: called with mutex_ held from updateBosses().
    if (gameDataMan_ == 0)
        return;
    auto eventFlagMan = api->getGameAddresses().eventFlagMan;
    auto manager = eventFlagMan == 0 ? 0 : util::MemoryHandle(eventFlagMan).as<uintptr_t &>();
    if (manager == 0)
        return;
    if (debugCategories_.empty()) {
        std::map<uint32_t, bool> categories;
        for (const auto &c: chests_) {
            if (c.flagId != 0) {
                categories[c.flagId / 1000] = true;
            }
        }
        for (const auto &[category, ignored]: categories) {
            (void)ignored;
            auto &dc = debugCategories_.emplace_back();
            dc.category = category;
        }
        return;
    }
    constexpr size_t kMaxDebugLogLines = 200;
    for (auto &dc: debugCategories_) {
        uint32_t bytes = 0;
        auto buffer = util::resolveFlagCategory(manager, dc.category, &bytes);
        if (buffer == 0) {
            continue;
        }
        if (dc.snapshot.empty()) {
            dc.snapshot.resize(bytes);
            std::memcpy(dc.snapshot.data(), (const void *)buffer, bytes);
            continue;
        }
        if (bytes != dc.snapshot.size()) {
            // Buffer size changed (save reload): re-snapshot without reporting.
            dc.snapshot.assign(bytes, 0);
            std::memcpy(dc.snapshot.data(), (const void *)buffer, bytes);
            continue;
        }
        for (uint32_t b = 0; b < bytes; b++) {
            uint8_t now = std::atomic_ref(util::MemoryHandle(buffer + b).as<uint8_t &>()).load(std::memory_order_relaxed);
            uint8_t before = dc.snapshot[b];
            if (now == before) {
                continue;
            }
            dc.snapshot[b] = now;
            uint8_t diff = static_cast<uint8_t>(now ^ before);
            for (int bit = 0; bit < 8; bit++) {
                if ((diff & (1u << bit)) == 0) {
                    continue;
                }
                uint32_t flagId = dc.category * 1000 + b * 8 + (7 - bit);
                char line[64];
                std::snprintf(line, sizeof(line), "%u %d->%d", flagId, (before >> bit) & 1, (now >> bit) & 1);
                chestDebugLog_.emplace_back(line);
            }
        }
    }
    if (chestDebugLog_.size() > kMaxDebugLogLines) {
        chestDebugLog_.erase(chestDebugLog_.begin(), chestDebugLog_.end() - kMaxDebugLogLines);
    }
}

} // namespace er::bosses
