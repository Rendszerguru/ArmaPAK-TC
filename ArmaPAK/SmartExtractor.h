#pragma once
#include <string>
#include <unordered_set>
#include <vector>
#include <cstdint>

class PakArchive;

class SmartExtractor {
public:
    static bool ExtractWithDependencies(PakArchive* sourceArc, int index, const std::string& destPath, std::unordered_set<std::string>& processed);

private:
    static std::vector<std::string> FindDependencies(PakArchive* sourceArc, const std::vector<uint8_t>& data);
    static bool LooksLikePath(const std::string& s);
};