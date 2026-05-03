#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#ifndef TCM_GETITEMCOUNT
#define TCM_GETITEMCOUNT (TCM_FIRST + 4)
#endif

#include <windows.h>
#include <commctrl.h>
#include <vector>
#include <string>
#include <fstream>
#include <algorithm>
#include <stdexcept>
#include <zlib.h>
#include <memory>#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#ifndef TCM_GETITEMCOUNT
#define TCM_GETITEMCOUNT (TCM_FIRST + 4)
#endif

#include <windows.h>
#include <commctrl.h>
#include <vector>
#include <string>
#include <fstream>
#include <algorithm>
#include <stdexcept>
#include <zlib.h>
#include <memory>
#include <map>
#include <ctime>
#include <assert.h>
#include <filesystem>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <cctype>
#include <unordered_set>
#include <mutex>
#include <span>
#include <queue>
#include <atomic>
#include <unordered_map>
#include <thread>
#include <condition_variable>
#include <functional>
#include <future>
#include <type_traits>
#include <tlhelp32.h>

namespace fs = std::filesystem;

namespace ImageModule {
	struct Snapshot {
		bool enabled = false;
		int format = 0;
	};

	inline void LoadSettings(const std::string&, const std::string&) {}
	inline void SaveSettings(const std::string&, const std::string&) {}
	inline Snapshot GetGlobalSnapshot() { return { false, 0 }; }
	inline Snapshot ResolveSnapshot(bool, bool) { return { false, 0 }; }
	inline void AdjustDisplayName(std::string&, bool, const Snapshot&, void*, int) {}
	inline std::filesystem::path ResolveExtractPath(const std::filesystem::path& p, const std::string&, const Snapshot&, bool, bool) { return p; }
	inline void InitSettingsUI(HWND) {}
	inline bool HandleSettingsCommand(HWND, WORD, WORD) { return false; }
	inline void UpdateSettingsUI(HWND, void*) {}
	inline bool ApplyAndRefreshIfNeeded(HWND, void*, bool) { return false; }

	inline bool SmartSave(const std::string&, const std::filesystem::path& finalPath, const std::vector<uint8_t>& data, const Snapshot&, bool) {
		std::ofstream out(finalPath, std::ios::binary);
		if (!out) return false;
		out.write(reinterpret_cast<const char*>(data.data()), data.size());
		return out.good();
	}
}

class PakEntry {
public:
	enum class CompressionType : uint32_t {
		None = 0,
		Zlib = 0x106
	};

	uint32_t timestamp = 0;
	std::string name;
	uint32_t offset = 0;
	uint32_t size = 0;
	uint32_t originalSize = 0;
	CompressionType compression = CompressionType::None;
	bool isDirectory = false;
	std::vector<std::shared_ptr<PakEntry>> children;
};

#include "ThreadPool.h"

class PakArchive;
class PakIndex;

#include "pak_index.h"
#include "SmartExtractor.h"
#include "WorkbenchModule.h"
#include "wcxhead.h"
#include "resource.h"

PakEntry g_CurrentEntryForDialog;
PakArchive* g_CurrentArchiveForDialog = nullptr;

const char* PLUGIN_VERSION_STRING = "1.2.2";
extern std::unique_ptr<ThreadPool> g_ThreadPool;

void LogError(const std::string& message);
void LogInfo(const std::string& message);

static std::ofstream debugLog;
static bool logInitialized = false;
static std::mutex g_LogMutex;
std::mutex g_CallbackMutex;
static std::mutex g_SearchTextMutex;

std::vector<PakArchive*> g_OpenedArchives;
std::mutex g_ArchivesMutex;

bool g_EnableLogInfo = false;
bool g_EnableSmartExtract = false;
bool g_KeepDirectoryStructure = true;
bool g_ShowExtractPrompt = true;
static std::wstring SearchTextW;

static std::unordered_map<std::string, std::unique_ptr<std::mutex>> g_FileWriteLocks;
static std::mutex g_FileWriteLocksMutex;

const char* const INI_KEY_SMART_EXTRACT = "EnableSmartExtract";
const char* const INI_KEY_KEEP_STRUCT = "KeepDirectoryStructure";
const char* const INI_FILE_NAME = "pak_plugin.ini";
const char* const INI_SECTION_NAME = "Settings";
const char* const INI_KEY_LOG_INFO = "EnableLogInfo";
const char* const LOG_FILE_NAME = "pak_plugin.log";

static HMODULE g_hModule = NULL;

static std::string GetPluginPath() {
	char path[MAX_PATH];
	if (g_hModule != NULL) {
		GetModuleFileNameA(g_hModule, path, MAX_PATH);
	} else {
		GetModuleFileNameA(NULL, path, MAX_PATH);
	}
	std::filesystem::path plugin_path(path);
	return plugin_path.parent_path().string();
}

static std::string GetLogPath() {
	return GetPluginPath() + "\\" + LOG_FILE_NAME;
}

static std::string GetIniPath() {
	return GetPluginPath() + "\\" + INI_FILE_NAME;
}

static void CheckLogRotation() {
	std::string path = GetLogPath();

	std::error_code ec;
	if (fs::exists(path, ec)) {
		if (fs::file_size(path, ec) > 5 * 1024 * 1024) {
			if (debugLog.is_open()) debugLog.close();

			debugLog.open(path, std::ios::out | std::ios::trunc);

			if (debugLog.is_open()) {
				debugLog << "[INFO] Log file rotated (size limit reached, cleared).\n";
				debugLog.flush();
			}
		}
	}
}

void LogError(const std::string& message) {
	std::lock_guard<std::mutex> lock(g_LogMutex);
	CheckLogRotation();

	if (logInitialized && debugLog.is_open()) {
		debugLog << "[ERROR] " << message << "\n";
		debugLog.flush();
	}
}

void LogInfo(const std::string& message) {
	if (!g_EnableLogInfo) return;
	std::lock_guard<std::mutex> lock(g_LogMutex);
	CheckLogRotation();

	if (logInitialized && debugLog.is_open()) {
		debugLog << "[INFO] " << message << "\n";
		debugLog.flush();
	}
}

static void LoadSettings() {
	std::string iniPath = GetIniPath();

	ImageModule::LoadSettings(iniPath, INI_SECTION_NAME);

	g_EnableLogInfo= GetPrivateProfileIntA(INI_SECTION_NAME, INI_KEY_LOG_INFO, 0, iniPath.c_str()) != 0;
	g_EnableSmartExtract= GetPrivateProfileIntA(INI_SECTION_NAME, INI_KEY_SMART_EXTRACT, 1, iniPath.c_str()) != 0;
	g_KeepDirectoryStructure = GetPrivateProfileIntA(INI_SECTION_NAME, INI_KEY_KEEP_STRUCT, 1, iniPath.c_str()) != 0;
	g_ShowExtractPrompt= GetPrivateProfileIntA(INI_SECTION_NAME, "ShowExtractPrompt", 1, iniPath.c_str()) != 0;
}

static void SaveSettings() {
	std::string iniPath = GetIniPath();

	ImageModule::SaveSettings(iniPath, INI_SECTION_NAME);

	WritePrivateProfileStringA(INI_SECTION_NAME, INI_KEY_LOG_INFO, g_EnableLogInfo ? "1" : "0", iniPath.c_str());
	WritePrivateProfileStringA(INI_SECTION_NAME, INI_KEY_SMART_EXTRACT, g_EnableSmartExtract ? "1" : "0", iniPath.c_str());
	WritePrivateProfileStringA(INI_SECTION_NAME, INI_KEY_KEEP_STRUCT, g_KeepDirectoryStructure ? "1" : "0", iniPath.c_str());
	WritePrivateProfileStringA(INI_SECTION_NAME, "ShowExtractPrompt", g_ShowExtractPrompt ? "1" : "0", iniPath.c_str());
}

static unsigned int SystemTimeToDosDateTime(const SYSTEMTIME& st) {
	const unsigned int year= static_cast<unsigned int>(st.wYear);
	const unsigned int month= static_cast<unsigned int>(st.wMonth);
	const unsigned int day= static_cast<unsigned int>(st.wDay);
	const unsigned int hour= static_cast<unsigned int>(st.wHour);
	const unsigned int minute = static_cast<unsigned int>(st.wMinute);
	const unsigned int second = static_cast<unsigned int>(st.wSecond);
	unsigned int dosDate = ((year - 1980) << 9) | (month << 5) | day;
	unsigned int dosTime = (hour << 11) | (minute << 5) | (second / 2);
	return (dosDate << 16) | (dosTime & 0xFFFF);
}

static std::string WStringToUTF8(const wchar_t* ws) {
	if (!ws || ws[0] == L'\0') return "";
	int len = (int)wcslen(ws);
	int size_needed = WideCharToMultiByte(CP_UTF8, 0, ws, len, nullptr, 0, nullptr, nullptr);
	if (size_needed <= 0) return "";
	std::string result(size_needed, 0);
	WideCharToMultiByte(CP_UTF8, 0, ws, len, &result[0], size_needed, nullptr, nullptr);
	return result;
}

static std::string WStringToUTF8(const std::wstring& ws) {
	return WStringToUTF8(ws.c_str());
}

static std::string WCharToUTF8(const WCHAR* ws) {
	return WStringToUTF8(ws);
}

static std::wstring UTF8ToWString(const std::string& str) {
	if (str.empty()) return L"";
	int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), NULL, 0);
	if (size_needed <= 0) return L"";
	std::wstring wstrTo(size_needed, 0);
	MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstrTo[0], size_needed);
	return wstrTo;
}

class PakArchive {
private:
	std::shared_ptr<PakEntry> root;
	std::vector<std::shared_ptr<PakEntry>> flatEntries;
	std::string filename;
	bool initialized = false;
	long long actualFileSize = 0;
	int m_OpenMode = 0;
	HANDLE hFile = INVALID_HANDLE_VALUE;

	std::atomic<int> m_CurrentIndex{0};
	std::atomic<int> m_LastIndex{-1};
	std::mutex indexMutex;
	mutable std::mutex m_FileMutex;

	std::unique_ptr<PakIndex> m_index;
	tProcessDataProc m_pProcessDataProc = nullptr;

	std::unordered_map<std::string, int> m_LookupTable;

	ImageModule::Snapshot m_ImageSnapshot;

	struct IffChunk {
		char id[4];
		uint32_t size;
		uint64_t dataStart;
		uint64_t dataEnd;
	};

	bool InternalRead(void* buffer, DWORD size) {
		DWORD bytesRead = 0;
		return ReadFile(hFile, buffer, size, &bytesRead, NULL) && bytesRead == size;
	}

	uint32_t ReadU32BE() {
		uint32_t val;
		if (!InternalRead(&val, 4)) throw std::runtime_error("Read error (U32BE)");
		return _byteswap_ulong(val);
	}

	uint32_t ReadU32LE() {
		uint32_t val;
		if (!InternalRead(&val, 4)) throw std::runtime_error("Read error (U32LE)");
		return val;
	}

	uint8_t ReadU8() {
		uint8_t val;
		if (!InternalRead(&val, 1)) throw std::runtime_error("Read error (U8)");
		return val;
	}

	std::string ReadString(uint8_t length) {
		if (length == 0) return "";
		std::string str(length, '\0');
		if (!InternalRead(&str[0], length)) throw std::runtime_error("Read error (String)");
		return str;
	}

	bool ReadNextChunk(IffChunk& chunk) {
		LARGE_INTEGER currentPos;
		currentPos.QuadPart = 0;
		if (!SetFilePointerEx(hFile, currentPos, &currentPos, FILE_CURRENT)) return false;

		if (currentPos.QuadPart + 8 > actualFileSize) return false;

		if (!InternalRead(chunk.id, 4)) return false;
		chunk.size = ReadU32BE();
		chunk.dataStart = currentPos.QuadPart + 8;
		chunk.dataEnd = chunk.dataStart + chunk.size;

		if (chunk.dataEnd > (uint64_t)actualFileSize) {
			LogError("Chunk '" + std::string(chunk.id, 4) + "' size exceeds file bounds.");
			return false;
		}
		return true;
	}

	void ProcessHeadChunk(const IffChunk& chunk) {
		LARGE_INTEGER li;
		li.QuadPart = chunk.dataEnd;
		SetFilePointerEx(hFile, li, NULL, FILE_BEGIN);
	}

	void ProcessFileChunk(const IffChunk& chunk) {
		uint8_t entryType = ReadU8();
		uint8_t nameLength = ReadU8();
		std::string name = ReadString(nameLength);

		if (entryType == 0) root->children.push_back(ReadDirectoryEntry(name));
		else root->children.push_back(ReadFileEntry(name));

		LARGE_INTEGER li;
		li.QuadPart = chunk.dataEnd;
		SetFilePointerEx(hFile, li, NULL, FILE_BEGIN);
	}

	std::shared_ptr<PakEntry> ReadDirectoryEntry(const std::string& name) {
		auto entry = std::make_shared<PakEntry>();
		entry->name = name;
		entry->isDirectory = true;

		uint32_t childCount = ReadU32LE();

		entry->timestamp = static_cast<uint32_t>(time(nullptr));

		for (uint32_t i = 0; i < childCount; i++) {
			uint8_t childType = ReadU8();
			uint8_t childNameLength = ReadU8();
			std::string childName = ReadString(childNameLength);

			if (childType == 0) {
				entry->children.push_back(ReadDirectoryEntry(childName));
			} else {
				entry->children.push_back(ReadFileEntry(childName));
			}
		}
		return entry;
	}

	std::shared_ptr<PakEntry> ReadFileEntry(const std::string& name) {
		auto entry = std::make_shared<PakEntry>();
		entry->name = name;
		entry->isDirectory = false;
		entry->offset = ReadU32LE();
		entry->size = ReadU32LE();
		entry->originalSize = ReadU32LE();

		LARGE_INTEGER li; li.QuadPart = 4;
		SetFilePointerEx(hFile, li, NULL, FILE_CURRENT);
		entry->compression = static_cast<PakEntry::CompressionType>(ReadU32BE());
		entry->timestamp = ReadU32LE();

		return entry;
	}

	void FlattenEntries(const std::shared_ptr<PakEntry>& entry, const std::string& path = "") {
		std::string fullPath = path;
		if (entry.get() != root.get()) {
			if (!fullPath.empty()) fullPath += "\\";
			fullPath += entry->name;
		}
		if (entry.get() != root.get()) {
			auto flatEntry = std::make_shared<PakEntry>(*entry);
			flatEntry->name = fullPath;
			flatEntries.push_back(flatEntry);
		}
		for (const auto& child : entry->children) FlattenEntries(child, fullPath);
	}

	std::string NormalizePath(const std::string& name) const {
		std::string result = name;
		std::replace(result.begin(), result.end(), '/', '\\');
		std::transform(result.begin(), result.end(), result.begin(), ::tolower);
		return result;
	}

public:
	void AddVirtualEntry(const std::string& name) {
		auto entry = std::make_shared<PakEntry>();
		entry->name = name;
		entry->isDirectory = false;
		entry->size = 1;
		entry->originalSize = 1;
		entry->offset = 0;
		entry->compression = PakEntry::CompressionType::None;

		entry->timestamp = static_cast<uint32_t>(time(nullptr));

		flatEntries.push_back(entry);
	}

	void BuildIndex() {
		m_LookupTable.clear();

		for (size_t i = 0; i < flatEntries.size(); ++i) {
			std::string normalizedName = NormalizePath(flatEntries[i]->name);
			m_LookupTable[normalizedName] = (int)i;
		}

		if (m_index && !flatEntries.empty()) {
			m_index->Build(flatEntries);
		}
	}

	int FindIndexByName(const std::string& name) const {
		auto it = m_LookupTable.find(NormalizePath(name));

		if (it != m_LookupTable.end()) {
			return it->second;
		}
		return -1;
	}

	std::string GetFilename() const { return filename; }

	int GetEntryIndex(const PakEntry* entry) const {
		if (!entry || flatEntries.empty()) return -1;

		for (size_t i = 0; i < flatEntries.size(); ++i) {
			if (flatEntries[i].get() == entry) return (int)i;
		}

		return -1;
	}

	const PakEntry* FindEntryByName(const std::string& name) const {
		std::string norm = name;
		std::replace(norm.begin(), norm.end(), '/', '\\');
		std::transform(norm.begin(), norm.end(), norm.begin(), ::tolower);

		LogInfo("[FindEntry] SEARCH: " + norm);

		auto tryFindExact = [&](const std::string& p) -> const PakEntry* {
			auto it = m_LookupTable.find(p);
			if (it != m_LookupTable.end()) {
				return flatEntries[it->second].get();
			}

			{
				std::lock_guard<std::mutex> lock(g_ArchivesMutex);
				for (auto* otherArchive : g_OpenedArchives) {
					if (otherArchive == this) continue;

					auto itOther = otherArchive->m_LookupTable.find(p);
					if (itOther != otherArchive->m_LookupTable.end()) {
						return otherArchive->flatEntries[itOther->second].get();
					}

					if (otherArchive->m_LookupTable.empty()) {
						for (const auto& entry : otherArchive->flatEntries) {
							std::string entryNorm = entry->name;
							std::replace(entryNorm.begin(), entryNorm.end(), '/', '\\');
							std::transform(entryNorm.begin(), entryNorm.end(), entryNorm.begin(), ::tolower);
							if (entryNorm == p) return entry.get();
						}
					}
				}
			}
			return nullptr;
		};

		if (auto* e = tryFindExact(norm)) {
			LogInfo("[FindEntry] FULL MATCH: " + norm);
			return e;
		}

		if (norm.find("assets\\") != 0 && norm.find("common\\") != 0) {
			std::string fixed = "assets\\" + norm;
			if (auto* e = tryFindExact(fixed)) {
				LogInfo("[FindEntry] FIXED (assets\\ prefix): " + fixed);
				return e;
			}
		}

		if (norm.find("common\\") == std::string::npos) {
			std::string fixed = "common\\" + norm;
			if (auto* e = tryFindExact(fixed)) {
				LogInfo("[FindEntry] FIXED (common\\ prefix): " + fixed);
				return e;
			}
		}

		LogInfo("[FindEntry] NOT FOUND: " + norm);
		return nullptr;
	}

