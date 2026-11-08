#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "randolog.hpp"

namespace er::bosses {

// Game memory offsets
namespace offsets {
constexpr uintptr_t kInGameTime = 0xA0;
constexpr uintptr_t kDeathCount = 0x94;
constexpr uintptr_t kMapIdPre1_12 = 0xE4;
constexpr uintptr_t kMapIdPost1_12 = 0xE8;
constexpr uint64_t kVersionThreshold1_12 = 0x0002000200000000ULL;
} // namespace offsets

constexpr uint32_t kStrandedGraveyardFlagId = 101;
constexpr int kUpdateTickMask = 0x1F; // update every 32 ticks (~60Hz / 32 ≈ ~2Hz)
constexpr size_t kMaxKillNotifications = 8;
constexpr int kKillNotificationMaxAgeSeconds = 6;
constexpr size_t kNoRegion = static_cast<size_t>(-1);

struct BossData {
    std::string boss;
    std::string place;
    // Boss name including the randomized placement (when different from boss); empty otherwise.
    std::string displayLabel;
    uint32_t flagId = 0;
    uintptr_t offset = 0;
    uint8_t bits = 0;
    // Enemies placed at this location by the randomizer, from the parsed spoiler log
    // (more than one for duo/trio fights); null in vanilla mode.
    const std::vector<RandoPlacement> *randoPlacements = nullptr;
    struct ReviveFlag {
        uint32_t flagId = 0;
        bool value = false;
    };
    std::vector<ReviveFlag> reviveFlags;
    bool reviveFlagsValid = true;
    size_t index = 0;
    size_t regionIndex = 0;
};

// A treasure chest tracked by its open event flag (Chest Challenge dataset).
// flagId 0 = no flag known; the chest is then only markable in-game manually.
struct ChestData {
    std::string chest;
    std::string place;
    uint32_t flagId = 0;
    uintptr_t offset = 0;
    uint8_t bits = 0;
    size_t index = 0;
    size_t regionIndex = 0;
};

// A boss defeat detected in real time, shown as a toast by the renderer.
struct KillNotification {
    std::string boss;   // Vanilla boss name of the location
    std::string place;  // Location name (may be empty)
    std::string placed; // Name of the enemy the randomizer placed there
    std::chrono::steady_clock::time_point time;
};

struct RegionData {
    std::string name;
    std::vector<size_t> bosses;
    std::vector<size_t> chests;
    // Child regions (Chest Challenge tree); empty for flat datasets.
    std::vector<size_t> children;
    size_t parent = kNoRegion;
};

// Thread-safe snapshot of mutable state for the render thread.
// Populated by BossDataSet::fillRenderState() under mutex, then read freely by the render thread.
struct RenderState {
    int count = 0;
    int deaths = 0;
    int regionIndex = -1;
    int inGameTime = 0;
    bool challengeMode = false;
    int challengeBest = 0;
    int challengeTries = 0;
    std::vector<uint8_t> dead;
    std::vector<int> regionCounts;
    std::vector<KillNotification> notifications;
    // Chest tracking (Chest Challenge dataset); empty in boss-only mode.
    std::vector<uint8_t> chestsOpened;
    int chestCount = 0;
    int chestTotal = 0;
};

// Challenge mode state and persistence (config read/write for Challenge.txt).
struct ChallengeState {
    bool enabled = false;
    int maxDeaths = 0;
    int best = 0;
    int deathsOnStart = 0;
    int tries = 0;
    int playerDeaths = 0;

    uintptr_t reachFlagOffset = 0;
    uint8_t reachFlagBits = 0;
    bool reachedGraveyard = true;

    HANDLE changeEvent = INVALID_HANDLE_VALUE;

    [[nodiscard]] int deaths() const { return enabled ? (reachedGraveyard ? playerDeaths - deathsOnStart : 0) : playerDeaths; }

    void loadConfig(const wchar_t *modulePath);
    void saveConfig(const wchar_t *modulePath);
    void checkForChange(const wchar_t *modulePath);
    void cleanup() noexcept;
};

class BossDataSet {
public:
    ~BossDataSet() noexcept;

    void load(bool hasDLC);
    // Load randomized placements from a spoiler log (randomizer integration).
    void loadRandoLog();
    void initMemoryAddresses();
    void update();
    [[nodiscard]] bool revive(int index);

    // Immutable after load() — safe to read from any thread without synchronization.
    [[nodiscard]] int toggleFullModeKey() const { return toggleFullModeKey_; }
    [[nodiscard]] const std::vector<BossData> &bosses() const { return bosses_; }
    [[nodiscard]] const std::vector<ChestData> &chests() const { return chests_; }
    [[nodiscard]] const std::vector<RegionData> &regions() const { return regions_; }
    [[nodiscard]] int total() const { return static_cast<int>(bosses_.size()); }
    [[nodiscard]] int chestTotal() const { return static_cast<int>(chests_.size()); }
    // True when the loaded dataset has a region tree (Chest Challenge mode).
    [[nodiscard]] bool hasTree() const { return !chests_.empty() || treeMode_; }
    // True when a randomizer spoiler log with boss placements was loaded.
    [[nodiscard]] bool randoLoaded() const { return randoLog_.loaded(); }
    // True when the randomizer's enemy data (entity -> defeat flag table) was loaded.
    [[nodiscard]] bool randoEnemyDataLoaded() const { return randoLog_.enemyDataLoaded(); }
    // Path of the loaded spoiler log, for the UI tooltip.
    [[nodiscard]] const std::wstring &randoPath() const { return randoLog_.path(); }

    // Thread-safe snapshot for the render thread: copies all mutable state under mutex.
    // The caller should keep a RenderState member to reuse vector capacity across frames.
    void fillRenderState(RenderState &out);

private:
    void updateBosses();
    void updateNormalMode();
    void updateChallengeMode();
    [[nodiscard]] int readDeathCount() const;
    [[nodiscard]] int readInGameTime() const;

private:
    int toggleFullModeKey_ = 0;
    bool allowRevive_ = false;
    bool notificationsEnabled_ = true;
    bool firstUpdate_ = true;
    bool treeMode_ = false;
    std::vector<BossData> bosses_;
    std::vector<ChestData> chests_;
    std::vector<RegionData> regions_;
    std::map<uint32_t, int> regionMap_;
    RandoLog randoLog_;

    // Mutable state (protected by mutex_)
    std::mutex mutex_;
    int count_ = 0;
    int chestCount_ = 0;
    uint32_t mapId_ = 0;
    int regionIndex_ = -1;
    std::vector<uint8_t> dead_;
    std::vector<uint8_t> chestsOpened_;
    std::vector<int> regionCounts_;
    std::vector<KillNotification> notifications_;

    // Boss flag resolution
    uintptr_t gameDataMan_ = 0;
    uintptr_t fieldArea_ = 0;

    // Swap buffers (avoid per-frame heap allocation in updateBosses)
    std::vector<uint8_t> deadSwapBuf_;
    std::vector<uint8_t> chestSwapBuf_;
    std::vector<int> regionCountSwapBuf_;

    ChallengeState challenge_;
};

extern BossDataSet gBossDataSet;

} // namespace er::bosses
