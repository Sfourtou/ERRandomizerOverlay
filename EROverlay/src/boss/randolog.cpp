#include "randolog.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace er::bosses {

namespace {

// Returns true when the line starts with the given prefix at the given offset.
bool startsWith(const std::string &s, size_t offset, const char *prefix) {
    size_t len = std::char_traits<char>::length(prefix);
    return s.size() >= offset + len && s.compare(offset, len, prefix) == 0;
}

// Parses a decimal uint32 between [begin, end). Returns 0 on failure.
uint32_t parseUint(const std::string &s, size_t begin, size_t end) {
    if (begin >= end || end - begin > 10) return 0;
    uint32_t value = 0;
    for (size_t i = begin; i < end; i++) {
        auto c = s[i];
        if (c < '0' || c > '9') return 0;
        value = value * 10 + static_cast<uint32_t>(c - '0');
    }
    return value;
}

// Extracts "NAME (#ID)" at the given position. On success, sets name and id and
// returns the offset just past the closing parenthesis. Returns npos on failure.
size_t parseNameAndId(const std::string &line, size_t pos, std::string &name, uint32_t &id) {
    auto hashPos = line.find("(#", pos);
    if (hashPos == std::string::npos) return std::string::npos;
    auto closePos = line.find(')', hashPos);
    if (closePos == std::string::npos) return std::string::npos;
    id = parseUint(line, hashPos + 2, closePos);
    if (id == 0) return std::string::npos;
    auto nameBegin = pos;
    auto nameEnd = hashPos;
    while (nameEnd > nameBegin && (line[nameEnd - 1] == ' ' || line[nameEnd - 1] == '\t')) nameEnd--;
    if (nameEnd <= nameBegin) return std::string::npos;
    name = line.substr(nameBegin, nameEnd - nameBegin);
    return closePos + 1;
}

} // namespace

bool RandoLog::loadEnemyData(const std::wstring &path) {
    entityToFlag_.clear();
    enemyDataPath_.clear();
    if (path.empty()) return false;
    std::ifstream ifs(std::filesystem::path(path), std::ios::binary);
    if (!ifs) return false;

    // The file is a YAML-ish annotation file with CRLF line endings. Enemy
    // entries look like:
    //   - ID: 1052380800
    //     Map: m60_13_09_02
    //     Class: Boss
    //     DefeatFlag: 1252380800
    // Only the entity ID and DefeatFlag lines are relevant.
    std::string line;
    uint32_t currentEntity = 0;
    while (std::getline(ifs, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        // Top-level entries start with "- ID: " (two spaces would nest).
        if (startsWith(line, 0, "- ID: ")) {
            currentEntity = parseUint(line, sizeof("- ID: ") - 1, line.size());
            continue;
        }
        if (currentEntity == 0) continue;
        auto flagPos = line.find("DefeatFlag: ");
        if (flagPos == std::string::npos) continue;
        auto flag = parseUint(line, flagPos + std::char_traits<char>::length("DefeatFlag: "), line.size());
        if (flag != 0) {
            entityToFlag_[currentEntity] = flag;
            currentEntity = 0;
        }
    }
    if (entityToFlag_.empty()) return false;
    enemyDataPath_ = path;
    return true;
}

bool RandoLog::parse(const std::string &content) {
    placements_.clear();
    loaded_ = false;
    placementCount_ = 0;
    RandoSection currentSection = RandoSection::None;

    size_t pos = 0;
    // Strip a UTF-8 BOM if present.
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF && static_cast<unsigned char>(content[1]) == 0xBB && static_cast<unsigned char>(content[2]) == 0xBF) {
        pos = 3;
    }
    while (pos < content.size()) {
        auto eol = content.find('\n', pos);
        std::string line = content.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = eol == std::string::npos ? content.size() : eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (line.empty()) continue;

        if (startsWith(line, 0, "--")) {
            // Section header.
            if (line.find("Boss placements") != std::string::npos) {
                currentSection = RandoSection::Boss;
            } else if (line.find("Miniboss placements") != std::string::npos) {
                currentSection = RandoSection::Miniboss;
            } else if (line.find("Basic placements") != std::string::npos) {
                currentSection = RandoSection::Basic;
            } else {
                currentSection = RandoSection::None;
            }
            continue;
        }
        if (currentSection == RandoSection::None) continue;
        if (!startsWith(line, 0, "Replacing ")) continue;

        // Expected: Replacing <target name> (#<targetId>) in <place>: <source name> (#<sourceId>) from <place>[(scaling ...)]
        // Targets printed as "missing #..." cannot be matched and are skipped.
        std::string targetName;
        uint32_t targetId = 0;
        auto afterId = parseNameAndId(line, sizeof("Replacing ") - 1, targetName, targetId);
        if (afterId == std::string::npos || targetId == 0) continue;
        if (!startsWith(line, afterId, " in ")) continue;

        auto colonPos = line.find(": ", afterId);
        if (colonPos == std::string::npos) continue;
        size_t sourceBegin = colonPos + 2;

        std::string sourceName;
        uint32_t sourceId = 0;
        if (parseNameAndId(line, sourceBegin, sourceName, sourceId) == std::string::npos) continue;
        if (sourceName.empty()) continue;

        // Source place: text after " from ", up to an optional " (scaling" suffix.
        std::string sourcePlace;
        auto fromPos = line.find(" from ", sourceBegin);
        if (fromPos != std::string::npos) {
            auto placeBegin = fromPos + sizeof(" from ") - 1;
            auto scalePos = line.find(" (scaling", placeBegin);
            sourcePlace = line.substr(placeBegin, scalePos == std::string::npos ? std::string::npos : scalePos - placeBegin);
        }

        RandoPlacement placement;
        placement.name = std::move(sourceName);
        placement.sourcePlace = std::move(sourcePlace);
        placement.sourceId = sourceId;
        placement.targetId = targetId;
        placement.section = currentSection;

        // Key the placement by the location's defeat flag. The target entity ID
        // usually equals the flag; otherwise the randomizer's enemy data (entity
        // -> DefeatFlag, loaded before the log) resolves it. Unknown entity IDs
        // fall back to the raw entity ID. Duo/trio companions share their defeat
        // flag in the enemy data, so they merge under one key automatically.
        uint32_t flagId = targetId;
        if (!entityToFlag_.empty()) {
            auto flag = entityToFlag_.find(targetId);
            if (flag != entityToFlag_.end()) flagId = flag->second;
        }
        auto &list = placements_[flagId];
        if (list.empty()) {
            list.push_back(std::move(placement));
        } else if (static_cast<int>(placement.section) > static_cast<int>(list.front().section)) {
            // A higher-priority section (Boss > Miniboss > Basic) wins: put it first.
            list.insert(list.begin(), std::move(placement));
        } else {
            list.push_back(std::move(placement));
        }
        placementCount_++;
    }
    loaded_ = !placements_.empty();
    return loaded_;
}

