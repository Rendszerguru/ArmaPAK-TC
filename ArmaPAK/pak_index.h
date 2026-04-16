#ifndef PAK_INDEX_H
#define PAK_INDEX_H

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <algorithm>
#include <shared_mutex>
#include <future>
#include <functional>

class PakEntry;
class ThreadPool;
extern std::unique_ptr<ThreadPool> g_ThreadPool;

class PakIndex {
private:
	mutable std::shared_mutex m_IndexMutex;

	std::unordered_map<std::string, int> m_PathToIndex;
	std::unordered_map<std::string, std::vector<int>> m_FileNameToIndex;

	inline std::string Normalize(const std::string& s) const {
		std::string res = s;
		std::transform(res.begin(), res.end(), res.begin(), ::tolower);
		std::replace(res.begin(), res.end(), '/', '\\');
		return res;
	}

	inline std::string GetFileName(const std::string& path) const {
		size_t lastSlash = path.find_last_of("\\/");
		if (lastSlash == std::string::npos) return path;
		return path.substr(lastSlash + 1);
	}

	int GetPriority(const std::string& path) const {
		if (path.find("_bcr.edds") != std::string::npos) return 1;
		if (path.find("_mcr.edds") != std::string::npos) return 2;
		if (path.find("_co.edds") != std::string::npos) return 3;
		if (path.find("_nohq.edds") != std::string::npos) return 4;
		return 10;
	}

	void BuildSerial(const std::vector<std::shared_ptr<PakEntry>>& entries) {
		m_PathToIndex.reserve(entries.size());
		m_FileNameToIndex.reserve(entries.size() / 2);

		for (int i = 0; i < (int)entries.size(); ++i) {
			if (!entries[i] || entries[i]->isDirectory) continue;

			std::string normPath = Normalize(entries[i]->name);
			std::string fName = GetFileName(normPath);

			auto it = m_PathToIndex.find(normPath);
			if (it != m_PathToIndex.end()) {
				if (GetPriority(entries[i]->name) < GetPriority(entries[it->second]->name)) {
					m_PathToIndex[normPath] = i;
				}
			} else {
				m_PathToIndex[normPath] = i;
			}
			m_FileNameToIndex[fName].push_back(i);
		}
	}

public:
	PakIndex() = default;

	void Build(const std::vector<std::shared_ptr<PakEntry>>& entries) {
		std::unique_lock<std::shared_mutex> lock(m_IndexMutex);
		m_PathToIndex.clear();
		m_FileNameToIndex.clear();

		if (entries.empty()) return;

		if (entries.size() < 1000 || !g_ThreadPool) {
			BuildSerial(entries);
			return;
		}

		size_t numEntries = entries.size();
		size_t numThreads = std::thread::hardware_concurrency();
		if (numThreads == 0) numThreads = 2;
		size_t chunkSize = (numEntries + numThreads - 1) / numThreads;

		using PartialResult = std::pair<std::unordered_map<std::string, int>, std::unordered_map<std::string, std::vector<int>>>;
		std::vector<std::future<PartialResult>> futures;

		for (size_t i = 0; i < numThreads; ++i) {
			size_t start = i * chunkSize;
			size_t end = std::min(start + chunkSize, numEntries);
			if (start >= numEntries) break;

			futures.push_back(g_ThreadPool->enqueue([this, &entries, start, end]() {
				PartialResult res;
				for (size_t j = start; j < end; ++j) {
					if (!entries[j] || entries[j]->isDirectory) continue;

					std::string normPath = Normalize(entries[j]->name);
					std::string fName = GetFileName(normPath);

					auto it = res.first.find(normPath);
					if (it != res.first.end()) {
						if (GetPriority(entries[j]->name) < GetPriority(entries[it->second]->name)) {
							res.first[normPath] = (int)j;
						}
					} else {
						res.first[normPath] = (int)j;
					}
					res.second[fName].push_back((int)j);
				}
				return res;
			}));
		}

		for (auto& f : futures) {
			PartialResult part = f.get();
			for (auto const& [path, idx] : part.first) {
				auto it = m_PathToIndex.find(path);
				if (it != m_PathToIndex.end()) {
					if (GetPriority(entries[idx]->name) < GetPriority(entries[it->second]->name)) {
						m_PathToIndex[path] = idx;
					}
				} else {
					m_PathToIndex[path] = idx;
				}
			}
			for (auto& [name, indices] : part.second) {
				auto& masterIndices = m_FileNameToIndex[name];
				masterIndices.insert(masterIndices.end(), indices.begin(), indices.end());
			}
		}
	}

	int FindBestMatch(const std::string& fileName, const std::vector<std::shared_ptr<PakEntry>>& entries) const {
		std::shared_lock<std::shared_mutex> lock(m_IndexMutex);
		if (m_PathToIndex.empty()) return -1;

		std::string searchName = Normalize(fileName);

		auto itPath = m_PathToIndex.find(searchName);
		if (itPath != m_PathToIndex.end()) return itPath->second;

		std::string justFileName = GetFileName(searchName);
		auto itFile = m_FileNameToIndex.find(justFileName);

		if (itFile != m_FileNameToIndex.end()) {
			int bestIdx = -1;
			int bestScore = 101;
			for (int idx : itFile->second) {
				int score = GetPriority(entries[idx]->name);
				if (score < bestScore) {
					bestScore = score;
					bestIdx = idx;
				}
			}
			return bestIdx;
		}
		return -1;
	}

	std::vector<int> GetRelatedEntries(const std::string& baseName) const {
		std::shared_lock<std::shared_mutex> lock(m_IndexMutex);
		std::string cleanName = baseName;
		size_t dotPos = cleanName.find_last_of('.');
		if (dotPos != std::string::npos) cleanName = cleanName.substr(0, dotPos);

		std::string normBase = Normalize(cleanName);
		std::vector<int> related;

		for (auto const& [fName, indices] : m_FileNameToIndex) {
			if (fName.find(normBase) != std::string::npos) {
				for (int idx : indices) {
					related.push_back(idx);
				}
			}
		}
		return related;
	}
};

#endif