	std::vector<uint8_t> DecompressEntryData(const PakEntry* entry) {
		if (!entry || entry->isDirectory) {
			throw std::runtime_error("Invalid or directory entry for decompression.");
		}

		if (entry->size == 0) {
			return {};
		}

		std::lock_guard<std::mutex> readLock(m_FileMutex);

		LARGE_INTEGER li;
		li.QuadPart = entry->offset;
		if (!SetFilePointerEx(hFile, li, NULL, FILE_BEGIN)) {
			throw std::runtime_error("Failed to seek to entry: " + entry->name);
		}

		uint64_t endPos = static_cast<uint64_t>(entry->offset) + entry->size;
		if (endPos > static_cast<uint64_t>(actualFileSize)) {
			LogError("[DecompressEntryData] Entry data goes beyond archive bounds: " + entry->name);
			throw std::runtime_error("Entry data out of bounds.");
		}

		if (entry->size > 1024 * 1024 * 1024) {
			LogError("[DecompressEntryData] Compressed entry too large (over 1GB): " + entry->name);
			throw std::runtime_error("Compressed entry too large");
		}

		std::vector<uint8_t> rawBuffer(entry->size);
		DWORD read = 0;
		if (!ReadFile(hFile, rawBuffer.data(), entry->size, &read, NULL) || read != entry->size) {
			LogError("[DecompressEntryData] Read failed for " + entry->name);
			throw std::runtime_error("Read failed.");
		}

		if (entry->compression == PakEntry::CompressionType::Zlib) {
			if (entry->originalSize == 0) {
				LogError("[DecompressEntryData] originalSize is 0 for compressed entry: " + entry->name);
				throw std::runtime_error("Invalid original size");
			}

			double ratio = (entry->size > 0) ? (static_cast<double>(entry->originalSize) / entry->size) : 0;

			if (entry->originalSize > 1024 * 1024 * 1024 || ratio > 1000.0) {
				LogError("[DecompressEntryData] Compression bomb detected (Ratio: " + std::to_string(ratio) + "): " + entry->name);
				throw std::runtime_error("Uncompressed size too large or suspicious ratio");
			}

			std::vector<uint8_t> processedContent(entry->originalSize);
			uLongf destLen = entry->originalSize;
			int zResult = uncompress(
				reinterpret_cast<Bytef*>(processedContent.data()), &destLen,
				reinterpret_cast<const Bytef*>(rawBuffer.data()), (uLong)entry->size);

			if (zResult != Z_OK || destLen != entry->originalSize) {
				LogError("[DecompressEntryData] Zlib error code: " + std::to_string(zResult) + " for " + entry->name);
				throw std::runtime_error("Zlib decompression failed.");
			}
			return processedContent;
		}

		return rawBuffer;
	}

	PakArchive(const std::string& filename) : filename(filename) {
		try {
			m_ImageSnapshot = ImageModule::GetGlobalSnapshot();

			hFile = CreateFileA(filename.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			if (hFile == INVALID_HANDLE_VALUE) {
				LogError("Failed to open PAK file (WinAPI): " + filename);
				throw std::runtime_error("Failed to open PAK file");
			}

			std::lock_guard<std::mutex> lock(m_FileMutex);

			LARGE_INTEGER fileSize;
			if (!GetFileSizeEx(hFile, &fileSize)) throw std::runtime_error("Failed to get file size");
			actualFileSize = fileSize.QuadPart;

			char formSig[4];
			if (!InternalRead(formSig, 4) || memcmp(formSig, "FORM", 4) != 0) {
				LogError("PAK file does not start with 'FORM' signature: " + filename);
				throw std::runtime_error("Invalid PAK signature.");
			}

			uint32_t formSize = ReadU32BE();
			uint64_t expectedTotalSize = 8 + (uint64_t)formSize;
			if (actualFileSize != static_cast<long long>(expectedTotalSize)) {
				LogError("Archive header size mismatch. Expected: " + std::to_string(expectedTotalSize));
				throw std::runtime_error("Archive size mismatch.");
			}

			char pac1Sig[4];
			if (!InternalRead(pac1Sig, 4) || memcmp(pac1Sig, "PAC1", 4) != 0) {
				LogError("PAK file FORM chunk type is not 'PAC1'.");
				throw std::runtime_error("Invalid PAK type.");
			}

			root = std::make_shared<PakEntry>();
			root->name = "";
			root->isDirectory = true;

			while (true) {
				LARGE_INTEGER cur; cur.QuadPart = 0;
				SetFilePointerEx(hFile, cur, &cur, FILE_CURRENT);
				if ((long long)cur.QuadPart >= actualFileSize) break;

				IffChunk chunk;
				if (!ReadNextChunk(chunk)) break;

				if (strncmp(chunk.id, "HEAD", 4) == 0) ProcessHeadChunk(chunk);
				else if (strncmp(chunk.id, "FILE", 4) == 0) ProcessFileChunk(chunk);
				else if (strncmp(chunk.id, "DATA", 4) == 0) {
					LARGE_INTEGER li; li.QuadPart = chunk.dataEnd;
					SetFilePointerEx(hFile, li, NULL, FILE_BEGIN);
				}
				else {
					LogInfo("Skipping unknown chunk: " + std::string(chunk.id, 4));
					LARGE_INTEGER li; li.QuadPart = chunk.dataEnd;
					SetFilePointerEx(hFile, li, NULL, FILE_BEGIN);
				}
			}
			if (root) FlattenEntries(root);

			m_index = std::make_unique<PakIndex>();
			m_index->Build(flatEntries);

			{
				std::lock_guard<std::mutex> lock(g_ArchivesMutex);
				g_OpenedArchives.push_back(this);
			}

			ResetIndex();

			initialized = true;
		} catch (const std::exception& ex) {
			LogError("PakArchive construction EXCEPTION: " + std::string(ex.what()));
			initialized = false;
		}
	}

	~PakArchive() {
		{
			std::lock_guard<std::mutex> lock(g_ArchivesMutex);
			auto it = std::find(g_OpenedArchives.begin(), g_OpenedArchives.end(), this);
			if (it != g_OpenedArchives.end()) g_OpenedArchives.erase(it);
		}

		if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
	}

	bool IsInitialized() const { return initialized; }
	int GetEntryCount() const { return static_cast<int>(flatEntries.size()); }

	const PakEntry* GetEntry(int index) const {
		if (index < 0 || index >= static_cast<int>(flatEntries.size())) return nullptr;
		return flatEntries[index].get();
	}

	int GetAndIncrementIndex() {
		return m_CurrentIndex.fetch_add(1, std::memory_order_relaxed);
	}

	int GetLastProcessedIndex() {
		return m_CurrentIndex.load(std::memory_order_relaxed) - 1;
	}

	void SetLastIndex(int idx) {
		m_LastIndex.store(idx, std::memory_order_relaxed);
	}

	int GetLastIndex() const {
		return m_LastIndex.load(std::memory_order_relaxed);
	}

	void ResetIndex() {
		std::lock_guard<std::mutex> lock(indexMutex);
		m_CurrentIndex.store(0, std::memory_order_relaxed);
		m_LastIndex.store(-1, std::memory_order_relaxed);
	}

	ImageModule::Snapshot GetImageSnapshot() const { return m_ImageSnapshot; }
	void SetImageSnapshot(const ImageModule::Snapshot& s) { m_ImageSnapshot = s; }

	void SetProcessDataProc(tProcessDataProc p) { m_pProcessDataProc = p; }
	tProcessDataProc GetProcessDataProc() const { return m_pProcessDataProc; }

	void SetOpenMode(int mode) { m_OpenMode = mode; }
	int GetOpenMode() const { return m_OpenMode; }

	static fs::path BuildFinalPath(const std::string& base, const std::string& entryName) {
		fs::path basePath = base.empty()
			? fs::current_path()
			: fs::absolute(fs::path(base));

		if (basePath.has_filename() && basePath.extension() != "") {
			basePath = basePath.parent_path();
		}

		std::string safePath = g_KeepDirectoryStructure
			? entryName
			: fs::path(entryName).filename().string();

		size_t drivePos = safePath.find(':');
		if (drivePos != std::string::npos) {
			safePath = safePath.substr(drivePos + 1);
		}

		while (!safePath.empty() && (safePath[0] == '\\' || safePath[0] == '/')) {
			safePath.erase(0, 1);
		}

		std::replace(safePath.begin(), safePath.end(), '/', '\\');

		fs::path finalPath = basePath / fs::path(safePath);

		return fs::absolute(finalPath).lexically_normal();
	}

	static inline std::string PathToLog(const fs::path& p) {
		auto u8 = p.u8string();
		return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
	}

	static bool DecompressEntryFast(PakArchive* arc, const PakEntry* entry, std::vector<uint8_t>& out) {
		static std::mutex g_DecompressMutex;
		std::lock_guard<std::mutex> lock(g_DecompressMutex);

		out = arc->DecompressEntryData(entry);

		if (out.empty() && entry->originalSize > 0) {
			LogError("[ExtractFile] Decompression failed: " + entry->name);
			return false;
		}
		return true;
	}

	static fs::path ResolveTargetPath(const std::string& destPath, const PakEntry* entry, bool& isDirectFileTarget) {
		fs::path input(destPath);
		isDirectFileTarget = input.has_filename() && !input.extension().empty();

		if (isDirectFileTarget) {
			LogInfo("[ExtractFile][DEBUG] Direct Target: " + PathToLog(input));
			return input;
		}

		fs::path result = PakArchive::BuildFinalPath(destPath, entry->name);
		LogInfo("[ExtractFile][DEBUG] Directory Target: " + PathToLog(result));
		return result;
	}

	static inline bool EnsureDirFast(const fs::path& path) {
		auto dir = path.parent_path();
		if (dir.empty()) return true;

		std::error_code ec;
		if (!fs::exists(dir) && !fs::create_directories(dir, ec) && ec) {
			LogError("[ExtractFile] Dir create failed: " + PathToLog(dir));
			return false;
		}
		return true;
	}

	static bool ReportProgressFast(PakArchive* arc, const PakEntry* entry) {
		auto cb = arc->GetProcessDataProc();
		if (!cb) return true;

		const size_t CHUNK = 16384;
		size_t total = 0;

		if (entry->originalSize == 0) {
			std::lock_guard<std::mutex> lock(g_CallbackMutex);
			cb(const_cast<char*>(entry->name.c_str()), 0);
			return true;
		}

		while (total < (size_t)entry->originalSize) {
			size_t step = std::min(CHUNK, (size_t)entry->originalSize - total);

			int res = 0;
			{
				std::lock_guard<std::mutex> lock(g_CallbackMutex);
				res = cb(const_cast<char*>(entry->name.c_str()), (int)step);
			}

			if (res == 0) return false;
			total += step;
		}

		return true;
	}

	bool ExtractFile(int index, const std::string& destPath) {
		const PakEntry* entry = GetEntry(index);

		if (!entry || entry->isDirectory) {
			LogError("[ExtractFile] Invalid entry: " + std::to_string(index));
			return false;
		}

		try {
			bool isDirect = false;
			fs::path finalPath = ResolveTargetPath(destPath, entry, isDirect);

			if (!EnsureDirFast(finalPath)) return false;

			std::vector<uint8_t> data;
			if (!DecompressEntryFast(this, entry, data)) return false;

			bool ok = ImageModule::SmartSave(entry->name, finalPath, data, m_ImageSnapshot, isDirect);

			if (!ok) {
				LogError("[ExtractFile] Write/Convert failed: " + PathToLog(finalPath));
				return false;
			}

			if (!ReportProgressFast(this, entry)) {
				LogInfo("[ExtractFile] Aborted by user");
				return false;
			}

			return true;
		}
		catch (const std::exception& ex) {
			LogError("[ExtractFile] EXCEPTION: " + std::string(ex.what()));
			return false;
		}
		catch (...) {
			LogError("[ExtractFile] Unknown EXCEPTION");
			return false;
		}
	}
};

inline std::string ws2s(const std::wstring& wstr)
{
	if (wstr.empty()) return std::string();
	int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), NULL, 0, NULL, NULL);
	std::string strTo(size_needed, 0);
	WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
	return strTo;
}

static PakArchive* GetArchive(HANDLE hArcData) {
	return reinterpret_cast<PakArchive*>(hArcData);
}

std::unique_ptr<ThreadPool> g_ThreadPool = nullptr;

static std::string g_LastOpenedArcName = "";
static std::wstring g_LastTargetDir = L"";
static bool g_ExtractOptionsShown = false;
static std::atomic<bool> g_RequireReload{ false };
static FILETIME g_OriginalArchiveTime = { 0 };
static std::string g_OriginalArchivePath = "";
static std::chrono::steady_clock::time_point g_LastOperationEndTime = std::chrono::steady_clock::now();

static std::string g_LastClosedListArcName = "";
static std::chrono::steady_clock::time_point g_LastListCloseTime;
static std::mutex g_LastCloseMutex;

// ============================================================================
// OPENARCHIVE
// ============================================================================
template <typename T>
static HANDLE OpenArchiveInternal(const std::string& arcName, T* ArchiveData) {
	std::unique_ptr<PakArchive> newArchive;
	try {
		auto now = std::chrono::steady_clock::now();
		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_LastOperationEndTime).count();

		if (elapsed > 1000) {
			g_ExtractOptionsShown = false;
		}

		bool isSameFile = (arcName == g_LastOpenedArcName);
		bool forceReload = g_RequireReload.load();

		if (forceReload && !g_OriginalArchivePath.empty()) {
			HANDLE hRestore = CreateFileA(g_OriginalArchivePath.c_str(), FILE_WRITE_ATTRIBUTES,
										 FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
			if (hRestore != INVALID_HANDLE_VALUE) {
				if (SetFileTime(hRestore, NULL, NULL, &g_OriginalArchiveTime)) {
					LogInfo("[OpenArchive] Timestamp RESTORED to original after reload: " + g_OriginalArchivePath);
				} else {
					LogError("[OpenArchive] Failed to restore timestamp in Open: " + std::to_string(GetLastError()));
				}
				CloseHandle(hRestore);
			}
			g_OriginalArchiveTime = { 0 };
			g_OriginalArchivePath = "";
		}

		if (forceReload) {
			LogInfo("[OpenArchive] FORCE RELOAD triggered for: " + arcName);
		}

		LogInfo("[OpenArchive] Opening archive: " + arcName);
		newArchive = std::make_unique<PakArchive>(arcName);

		if (!newArchive->IsInitialized()) {
			LogError("[OpenArchive] Initialization failed for: " + arcName);
			ArchiveData->OpenResult = E_EOPEN;
			return nullptr;
		}

		newArchive->SetOpenMode(ArchiveData->OpenMode);
		LogInfo("[OpenArchive] OpenMode: " + std::to_string(ArchiveData->OpenMode));

		ImageModule::Snapshot currentSnapshot = ImageModule::ResolveSnapshot(isSameFile, forceReload);

		if (forceReload) {
			g_RequireReload = false;
		}

		newArchive->SetImageSnapshot(currentSnapshot);

		LogInfo("[OpenArchive] Snapshot applied. Conv: " +
			std::to_string(currentSnapshot.enabled) +
			" Format: " + std::to_string(static_cast<int>(currentSnapshot.format)));

		g_LastOpenedArcName = arcName;

		LogInfo("[OpenArchive] Adding virtual entry: pak_plugin.ini");
		newArchive->AddVirtualEntry("pak_plugin.ini");

		newArchive->BuildIndex();
		newArchive->ResetIndex();

		ArchiveData->OpenResult = 0;

		LogInfo("Successfully opened archive: " + arcName + " (with virtual plugin entry)");

		LogInfo("[OpenArchive] instance=" + std::to_string((uintptr_t)newArchive.get()));

		return reinterpret_cast<HANDLE>(newArchive.release());
	}
	catch (const std::exception& ex) {
		LogError("[OpenArchive] EXCEPTION: " + std::string(ex.what()));
		ArchiveData->OpenResult = E_EOPEN;
		return nullptr;
	}
	catch (...) {
		LogError("[OpenArchive] Unknown EXCEPTION caught.");
		ArchiveData->OpenResult = E_EOPEN;
		return nullptr;
	}
}

HANDLE __stdcall OpenArchive(tOpenArchiveData* ArchiveData) {
	if (!ArchiveData || !ArchiveData->ArcName) {
		LogError("[OpenArchive] Invalid archive data or missing filename.");
		if (ArchiveData) ArchiveData->OpenResult = E_BAD_ARCHIVE;
		return nullptr;
	}

	return OpenArchiveInternal<tOpenArchiveData>(
		std::string(ArchiveData->ArcName),
		ArchiveData
	);
}

HANDLE __stdcall OpenArchiveW(tOpenArchiveDataW* ArchiveDataW) {
	if (!ArchiveDataW || !ArchiveDataW->ArcName) {
		LogError("[OpenArchiveW] Invalid archive data or missing filename.");
		if (ArchiveDataW) ArchiveDataW->OpenResult = E_BAD_ARCHIVE;
		return nullptr;
	}

	return OpenArchiveInternal<tOpenArchiveDataW>(
		WCharToUTF8(ArchiveDataW->ArcName),
		ArchiveDataW
	);
}

