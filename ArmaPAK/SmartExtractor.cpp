#include "SmartExtractor.h"
#include "ThreadPool.h"
#include <algorithm>
#include <filesystem>
#include <mutex>
#include <queue>
#include <future>
#include <unordered_set>
#include <windows.h>

namespace fs = std::filesystem;

class PakEntry {
public:
    enum class CompressionType : uint32_t { None = 0, Zlib = 0x106 };
    std::string name;
    uint32_t offset = 0;
    uint32_t size = 0;
    uint32_t originalSize = 0;
    CompressionType compression = CompressionType::None;
    bool isDirectory = false;
    std::vector<std::shared_ptr<PakEntry>> children;
};

class PakArchive {
public:
    const PakEntry* GetEntry(int index) const;
    int GetEntryCount() const;
    std::string GetFilename() const;
    std::vector<uint8_t> DecompressEntryData(const PakEntry* entry);
    bool ExtractFile(int index, const std::string& destPath);
    const PakEntry* FindEntryByName(const std::string& name) const;
};

extern std::unique_ptr<ThreadPool> g_ThreadPool;
extern bool g_EnableEddsConversion;
extern std::vector<PakArchive*> g_OpenedArchives;
extern std::mutex g_ArchivesMutex;
void LogInfo(const std::string& message);

bool SmartExtractor::ExtractWithDependencies(PakArchive* sourceArc, int index, const std::string& destPath, std::unordered_set<std::string>& processed) {
    struct TaskInfo {
        int entryIndex;
        std::string targetFullPath;
        PakArchive* sourceArchive;
    };

    std::vector<std::future<bool>> activeTasks;
    std::queue<TaskInfo> pendingTasks;

    const PakEntry* rootEntry = sourceArc->GetEntry(index);
    if (!rootEntry) return false;

    fs::path winDestFile(destPath);
    std::string pakPathStr = rootEntry->name;
    std::string winDestStr = winDestFile.string();
    fs::path baseExtractionDir;

    if (winDestStr.length() >= pakPathStr.length()) {
        baseExtractionDir = winDestStr.substr(0, winDestStr.length() - pakPathStr.length());
    } else {
        baseExtractionDir = winDestFile.parent_path();
    }

    pendingTasks.push({ index, destPath, sourceArc });

    while (!pendingTasks.empty() || !activeTasks.empty()) {

        while (!pendingTasks.empty()) {
            TaskInfo current = pendingTasks.front();
            pendingTasks.pop();

            const PakEntry* entry = current.sourceArchive->GetEntry(current.entryIndex);
            if (!entry || entry->isDirectory) continue;

            std::string uniqueKey = current.sourceArchive->GetFilename() + "|" + entry->name;
            if (processed.count(uniqueKey)) continue;

            if (processed.size() > 8000) {
                LogInfo("[LIMIT] Reached 8000 files, stopping dependency chain.");
                break;
            }
            processed.insert(uniqueKey);

            LogInfo("[EXTRACT] " + entry->name);

            fs::path fPath(current.targetFullPath);
            fs::create_directories(fPath.parent_path());

            std::string ext = fs::path(entry->name).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            if (ext == ".xob" || ext == ".emat") {
                try {
                    auto data = current.sourceArchive->DecompressEntryData(entry);
                    auto deps = FindDependencies(current.sourceArchive, data);

                    for (const auto& depLine : deps) {
                        LogInfo("[DEP RAW] " + depLine);

                        std::string cleanPath = depLine;

                        size_t bracePos = cleanPath.find('}');
                        if (bracePos != std::string::npos) cleanPath = cleanPath.substr(bracePos + 1);

                        auto startIdx = cleanPath.find_first_not_of(" \t\n\r");
                        auto endIdx = cleanPath.find_last_not_of(" \t\n\r");
                        if (startIdx == std::string::npos) continue;
                        cleanPath = cleanPath.substr(startIdx, endIdx - startIdx + 1);

                        std::replace(cleanPath.begin(), cleanPath.end(), '/', '\\');
                        std::transform(cleanPath.begin(), cleanPath.end(), cleanPath.begin(), ::tolower);

                        size_t assetsPos = cleanPath.find("assets\\");
                        if (assetsPos != std::string::npos) cleanPath = cleanPath.substr(assetsPos);
                        size_t commonPos = cleanPath.find("common\\");
                        if (commonPos != std::string::npos) cleanPath = cleanPath.substr(commonPos);

                        const PakEntry* depEntry = nullptr;
                        PakArchive* targetArchive = nullptr;

                        auto tryFind = [&](const std::string& p) -> const PakEntry* {
                            std::lock_guard<std::mutex> lock(g_ArchivesMutex);
                            for (auto* arc : g_OpenedArchives) {
                                if (auto* e = arc->FindEntryByName(p)) {
                                    targetArchive = arc;
                                    return e;
                                }
                            }
                            return nullptr;
                        };

                        depEntry = tryFind(cleanPath);

                        if (depEntry && targetArchive) {
                            std::string depKey = targetArchive->GetFilename() + "|" + depEntry->name;
                            if (!processed.count(depKey)) {

                                fs::path depPath(depEntry->name);

                                if (depPath.is_absolute()) {
                                    depPath = depPath.relative_path();
                                }

                                if (depPath.has_root_name()) {
                                    depPath = depPath.relative_path();
                                }

                                std::string depStr = depPath.string();
                                if (!depStr.empty() && (depStr[0] == '\\' || depStr[0] == '/')) {
                                    depPath = depPath.relative_path();
                                }

                                fs::path subDest = baseExtractionDir / depPath;

                                std::string realExt = fs::path(depEntry->name).extension().string();
                                std::transform(realExt.begin(), realExt.end(), realExt.begin(), ::tolower);

                                if (realExt == ".edds" && g_EnableEddsConversion) {
                                    subDest.replace_extension(".dds");
                                }

                                for (int i = 0; i < (int)targetArchive->GetEntryCount(); i++) {
                                    if (targetArchive->GetEntry(i)->name == depEntry->name) {
                                        pendingTasks.push({ i, subDest.string(), targetArchive });
                                        break;
                                    }
                                }
                            }
                        }
                        else {
                            LogInfo("[DEP NOT FOUND] " + cleanPath);
                        }
                    }
                }
                catch (...) {
                    LogInfo("[DEP ERROR] Failed to process dependencies for: " + entry->name);
                }
            }

            if (g_ThreadPool) {
                activeTasks.push_back(g_ThreadPool->enqueue([src = current.sourceArchive, idx = current.entryIndex, p = current.targetFullPath]() {
                    return src->ExtractFile(idx, p);
                    }));
            }
            else {
                current.sourceArchive->ExtractFile(current.entryIndex, current.targetFullPath);
            }
        }

        for (auto& fut : activeTasks) {
            if (fut.valid()) fut.get();
        }
        activeTasks.clear();
    }

    return true;
}