const std::vector<RandoPlacement> *RandoLog::find(uint32_t flagId) const {
    auto it = placements_.find(flagId);
    return it == placements_.end() ? nullptr : &it->second;
}

bool RandoLog::load(const std::wstring &path) {
    loaded_ = false;
    path_.clear();
    placements_.clear();
    placementCount_ = 0;
    if (path.empty()) return false;
    std::ifstream ifs(std::filesystem::path(path), std::ios::binary);
    if (!ifs) return false;
    std::ostringstream ss;
    ss << ifs.rdbuf();
    if (!parse(ss.str())) return false;
    path_ = path;
    return true;
}

std::wstring RandoLog::findNewest(const std::wstring &directory) {
#if defined(_WIN32)
    if (directory.empty()) return L"";
    std::wstring pattern = directory;
    if (pattern.back() != L'\\' && pattern.back() != L'/') pattern += L'\\';
    pattern += L"*";
    WIN32_FIND_DATAW findData;
    HANDLE handle = FindFirstFileW(pattern.c_str(), &findData);
    if (handle == INVALID_HANDLE_VALUE) return L"";
    std::wstring newest;
    FILETIME newestTime{};
    do {
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = findData.cFileName;
        // Randomizer log files are named "<date>_<time>_log_<seed>_<hash>.txt".
        if (name.size() < 6 || name.compare(name.size() - 4, 4, L".txt") != 0) continue;
        if (name.find(L"_log_") == std::wstring::npos) continue;
        if (CompareFileTime(&findData.ftLastWriteTime, &newestTime) > 0) {
            newestTime = findData.ftLastWriteTime;
            newest = directory + (directory.back() == L'\\' || directory.back() == L'/' ? L"" : L"\\") + name;
        }
    } while (FindNextFileW(handle, &findData));
    FindClose(handle);
    return newest;
#else
    (void)directory;
    return L"";
#endif
}

} // namespace er::bosses