enum class HeaderResult {
	OK,
	END,
	BAD_ARCHIVE,
	BAD_DATA
};

struct PreparedHeader {
	const PakEntry* entry = nullptr;
	std::string displayName;
	uint32_t dosTime = 0;
};

static HeaderResult PrepareHeaderData(HANDLE hArcData, PreparedHeader& out) {
	PakArchive* arc = GetArchive(hArcData);
	if (!arc) return HeaderResult::BAD_ARCHIVE;

	int idx = arc->GetAndIncrementIndex();
	if (idx >= arc->GetEntryCount()) return HeaderResult::END;

	arc->SetLastIndex(idx);

	out.entry = arc->GetEntry(idx);
	if (!out.entry) return HeaderResult::BAD_DATA;

	out.displayName = out.entry->name;

	ImageModule::AdjustDisplayName(out.displayName, out.entry->isDirectory, arc->GetImageSnapshot(), arc, idx);

	if (out.entry->timestamp != 0) {
		FILETIME ft;
		LONGLONG ll = Int32x32To64(out.entry->timestamp, 10000000) + 116444736000000000;
		ft.dwLowDateTime = (DWORD)ll;
		ft.dwHighDateTime = (DWORD)(ll >> 32);

		SYSTEMTIME st;
		if (FileTimeToSystemTime(&ft, &st)) {
			out.dosTime = SystemTimeToDosDateTime(st);
		} else {
			out.dosTime = 0;
		}
	} else {
		out.dosTime = 0;
	}

	return HeaderResult::OK;
}

static int MapHeaderResult(HeaderResult res) {
	switch (res) {
		case HeaderResult::END:         return E_END_ARCHIVE;
		case HeaderResult::BAD_ARCHIVE: return E_BAD_ARCHIVE;
		case HeaderResult::BAD_DATA:    return E_BAD_DATA;
		default:                        return 0;
	}
}

template<typename T>
void FillCommon(T& h, const PreparedHeader& ph) {
	h.PackSize = ph.entry->isDirectory ? 0 : (uint32_t)ph.entry->size;
	h.UnpSize  = ph.entry->isDirectory ? 0 : (uint32_t)ph.entry->originalSize;
	h.FileAttr = ph.entry->isDirectory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
	h.FileTime = ph.dosTime;
}

int __stdcall ReadHeader(HANDLE hArcData, tHeaderData* h) {
	PreparedHeader ph;
	HeaderResult res = PrepareHeaderData(hArcData, ph);
	if (res != HeaderResult::OK) return MapHeaderResult(res);

	*h = {};
	FillCommon(*h, ph);

	strncpy_s(h->FileName, _countof(h->FileName), ph.displayName.c_str(), _TRUNCATE);

	return 0;
}

int __stdcall ReadHeaderEx(HANDLE hArcData, tHeaderDataEx* h) {
	PreparedHeader ph;
	HeaderResult res = PrepareHeaderData(hArcData, ph);
	if (res != HeaderResult::OK) return MapHeaderResult(res);

	*h = {};
	FillCommon(*h, ph);

	strncpy_s(h->FileName, _countof(h->FileName), ph.displayName.c_str(), _TRUNCATE);

	return 0;
}

int __stdcall ReadHeaderExW(HANDLE hArcData, tHeaderDataExW* h) {
	PreparedHeader ph;
	HeaderResult res = PrepareHeaderData(hArcData, ph);
	if (res != HeaderResult::OK) return MapHeaderResult(res);

	*h = {};
	FillCommon(*h, ph);

	int len = MultiByteToWideChar(
		CP_UTF8, 0,
		ph.displayName.c_str(),
		-1,
		h->FileName,
		_countof(h->FileName)
	);

	if (len == 0) {
		h->FileName[0] = L'\0';
	}

	return 0;
}

int __stdcall ProcessFile(HANDLE hArcData, int Operation, char* DestPath, char* DestName)
{
	wchar_t wideDestPath[MAX_PATH] = { 0 };
	wchar_t wideDestName[MAX_PATH] = { 0 };

	if (DestPath)
		MultiByteToWideChar(CP_ACP, 0, DestPath, -1, wideDestPath, MAX_PATH);
	if (DestName)
		MultiByteToWideChar(CP_ACP, 0, DestName, -1, wideDestName, MAX_PATH);

	return ProcessFileW(hArcData, Operation,
		DestPath ? wideDestPath : nullptr,
		DestName ? wideDestName : nullptr);
}

static INT_PTR CALLBACK SettingsDialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);

// ============================
// Viewer / Temp check
// ============================
static bool IsViewerRequest(const std::wstring& name) {
	std::wstring lower = name;
	std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
	return lower.find(L"\\_tc\\") != std::wstring::npos ||
		   lower.find(L"/_tc/") != std::wstring::npos;
}

static bool IsSilentOperation(const wchar_t* destPath, const wchar_t* destName) {
	std::wstring fullPath;
	if (destPath) fullPath += destPath;
	if (destName) {
		if (!fullPath.empty() && fullPath.back() != L'\\' && fullPath.back() != L'/') fullPath += L"\\";
		fullPath += destName;
	}

	if (fullPath.empty()) return false;

	std::wstring lower = fullPath;
	std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

	if (lower.find(L"\\_tc\\") != std::wstring::npos || lower.find(L"/_tc/") != std::wstring::npos) return true;
	if (lower.find(L"\\temp\\") != std::wstring::npos || lower.find(L"/temp/") != std::wstring::npos) return true;
	if (lower.find(L"\\appdata\\local\\temp") != std::wstring::npos) return true;

	wchar_t systemTemp[MAX_PATH];
	if (GetTempPathW(MAX_PATH, systemTemp)) {
		std::wstring sTemp = systemTemp;
		std::transform(sTemp.begin(), sTemp.end(), sTemp.begin(), ::tolower);

		if (!sTemp.empty() && (sTemp.back() == L'\\' || sTemp.back() == L'/')) {
			sTemp.pop_back();
		}

		if (!sTemp.empty() && lower.find(sTemp) != std::wstring::npos) {
			return true;
		}
	}

	std::lock_guard<std::mutex> lock(g_SearchTextMutex);
	if (!SearchTextW.empty()) return true;

	return false;
}

// ============================
// Folder copy detection
// ============================
static bool DetectFolderCopy(const std::wstring& wDestName, const PakEntry* entry, bool isSilent) {
	if (wDestName.empty() || isSilent) return false;

	fs::path pDest(wDestName);
	fs::path pInternal(UTF8ToWString(entry->name));

	if (pDest.has_parent_path() && pInternal.has_parent_path()) {
		std::wstring destLast = pDest.parent_path().filename().wstring();
		std::wstring intLast  = pInternal.parent_path().filename().wstring();

		std::transform(destLast.begin(), destLast.end(), destLast.begin(), ::tolower);
		std::transform(intLast.begin(), intLast.end(), intLast.begin(), ::tolower);

		if (!destLast.empty() && destLast == intLast) {
			LogInfo("[ProcessFileW][DEBUG] Folder copy detected (Parent match: " + ws2s(destLast) + ")");
			return true;
		}
	}
	return false;
}

// ============================================================================
// 🔥 SETTINGS HANDLING
// ============================================================================
static int HandleSettingsFile(PakArchive* currentArchive, const PakEntry* entry, const wchar_t* DestName) {
	LogInfo("[ProcessFileW] Settings file triggered via viewer.");
	g_CurrentArchiveForDialog = currentArchive;

	INT_PTR settingsResult = DialogBoxParam(g_hModule, MAKEINTRESOURCE(IDD_ARMAPAK_SETTINGS), NULL, SettingsDialogProc, 0);

	if (settingsResult == IDOK) {
		if (DestName) {
			HANDLE hFile = CreateFileW(DestName, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
			if (hFile != INVALID_HANDLE_VALUE) {
				std::string msg = "Settings applied successfully.";
				DWORD written = 0;
				WriteFile(hFile, msg.c_str(), (DWORD)msg.length(), &written, NULL);
				CloseHandle(hFile);
			}
		}
		return 0;
	}
	return E_EABORTED;
}

// ============================================================================
// 🛠️ WORKBENCH LAUNCH
// ============================================================================
static int HandleWorkbenchLaunch(PakArchive* arc) {
	LogInfo("[ProcessFileW] Workbench launch triggered.");

	ImageModule::Snapshot originalSnapshot = arc->GetImageSnapshot();

	ImageModule::Snapshot workbenchSnap = originalSnapshot;
	workbenchSnap.enabled = false;
	arc->SetImageSnapshot(workbenchSnap);

	static std::atomic<uint64_t> counter{0};
	auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	DWORD pid = GetCurrentProcessId();
	uint64_t localCounter = counter.fetch_add(1, std::memory_order_relaxed);

	std::string tempBase = "C:\\Temp\\ArmaPak_Preview_" +
		std::to_string(pid) + "_" +
		std::to_string(now) + "_" +
		std::to_string(localCounter);

	try {
		fs::create_directories(tempBase);
		LogInfo("[Workbench] Using temp: " + tempBase);
	} catch (...) {
		LogError("[Workbench] Failed to create temp directory: " + tempBase);
		arc->SetImageSnapshot(originalSnapshot);
		return -1;
	}

	std::unordered_set<std::string> processed;
	std::string finalPath = (fs::path(tempBase) / g_CurrentEntryForDialog.name).string();

	bool ok = SmartExtractor::ExtractWithDependencies(
		arc,
		arc->GetLastIndex(),
		finalPath,
		processed
	);

	arc->SetImageSnapshot(originalSnapshot);

	if (ok) {
		WorkbenchModule::OpenInWorkbench(g_CurrentEntryForDialog.name, tempBase);

		std::thread([tempBase]() {
			LogInfo("[Cleanup] Waiting before deletion: " + tempBase);
			std::this_thread::sleep_for(std::chrono::seconds(10));

			int safetyCounter = 0;
			while (WorkbenchModule::IsWorkbenchRunning()) {
				std::this_thread::sleep_for(std::chrono::seconds(2));
				if (++safetyCounter > 300) {
					LogInfo("[Cleanup] Safety timeout reached, forcing cleanup.");
					break;
				}
			}

			std::this_thread::sleep_for(std::chrono::seconds(1));

			try {
				if (fs::exists(tempBase)) {
					fs::remove_all(tempBase);
					LogInfo("[Cleanup] Deleted temp: " + tempBase);
				}
			} catch (...) {
				LogError("[Cleanup] Failed to delete temp: " + tempBase);
			}
		}).detach();
	} else {
		LogError("[Workbench] Extraction failed for: " + g_CurrentEntryForDialog.name);
	}

	return 0;
}

// ============================
// TEST
// ============================
static int HandleTest(PakArchive* arc, const PakEntry* entry) {
	if (entry->isDirectory) return 0;

	std::vector<uint8_t> data = arc->DecompressEntryData(entry);

	auto cb = arc->GetProcessDataProc();
	if (!cb) return 0;

	const size_t CHUNK = 16384;

	if (entry->originalSize == 0) {
		std::lock_guard<std::mutex> lock(g_CallbackMutex);
		cb(const_cast<char*>(entry->name.c_str()), 0);
		return 0;
	}

	size_t pos = 0;
	while (pos < data.size()) {
		size_t chunkSize = std::min(CHUNK, data.size() - pos);

		int result;
		{
			std::lock_guard<std::mutex> lock(g_CallbackMutex);
			result = cb(const_cast<char*>(entry->name.c_str()), (int)chunkSize);
		}

		if (result == 0) {
			LogError("[ProcessFileW] PK_TEST aborted.");
			return E_EABORTED;
		}

		pos += chunkSize;
	}

	return 0;
}

// ============================
// EXTRACT
// ============================
static int HandleExtract(
	PakArchive* arc,
	int entryIndex,
	const PakEntry* entry,
	const std::wstring& wDestPath,
	const std::wstring& wDestName,
	bool isSilent
) {
	if (entry && entry->name == "pak_plugin.ini") {
		return 0;
	}

	bool isFolderCopy = DetectFolderCopy(wDestName, entry, isSilent);

	fs::path fullTargetPath;
	if (!wDestName.empty()) fullTargetPath = fs::path(wDestName);
	else if (!wDestPath.empty()) fullTargetPath = fs::path(wDestPath);
	else fullTargetPath = fs::current_path();

	fullTargetPath = fullTargetPath.lexically_normal();

	bool forceOriginalName = isSilent;

	fullTargetPath = ImageModule::ResolveExtractPath(
		fullTargetPath,
		entry->name,
		arc->GetImageSnapshot(),
		forceOriginalName,
		isFolderCopy
	);

	fs::path basePath = fullTargetPath;

	if (isFolderCopy || (basePath.has_filename() && basePath.has_extension())) {
		basePath = basePath.parent_path();
	}

	auto u8str = basePath.u8string();
	std::string baseDest(reinterpret_cast<const char*>(u8str.c_str()));

	if (entry->isDirectory) {
		fs::path dirPath = isFolderCopy ? fullTargetPath :
						   fs::path(PakArchive::BuildFinalPath(baseDest, entry->name));
		try {
			fs::create_directories(dirPath);
			return 0;
		} catch (...) {
			auto u8err = dirPath.u8string();
			LogError("[HandleExtract] Failed to create dir: " + std::string(reinterpret_cast<const char*>(u8err.c_str())));
			return E_EWRITE;
		}
	}

	bool success = false;
	std::string finalPath;

	if (isSilent || isFolderCopy) {
		auto u8dir = fullTargetPath.u8string();
		std::string direct(reinterpret_cast<const char*>(u8dir.c_str()));

		success = arc->ExtractFile(entryIndex, direct);
		finalPath = direct;
	}
	else {
		if (g_EnableSmartExtract) {
			std::unordered_set<std::string> processed;
			auto u8final = PakArchive::BuildFinalPath(baseDest, entry->name).u8string();
			finalPath = std::string(reinterpret_cast<const char*>(u8final.c_str()));
			success = SmartExtractor::ExtractWithDependencies(arc, entryIndex, finalPath, processed);
		}

		if (!success) {
			success = arc->ExtractFile(entryIndex, baseDest);
			auto u8final = PakArchive::BuildFinalPath(baseDest, entry->name).u8string();
			finalPath = std::string(reinterpret_cast<const char*>(u8final.c_str()));
		}
	}

	if (!success) {
		LogError("[HandleExtract] Extraction failed for: " + entry->name);
		return E_EWRITE;
	}

	return 0;
}

// ============================================================================
// 🔥 PROCESSFILEW
// ============================================================================
int __stdcall ProcessFileW(HANDLE hArcData, int Operation, const wchar_t* DestPath, const wchar_t* DestName)
{
	PakArchive* arc = GetArchive(hArcData);

	try {
		if (!arc) return E_BAD_ARCHIVE;

		int idx = arc->GetLastIndex();
		const PakEntry* entry = arc->GetEntry(idx);

		std::wstring currentDestPath = DestPath ? DestPath : L"";
		std::wstring wDestName = DestName ? DestName : L"";

		bool isSilent = IsSilentOperation(DestPath, DestName);

		bool isExternalOperation = false;
		if (arc->GetOpenMode() == 1) {
			std::lock_guard<std::mutex> lock(g_LastCloseMutex);
			auto now = std::chrono::steady_clock::now();
			auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_LastListCloseTime).count();

			if (g_LastClosedListArcName == arc->GetFilename() && elapsed < 2000) {
				isExternalOperation = true;
				LogInfo("[ProcessFileW] Alt+F9 detected (recent list session found).");
			} else {
				isExternalOperation = false;
				LogInfo("[ProcessFileW] F5 detected (no recent list session).");
			}
		}

		std::wstring fullPathCheck = currentDestPath;
		if (!wDestName.empty()) {
			if (!fullPathCheck.empty() && fullPathCheck.back() != L'\\' && fullPathCheck.back() != L'/') fullPathCheck += L"\\";
			fullPathCheck += wDestName;
		}
		bool isViewer = IsViewerRequest(fullPathCheck);

		if (entry && entry->name == "pak_plugin.ini") {
			if (Operation == PK_EXTRACT) {
				bool isSearchActive = false;
				{
					std::lock_guard<std::mutex> lock(g_SearchTextMutex);
					isSearchActive = !SearchTextW.empty();
				}

				if (isViewer && !isSearchActive) {
					return HandleSettingsFile(arc, entry, DestName);
				}
				else {
					LogInfo("[ProcessFileW] Skipping virtual file: pak_plugin.ini (not a viewer request)");
					return 0;
				}
			}
		}

		if (Operation == PK_EXTRACT) {
			if (!isSilent && !isExternalOperation) {
				if (currentDestPath != g_LastTargetDir) {
					g_ExtractOptionsShown = false;
					g_LastTargetDir = currentDestPath;
				}

				if (!g_ExtractOptionsShown) {
					if (g_ShowExtractPrompt) {
						g_CurrentArchiveForDialog = arc;
						if (entry) g_CurrentEntryForDialog = *entry;

						INT_PTR result = DialogBoxParam(g_hModule, MAKEINTRESOURCE(IDD_EXTRACT_OPTIONS), NULL, SettingsDialogProc, 0);

						if (result == IDC_OPEN_WORKBENCH) {
							return HandleWorkbenchLaunch(arc);
						}

						if (result == IDCANCEL) {
							return E_EABORTED;
						}

						if (result == IDOK) {
							LogInfo("[ProcessFileW] Extract dialog finished (F5 mode).");
						}
					}
					g_ExtractOptionsShown = true;
				}
			} else if (isExternalOperation && !g_ExtractOptionsShown) {
				LogInfo("[ProcessFileW] External Alt+F9 operation detected, skipping extract dialog.");
				g_ExtractOptionsShown = true;
			}
		}

		if (idx < 0 || idx >= (int)arc->GetEntryCount()) return E_NO_FILES;
		if (!entry) return E_NO_FILES;

		if (Operation == PK_TEST) {
			return HandleTest(arc, entry);
		}

		if (Operation == PK_EXTRACT) {
			return HandleExtract(arc, idx, entry, currentDestPath, wDestName, isSilent);
		}

		return 0;
	}
	catch (...) {
		LogError("[ProcessFileW] Exception");
		return E_EWRITE;
	}
}