std::vector<std::string> SmartExtractor::FindDependencies(PakArchive* sourceArc, const std::vector<uint8_t>& data) {
    std::vector<std::string> results;
    if (data.empty()) return results;

    const std::vector<std::string> extensions = { ".emat", ".edds", ".dds", ".xob", ".gamemat" };

    for (size_t i = 0; i < data.size(); ) {
        if (data[i] >= 32 && data[i] <= 126) {
            size_t start = i;

            while (i < data.size() && data[i] >= 32 && data[i] <= 126) {
                i++;
            }

            std::string block(reinterpret_cast<const char*>(&data[start]), i - start);

            std::string lowerBlock = block;
            std::transform(lowerBlock.begin(), lowerBlock.end(), lowerBlock.begin(), ::tolower);

            size_t p = std::string::npos;
            size_t pA = lowerBlock.find("assets/");
            size_t pC = lowerBlock.find("common/");

            if (pA != std::string::npos) p = pA;
            else if (pC != std::string::npos) p = pC;

            if (p == std::string::npos)
                continue;

            std::string candidate = block.substr(p);

            size_t end = 0;
            for (; end < candidate.size(); end++) {
                unsigned char c = (unsigned char)candidate[end];

                if (!(c > 32 &&
                    c != '"' && c != '\'' &&
                    c != '<' && c != '>' &&
                    c != '{' && c != '}' &&
                    c != '(' && c != ')' &&
                    c != '[' && c != ']' &&
                    c != '|'))
                {
                    break;
                }
            }

            candidate = candidate.substr(0, end);

            auto s = candidate.find_first_not_of(" \t\r\n");
            auto e = candidate.find_last_not_of(" \t\r\n");

            if (s == std::string::npos || e == std::string::npos)
                continue;

            candidate = candidate.substr(s, e - s + 1);

            if (!candidate.empty() && candidate.front() == '"') candidate.erase(candidate.begin());
            if (!candidate.empty() && candidate.back() == '"') candidate.pop_back();

            std::replace(candidate.begin(), candidate.end(), '/', '\\');

            std::string lowerCand = candidate;
            std::transform(lowerCand.begin(), lowerCand.end(), lowerCand.begin(), ::tolower);

            bool validExt = false;
            for (const auto& ext : extensions) {
                if (lowerCand.size() >= ext.size() &&
                    lowerCand.compare(lowerCand.size() - ext.size(), ext.size(), ext) == 0)
                {
                    validExt = true;
                    break;
                }
            }

            if (!validExt)
                continue;

            if (candidate.length() >= 5 && candidate.length() <= 260) {
                if (LooksLikePath(candidate)) {

                    if (lowerCand.find("assets\\") != std::string::npos) {
                        LogInfo("[DEP PARSED] " + candidate);
                        results.push_back(candidate);
                    }
                    else {
                        LogInfo("[DEP PARSED fallback] " + candidate);
                        results.push_back(candidate);
                    }
                }
            }
        }
        else {
            i++;
        }
    }

    std::sort(results.begin(), results.end());
    results.erase(std::unique(results.begin(), results.end()), results.end());

    return results;
}

bool SmartExtractor::LooksLikePath(const std::string& s) {
    if (s.length() < 5 || s.length() > 260) return false;
    if (s.find('.') == std::string::npos) return false;
    if (s.find('\\') == std::string::npos && s.find('/') == std::string::npos) return false;

    return std::all_of(s.begin(), s.end(), [](char c) {
        return std::isprint((unsigned char)c);
        });
}