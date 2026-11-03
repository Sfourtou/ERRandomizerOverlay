#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace er::bosses {

// Randomizer spoiler log parsing.
//
// The Elden Ring item/enemy randomizer (thefifthmatt) writes a spoiler log to
// its spoiler_logs directory on every randomization. The "Boss placements" and
// "Miniboss placements" sections contain lines of the form:
//
//   Replacing <target name> (#<targetId>) in <target place>: <source name> (#<sourceId>) from <source place>[(scaling ...)]
//
// where targetId is the entity ID of the enemy slot in the world and sourceId
// identifies the enemy that the randomizer placed there.
//
// The overlay's boss data identifies each boss location by its defeat event
// flag, which usually equals the target entity ID but differs for a handful of
// bosses (festival Radahn, Night's Cavalry, phase entities, duo companions...).
// The randomizer's own data file diste/Base/enemy.txt maps every enemy entity
// ID to its DefeatFlag; parsing it lets this module key every placement by the
// defeat flag the overlay queries. Duo/trio companions (entity IDs flagId+1,
// +2) share their defeat flag in that file, so they merge automatically.

// Which placements section a mapping came from.
enum class RandoSection : int {
    None = 0,
    Basic,
    Miniboss,
    Boss,
};

struct RandoPlacement {
    std::string name;        // Name of the placed (source) enemy, e.g. "Messmer the Impaler"
    std::string sourcePlace; // Vanilla location of the placed enemy, e.g. "Shadow Keep"
    uint32_t sourceId = 0;   // Entity ID of the placed enemy
    uint32_t targetId = 0;   // Entity ID of the world slot the enemy was placed into
    RandoSection section = RandoSection::None;
};

class RandoLog {
public:
    // Parse the randomizer's enemy data (diste/Base/enemy.txt) to build the
    // entity ID -> defeat flag table. Optional but recommended; without it,
    // entity IDs are assumed to equal defeat flags. Returns true on success.
    bool loadEnemyData(const std::wstring &path);

    // Load placements from a spoiler log file. Returns true when the file was
    // read and at least one boss/miniboss placement was found.
    bool load(const std::wstring &path);

    // Load placements from an already-read log. Exposed for testing.
    bool parse(const std::string &content);

    // Returns the enemies placed at the location identified by a defeat flag,
    // or nullptr when the log has no mapping for it. The map is immutable
    // after load(), so the returned pointer stays valid for the process
    // lifetime. Several placements can share one defeat flag (duo/trio fights).
    [[nodiscard]] const std::vector<RandoPlacement> *find(uint32_t flagId) const;

    // True when enemy data (entity -> defeat flag table) was loaded.
    [[nodiscard]] bool enemyDataLoaded() const { return !entityToFlag_.empty(); }

    // True when a log was loaded successfully and has randomized placements.
    [[nodiscard]] bool loaded() const { return loaded_; }

    // Paths of the last successfully loaded files (for logging).
    [[nodiscard]] const std::wstring &path() const { return path_; }
    [[nodiscard]] const std::wstring &enemyDataPath() const { return enemyDataPath_; }

    // Number of parsed placements (all sections).
    [[nodiscard]] size_t size() const { return placementCount_; }

    // Find the most recent *_log_*.txt file in a directory. Returns an empty
    // string when the directory does not exist or contains no log.
    [[nodiscard]] static std::wstring findNewest(const std::wstring &directory);

private:
    bool loaded_ = false;
    size_t placementCount_ = 0;
    std::wstring path_;
    std::wstring enemyDataPath_;
    std::map<uint32_t, uint32_t> entityToFlag_;
    std::map<uint32_t, std::vector<RandoPlacement>> placements_;
};

} // namespace er::bosses