// ============================================================================
// CLOSEARCHIVE
// ============================================================================
int __stdcall CloseArchive(HANDLE hArcData) {
	if (!hArcData) {
		LogError("CloseArchive: NULL handle.");
		return E_ECLOSE;
	}

	PakArchive* archiveToClose = reinterpret_cast<PakArchive*>(hArcData);

	if (archiveToClose && archiveToClose->GetOpenMode() == 0) {
		std::lock_guard<std::mutex> lock(g_LastCloseMutex);
		g_LastClosedListArcName = archiveToClose->GetFilename();
		g_LastListCloseTime = std::chrono::steady_clock::now();
	}

	g_LastOperationEndTime = std::chrono::steady_clock::now();

	if (!g_OriginalArchivePath.empty() && (g_OriginalArchiveTime.dwLowDateTime != 0 || g_OriginalArchiveTime.dwHighDateTime != 0)) {

		if (g_RequireReload) {
			LogInfo("[CloseArchive] Refresh in progress, leaving touched timestamp for TC: " + g_OriginalArchivePath);
		}
		else {
			HANDLE hRestore = CreateFileA(g_OriginalArchivePath.c_str(), FILE_WRITE_ATTRIBUTES,
										 FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);

			if (hRestore != INVALID_HANDLE_VALUE) {
				if (SetFileTime(hRestore, NULL, NULL, &g_OriginalArchiveTime)) {
					LogInfo("[CloseArchive] Archive timestamp restored to exact original: " + g_OriginalArchivePath);
				} else {
					LogError("[CloseArchive] Failed to restore timestamp: " + std::to_string(GetLastError()));
				}
				CloseHandle(hRestore);
			} else {
				LogError("[CloseArchive] Failed to open archive for timestamp restoration: " + g_OriginalArchivePath);
			}

			g_OriginalArchiveTime = { 0 };
			g_OriginalArchivePath = "";
		}
	}

	try {
		if (archiveToClose) {
			LogInfo("Archive successfully closed and resources released.");
			LogInfo("[CloseArchive] instance=" + std::to_string((uintptr_t)archiveToClose));
			delete archiveToClose;
		}
	}
	catch (const std::exception& ex) {
		LogError(std::string("CloseArchive EXCEPTION during delete: ") + ex.what());
		return E_ECLOSE;
	}
	catch (...) {
		LogError("CloseArchive Unknown EXCEPTION during delete.");
		return E_ECLOSE;
	}

	g_RequireReload = false;

	return 0;
}

void __stdcall SetChangeVolProc(HANDLE hArcData, tChangeVolProc pChangeVolProc) {}

void __stdcall SetProcessDataProc(HANDLE hArcData, tProcessDataProc pProcessDataProc) {
	PakArchive* currentArchive = GetArchive(hArcData);
	if (currentArchive) {
		currentArchive->SetProcessDataProc(pProcessDataProc);
	}
}

extern "C" __declspec(dllexport) void __stdcall SetSearchTextW(const WCHAR* SearchString) {
	std::lock_guard<std::mutex> lock(g_SearchTextMutex);
	if (SearchString) {
		SearchTextW = SearchString;
		LogInfo("[SetSearchTextW] Search pattern set (W): " + WStringToUTF8(SearchTextW));
	}
	else {
		SearchTextW.clear();
		LogInfo("[SetSearchTextW] Search pattern cleared (W).");
	}
}

extern "C" __declspec(dllexport) void __stdcall SetSearchText(const char* SearchString) {
	if (SearchString) {
		int len = MultiByteToWideChar(CP_ACP, 0, SearchString, -1, nullptr, 0);
		if (len > 0) {
			std::wstring wideTemp(len - 1, L'\0');
			MultiByteToWideChar(CP_ACP, 0, SearchString, -1, &wideTemp[0], len);
			SetSearchTextW(wideTemp.c_str());
		}
	}
	else {
		SetSearchTextW(nullptr);
	}
}

static INT_PTR CALLBACK AboutDialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
	switch (message) {
	case WM_INITDIALOG: {
		HWND hLogoCtrl = GetDlgItem(hDlg, IDC_ABOUT_LOGO);
		if (hLogoCtrl) {
			RECT rect;
			GetClientRect(hLogoCtrl, &rect);
			int width = rect.right - rect.left;
			int height = rect.bottom - rect.top;

			HBITMAP hBmp = (HBITMAP)LoadImage(g_hModule,
											MAKEINTRESOURCE(IDB_LOGO),
											IMAGE_BITMAP,
											width,
											height,
											LR_CREATEDIBSECTION);

			if (hBmp) {
				SendMessage(hLogoCtrl, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)hBmp);
			} else {
				LogError("Failed to load/scale About dialog bitmap IDB_LOGO.");
			}
		}

		std::wstring versionText = L"ARMA PAK Plugin v" + UTF8ToWString(PLUGIN_VERSION_STRING);
		SetDlgItemTextW(hDlg, IDC_ABOUT_VERSION_TEXT, versionText.c_str());
		SetDlgItemTextW(hDlg, IDC_ABOUT_AUTHOR_TEXT, L"by Icebird");
		SetDlgItemTextW(hDlg, IDC_ABOUT_SUPPORT_TEXT, L"\u00A9 2026 Icebird. All rights reserved.");

		return TRUE;
	}
	case WM_COMMAND: {
		if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
			EndDialog(hDlg, LOWORD(wParam));
			return TRUE;
		}
		break;
	}
	case WM_DESTROY: {
		HWND hLogoCtrl = GetDlgItem(hDlg, IDC_ABOUT_LOGO);
		if (hLogoCtrl) {
			HBITMAP hBmp = (HBITMAP)SendMessage(hLogoCtrl, STM_GETIMAGE, IMAGE_BITMAP, 0);
			if (hBmp) {
				DeleteObject(hBmp);
			}
		}
		return TRUE;
	}
	}
	return FALSE;
}

// ============================================================================
// ⚙️ SETTINGS DIALOG PROC
// ============================================================================
static INT_PTR CALLBACK SettingsDialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
	static bool isSettingsOnlyMode = false;

	auto RefreshImageUI = [hDlg]() {
		if (g_CurrentArchiveForDialog) {
			ImageModule::UpdateSettingsUI(hDlg, g_CurrentArchiveForDialog);
		}
	};

	switch (message) {
	case WM_INITDIALOG: {
		isSettingsOnlyMode = (lParam == 1);

		HWND hLogoCtrl = GetDlgItem(hDlg, IDC_LOGO);
		if (hLogoCtrl) {
			RECT rect = { 0 };
			GetClientRect(hLogoCtrl, &rect);
			HBITMAP hBmp = (HBITMAP)LoadImage(g_hModule, MAKEINTRESOURCE(IDB_LOGO), IMAGE_BITMAP,
											rect.right - rect.left, rect.bottom - rect.top, LR_CREATEDIBSECTION);
			if (hBmp) {
				SendMessage(hLogoCtrl, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)hBmp);
			} else {
				LogError("Failed to load/scale settings dialog bitmap IDB_LOGO.");
			}
		}

		CheckDlgButton(hDlg, IDC_ENABLE_SMART_EXTRACT, g_EnableSmartExtract ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(hDlg, IDC_KEEP_STRUCT, g_KeepDirectoryStructure ? BST_CHECKED : BST_UNCHECKED);

		if (GetDlgItem(hDlg, IDC_ENABLE_LOG_INFO)) {
			CheckDlgButton(hDlg, IDC_ENABLE_LOG_INFO, g_EnableLogInfo ? BST_CHECKED : BST_UNCHECKED);
		}

		if (GetDlgItem(hDlg, IDC_SHOW_EXTRACT_PROMPT)) {
			CheckDlgButton(hDlg, IDC_SHOW_EXTRACT_PROMPT, g_ShowExtractPrompt ? BST_CHECKED : BST_UNCHECKED);
		}

		ImageModule::InitSettingsUI(hDlg);

		HWND hWorkbenchBtn = GetDlgItem(hDlg, IDC_OPEN_WORKBENCH);
		if (hWorkbenchBtn) {
			bool shouldShow = false;
			if (!g_CurrentEntryForDialog.name.empty() && !g_CurrentEntryForDialog.isDirectory) {
				std::string ext = fs::path(g_CurrentEntryForDialog.name).extension().string();
				std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
				if (WorkbenchModule::IsWorkbenchSupported(ext)) shouldShow = true;
			}
			ShowWindow(hWorkbenchBtn, shouldShow ? SW_SHOW : SW_HIDE);
		}

		RefreshImageUI();
		return TRUE;
	}

	case WM_COMMAND: {
		WORD controlId = LOWORD(wParam);
		WORD notification = HIWORD(wParam);

		if (ImageModule::HandleSettingsCommand(hDlg, controlId, notification)) {
			RefreshImageUI();
			return TRUE;
		}

		switch (controlId) {
		case IDOK: {
			g_EnableSmartExtract = (IsDlgButtonChecked(hDlg, IDC_ENABLE_SMART_EXTRACT) == BST_CHECKED);
			g_KeepDirectoryStructure = (IsDlgButtonChecked(hDlg, IDC_KEEP_STRUCT) == BST_CHECKED);

			if (GetDlgItem(hDlg, IDC_ENABLE_LOG_INFO)) {
				g_EnableLogInfo = (IsDlgButtonChecked(hDlg, IDC_ENABLE_LOG_INFO) == BST_CHECKED);
			}

			if (GetDlgItem(hDlg, IDC_SHOW_EXTRACT_PROMPT)) {
				g_ShowExtractPrompt = (IsDlgButtonChecked(hDlg, IDC_SHOW_EXTRACT_PROMPT) == BST_CHECKED);
			}

			if (ImageModule::ApplyAndRefreshIfNeeded(hDlg, g_CurrentArchiveForDialog, isSettingsOnlyMode)) {
				g_RequireReload = true;
				g_LastOpenedArcName = "";
			} else {
				LogInfo("[Settings] Refresh skipped (No image change or settings-only mode).");
			}

			SaveSettings();
			EndDialog(hDlg, IDOK);
			return TRUE;
		}
		case IDC_OPEN_WORKBENCH: {
			EndDialog(hDlg, IDC_OPEN_WORKBENCH);
			return TRUE;
		}
		case IDCANCEL: {
			EndDialog(hDlg, LOWORD(wParam));
			return TRUE;
		}
		case IDC_ABOUT_BUTTON: {
			DialogBoxParam(g_hModule, MAKEINTRESOURCE(IDD_ABOUTBOX), hDlg, AboutDialogProc, 0);
			return TRUE;
		}
		}
		break;
	}

	case WM_DESTROY: {
		HWND hLogoCtrl = GetDlgItem(hDlg, IDC_LOGO);
		if (hLogoCtrl) {
			HBITMAP hBmp = (HBITMAP)SendMessage(hLogoCtrl, STM_GETIMAGE, IMAGE_BITMAP, 0);
			if (hBmp) DeleteObject(hBmp);
		}
		return TRUE;
	}
	}
	return FALSE;
}

// ============================================================================
// EXPORTED FUNCTIONS
// ============================================================================
extern "C" __declspec(dllexport) int __stdcall ConfigurePacker(HWND Parent, HINSTANCE DllInstance) {
	g_LastOpenedArcName = "";
	DialogBoxParam(DllInstance, MAKEINTRESOURCE(IDD_ARMAPAK_SETTINGS), Parent, SettingsDialogProc, 1);
	return 0;
}

extern "C" __declspec(dllexport) void __stdcall About(HWND Parent) {
	DialogBoxParam(g_hModule, MAKEINTRESOURCE(IDD_ABOUTBOX), Parent, AboutDialogProc, 0);
}

extern "C" __declspec(dllexport) int __stdcall GetPackerCaps() {
	return PK_CAPS_MULTIPLE | PK_CAPS_BY_CONTENT | PK_CAPS_OPTIONS | PK_CAPS_SEARCHTEXT | PK_CAPS_MEMPACK;
}

// ============================================================================
// DLL MAIN
// ============================================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
	switch (ul_reason_for_call) {
	case DLL_PROCESS_ATTACH:
		g_hModule = hModule;

		{
			std::error_code ec;
			if (!fs::exists(GetIniPath(), ec)) {
				SaveSettings();
			} else {
				LoadSettings();
			}
		}

		if (!g_ThreadPool) {
			unsigned int numThreads = std::thread::hardware_concurrency();
			if (numThreads == 0) numThreads = 4;
			g_ThreadPool = std::make_unique<ThreadPool>(numThreads);
		}

		{
			std::string logFilePath = GetLogPath();
			debugLog.open(logFilePath, std::ios::out | std::ios::app);
			if (debugLog.is_open()) {
				logInitialized = true;
				time_t now = time(nullptr);
				char timeStr[26];
				ctime_s(timeStr, sizeof(timeStr), &now);
				if (timeStr[strlen(timeStr) - 1] == '\n') {
					timeStr[strlen(timeStr) - 1] = '\0';
				}
				debugLog << "\n\n=== DLL ATTACH: New session === " << timeStr << "\n";
				debugLog << "Plugin version: " << PLUGIN_VERSION_STRING << "\n";

				debugLog << "[INIT] Settings loaded:\n";
				debugLog << " - Smart Extract: " << (g_EnableSmartExtract ? "Enabled" : "Disabled") << "\n";
				debugLog << " - Keep Directory Structure: " << (g_KeepDirectoryStructure ? "Enabled" : "Disabled") << "\n";
				debugLog << " - Verbose Logging: " << (g_EnableLogInfo ? "Enabled" : "Disabled") << "\n";

				debugLog.flush();
			} else {
				logInitialized = false;
			}
		}
		break;
	case DLL_PROCESS_DETACH:
		if (g_ThreadPool) {
			g_ThreadPool.reset();
		}

		if (logInitialized && debugLog.is_open()) {
			debugLog << "[INFO] === DLL DETACH: Session end ===\n";
			debugLog.close();
		}
		break;
	}
	return TRUE;
}

bool SmartExtractor::ExtractWithDependencies(PakArchive* sourceArc, int index, const std::string& destPath, std::unordered_set<std::string>& processed)
{
	struct TaskInfo {
		int entryIndex;
		std::string targetFullPath;
		PakArchive* sourceArchive;
	};

	static std::mutex g_BucketMutexes[64];
	auto GetBucketLock = [](const std::string& path) -> std::mutex& {
		size_t h = std::hash<std::string>{}(path);
		return g_BucketMutexes[h % 64];
	};

	static std::mutex g_FileWriteMutex;

	std::vector<std::future<bool>> activeTasks;
	std::queue<TaskInfo> pendingTasks;

	const PakEntry* rootEntry = sourceArc->GetEntry(index);
	if (!rootEntry) return false;

	fs::path winDestFile(destPath);

	bool isViewer = winDestFile.has_filename() && winDestFile.extension() != "";
	fs::path baseExtractionDir = winDestFile.parent_path();
	fs::path rootParent = fs::path(rootEntry->name).parent_path();

	fs::path finalRootPath;
	if (isViewer) {
		finalRootPath = winDestFile;
	} else {
		finalRootPath = PakArchive::BuildFinalPath(baseExtractionDir.string(), rootEntry->name);
	}

	pendingTasks.push({ index, finalRootPath.string(), sourceArc });

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
			if (!fs::exists(fPath.parent_path())) {
				fs::create_directories(fPath.parent_path());
			}

			std::string ext = fs::path(entry->name).extension().string();
			std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

			if (ext == ".xob" || ext == ".emat") {
				try {
					static std::mutex g_DecompressMutex;
					std::vector<uint8_t> data;
					{
						std::lock_guard<std::mutex> lock(g_DecompressMutex);
						data = current.sourceArchive->DecompressEntryData(entry);
					}

					auto deps = FindDependencies(current.sourceArchive, data);

					for (const auto& depLine : deps) {
						LogInfo("[DEP RAW] " + depLine);

						std::string cleanPath = depLine;

						size_t bracePos = cleanPath.find('}');
						if (bracePos != std::string::npos) cleanPath = cleanPath.substr(bracePos + 1);

						auto startIdx = cleanPath.find_first_not_of(" \t\n\r");
						auto endIdx = cleanPath.find_last_not_of(" \t\n\r");
						if (startIdx == std::string::npos || endIdx == std::string::npos) continue;
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

								fs::path relDep = fs::relative(depEntry->name, rootParent);
								fs::path subDest = baseExtractionDir / relDep;

								int depIndex = targetArchive->GetEntryIndex(depEntry);
								if (depIndex != -1) {
									pendingTasks.push({ depIndex, subDest.string(), targetArchive });
								} else {
									LogInfo("[DEP ERROR] Index mapping failed for: " + depEntry->name);
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
				activeTasks.push_back(g_ThreadPool->enqueue([src = current.sourceArchive, idx = current.entryIndex, p = current.targetFullPath, &GetBucketLock]() {

					if (fs::exists(p)) {
						LogInfo("[SKIP] Already exists: " + p);
						return true;
					}

					std::lock_guard<std::mutex> lock(GetBucketLock(p));

					if (fs::exists(p)) {
						LogInfo("[SKIP AFTER LOCK] Already exists: " + p);
						return true;
					}

					return src->ExtractFile(idx, p);
				}));
			}
			else {
				std::lock_guard<std::mutex> lock(GetBucketLock(current.targetFullPath));

				if (fs::exists(current.targetFullPath)) {
					LogInfo("[SKIP] Already exists: " + current.targetFullPath);
					continue;
				}

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

	const std::vector<std::string> extensions = { ".emat", ".edds", ".xob", ".gamemat" };

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

// ============================================================================
// EXTERNAL HELPERS
// ============================================================================

void SetArchiveSnapshotExternal(void* arc_ptr, ImageModule::Snapshot snap) {
	if (arc_ptr) reinterpret_cast<PakArchive*>(arc_ptr)->SetImageSnapshot(snap);
}

ImageModule::Snapshot GetArchiveSnapshotExternal(void* arc_ptr) {
	if (arc_ptr) return reinterpret_cast<PakArchive*>(arc_ptr)->GetImageSnapshot();
	return ImageModule::Snapshot();
}

std::string GetArchiveFilenameExternal(void* arc_ptr) {
	if (arc_ptr) return reinterpret_cast<PakArchive*>(arc_ptr)->GetFilename();
	return "";
}

int FindIndexByNameExternal(void* arc_ptr, const std::string& name) {
	if (arc_ptr) return reinterpret_cast<PakArchive*>(arc_ptr)->FindIndexByName(name);
	return -1;
}

#include <map>
#include <ctime>
#include <assert.h>
#include <filesystem>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <cctype>
#include <unordered_set>
#include <mutex>
#include <span>
#include <queue>
#include <atomic>
#include <unordered_map>
#include <thread>
#include <condition_variable>
#include <functional>
#include <future>
#include <type_traits>
#include <tlhelp32.h>

namespace fs = std::filesystem;

namespace ImageModule {
	struct Snapshot {
		bool enabled = false;
		int format = 0;
	};

	inline void LoadSettings(const std::string&, const std::string&) {}
	inline void SaveSettings(const std::string&, const std::string&) {}
	inline Snapshot GetGlobalSnapshot() { return { false, 0 }; }
	inline Snapshot ResolveSnapshot(bool, bool) { return { false, 0 }; }
	inline void AdjustDisplayName(std::string&, bool, const Snapshot&, void*, int) {}
	inline std::filesystem::path ResolveExtractPath(const std::filesystem::path& p, const std::string&, const Snapshot&, bool, bool) { return p; }
	inline void InitSettingsUI(HWND) {}
	inline bool HandleSettingsCommand(HWND, WORD, WORD) { return false; }
	inline void UpdateSettingsUI(HWND, void*) {}
	inline bool ApplyAndRefreshIfNeeded(HWND, void*, bool) { return false; }

	inline bool SmartSave(const std::string&, const std::filesystem::path& finalPath, const std::vector<uint8_t>& data, const Snapshot&, bool) {
		std::ofstream out(finalPath, std::ios::binary);
		if (!out) return false;
		out.write(reinterpret_cast<const char*>(data.data()), data.size());
		return out.good();
	}
}

class PakEntry {
public:
	enum class CompressionType : uint32_t {
		None = 0,
		Zlib = 0x106
	};

	uint32_t timestamp = 0;
	std::string name;
	uint32_t offset = 0;
	uint32_t size = 0;
	uint32_t originalSize = 0;
	CompressionType compression = CompressionType::None;
	bool isDirectory = false;
	std::vector<std::shared_ptr<PakEntry>> children;
};

#include "ThreadPool.h"

class PakArchive;
class PakIndex;

#include "pak_index.h"
#include "SmartExtractor.h"
#include "WorkbenchModule.h"
#include "wcxhead.h"
#include "resource.h"

PakEntry g_CurrentEntryForDialog;
PakArchive* g_CurrentArchiveForDialog = nullptr;

const char* PLUGIN_VERSION_STRING = "1.2.0";
extern std::unique_ptr<ThreadPool> g_ThreadPool;

void LogError(const std::string& message);
void LogInfo(const std::string& message);

static std::ofstream debugLog;
static bool logInitialized = false;
static std::mutex g_LogMutex;
std::mutex g_CallbackMutex;
static std::mutex g_SearchTextMutex;

std::vector<PakArchive*> g_OpenedArchives;
std::mutex g_ArchivesMutex;

bool g_EnableLogInfo = false;
bool g_EnableSmartExtract = false;
bool g_KeepDirectoryStructure = true;
bool g_ShowExtractPrompt = true;
static std::wstring SearchTextW;

static std::unordered_map<std::string, std::unique_ptr<std::mutex>> g_FileWriteLocks;
static std::mutex g_FileWriteLocksMutex;

const char* const INI_KEY_SMART_EXTRACT = "EnableSmartExtract";
const char* const INI_KEY_KEEP_STRUCT = "KeepDirectoryStructure";
const char* const INI_FILE_NAME = "pak_plugin.ini";
const char* const INI_SECTION_NAME = "Settings";
const char* const INI_KEY_LOG_INFO = "EnableLogInfo";
const char* const LOG_FILE_NAME = "pak_plugin.log";

static HMODULE g_hModule = NULL;

static std::string GetPluginPath() {
	char path[MAX_PATH];
	if (g_hModule != NULL) {
		GetModuleFileNameA(g_hModule, path, MAX_PATH);
	} else {
		GetModuleFileNameA(NULL, path, MAX_PATH);
	}
	std::filesystem::path plugin_path(path);
	return plugin_path.parent_path().string();
}

static std::string GetLogPath() {
	return GetPluginPath() + "\\" + LOG_FILE_NAME;
}

static std::string GetIniPath() {
	return GetPluginPath() + "\\" + INI_FILE_NAME;
}

static void CheckLogRotation() {
	std::string path = GetLogPath();

	std::error_code ec;
	if (fs::exists(path, ec)) {
		if (fs::file_size(path, ec) > 5 * 1024 * 1024) {
			if (debugLog.is_open()) debugLog.close();

			debugLog.open(path, std::ios::out | std::ios::trunc);

			if (debugLog.is_open()) {
				debugLog << "[INFO] Log file rotated (size limit reached, cleared).\n";
				debugLog.flush();
			}
		}
	}
}

void LogError(const std::string& message) {
	std::lock_guard<std::mutex> lock(g_LogMutex);
	CheckLogRotation();

	if (logInitialized && debugLog.is_open()) {
		debugLog << "[ERROR] " << message << "\n";
		debugLog.flush();
	}
}

void LogInfo(const std::string& message) {
	if (!g_EnableLogInfo) return;
	std::lock_guard<std::mutex> lock(g_LogMutex);
	CheckLogRotation();

	if (logInitialized && debugLog.is_open()) {
		debugLog << "[INFO] " << message << "\n";
		debugLog.flush();
	}
}

static void LoadSettings() {
	std::string iniPath = GetIniPath();

	ImageModule::LoadSettings(iniPath, INI_SECTION_NAME);

	g_EnableLogInfo= GetPrivateProfileIntA(INI_SECTION_NAME, INI_KEY_LOG_INFO, 0, iniPath.c_str()) != 0;
	g_EnableSmartExtract= GetPrivateProfileIntA(INI_SECTION_NAME, INI_KEY_SMART_EXTRACT, 1, iniPath.c_str()) != 0;
	g_KeepDirectoryStructure = GetPrivateProfileIntA(INI_SECTION_NAME, INI_KEY_KEEP_STRUCT, 1, iniPath.c_str()) != 0;
	g_ShowExtractPrompt= GetPrivateProfileIntA(INI_SECTION_NAME, "ShowExtractPrompt", 1, iniPath.c_str()) != 0;
}

static void SaveSettings() {
	std::string iniPath = GetIniPath();

	ImageModule::SaveSettings(iniPath, INI_SECTION_NAME);

	WritePrivateProfileStringA(INI_SECTION_NAME, INI_KEY_LOG_INFO, g_EnableLogInfo ? "1" : "0", iniPath.c_str());
	WritePrivateProfileStringA(INI_SECTION_NAME, INI_KEY_SMART_EXTRACT, g_EnableSmartExtract ? "1" : "0", iniPath.c_str());
	WritePrivateProfileStringA(INI_SECTION_NAME, INI_KEY_KEEP_STRUCT, g_KeepDirectoryStructure ? "1" : "0", iniPath.c_str());
	WritePrivateProfileStringA(INI_SECTION_NAME, "ShowExtractPrompt", g_ShowExtractPrompt ? "1" : "0", iniPath.c_str());
}

static unsigned int SystemTimeToDosDateTime(const SYSTEMTIME& st) {
	const unsigned int year= static_cast<unsigned int>(st.wYear);
	const unsigned int month= static_cast<unsigned int>(st.wMonth);
	const unsigned int day= static_cast<unsigned int>(st.wDay);
	const unsigned int hour= static_cast<unsigned int>(st.wHour);
	const unsigned int minute = static_cast<unsigned int>(st.wMinute);
	const unsigned int second = static_cast<unsigned int>(st.wSecond);
	unsigned int dosDate = ((year - 1980) << 9) | (month << 5) | day;
	unsigned int dosTime = (hour << 11) | (minute << 5) | (second / 2);
	return (dosDate << 16) | (dosTime & 0xFFFF);
}

static std::string WStringToUTF8(const wchar_t* ws) {
	if (!ws || ws[0] == L'\0') return "";
	int len = (int)wcslen(ws);
	int size_needed = WideCharToMultiByte(CP_UTF8, 0, ws, len, nullptr, 0, nullptr, nullptr);
	if (size_needed <= 0) return "";
	std::string result(size_needed, 0);
	WideCharToMultiByte(CP_UTF8, 0, ws, len, &result[0], size_needed, nullptr, nullptr);
	return result;
}

static std::string WStringToUTF8(const std::wstring& ws) {
	return WStringToUTF8(ws.c_str());
}

static std::string WCharToUTF8(const WCHAR* ws) {
	return WStringToUTF8(ws);
}

static std::wstring UTF8ToWString(const std::string& str) {
	if (str.empty()) return L"";
	int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), NULL, 0);
	if (size_needed <= 0) return L"";
	std::wstring wstrTo(size_needed, 0);
	MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstrTo[0], size_needed);
	return wstrTo;
}

class PakArchive {
private:
	std::shared_ptr<PakEntry> root;
	std::vector<std::shared_ptr<PakEntry>> flatEntries;
	std::string filename;
	bool initialized = false;
	long long actualFileSize = 0;

	HANDLE hFile = INVALID_HANDLE_VALUE;

	std::atomic<int> m_CurrentIndex{0};
	std::atomic<int> m_LastIndex{-1};
	std::mutex indexMutex;
	mutable std::mutex m_FileMutex;

	std::unique_ptr<PakIndex> m_index;
	tProcessDataProc m_pProcessDataProc = nullptr;

	std::unordered_map<std::string, int> m_LookupTable;

	ImageModule::Snapshot m_ImageSnapshot;

	struct IffChunk {
		char id[4];
		uint32_t size;
		uint64_t dataStart;
		uint64_t dataEnd;
	};

	bool InternalRead(void* buffer, DWORD size) {
		DWORD bytesRead = 0;
		return ReadFile(hFile, buffer, size, &bytesRead, NULL) && bytesRead == size;
	}

	uint32_t ReadU32BE() {
		uint32_t val;
		if (!InternalRead(&val, 4)) throw std::runtime_error("Read error (U32BE)");
		return _byteswap_ulong(val);
	}

	uint32_t ReadU32LE() {
		uint32_t val;
		if (!InternalRead(&val, 4)) throw std::runtime_error("Read error (U32LE)");
		return val;
	}

	uint8_t ReadU8() {
		uint8_t val;
		if (!InternalRead(&val, 1)) throw std::runtime_error("Read error (U8)");
		return val;
	}

	std::string ReadString(uint8_t length) {
		if (length == 0) return "";
		std::string str(length, '\0');
		if (!InternalRead(&str[0], length)) throw std::runtime_error("Read error (String)");
		return str;
	}

	bool ReadNextChunk(IffChunk& chunk) {
		LARGE_INTEGER currentPos;
		currentPos.QuadPart = 0;
		if (!SetFilePointerEx(hFile, currentPos, &currentPos, FILE_CURRENT)) return false;

		if (currentPos.QuadPart + 8 > actualFileSize) return false;

		if (!InternalRead(chunk.id, 4)) return false;
		chunk.size = ReadU32BE();
		chunk.dataStart = currentPos.QuadPart + 8;
		chunk.dataEnd = chunk.dataStart + chunk.size;

		if (chunk.dataEnd > (uint64_t)actualFileSize) {
			LogError("Chunk '" + std::string(chunk.id, 4) + "' size exceeds file bounds.");
			return false;
		}
		return true;
	}

	void ProcessHeadChunk(const IffChunk& chunk) {
		LARGE_INTEGER li;
		li.QuadPart = chunk.dataEnd;
		SetFilePointerEx(hFile, li, NULL, FILE_BEGIN);
	}

	void ProcessFileChunk(const IffChunk& chunk) {
		uint8_t entryType = ReadU8();
		uint8_t nameLength = ReadU8();
		std::string name = ReadString(nameLength);

		if (entryType == 0) root->children.push_back(ReadDirectoryEntry(name));
		else root->children.push_back(ReadFileEntry(name));

		LARGE_INTEGER li;
		li.QuadPart = chunk.dataEnd;
		SetFilePointerEx(hFile, li, NULL, FILE_BEGIN);
	}

	std::shared_ptr<PakEntry> ReadDirectoryEntry(const std::string& name) {
		auto entry = std::make_shared<PakEntry>();
		entry->name = name;
		entry->isDirectory = true;

		uint32_t childCount = ReadU32LE();

		entry->timestamp = static_cast<uint32_t>(time(nullptr));

		for (uint32_t i = 0; i < childCount; i++) {
			uint8_t childType = ReadU8();
			uint8_t childNameLength = ReadU8();
			std::string childName = ReadString(childNameLength);

			if (childType == 0) {
				entry->children.push_back(ReadDirectoryEntry(childName));
			} else {
				entry->children.push_back(ReadFileEntry(childName));
			}
		}
		return entry;
	}

	std::shared_ptr<PakEntry> ReadFileEntry(const std::string& name) {
		auto entry = std::make_shared<PakEntry>();
		entry->name = name;
		entry->isDirectory = false;
		entry->offset = ReadU32LE();
		entry->size = ReadU32LE();
		entry->originalSize = ReadU32LE();

		LARGE_INTEGER li; li.QuadPart = 4;
		SetFilePointerEx(hFile, li, NULL, FILE_CURRENT);
		entry->compression = static_cast<PakEntry::CompressionType>(ReadU32BE());
		entry->timestamp = ReadU32LE();

		return entry;
	}

	void FlattenEntries(const std::shared_ptr<PakEntry>& entry, const std::string& path = "") {
		std::string fullPath = path;
		if (entry.get() != root.get()) {
			if (!fullPath.empty()) fullPath += "\\";
			fullPath += entry->name;
		}
		if (entry.get() != root.get()) {
			auto flatEntry = std::make_shared<PakEntry>(*entry);
			flatEntry->name = fullPath;
			flatEntries.push_back(flatEntry);
		}
		for (const auto& child : entry->children) FlattenEntries(child, fullPath);
	}

	std::string NormalizePath(const std::string& name) const {
		std::string result = name;
		std::replace(result.begin(), result.end(), '/', '\\');
		std::transform(result.begin(), result.end(), result.begin(), ::tolower);
		return result;
	}

public:
	void AddVirtualEntry(const std::string& name) {
		auto entry = std::make_shared<PakEntry>();
		entry->name = name;
		entry->isDirectory = false;
		entry->size = 1;
		entry->originalSize = 1;
		entry->offset = 0;
		entry->compression = PakEntry::CompressionType::None;

		entry->timestamp = static_cast<uint32_t>(time(nullptr));

		flatEntries.push_back(entry);
	}

	void BuildIndex() {
		m_LookupTable.clear();

		for (size_t i = 0; i < flatEntries.size(); ++i) {
			std::string normalizedName = NormalizePath(flatEntries[i]->name);
			m_LookupTable[normalizedName] = (int)i;
		}

		if (m_index && !flatEntries.empty()) {
			m_index->Build(flatEntries);
		}
	}

	int FindIndexByName(const std::string& name) const {
		auto it = m_LookupTable.find(NormalizePath(name));

		if (it != m_LookupTable.end()) {
			return it->second;
		}
		return -1;
	}

	std::string GetFilename() const { return filename; }

	int GetEntryIndex(const PakEntry* entry) const {
		if (!entry || flatEntries.empty()) return -1;

		for (size_t i = 0; i < flatEntries.size(); ++i) {
			if (flatEntries[i].get() == entry) return (int)i;
		}

		return -1;
	}

	const PakEntry* FindEntryByName(const std::string& name) const {
		std::string norm = name;
		std::replace(norm.begin(), norm.end(), '/', '\\');
		std::transform(norm.begin(), norm.end(), norm.begin(), ::tolower);

		LogInfo("[FindEntry] SEARCH: " + norm);

		auto tryFindExact = [&](const std::string& p) -> const PakEntry* {
			auto it = m_LookupTable.find(p);
			if (it != m_LookupTable.end()) {
				return flatEntries[it->second].get();
			}

			{
				std::lock_guard<std::mutex> lock(g_ArchivesMutex);
				for (auto* otherArchive : g_OpenedArchives) {
					if (otherArchive == this) continue;

					auto itOther = otherArchive->m_LookupTable.find(p);
					if (itOther != otherArchive->m_LookupTable.end()) {
						return otherArchive->flatEntries[itOther->second].get();
					}

					if (otherArchive->m_LookupTable.empty()) {
						for (const auto& entry : otherArchive->flatEntries) {
							std::string entryNorm = entry->name;
							std::replace(entryNorm.begin(), entryNorm.end(), '/', '\\');
							std::transform(entryNorm.begin(), entryNorm.end(), entryNorm.begin(), ::tolower);
							if (entryNorm == p) return entry.get();
						}
					}
				}
			}
			return nullptr;
		};

		if (auto* e = tryFindExact(norm)) {
			LogInfo("[FindEntry] FULL MATCH: " + norm);
			return e;
		}

		if (norm.find("assets\\") != 0 && norm.find("common\\") != 0) {
			std::string fixed = "assets\\" + norm;
			if (auto* e = tryFindExact(fixed)) {
				LogInfo("[FindEntry] FIXED (assets\\ prefix): " + fixed);
				return e;
			}
		}

		if (norm.find("common\\") == std::string::npos) {
			std::string fixed = "common\\" + norm;
			if (auto* e = tryFindExact(fixed)) {
				LogInfo("[FindEntry] FIXED (common\\ prefix): " + fixed);
				return e;
			}
		}

		LogInfo("[FindEntry] NOT FOUND: " + norm);
		return nullptr;
	}

	std::vector<uint8_t> DecompressEntryData(const PakEntry* entry) {
		if (!entry || entry->isDirectory) {
			throw std::runtime_error("Invalid or directory entry for decompression.");
		}

		if (entry->size == 0) {
			return {};
		}

		std::lock_guard<std::mutex> readLock(m_FileMutex);

		LARGE_INTEGER li;
		li.QuadPart = entry->offset;
		if (!SetFilePointerEx(hFile, li, NULL, FILE_BEGIN)) {
			throw std::runtime_error("Failed to seek to entry: " + entry->name);
		}

		uint64_t endPos = static_cast<uint64_t>(entry->offset) + entry->size;
		if (endPos > static_cast<uint64_t>(actualFileSize)) {
			LogError("[DecompressEntryData] Entry data goes beyond archive bounds: " + entry->name);
			throw std::runtime_error("Entry data out of bounds.");
		}

		if (entry->size > 1024 * 1024 * 1024) {
			LogError("[DecompressEntryData] Compressed entry too large (over 1GB): " + entry->name);
			throw std::runtime_error("Compressed entry too large");
		}

		std::vector<uint8_t> rawBuffer(entry->size);
		DWORD read = 0;
		if (!ReadFile(hFile, rawBuffer.data(), entry->size, &read, NULL) || read != entry->size) {
			LogError("[DecompressEntryData] Read failed for " + entry->name);
			throw std::runtime_error("Read failed.");
		}

		if (entry->compression == PakEntry::CompressionType::Zlib) {
			if (entry->originalSize == 0) {
				LogError("[DecompressEntryData] originalSize is 0 for compressed entry: " + entry->name);
				throw std::runtime_error("Invalid original size");
			}

			double ratio = (entry->size > 0) ? (static_cast<double>(entry->originalSize) / entry->size) : 0;

			if (entry->originalSize > 1024 * 1024 * 1024 || ratio > 1000.0) {
				LogError("[DecompressEntryData] Compression bomb detected (Ratio: " + std::to_string(ratio) + "): " + entry->name);
				throw std::runtime_error("Uncompressed size too large or suspicious ratio");
			}

			std::vector<uint8_t> processedContent(entry->originalSize);
			uLongf destLen = entry->originalSize;
			int zResult = uncompress(
				reinterpret_cast<Bytef*>(processedContent.data()), &destLen,
				reinterpret_cast<const Bytef*>(rawBuffer.data()), (uLong)entry->size);

			if (zResult != Z_OK || destLen != entry->originalSize) {
				LogError("[DecompressEntryData] Zlib error code: " + std::to_string(zResult) + " for " + entry->name);
				throw std::runtime_error("Zlib decompression failed.");
			}
			return processedContent;
		}

		return rawBuffer;
	}

	PakArchive(const std::string& filename) : filename(filename) {
		try {
			m_ImageSnapshot = ImageModule::GetGlobalSnapshot();

			hFile = CreateFileA(filename.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			if (hFile == INVALID_HANDLE_VALUE) {
				LogError("Failed to open PAK file (WinAPI): " + filename);
				throw std::runtime_error("Failed to open PAK file");
			}

			std::lock_guard<std::mutex> lock(m_FileMutex);

			LARGE_INTEGER fileSize;
			if (!GetFileSizeEx(hFile, &fileSize)) throw std::runtime_error("Failed to get file size");
			actualFileSize = fileSize.QuadPart;

			char formSig[4];
			if (!InternalRead(formSig, 4) || memcmp(formSig, "FORM", 4) != 0) {
				LogError("PAK file does not start with 'FORM' signature: " + filename);
				throw std::runtime_error("Invalid PAK signature.");
			}

			uint32_t formSize = ReadU32BE();
			uint64_t expectedTotalSize = 8 + (uint64_t)formSize;
			if (actualFileSize != static_cast<long long>(expectedTotalSize)) {
				LogError("Archive header size mismatch. Expected: " + std::to_string(expectedTotalSize));
				throw std::runtime_error("Archive size mismatch.");
			}

			char pac1Sig[4];
			if (!InternalRead(pac1Sig, 4) || memcmp(pac1Sig, "PAC1", 4) != 0) {
				LogError("PAK file FORM chunk type is not 'PAC1'.");
				throw std::runtime_error("Invalid PAK type.");
			}

			root = std::make_shared<PakEntry>();
			root->name = "";
			root->isDirectory = true;

			while (true) {
				LARGE_INTEGER cur; cur.QuadPart = 0;
				SetFilePointerEx(hFile, cur, &cur, FILE_CURRENT);
				if ((long long)cur.QuadPart >= actualFileSize) break;

				IffChunk chunk;
				if (!ReadNextChunk(chunk)) break;

				if (strncmp(chunk.id, "HEAD", 4) == 0) ProcessHeadChunk(chunk);
				else if (strncmp(chunk.id, "FILE", 4) == 0) ProcessFileChunk(chunk);
				else if (strncmp(chunk.id, "DATA", 4) == 0) {
					LARGE_INTEGER li; li.QuadPart = chunk.dataEnd;
					SetFilePointerEx(hFile, li, NULL, FILE_BEGIN);
				}
				else {
					LogInfo("Skipping unknown chunk: " + std::string(chunk.id, 4));
					LARGE_INTEGER li; li.QuadPart = chunk.dataEnd;
					SetFilePointerEx(hFile, li, NULL, FILE_BEGIN);
				}
			}
			if (root) FlattenEntries(root);

			m_index = std::make_unique<PakIndex>();
			m_index->Build(flatEntries);

			{
				std::lock_guard<std::mutex> lock(g_ArchivesMutex);
				g_OpenedArchives.push_back(this);
			}

			ResetIndex();

			initialized = true;
		} catch (const std::exception& ex) {
			LogError("PakArchive construction EXCEPTION: " + std::string(ex.what()));
			initialized = false;
		}
	}

	~PakArchive() {
		{
			std::lock_guard<std::mutex> lock(g_ArchivesMutex);
			auto it = std::find(g_OpenedArchives.begin(), g_OpenedArchives.end(), this);
			if (it != g_OpenedArchives.end()) g_OpenedArchives.erase(it);
		}

		if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
	}

	bool IsInitialized() const { return initialized; }
	int GetEntryCount() const { return static_cast<int>(flatEntries.size()); }

	const PakEntry* GetEntry(int index) const {
		if (index < 0 || index >= static_cast<int>(flatEntries.size())) return nullptr;
		return flatEntries[index].get();
	}

	int GetAndIncrementIndex() {
		return m_CurrentIndex.fetch_add(1, std::memory_order_relaxed);
	}

	int GetLastProcessedIndex() {
		return m_CurrentIndex.load(std::memory_order_relaxed) - 1;
	}

	void SetLastIndex(int idx) {
		m_LastIndex.store(idx, std::memory_order_relaxed);
	}

	int GetLastIndex() const {
		return m_LastIndex.load(std::memory_order_relaxed);
	}

	void ResetIndex() {
		std::lock_guard<std::mutex> lock(indexMutex);
		m_CurrentIndex.store(0, std::memory_order_relaxed);
		m_LastIndex.store(-1, std::memory_order_relaxed);
	}

	ImageModule::Snapshot GetImageSnapshot() const { return m_ImageSnapshot; }
	void SetImageSnapshot(const ImageModule::Snapshot& s) { m_ImageSnapshot = s; }

	void SetProcessDataProc(tProcessDataProc p) { m_pProcessDataProc = p; }
	tProcessDataProc GetProcessDataProc() const { return m_pProcessDataProc; }

	static fs::path BuildFinalPath(const std::string& base, const std::string& entryName) {
		fs::path basePath = base.empty()
			? fs::current_path()
			: fs::absolute(fs::path(base));

		if (basePath.has_filename() && basePath.extension() != "") {
			basePath = basePath.parent_path();
		}

		std::string safePath = g_KeepDirectoryStructure
			? entryName
			: fs::path(entryName).filename().string();

		size_t drivePos = safePath.find(':');
		if (drivePos != std::string::npos) {
			safePath = safePath.substr(drivePos + 1);
		}

		while (!safePath.empty() && (safePath[0] == '\\' || safePath[0] == '/')) {
			safePath.erase(0, 1);
		}

		std::replace(safePath.begin(), safePath.end(), '/', '\\');

		fs::path finalPath = basePath / fs::path(safePath);

		return fs::absolute(finalPath).lexically_normal();
	}

	static inline std::string PathToLog(const fs::path& p) {
		auto u8 = p.u8string();
		return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
	}

	static bool DecompressEntryFast(PakArchive* arc, const PakEntry* entry, std::vector<uint8_t>& out) {
		static std::mutex g_DecompressMutex;
		std::lock_guard<std::mutex> lock(g_DecompressMutex);

		out = arc->DecompressEntryData(entry);

		if (out.empty() && entry->originalSize > 0) {
			LogError("[ExtractFile] Decompression failed: " + entry->name);
			return false;
		}
		return true;
	}

	static fs::path ResolveTargetPath(const std::string& destPath, const PakEntry* entry, bool& isDirectFileTarget) {
		fs::path input(destPath);
		isDirectFileTarget = input.has_filename() && !input.extension().empty();

		if (isDirectFileTarget) {
			LogInfo("[ExtractFile][DEBUG] Direct Target: " + PathToLog(input));
			return input;
		}

		fs::path result = PakArchive::BuildFinalPath(destPath, entry->name);
		LogInfo("[ExtractFile][DEBUG] Directory Target: " + PathToLog(result));
		return result;
	}

	static inline bool EnsureDirFast(const fs::path& path) {
		auto dir = path.parent_path();
		if (dir.empty()) return true;

		std::error_code ec;
		if (!fs::exists(dir) && !fs::create_directories(dir, ec) && ec) {
			LogError("[ExtractFile] Dir create failed: " + PathToLog(dir));
			return false;
		}
		return true;
	}

	static bool ReportProgressFast(PakArchive* arc, const PakEntry* entry) {
		auto cb = arc->GetProcessDataProc();
		if (!cb) return true;

		const size_t CHUNK = 16384;
		size_t total = 0;

		if (entry->originalSize == 0) {
			std::lock_guard<std::mutex> lock(g_CallbackMutex);
			cb(const_cast<char*>(entry->name.c_str()), 0);
			return true;
		}

		while (total < (size_t)entry->originalSize) {
			size_t step = std::min(CHUNK, (size_t)entry->originalSize - total);

			int res = 0;
			{
				std::lock_guard<std::mutex> lock(g_CallbackMutex);
				res = cb(const_cast<char*>(entry->name.c_str()), (int)step);
			}

			if (res == 0) return false;
			total += step;
		}

		return true;
	}

	bool ExtractFile(int index, const std::string& destPath) {
		const PakEntry* entry = GetEntry(index);

		if (!entry || entry->isDirectory) {
			LogError("[ExtractFile] Invalid entry: " + std::to_string(index));
			return false;
		}

		try {
			bool isDirect = false;
			fs::path finalPath = ResolveTargetPath(destPath, entry, isDirect);

			if (!EnsureDirFast(finalPath)) return false;

			std::vector<uint8_t> data;
			if (!DecompressEntryFast(this, entry, data)) return false;

			bool ok = ImageModule::SmartSave(entry->name, finalPath, data, m_ImageSnapshot, isDirect);

			if (!ok) {
				LogError("[ExtractFile] Write/Convert failed: " + PathToLog(finalPath));
				return false;
			}

			if (!ReportProgressFast(this, entry)) {
				LogInfo("[ExtractFile] Aborted by user");
				return false;
			}

			return true;
		}
		catch (const std::exception& ex) {
			LogError("[ExtractFile] EXCEPTION: " + std::string(ex.what()));
			return false;
		}
		catch (...) {
			LogError("[ExtractFile] Unknown EXCEPTION");
			return false;
		}
	}
};

inline std::string ws2s(const std::wstring& wstr)
{
	if (wstr.empty()) return std::string();
	int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), NULL, 0, NULL, NULL);
	std::string strTo(size_needed, 0);
	WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
	return strTo;
}

static PakArchive* GetArchive(HANDLE hArcData) {
	return reinterpret_cast<PakArchive*>(hArcData);
}

std::unique_ptr<ThreadPool> g_ThreadPool = nullptr;

static std::string g_LastOpenedArcName = "";
static std::wstring g_LastTargetDir = L"";
static bool g_ExtractOptionsShown = false;
static std::atomic<bool> g_RequireReload{ false };
static FILETIME g_OriginalArchiveTime = { 0 };
static std::string g_OriginalArchivePath = "";
static std::chrono::steady_clock::time_point g_LastOperationEndTime = std::chrono::steady_clock::now();

// ============================================================================
// OPENARCHIVE
// ============================================================================
template <typename T>
static HANDLE OpenArchiveInternal(const std::string& arcName, T* ArchiveData) {
	std::unique_ptr<PakArchive> newArchive;
	try {
		auto now = std::chrono::steady_clock::now();
		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_LastOperationEndTime).count();

		if (elapsed > 1000) {
			g_ExtractOptionsShown = false;
		}

		bool isSameFile = (arcName == g_LastOpenedArcName);
		bool forceReload = g_RequireReload.load();

		if (forceReload && !g_OriginalArchivePath.empty()) {
			HANDLE hRestore = CreateFileA(g_OriginalArchivePath.c_str(), FILE_WRITE_ATTRIBUTES,
										 FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
			if (hRestore != INVALID_HANDLE_VALUE) {
				if (SetFileTime(hRestore, NULL, NULL, &g_OriginalArchiveTime)) {
					LogInfo("[OpenArchive] Timestamp RESTORED to original after reload: " + g_OriginalArchivePath);
				} else {
					LogError("[OpenArchive] Failed to restore timestamp in Open: " + std::to_string(GetLastError()));
				}
				CloseHandle(hRestore);
			}
			g_OriginalArchiveTime = { 0 };
			g_OriginalArchivePath = "";
		}

		if (forceReload) {
			LogInfo("[OpenArchive] FORCE RELOAD triggered for: " + arcName);
		}

		LogInfo("[OpenArchive] Opening archive: " + arcName);
		newArchive = std::make_unique<PakArchive>(arcName);

		if (!newArchive->IsInitialized()) {
			LogError("[OpenArchive] Initialization failed for: " + arcName);
			ArchiveData->OpenResult = E_EOPEN;
			return nullptr;
		}

		ImageModule::Snapshot currentSnapshot = ImageModule::ResolveSnapshot(isSameFile, forceReload);

		if (forceReload) {
			g_RequireReload = false;
		}

		newArchive->SetImageSnapshot(currentSnapshot);

		LogInfo("[OpenArchive] Snapshot applied. Conv: " +
			std::to_string(currentSnapshot.enabled) +
			" Format: " + std::to_string(static_cast<int>(currentSnapshot.format)));

		g_LastOpenedArcName = arcName;

		LogInfo("[OpenArchive] Adding virtual entry: pak_plugin.ini");
		newArchive->AddVirtualEntry("pak_plugin.ini");

		newArchive->BuildIndex();
		newArchive->ResetIndex();

		ArchiveData->OpenResult = 0;

		LogInfo("Successfully opened archive: " + arcName + " (with virtual plugin entry)");

		LogInfo("[OpenArchive] instance=" + std::to_string((uintptr_t)newArchive.get()));

		return reinterpret_cast<HANDLE>(newArchive.release());
	}
	catch (const std::exception& ex) {
		LogError("[OpenArchive] EXCEPTION: " + std::string(ex.what()));
		ArchiveData->OpenResult = E_EOPEN;
		return nullptr;
	}
	catch (...) {
		LogError("[OpenArchive] Unknown EXCEPTION caught.");
		ArchiveData->OpenResult = E_EOPEN;
		return nullptr;
	}
}

HANDLE __stdcall OpenArchive(tOpenArchiveData* ArchiveData) {
	if (!ArchiveData || !ArchiveData->ArcName) {
		LogError("[OpenArchive] Invalid archive data or missing filename.");
		if (ArchiveData) ArchiveData->OpenResult = E_BAD_ARCHIVE;
		return nullptr;
	}

	return OpenArchiveInternal<tOpenArchiveData>(
		std::string(ArchiveData->ArcName),
		ArchiveData
	);
}

HANDLE __stdcall OpenArchiveW(tOpenArchiveDataW* ArchiveDataW) {
	if (!ArchiveDataW || !ArchiveDataW->ArcName) {
		LogError("[OpenArchiveW] Invalid archive data or missing filename.");
		if (ArchiveDataW) ArchiveDataW->OpenResult = E_BAD_ARCHIVE;
		return nullptr;
	}

	return OpenArchiveInternal<tOpenArchiveDataW>(
		WCharToUTF8(ArchiveDataW->ArcName),
		ArchiveDataW
	);
}

enum class HeaderResult {
	OK,
	END,
	BAD_ARCHIVE,
	BAD_DATA
};

struct PreparedHeader {
	const PakEntry* entry = nullptr;
	std::string displayName;
	uint32_t dosTime = 0;
};

static HeaderResult PrepareHeaderData(HANDLE hArcData, PreparedHeader& out) {
	PakArchive* arc = GetArchive(hArcData);
	if (!arc) return HeaderResult::BAD_ARCHIVE;

	int idx = arc->GetAndIncrementIndex();
	if (idx >= arc->GetEntryCount()) return HeaderResult::END;

	arc->SetLastIndex(idx);

	out.entry = arc->GetEntry(idx);
	if (!out.entry) return HeaderResult::BAD_DATA;

	out.displayName = out.entry->name;

	ImageModule::AdjustDisplayName(out.displayName, out.entry->isDirectory, arc->GetImageSnapshot(), arc, idx);

	if (out.entry->timestamp != 0) {
		FILETIME ft;
		LONGLONG ll = Int32x32To64(out.entry->timestamp, 10000000) + 116444736000000000;
		ft.dwLowDateTime = (DWORD)ll;
		ft.dwHighDateTime = (DWORD)(ll >> 32);

		SYSTEMTIME st;
		if (FileTimeToSystemTime(&ft, &st)) {
			out.dosTime = SystemTimeToDosDateTime(st);
		} else {
			out.dosTime = 0;
		}
	} else {
		out.dosTime = 0;
	}

	return HeaderResult::OK;
}

static int MapHeaderResult(HeaderResult res) {
	switch (res) {
		case HeaderResult::END:         return E_END_ARCHIVE;
		case HeaderResult::BAD_ARCHIVE: return E_BAD_ARCHIVE;
		case HeaderResult::BAD_DATA:    return E_BAD_DATA;
		default:                        return 0;
	}
}

template<typename T>
void FillCommon(T& h, const PreparedHeader& ph) {
	h.PackSize = ph.entry->isDirectory ? 0 : (uint32_t)ph.entry->size;
	h.UnpSize  = ph.entry->isDirectory ? 0 : (uint32_t)ph.entry->originalSize;
	h.FileAttr = ph.entry->isDirectory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
	h.FileTime = ph.dosTime;
}

int __stdcall ReadHeader(HANDLE hArcData, tHeaderData* h) {
	PreparedHeader ph;
	HeaderResult res = PrepareHeaderData(hArcData, ph);
	if (res != HeaderResult::OK) return MapHeaderResult(res);

	*h = {};
	FillCommon(*h, ph);

	strncpy_s(h->FileName, _countof(h->FileName), ph.displayName.c_str(), _TRUNCATE);

	return 0;
}

int __stdcall ReadHeaderEx(HANDLE hArcData, tHeaderDataEx* h) {
	PreparedHeader ph;
	HeaderResult res = PrepareHeaderData(hArcData, ph);
	if (res != HeaderResult::OK) return MapHeaderResult(res);

	*h = {};
	FillCommon(*h, ph);

	strncpy_s(h->FileName, _countof(h->FileName), ph.displayName.c_str(), _TRUNCATE);

	return 0;
}

int __stdcall ReadHeaderExW(HANDLE hArcData, tHeaderDataExW* h) {
	PreparedHeader ph;
	HeaderResult res = PrepareHeaderData(hArcData, ph);
	if (res != HeaderResult::OK) return MapHeaderResult(res);

	*h = {};
	FillCommon(*h, ph);

	int len = MultiByteToWideChar(
		CP_UTF8, 0,
		ph.displayName.c_str(),
		-1,
		h->FileName,
		_countof(h->FileName)
	);

	if (len == 0) {
		h->FileName[0] = L'\0';
	}

	return 0;
}

int __stdcall ProcessFile(HANDLE hArcData, int Operation, char* DestPath, char* DestName)
{
	wchar_t wideDestPath[MAX_PATH] = { 0 };
	wchar_t wideDestName[MAX_PATH] = { 0 };

	if (DestPath)
		MultiByteToWideChar(CP_ACP, 0, DestPath, -1, wideDestPath, MAX_PATH);
	if (DestName)
		MultiByteToWideChar(CP_ACP, 0, DestName, -1, wideDestName, MAX_PATH);

	return ProcessFileW(hArcData, Operation,
		DestPath ? wideDestPath : nullptr,
		DestName ? wideDestName : nullptr);
}

static INT_PTR CALLBACK SettingsDialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);

// ============================
// Viewer / Temp check
// ============================
static bool IsViewerRequest(const std::wstring& name) {
	std::wstring lower = name;
	std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
	return lower.find(L"\\_tc\\") != std::wstring::npos ||
		   lower.find(L"/_tc/") != std::wstring::npos;
}

static bool IsSilentOperation(const wchar_t* destPath, const wchar_t* destName) {
	std::wstring fullPath;
	if (destPath) fullPath += destPath;
	if (destName) {
		if (!fullPath.empty() && fullPath.back() != L'\\' && fullPath.back() != L'/') fullPath += L"\\";
		fullPath += destName;
	}

	if (fullPath.empty()) return false;

	std::wstring lower = fullPath;
	std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

	if (lower.find(L"\\_tc\\") != std::wstring::npos || lower.find(L"/_tc/") != std::wstring::npos) return true;
	if (lower.find(L"\\temp\\") != std::wstring::npos || lower.find(L"/temp/") != std::wstring::npos) return true;
	if (lower.find(L"\\appdata\\local\\temp") != std::wstring::npos) return true;

	wchar_t systemTemp[MAX_PATH];
	if (GetTempPathW(MAX_PATH, systemTemp)) {
		std::wstring sTemp = systemTemp;
		std::transform(sTemp.begin(), sTemp.end(), sTemp.begin(), ::tolower);

		if (!sTemp.empty() && (sTemp.back() == L'\\' || sTemp.back() == L'/')) {
			sTemp.pop_back();
		}

		if (!sTemp.empty() && lower.find(sTemp) != std::wstring::npos) {
			return true;
		}
	}

	std::lock_guard<std::mutex> lock(g_SearchTextMutex);
	if (!SearchTextW.empty()) return true;

	return false;
}

// ============================
// Folder copy detection
// ============================
static bool DetectFolderCopy(const std::wstring& wDestName, const PakEntry* entry, bool isSilent) {
	if (wDestName.empty() || isSilent) return false;

	fs::path pDest(wDestName);
	fs::path pInternal(UTF8ToWString(entry->name));

	if (pDest.has_parent_path() && pInternal.has_parent_path()) {
		std::wstring destLast = pDest.parent_path().filename().wstring();
		std::wstring intLast  = pInternal.parent_path().filename().wstring();

		std::transform(destLast.begin(), destLast.end(), destLast.begin(), ::tolower);
		std::transform(intLast.begin(), intLast.end(), intLast.begin(), ::tolower);

		if (!destLast.empty() && destLast == intLast) {
			LogInfo("[ProcessFileW][DEBUG] Folder copy detected (Parent match: " + ws2s(destLast) + ")");
			return true;
		}
	}
	return false;
}

// ============================================================================
// 🔥 SETTINGS HANDLING
// ============================================================================
static int HandleSettingsFile(PakArchive* currentArchive, const PakEntry* entry, const wchar_t* DestName) {
	LogInfo("[ProcessFileW] Settings file triggered via viewer.");
	g_CurrentArchiveForDialog = currentArchive;

	INT_PTR settingsResult = DialogBoxParam(g_hModule, MAKEINTRESOURCE(IDD_ARMAPAK_SETTINGS), NULL, SettingsDialogProc, 0);

	if (settingsResult == IDOK) {
		if (DestName) {
			HANDLE hFile = CreateFileW(DestName, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
			if (hFile != INVALID_HANDLE_VALUE) {
				std::string msg = "Settings applied successfully.";
				DWORD written = 0;
				WriteFile(hFile, msg.c_str(), (DWORD)msg.length(), &written, NULL);
				CloseHandle(hFile);
			}
		}
		return 0;
	}
	return E_EABORTED;
}

// ============================================================================
// 🛠️ WORKBENCH LAUNCH
// ============================================================================
static int HandleWorkbenchLaunch(PakArchive* arc) {
	LogInfo("[ProcessFileW] Workbench launch triggered.");

	ImageModule::Snapshot originalSnapshot = arc->GetImageSnapshot();

	ImageModule::Snapshot workbenchSnap = originalSnapshot;
	workbenchSnap.enabled = false;
	arc->SetImageSnapshot(workbenchSnap);

	static std::atomic<uint64_t> counter{0};
	auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	DWORD pid = GetCurrentProcessId();
	uint64_t localCounter = counter.fetch_add(1, std::memory_order_relaxed);

	std::string tempBase = "C:\\Temp\\ArmaPak_Preview_" +
		std::to_string(pid) + "_" +
		std::to_string(now) + "_" +
		std::to_string(localCounter);

	try {
		fs::create_directories(tempBase);
		LogInfo("[Workbench] Using temp: " + tempBase);
	} catch (...) {
		LogError("[Workbench] Failed to create temp directory: " + tempBase);
		arc->SetImageSnapshot(originalSnapshot);
		return -1;
	}

	std::unordered_set<std::string> processed;
	std::string finalPath = (fs::path(tempBase) / g_CurrentEntryForDialog.name).string();

	bool ok = SmartExtractor::ExtractWithDependencies(
		arc,
		arc->GetLastIndex(),
		finalPath,
		processed
	);

	arc->SetImageSnapshot(originalSnapshot);

	if (ok) {
		WorkbenchModule::OpenInWorkbench(g_CurrentEntryForDialog.name, tempBase);

		std::thread([tempBase]() {
			LogInfo("[Cleanup] Waiting before deletion: " + tempBase);
			std::this_thread::sleep_for(std::chrono::seconds(10));

			int safetyCounter = 0;
			while (WorkbenchModule::IsWorkbenchRunning()) {
				std::this_thread::sleep_for(std::chrono::seconds(2));
				if (++safetyCounter > 300) {
					LogInfo("[Cleanup] Safety timeout reached, forcing cleanup.");
					break;
				}
			}

			std::this_thread::sleep_for(std::chrono::seconds(1));

			try {
				if (fs::exists(tempBase)) {
					fs::remove_all(tempBase);
					LogInfo("[Cleanup] Deleted temp: " + tempBase);
				}
			} catch (...) {
				LogError("[Cleanup] Failed to delete temp: " + tempBase);
			}
		}).detach();
	} else {
		LogError("[Workbench] Extraction failed for: " + g_CurrentEntryForDialog.name);
	}

	return 0;
}

// ============================
// TEST
// ============================
static int HandleTest(PakArchive* arc, const PakEntry* entry) {
	if (entry->isDirectory) return 0;

	std::vector<uint8_t> data = arc->DecompressEntryData(entry);

	auto cb = arc->GetProcessDataProc();
	if (!cb) return 0;

	const size_t CHUNK = 16384;

	if (entry->originalSize == 0) {
		std::lock_guard<std::mutex> lock(g_CallbackMutex);
		cb(const_cast<char*>(entry->name.c_str()), 0);
		return 0;
	}

	size_t pos = 0;
	while (pos < data.size()) {
		size_t chunkSize = std::min(CHUNK, data.size() - pos);

		int result;
		{
			std::lock_guard<std::mutex> lock(g_CallbackMutex);
			result = cb(const_cast<char*>(entry->name.c_str()), (int)chunkSize);
		}

		if (result == 0) {
			LogError("[ProcessFileW] PK_TEST aborted.");
			return E_EABORTED;
		}

		pos += chunkSize;
	}

	return 0;
}

// ============================
// EXTRACT
// ============================
static int HandleExtract(
	PakArchive* arc,
	int entryIndex,
	const PakEntry* entry,
	const std::wstring& wDestPath,
	const std::wstring& wDestName,
	bool isSilent
) {
	if (entry && entry->name == "pak_plugin.ini") {
		return 0;
	}

	bool isFolderCopy = DetectFolderCopy(wDestName, entry, isSilent);

	fs::path fullTargetPath;
	if (!wDestName.empty()) fullTargetPath = fs::path(wDestName);
	else if (!wDestPath.empty()) fullTargetPath = fs::path(wDestPath);
	else fullTargetPath = fs::current_path();

	fullTargetPath = fullTargetPath.lexically_normal();

	bool forceOriginalName = isSilent;

	fullTargetPath = ImageModule::ResolveExtractPath(
		fullTargetPath,
		entry->name,
		arc->GetImageSnapshot(),
		forceOriginalName,
		isFolderCopy
	);

	fs::path basePath = fullTargetPath;

	if (isFolderCopy || (basePath.has_filename() && basePath.has_extension())) {
		basePath = basePath.parent_path();
	}

	auto u8str = basePath.u8string();
	std::string baseDest(reinterpret_cast<const char*>(u8str.c_str()));

	if (entry->isDirectory) {
		fs::path dirPath = isFolderCopy ? fullTargetPath :
						   fs::path(PakArchive::BuildFinalPath(baseDest, entry->name));
		try {
			fs::create_directories(dirPath);
			return 0;
		} catch (...) {
			auto u8err = dirPath.u8string();
			LogError("[HandleExtract] Failed to create dir: " + std::string(reinterpret_cast<const char*>(u8err.c_str())));
			return E_EWRITE;
		}
	}

	bool success = false;
	std::string finalPath;

	if (isSilent || isFolderCopy) {
		auto u8dir = fullTargetPath.u8string();
		std::string direct(reinterpret_cast<const char*>(u8dir.c_str()));

		success = arc->ExtractFile(entryIndex, direct);
		finalPath = direct;
	}
	else {
		if (g_EnableSmartExtract) {
			std::unordered_set<std::string> processed;
			auto u8final = PakArchive::BuildFinalPath(baseDest, entry->name).u8string();
			finalPath = std::string(reinterpret_cast<const char*>(u8final.c_str()));
			success = SmartExtractor::ExtractWithDependencies(arc, entryIndex, finalPath, processed);
		}

		if (!success) {
			success = arc->ExtractFile(entryIndex, baseDest);
			auto u8final = PakArchive::BuildFinalPath(baseDest, entry->name).u8string();
			finalPath = std::string(reinterpret_cast<const char*>(u8final.c_str()));
		}
	}

	if (!success) {
		LogError("[HandleExtract] Extraction failed for: " + entry->name);
		return E_EWRITE;
	}

	return 0;
}

// ============================================================================
// 🔥 PROCESSFILEW
// ============================================================================
int __stdcall ProcessFileW(HANDLE hArcData, int Operation, const wchar_t* DestPath, const wchar_t* DestName)
{
	PakArchive* arc = GetArchive(hArcData);

	try {
		if (!arc) return E_BAD_ARCHIVE;

		int idx = arc->GetLastIndex();
		const PakEntry* entry = arc->GetEntry(idx);

		std::wstring currentDestPath = DestPath ? DestPath : L"";
		std::wstring wDestName = DestName ? DestName : L"";

		bool isSilent = IsSilentOperation(DestPath, DestName);

		std::wstring fullPathCheck = currentDestPath;
		if (!wDestName.empty()) {
			if (!fullPathCheck.empty() && fullPathCheck.back() != L'\\' && fullPathCheck.back() != L'/') fullPathCheck += L"\\";
			fullPathCheck += wDestName;
		}
		bool isViewer = IsViewerRequest(fullPathCheck);

		if (entry && entry->name == "pak_plugin.ini") {
			if (Operation == PK_EXTRACT) {
				bool isSearchActive = false;
				{
					std::lock_guard<std::mutex> lock(g_SearchTextMutex);
					isSearchActive = !SearchTextW.empty();
				}

				if (isViewer && !isSearchActive) {
					return HandleSettingsFile(arc, entry, DestName);
				}
				else {
					LogInfo("[ProcessFileW] Skipping virtual file: pak_plugin.ini (not a viewer request)");
					return 0;
				}
			}
		}

		if (Operation == PK_EXTRACT) {
			if (!isSilent) {
				if (currentDestPath != g_LastTargetDir) {
					g_ExtractOptionsShown = false;
					g_LastTargetDir = currentDestPath;
				}

				if (!g_ExtractOptionsShown) {
					if (g_ShowExtractPrompt) {
						g_CurrentArchiveForDialog = arc;
						if (entry) g_CurrentEntryForDialog = *entry;

						INT_PTR result = DialogBoxParam(g_hModule, MAKEINTRESOURCE(IDD_EXTRACT_OPTIONS), NULL, SettingsDialogProc, 0);

						if (result == IDC_OPEN_WORKBENCH) {
							return HandleWorkbenchLaunch(arc);
						}

						if (result == IDCANCEL) {
							return E_EABORTED;
						}

						if (result == IDOK) {
							LogInfo("[ProcessFileW] Extract dialog finished.");
						}
					}
					g_ExtractOptionsShown = true;
				}
			}
		}

		if (idx < 0 || idx >= (int)arc->GetEntryCount()) return E_NO_FILES;
		if (!entry) return E_NO_FILES;

		if (Operation == PK_TEST) {
			return HandleTest(arc, entry);
		}

		if (Operation == PK_EXTRACT) {
			return HandleExtract(arc, idx, entry, currentDestPath, wDestName, isSilent);
		}

		return 0;
	}
	catch (...) {
		LogError("[ProcessFileW] Exception");
		return E_EWRITE;
	}
}

int __stdcall CloseArchive(HANDLE hArcData) {
	if (!hArcData) {
		LogError("CloseArchive: NULL handle.");
		return E_ECLOSE;
	}

	PakArchive* archiveToClose = reinterpret_cast<PakArchive*>(hArcData);

	g_LastOperationEndTime = std::chrono::steady_clock::now();

	if (!g_OriginalArchivePath.empty() && (g_OriginalArchiveTime.dwLowDateTime != 0 || g_OriginalArchiveTime.dwHighDateTime != 0)) {

		if (g_RequireReload) {
			LogInfo("[CloseArchive] Refresh in progress, leaving touched timestamp for TC: " + g_OriginalArchivePath);
		}
		else {
			HANDLE hRestore = CreateFileA(g_OriginalArchivePath.c_str(), FILE_WRITE_ATTRIBUTES,
										 FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);

			if (hRestore != INVALID_HANDLE_VALUE) {
				if (SetFileTime(hRestore, NULL, NULL, &g_OriginalArchiveTime)) {
					LogInfo("[CloseArchive] Archive timestamp restored to exact original: " + g_OriginalArchivePath);
				} else {
					LogError("[CloseArchive] Failed to restore timestamp: " + std::to_string(GetLastError()));
				}
				CloseHandle(hRestore);
			} else {
				LogError("[CloseArchive] Failed to open archive for timestamp restoration: " + g_OriginalArchivePath);
			}

			g_OriginalArchiveTime = { 0 };
			g_OriginalArchivePath = "";
		}
	}

	try {
		if (archiveToClose) {
			delete archiveToClose;
			LogInfo("Archive successfully closed and resources released.");
			LogInfo("[CloseArchive] instance=" + std::to_string((uintptr_t)archiveToClose));
		}
	}
	catch (const std::exception& ex) {
		LogError(std::string("CloseArchive EXCEPTION during delete: ") + ex.what());
		return E_ECLOSE;
	}
	catch (...) {
		LogError("CloseArchive Unknown EXCEPTION during delete.");
		return E_ECLOSE;
	}

	g_RequireReload = false;

	return 0;
}

void __stdcall SetChangeVolProc(HANDLE hArcData, tChangeVolProc pChangeVolProc) {}

void __stdcall SetProcessDataProc(HANDLE hArcData, tProcessDataProc pProcessDataProc) {
	PakArchive* currentArchive = GetArchive(hArcData);
	if (currentArchive) {
		currentArchive->SetProcessDataProc(pProcessDataProc);
	}
}

extern "C" __declspec(dllexport) void __stdcall SetSearchTextW(const WCHAR* SearchString) {
	std::lock_guard<std::mutex> lock(g_SearchTextMutex);
	if (SearchString) {
		SearchTextW = SearchString;
		LogInfo("[SetSearchTextW] Search pattern set (W): " + WStringToUTF8(SearchTextW));
	}
	else {
		SearchTextW.clear();
		LogInfo("[SetSearchTextW] Search pattern cleared (W).");
	}
}

extern "C" __declspec(dllexport) void __stdcall SetSearchText(const char* SearchString) {
	if (SearchString) {
		int len = MultiByteToWideChar(CP_ACP, 0, SearchString, -1, nullptr, 0);
		if (len > 0) {
			std::wstring wideTemp(len - 1, L'\0');
			MultiByteToWideChar(CP_ACP, 0, SearchString, -1, &wideTemp[0], len);
			SetSearchTextW(wideTemp.c_str());
		}
	}
	else {
		SetSearchTextW(nullptr);
	}
}

static INT_PTR CALLBACK AboutDialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
	switch (message) {
	case WM_INITDIALOG: {
		HWND hLogoCtrl = GetDlgItem(hDlg, IDC_ABOUT_LOGO);
		if (hLogoCtrl) {
			RECT rect;
			GetClientRect(hLogoCtrl, &rect);
			int width = rect.right - rect.left;
			int height = rect.bottom - rect.top;

			HBITMAP hBmp = (HBITMAP)LoadImage(g_hModule,
											MAKEINTRESOURCE(IDB_LOGO),
											IMAGE_BITMAP,
											width,
											height,
											LR_CREATEDIBSECTION);

			if (hBmp) {
				SendMessage(hLogoCtrl, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)hBmp);
			} else {
				LogError("Failed to load/scale About dialog bitmap IDB_LOGO.");
			}
		}

		std::wstring versionText = L"ARMA PAK Plugin v" + UTF8ToWString(PLUGIN_VERSION_STRING);
		SetDlgItemTextW(hDlg, IDC_ABOUT_VERSION_TEXT, versionText.c_str());
		SetDlgItemTextW(hDlg, IDC_ABOUT_AUTHOR_TEXT, L"by Icebird");
		SetDlgItemTextW(hDlg, IDC_ABOUT_SUPPORT_TEXT, L"\u00A9 2026 Icebird. All rights reserved.");

		return TRUE;
	}
	case WM_COMMAND: {
		if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
			EndDialog(hDlg, LOWORD(wParam));
			return TRUE;
		}
		break;
	}
	case WM_DESTROY: {
		HWND hLogoCtrl = GetDlgItem(hDlg, IDC_ABOUT_LOGO);
		if (hLogoCtrl) {
			HBITMAP hBmp = (HBITMAP)SendMessage(hLogoCtrl, STM_GETIMAGE, IMAGE_BITMAP, 0);
			if (hBmp) {
				DeleteObject(hBmp);
			}
		}
		return TRUE;
	}
	}
	return FALSE;
}

// ============================================================================
// ⚙️ SETTINGS DIALOG PROC
// ============================================================================
static INT_PTR CALLBACK SettingsDialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
	static bool isSettingsOnlyMode = false;

	auto RefreshImageUI = [hDlg]() {
		if (g_CurrentArchiveForDialog) {
			ImageModule::UpdateSettingsUI(hDlg, g_CurrentArchiveForDialog);
		}
	};

	switch (message) {
	case WM_INITDIALOG: {
		isSettingsOnlyMode = (lParam == 1);

		HWND hLogoCtrl = GetDlgItem(hDlg, IDC_LOGO);
		if (hLogoCtrl) {
			RECT rect = { 0 };
			GetClientRect(hLogoCtrl, &rect);
			HBITMAP hBmp = (HBITMAP)LoadImage(g_hModule, MAKEINTRESOURCE(IDB_LOGO), IMAGE_BITMAP,
											rect.right - rect.left, rect.bottom - rect.top, LR_CREATEDIBSECTION);
			if (hBmp) {
				SendMessage(hLogoCtrl, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)hBmp);
			} else {
				LogError("Failed to load/scale settings dialog bitmap IDB_LOGO.");
			}
		}

		CheckDlgButton(hDlg, IDC_ENABLE_SMART_EXTRACT, g_EnableSmartExtract ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(hDlg, IDC_KEEP_STRUCT, g_KeepDirectoryStructure ? BST_CHECKED : BST_UNCHECKED);

		if (GetDlgItem(hDlg, IDC_ENABLE_LOG_INFO)) {
			CheckDlgButton(hDlg, IDC_ENABLE_LOG_INFO, g_EnableLogInfo ? BST_CHECKED : BST_UNCHECKED);
		}

		if (GetDlgItem(hDlg, IDC_SHOW_EXTRACT_PROMPT)) {
			CheckDlgButton(hDlg, IDC_SHOW_EXTRACT_PROMPT, g_ShowExtractPrompt ? BST_CHECKED : BST_UNCHECKED);
		}

		ImageModule::InitSettingsUI(hDlg);

		HWND hWorkbenchBtn = GetDlgItem(hDlg, IDC_OPEN_WORKBENCH);
		if (hWorkbenchBtn) {
			bool shouldShow = false;
			if (!g_CurrentEntryForDialog.name.empty() && !g_CurrentEntryForDialog.isDirectory) {
				std::string ext = fs::path(g_CurrentEntryForDialog.name).extension().string();
				std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
				if (WorkbenchModule::IsWorkbenchSupported(ext)) shouldShow = true;
			}
			ShowWindow(hWorkbenchBtn, shouldShow ? SW_SHOW : SW_HIDE);
		}

		RefreshImageUI();
		return TRUE;
	}

	case WM_COMMAND: {
		WORD controlId = LOWORD(wParam);
		WORD notification = HIWORD(wParam);

		if (ImageModule::HandleSettingsCommand(hDlg, controlId, notification)) {
			RefreshImageUI();
			return TRUE;
		}

		switch (controlId) {
		case IDOK: {
			g_EnableSmartExtract = (IsDlgButtonChecked(hDlg, IDC_ENABLE_SMART_EXTRACT) == BST_CHECKED);
			g_KeepDirectoryStructure = (IsDlgButtonChecked(hDlg, IDC_KEEP_STRUCT) == BST_CHECKED);

			if (GetDlgItem(hDlg, IDC_ENABLE_LOG_INFO)) {
				g_EnableLogInfo = (IsDlgButtonChecked(hDlg, IDC_ENABLE_LOG_INFO) == BST_CHECKED);
			}

			if (GetDlgItem(hDlg, IDC_SHOW_EXTRACT_PROMPT)) {
				g_ShowExtractPrompt = (IsDlgButtonChecked(hDlg, IDC_SHOW_EXTRACT_PROMPT) == BST_CHECKED);
			}

			if (ImageModule::ApplyAndRefreshIfNeeded(hDlg, g_CurrentArchiveForDialog, isSettingsOnlyMode)) {
				g_RequireReload = true;
				g_LastOpenedArcName = "";
			} else {
				LogInfo("[Settings] Refresh skipped (No image change or settings-only mode).");
			}

			SaveSettings();
			EndDialog(hDlg, IDOK);
			return TRUE;
		}
		case IDC_OPEN_WORKBENCH: {
			EndDialog(hDlg, IDC_OPEN_WORKBENCH);
			return TRUE;
		}
		case IDCANCEL: {
			EndDialog(hDlg, LOWORD(wParam));
			return TRUE;
		}
		case IDC_ABOUT_BUTTON: {
			DialogBoxParam(g_hModule, MAKEINTRESOURCE(IDD_ABOUTBOX), hDlg, AboutDialogProc, 0);
			return TRUE;
		}
		}
		break;
	}

	case WM_DESTROY: {
		HWND hLogoCtrl = GetDlgItem(hDlg, IDC_LOGO);
		if (hLogoCtrl) {
			HBITMAP hBmp = (HBITMAP)SendMessage(hLogoCtrl, STM_GETIMAGE, IMAGE_BITMAP, 0);
			if (hBmp) DeleteObject(hBmp);
		}
		return TRUE;
	}
	}
	return FALSE;
}

// ============================================================================
// EXPORTED FUNCTIONS
// ============================================================================
extern "C" __declspec(dllexport) int __stdcall ConfigurePacker(HWND Parent, HINSTANCE DllInstance) {
	g_LastOpenedArcName = "";
	DialogBoxParam(DllInstance, MAKEINTRESOURCE(IDD_ARMAPAK_SETTINGS), Parent, SettingsDialogProc, 1);
	return 0;
}

extern "C" __declspec(dllexport) void __stdcall About(HWND Parent) {
	DialogBoxParam(g_hModule, MAKEINTRESOURCE(IDD_ABOUTBOX), Parent, AboutDialogProc, 0);
}

extern "C" __declspec(dllexport) int __stdcall GetPackerCaps() {
	return PK_CAPS_MULTIPLE | PK_CAPS_BY_CONTENT | PK_CAPS_OPTIONS | PK_CAPS_SEARCHTEXT | PK_CAPS_MEMPACK;
}

// ============================================================================
// DLL MAIN
// ============================================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
	switch (ul_reason_for_call) {
	case DLL_PROCESS_ATTACH:
		g_hModule = hModule;

		{
			std::error_code ec;
			if (!fs::exists(GetIniPath(), ec)) {
				SaveSettings();
			} else {
				LoadSettings();
			}
		}

		if (!g_ThreadPool) {
			unsigned int numThreads = std::thread::hardware_concurrency();
			if (numThreads == 0) numThreads = 4;
			g_ThreadPool = std::make_unique<ThreadPool>(numThreads);
		}

		{
			std::string logFilePath = GetLogPath();
			debugLog.open(logFilePath, std::ios::out | std::ios::app);
			if (debugLog.is_open()) {
				logInitialized = true;
				time_t now = time(nullptr);
				char timeStr[26];
				ctime_s(timeStr, sizeof(timeStr), &now);
				if (timeStr[strlen(timeStr) - 1] == '\n') {
					timeStr[strlen(timeStr) - 1] = '\0';
				}
				debugLog << "\n\n=== DLL ATTACH: New session === " << timeStr << "\n";
				debugLog << "Plugin version: " << PLUGIN_VERSION_STRING << "\n";

				debugLog << "[INIT] Settings loaded:\n";
				debugLog << " - Smart Extract: " << (g_EnableSmartExtract ? "Enabled" : "Disabled") << "\n";
				debugLog << " - Keep Directory Structure: " << (g_KeepDirectoryStructure ? "Enabled" : "Disabled") << "\n";
				debugLog << " - Verbose Logging: " << (g_EnableLogInfo ? "Enabled" : "Disabled") << "\n";

				debugLog.flush();
			} else {
				logInitialized = false;
			}
		}
		break;
	case DLL_PROCESS_DETACH:
		if (g_ThreadPool) {
			g_ThreadPool.reset();
		}

		if (logInitialized && debugLog.is_open()) {
			debugLog << "[INFO] === DLL DETACH: Session end ===\n";
			debugLog.close();
		}
		break;
	}
	return TRUE;
}

bool SmartExtractor::ExtractWithDependencies(PakArchive* sourceArc, int index, const std::string& destPath, std::unordered_set<std::string>& processed)
{
	struct TaskInfo {
		int entryIndex;
		std::string targetFullPath;
		PakArchive* sourceArchive;
	};

	static std::mutex g_BucketMutexes[64];
	auto GetBucketLock = [](const std::string& path) -> std::mutex& {
		size_t h = std::hash<std::string>{}(path);
		return g_BucketMutexes[h % 64];
	};

	static std::mutex g_FileWriteMutex;

	std::vector<std::future<bool>> activeTasks;
	std::queue<TaskInfo> pendingTasks;

	const PakEntry* rootEntry = sourceArc->GetEntry(index);
	if (!rootEntry) return false;

	fs::path winDestFile(destPath);

	bool isViewer = winDestFile.has_filename() && winDestFile.extension() != "";
	fs::path baseExtractionDir = winDestFile.parent_path();
	fs::path rootParent = fs::path(rootEntry->name).parent_path();

	fs::path finalRootPath;
	if (isViewer) {
		finalRootPath = winDestFile;
	} else {
		finalRootPath = PakArchive::BuildFinalPath(baseExtractionDir.string(), rootEntry->name);
	}

	pendingTasks.push({ index, finalRootPath.string(), sourceArc });

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
			if (!fs::exists(fPath.parent_path())) {
				fs::create_directories(fPath.parent_path());
			}

			std::string ext = fs::path(entry->name).extension().string();
			std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

			if (ext == ".xob" || ext == ".emat") {
				try {
					static std::mutex g_DecompressMutex;
					std::vector<uint8_t> data;
					{
						std::lock_guard<std::mutex> lock(g_DecompressMutex);
						data = current.sourceArchive->DecompressEntryData(entry);
					}

					auto deps = FindDependencies(current.sourceArchive, data);

					for (const auto& depLine : deps) {
						LogInfo("[DEP RAW] " + depLine);

						std::string cleanPath = depLine;

						size_t bracePos = cleanPath.find('}');
						if (bracePos != std::string::npos) cleanPath = cleanPath.substr(bracePos + 1);

						auto startIdx = cleanPath.find_first_not_of(" \t\n\r");
						auto endIdx = cleanPath.find_last_not_of(" \t\n\r");
						if (startIdx == std::string::npos || endIdx == std::string::npos) continue;
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

								fs::path relDep = fs::relative(depEntry->name, rootParent);
								fs::path subDest = baseExtractionDir / relDep;

								int depIndex = targetArchive->GetEntryIndex(depEntry);
								if (depIndex != -1) {
									pendingTasks.push({ depIndex, subDest.string(), targetArchive });
								} else {
									LogInfo("[DEP ERROR] Index mapping failed for: " + depEntry->name);
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
				activeTasks.push_back(g_ThreadPool->enqueue([src = current.sourceArchive, idx = current.entryIndex, p = current.targetFullPath, &GetBucketLock]() {

					if (fs::exists(p)) {
						LogInfo("[SKIP] Already exists: " + p);
						return true;
					}

					std::lock_guard<std::mutex> lock(GetBucketLock(p));

					if (fs::exists(p)) {
						LogInfo("[SKIP AFTER LOCK] Already exists: " + p);
						return true;
					}

					return src->ExtractFile(idx, p);
				}));
			}
			else {
				std::lock_guard<std::mutex> lock(GetBucketLock(current.targetFullPath));

				if (fs::exists(current.targetFullPath)) {
					LogInfo("[SKIP] Already exists: " + current.targetFullPath);
					continue;
				}

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

	const std::vector<std::string> extensions = { ".emat", ".edds", ".xob", ".gamemat" };

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

// ============================================================================
// EXTERNAL HELPERS
// ============================================================================

void SetArchiveSnapshotExternal(void* arc_ptr, ImageModule::Snapshot snap) {
	if (arc_ptr) reinterpret_cast<PakArchive*>(arc_ptr)->SetImageSnapshot(snap);
}

ImageModule::Snapshot GetArchiveSnapshotExternal(void* arc_ptr) {
	if (arc_ptr) return reinterpret_cast<PakArchive*>(arc_ptr)->GetImageSnapshot();
	return ImageModule::Snapshot();
}

std::string GetArchiveFilenameExternal(void* arc_ptr) {
	if (arc_ptr) return reinterpret_cast<PakArchive*>(arc_ptr)->GetFilename();
	return "";
}

int FindIndexByNameExternal(void* arc_ptr, const std::string& name) {
	if (arc_ptr) return reinterpret_cast<PakArchive*>(arc_ptr)->FindIndexByName(name);
	return -1;
}
