#define NOMINMAX

#include "wcxhead.h"
#include "edds_converter.h"
#include "ThreadPool.h"

#include <windows.h>
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
#include "resource.h"
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

namespace fs = std::filesystem;

const char* PLUGIN_VERSION_STRING = "1.1.6";

extern std::unique_ptr<ThreadPool> g_ThreadPool;

class PakArchive;
class PakIndex;

class PakEntry {
public:
	enum class CompressionType : uint32_t {
		None = 0,
		Zlib = 0x106
	};

	std::string name;
	uint32_t offset = 0;
	uint32_t size = 0;
	uint32_t originalSize = 0;
	CompressionType compression = CompressionType::None;
	bool isDirectory = false;
	std::vector<std::shared_ptr<PakEntry>> children;
};

#include "SmartExtractor.h"
#include "pak_index.h"

void LogError(const std::string& message);
void LogInfo(const std::string& message);

static std::ofstream debugLog;
static bool logInitialized = false;
static std::mutex g_LogMutex;
std::mutex g_CallbackMutex;
static std::mutex g_SearchTextMutex;

std::vector<PakArchive*> g_OpenedArchives;
std::mutex g_ArchivesMutex;

bool g_EnableEddsConversion = true;
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
const char* const INI_KEY_EDDS_CONVERSION = "EnableEddsConversion";
const char* const INI_KEY_LOG_INFO = "EnableLogInfo";
const char* const LOG_FILE_NAME = "pak_plugin.log";

static HMODULE g_hModule = NULL;

static std::string GetPluginPath() {
	char path[MAX_PATH];
	if (g_hModule == NULL) {
		GetModuleFileNameA(GetModuleHandleA("pak_plugin.wcx"), path, MAX_PATH);
	} else {
		GetModuleFileNameA(g_hModule, path, MAX_PATH);
	}
	fs::path plugin_path(path);
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
	g_EnableEddsConversion = GetPrivateProfileIntA(INI_SECTION_NAME, INI_KEY_EDDS_CONVERSION, 1, iniPath.c_str()) != 0;
	g_EnableLogInfo = GetPrivateProfileIntA(INI_SECTION_NAME, INI_KEY_LOG_INFO, 0, iniPath.c_str()) != 0;
	g_EnableSmartExtract = GetPrivateProfileIntA(INI_SECTION_NAME, INI_KEY_SMART_EXTRACT, 1, iniPath.c_str()) != 0;
	g_KeepDirectoryStructure = GetPrivateProfileIntA(INI_SECTION_NAME, INI_KEY_KEEP_STRUCT, 1, iniPath.c_str()) != 0;
	g_ShowExtractPrompt = GetPrivateProfileIntA(INI_SECTION_NAME, "ShowExtractPrompt", 1, iniPath.c_str()) != 0;
}

static void SaveSettings() {
	std::string iniPath = GetIniPath();
	WritePrivateProfileStringA(INI_SECTION_NAME, INI_KEY_EDDS_CONVERSION, g_EnableEddsConversion ? "1" : "0", iniPath.c_str());
	WritePrivateProfileStringA(INI_SECTION_NAME, INI_KEY_LOG_INFO, g_EnableLogInfo ? "1" : "0", iniPath.c_str());
	WritePrivateProfileStringA(INI_SECTION_NAME, INI_KEY_SMART_EXTRACT, g_EnableSmartExtract ? "1" : "0", iniPath.c_str());
	WritePrivateProfileStringA(INI_SECTION_NAME, INI_KEY_KEEP_STRUCT, g_KeepDirectoryStructure ? "1" : "0", iniPath.c_str());
	WritePrivateProfileStringA(INI_SECTION_NAME, "ShowExtractPrompt", g_ShowExtractPrompt ? "1" : "0", iniPath.c_str());
}

static unsigned int SystemTimeToDosDateTime(const SYSTEMTIME& st) {
	unsigned int dosTime = 0;
	dosTime = ((st.wYear - 1980) << 25) | (st.wMonth << 21) | (st.wDay << 16) | (st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2);
	return dosTime;
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

	std::unique_ptr<PakIndex> m_index;
	tProcessDataProc m_pProcessDataProc = nullptr;

	std::unordered_map<std::string, int> m_LookupTable;

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
		for (uint32_t i = 0; i < childCount; i++) {
			uint8_t childType = ReadU8();
			uint8_t childNameLength = ReadU8();
			std::string childName = ReadString(childNameLength);
			if (childType == 0) entry->children.push_back(ReadDirectoryEntry(childName));
			else entry->children.push_back(ReadFileEntry(childName));
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
		SetFilePointerEx(hFile, li, NULL, FILE_CURRENT);

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

public:
	void BuildIndex() {
		m_LookupTable.clear();
		for (size_t i = 0; i < flatEntries.size(); ++i) {
			std::string norm = flatEntries[i]->name;
			std::replace(norm.begin(), norm.end(), '/', '\\');
			std::transform(norm.begin(), norm.end(), norm.begin(), ::tolower);
			m_LookupTable[norm] = (int)i;
		}

		if (m_index && !flatEntries.empty()) {
			m_index->Build(flatEntries);
		}
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

		static std::mutex g_GlobalReadMutex;
		std::lock_guard<std::mutex> readLock(g_GlobalReadMutex);

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

		if (entry->size > 100 * 1024 * 1024) {
			LogError("[DecompressEntryData] Entry too large (possible corruption): " + entry->name);
			throw std::runtime_error("Entry too large (possible corruption)");
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

			if (entry->originalSize > 200 * 1024 * 1024 ||
				(entry->size > 0 && entry->originalSize > entry->size * 100)) {
				LogError("[DecompressEntryData] Compression bomb detected: " + entry->name);
				throw std::runtime_error("Uncompressed size too large or suspicious ratio");
			}

			std::vector<uint8_t> processedContent(entry->originalSize);
			uLongf destLen = entry->originalSize;
			int zResult = uncompress(
				reinterpret_cast<Bytef*>(processedContent.data()), &destLen,
				reinterpret_cast<const Bytef*>(rawBuffer.data()), entry->size);

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
			hFile = CreateFileA(filename.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			if (hFile == INVALID_HANDLE_VALUE) {
				LogError("Failed to open PAK file (WinAPI): " + filename);
				throw std::runtime_error("Failed to open PAK file");
			}

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

	bool ExtractFile(int index, const std::string& destPath) {
		const PakEntry* entry = GetEntry(index);
		if (!entry || entry->isDirectory) {
			LogError("[ExtractFile] Invalid file index or directory entry: " + std::to_string(index));
			return false;
		}

		try {
			std::vector<uint8_t> processedContent;
			{
				static std::mutex g_DecompressMutex;
				std::lock_guard<std::mutex> lock(g_DecompressMutex);
				processedContent = DecompressEntryData(entry);
			}

			if (processedContent.empty() && entry->originalSize > 0) {
				LogError("[ExtractFile] Decompression failed or returned empty data for: " + entry->name);
				return false;
			}

			fs::path finalDestPath;
			fs::path inputPath(destPath);

			bool isDirectFileTarget =
				inputPath.has_filename() &&
				inputPath.extension() != "";

			if (isDirectFileTarget) {
				finalDestPath = inputPath.lexically_normal();
			} else {
				finalDestPath = BuildFinalPath(destPath, entry->name);
			}

			fs::path finalDestDirPath = finalDestPath.parent_path();
			if (!finalDestDirPath.empty() && !fs::exists(finalDestDirPath)) {
				std::error_code ec;
				if (!fs::create_directories(finalDestDirPath, ec) && ec) {
					LogError("[ExtractFile] Cannot create output directory: " + finalDestDirPath.string() + " Error: " + ec.message());
					return false;
				}
			}

			fs::path originalPath(entry->name);
			std::string originalExt = originalPath.extension().string();
			std::transform(originalExt.begin(), originalExt.end(), originalExt.begin(), ::tolower);

			bool isConverted = false;
			bool shouldConvert = (originalExt == ".edds" && g_EnableEddsConversion);

			if (shouldConvert) {
				enfusion::EddsConverter converter(std::span<const uint8_t>(processedContent.data(), processedContent.size()));
				if (converter.is_edds()) {
					LogInfo("[ExtractFile] Converting EDDS to DDS: " + entry->name);
					std::vector<uint8_t> ddsData = converter.convert();

					finalDestPath.replace_extension(".dds");

					HANDLE hFile = CreateFileW(
						finalDestPath.wstring().c_str(),
						GENERIC_WRITE,
						FILE_SHARE_READ,
						NULL,
						CREATE_ALWAYS,
						FILE_ATTRIBUTE_NORMAL,
						NULL
					);

					if (hFile == INVALID_HANDLE_VALUE) {
						LogError("[ExtractFile] Cannot open output file for writing (DDS): " + finalDestPath.string());
						return false;
					}

					DWORD written = 0;
					BOOL result = WriteFile(
						hFile,
						ddsData.data(),
						static_cast<DWORD>(ddsData.size()),
						&written,
						NULL
					);

					CloseHandle(hFile);

					if (!result || written != ddsData.size()) {
						LogError("[ExtractFile] WriteFile failed (DDS): " + finalDestPath.string());
						return false;
					}

					isConverted = true;
				} else {
					LogInfo("[ExtractFile] Entry marked as .edds but is not valid EDDS format: " + entry->name);
				}
			}

			if (!isConverted) {
				HANDLE hFile = CreateFileW(
					finalDestPath.wstring().c_str(),
					GENERIC_WRITE,
					FILE_SHARE_READ,
					NULL,
					CREATE_ALWAYS,
					FILE_ATTRIBUTE_NORMAL,
					NULL
				);

				if (hFile == INVALID_HANDLE_VALUE) {
					LogError("[ExtractFile] Cannot open output file for writing: " + finalDestPath.string());
					return false;
				}

				DWORD written = 0;
				BOOL result = WriteFile(
					hFile,
					processedContent.data(),
					static_cast<DWORD>(processedContent.size()),
					&written,
					NULL
				);

				CloseHandle(hFile);

				if (!result || written != processedContent.size()) {
					LogError("[ExtractFile] WriteFile failed: " + finalDestPath.string());
					return false;
				}
			}

			auto cb = m_pProcessDataProc;
			if (cb) {
				int cbResult = 0;
				{
					std::lock_guard<std::mutex> lock(g_CallbackMutex);
					cbResult = cb(const_cast<char*>(entry->name.c_str()), (int)entry->originalSize);
				}
				if (cbResult == 0) {
					LogInfo("[ExtractFile] Operation aborted by user via callback.");
					return false;
				}
			}

			return true;
		}
		catch (const std::exception& ex) {
			LogError("[ExtractFile] EXCEPTION for " + entry->name + ": " + std::string(ex.what()));
			return false;
		}
		catch (...) {
			LogError("[ExtractFile] Unknown EXCEPTION caught for " + entry->name);
			return false;
		}
	}
};

static PakArchive* GetArchive(HANDLE hArcData) {
	return reinterpret_cast<PakArchive*>(hArcData);
}

std::unique_ptr<ThreadPool> g_ThreadPool = nullptr;

template <typename T>
static HANDLE OpenArchiveInternal(const std::string& arcName, T* ArchiveData) {
	std::unique_ptr<PakArchive> newArchive;
	try {
		LogInfo("[OpenArchive] Opening archive: " + arcName);

		newArchive = std::make_unique<PakArchive>(arcName);

		if (!newArchive->IsInitialized()) {
			LogError("[OpenArchive] Initialization failed for: " + arcName);
			ArchiveData->OpenResult = E_EOPEN;
			return nullptr;
		}

		ArchiveData->OpenResult = 0;
		newArchive->ResetIndex();

		newArchive->BuildIndex();

		LogInfo("Successfully opened archive: " + arcName);

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
	return OpenArchiveInternal(std::string(ArchiveData->ArcName), ArchiveData);
}

HANDLE __stdcall OpenArchiveW(tOpenArchiveDataW* ArchiveDataW) {
	if (!ArchiveDataW || !ArchiveDataW->ArcName) {
		LogError("[OpenArchiveW] Invalid archive data or missing filename.");
		if (ArchiveDataW) ArchiveDataW->OpenResult = E_BAD_ARCHIVE;
		return nullptr;
	}
	return OpenArchiveInternal(WCharToUTF8(ArchiveDataW->ArcName), ArchiveDataW);
}

int __stdcall ReadHeader(HANDLE hArcData, tHeaderData* HeaderData) {
	PakArchive* currentArchive = GetArchive(hArcData);
	if (!currentArchive) {
		LogError("Invalid archive handle in ReadHeader.");
		return E_BAD_ARCHIVE;
	}

	int currentIndex = currentArchive->GetAndIncrementIndex();

	if (currentIndex >= currentArchive->GetEntryCount()) {
		return E_END_ARCHIVE;
	}

	currentArchive->SetLastIndex(currentIndex);

	const PakEntry* entry = currentArchive->GetEntry(currentIndex);
	if (!entry) {
		LogError("Failed to get entry at index " + std::to_string(currentIndex) + " in ReadHeader.");
		return E_BAD_DATA;
	}

	memset(HeaderData, 0, sizeof(tHeaderData));

	std::string displayName = entry->name;
	if (g_EnableEddsConversion && displayName.length() >= 5) {
		size_t dotPos = displayName.find_last_of('.');
		if (dotPos != std::string::npos) {
			std::string ext = displayName.substr(dotPos);
			std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
			if (ext == ".edds") {
				displayName = displayName.substr(0, dotPos) + ".dds";
			}
		}
	}

	strncpy_s(HeaderData->FileName, displayName.c_str(), sizeof(HeaderData->FileName) - 1);
	HeaderData->PackSize = entry->isDirectory ? 0 : entry->size;
	HeaderData->UnpSize = entry->isDirectory ? 0 : entry->originalSize;
	HeaderData->FileAttr = entry->isDirectory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;

	SYSTEMTIME st;
	GetSystemTime(&st);
	HeaderData->FileTime = SystemTimeToDosDateTime(st);

	return 0;
}

int __stdcall ReadHeaderEx(HANDLE hArcData, tHeaderDataEx* HeaderDataEx) {
	tHeaderData header{};
	int res = ReadHeader(hArcData, &header);
	if (res != 0) return res;

	memset(HeaderDataEx, 0, sizeof(tHeaderDataEx));
	memcpy(HeaderDataEx->FileName, header.FileName, sizeof(header.FileName));
	HeaderDataEx->PackSize = header.PackSize;
	HeaderDataEx->UnpSize = header.UnpSize;
	HeaderDataEx->FileAttr = header.FileAttr;
	HeaderDataEx->FileTime = header.FileTime;

	return 0;
}

int __stdcall ReadHeaderExW(HANDLE hArcData, tHeaderDataExW* HeaderDataExW) {
	PakArchive* currentArchive = GetArchive(hArcData);
	if (!currentArchive) return E_BAD_ARCHIVE;

	int currentIndex = currentArchive->GetAndIncrementIndex();

	if (currentIndex >= currentArchive->GetEntryCount()) {
		return E_END_ARCHIVE;
	}

	currentArchive->SetLastIndex(currentIndex);

	const PakEntry* entry = currentArchive->GetEntry(currentIndex);
	if (!entry) return E_BAD_DATA;

	memset(HeaderDataExW, 0, sizeof(tHeaderDataExW));

	std::string displayName = entry->name;
	if (g_EnableEddsConversion && displayName.length() >= 5) {
		size_t dotPos = displayName.find_last_of('.');
		if (dotPos != std::string::npos) {
			std::string ext = displayName.substr(dotPos);
			std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
			if (ext == ".edds") {
				displayName = displayName.substr(0, dotPos) + ".dds";
			}
		}
	}

	if (!displayName.empty()) {
		MultiByteToWideChar(CP_UTF8, 0, displayName.c_str(), -1, HeaderDataExW->FileName, MAX_PATH);
	} else {
		HeaderDataExW->FileName[0] = L'\0';
	}

	HeaderDataExW->PackSize = entry->isDirectory ? 0 : entry->size;
	HeaderDataExW->UnpSize = entry->isDirectory ? 0 : entry->originalSize;
	HeaderDataExW->FileAttr = entry->isDirectory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;

	SYSTEMTIME st;
	GetSystemTime(&st);
	HeaderDataExW->FileTime = SystemTimeToDosDateTime(st);

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

inline std::string ws2s(const std::wstring& wstr)
{
	if (wstr.empty()) return std::string();
	int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), NULL, 0, NULL, NULL);
	std::string strTo(size_needed, 0);
	WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
	return strTo;
}

static bool g_ExtractOptionsShown = false;

static INT_PTR CALLBACK SettingsDialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);

int __stdcall ProcessFileW(HANDLE hArcData, int Operation, const wchar_t* DestPath, const wchar_t* DestName)
{
	PakArchive* currentArchive = GetArchive(hArcData);

	try {
		if (!currentArchive) {
			LogError("[ProcessFileW] Invalid archive handle or archive not open.");
			return E_BAD_ARCHIVE;
		}

		if (Operation == PK_EXTRACT && !g_ExtractOptionsShown) {
			std::wstring wCheckName = DestName ? DestName : L"";
			bool isViewerRequest =
				(wCheckName.find(L"\\_tc\\") != std::wstring::npos) ||
				(wCheckName.find(L"/_tc/") != std::wstring::npos);

			if (!isViewerRequest) {
				g_ExtractOptionsShown = true;
				if (g_ShowExtractPrompt) {
					DialogBoxParam(g_hModule, MAKEINTRESOURCE(IDD_EXTRACT_OPTIONS), NULL, SettingsDialogProc, 0);
				}
			}
		}

		int entryIndex = currentArchive->GetLastIndex();

		if (entryIndex < 0 || entryIndex >= static_cast<int>(currentArchive->GetEntryCount())) {
			LogError("[ProcessFileW] Entry index out of range: " + std::to_string(entryIndex));
			return E_NO_FILES;
		}

		const PakEntry* entry = currentArchive->GetEntry(entryIndex);
		if (!entry) {
			LogError("[ProcessFileW] Failed to get entry at index " + std::to_string(entryIndex));
			return E_NO_FILES;
		}

		LogInfo("[ProcessFileW] Starting operation " + std::to_string(Operation) +
			" on entry: " + entry->name +
			", size=" + std::to_string(entry->size) +
			", originalSize=" + std::to_string(entry->originalSize) +
			", compression=" + std::to_string(static_cast<int>(entry->compression)));

		if (Operation == PK_TEST) {
			if (entry->isDirectory) return 0;

			std::vector<uint8_t> processedContent = currentArchive->DecompressEntryData(entry);

			const size_t CHUNK_SIZE = 16384;
			size_t pos = 0;

			auto pDataProc = currentArchive->GetProcessDataProc();
			while (pos < processedContent.size()) {
				size_t chunkSize = std::min(CHUNK_SIZE, processedContent.size() - pos);
				int result = 1;

				if (pDataProc) {
					std::lock_guard<std::mutex> lock(g_CallbackMutex);
					result = pDataProc(const_cast<char*>(entry->name.c_str()), (int)chunkSize);
				}

				if (result == 0) {
					LogError("[ProcessFileW] PK_TEST streaming aborted by user.");
					return E_EABORTED;
				}

				pos += chunkSize;
			}
			return 0;
		}

		if (Operation == PK_EXTRACT) {
			std::wstring wDestPath = DestPath ? DestPath : L"";
			std::wstring wDestName = DestName ? DestName : L"";

			bool success = false;
			std::string finalLogPath;

			bool isViewer =
				(wDestName.find(L"\\_tc\\") != std::wstring::npos) ||
				(wDestName.find(L"/_tc/") != std::wstring::npos);

			LogInfo("[ProcessFileW][DEBUG] Viewer mode flag: " + std::string(isViewer ? "TRUE" : "FALSE"));
			LogInfo("[ProcessFileW][DEBUG] DestPath: " + std::string(ws2s(wDestPath)));
			LogInfo("[ProcessFileW][DEBUG] DestName: " + std::string(ws2s(wDestName)));

			bool isFolderCopy = false;
			if (!wDestName.empty() && !isViewer) {
				fs::path pDest(wDestName);
				fs::path pInternal(UTF8ToWString(entry->name));

				if (pDest.has_parent_path() && pInternal.has_parent_path()) {
					std::wstring destLastFolder = pDest.parent_path().filename().wstring();
					std::wstring intLastFolder = pInternal.parent_path().filename().wstring();

					std::transform(destLastFolder.begin(), destLastFolder.end(), destLastFolder.begin(), ::tolower);
					std::transform(intLastFolder.begin(), intLastFolder.end(), intLastFolder.begin(), ::tolower);

					if (!destLastFolder.empty() && destLastFolder == intLastFolder) {
						isFolderCopy = true;
						LogInfo("[ProcessFileW][DEBUG] Folder copy detected (Parent match: " + ws2s(destLastFolder) + ")");
					}
				}
			}

			LogInfo("[ProcessFileW][DEBUG] Flags -> SmartExtract: " + std::to_string(g_EnableSmartExtract) +
				", FolderCopy: " + std::to_string(isFolderCopy) +
				", Viewer: " + std::to_string(isViewer));

			fs::path fullTargetPath;
			if (!wDestName.empty()) {
				fullTargetPath = fs::path(wDestName);
			} else if (!wDestPath.empty()) {
				fullTargetPath = fs::path(wDestPath);
			} else {
				fullTargetPath = fs::current_path();
			}

			fullTargetPath = fullTargetPath.lexically_normal();

			fs::path basePath = fullTargetPath;
			if (isFolderCopy || (basePath.has_filename() && basePath.extension() != "")) {
				basePath = basePath.parent_path();
			}
			std::string baseDestPath = basePath.string();

			if (entry->isDirectory) {
				fs::path dirPath = isFolderCopy ? fullTargetPath : PakArchive::BuildFinalPath(baseDestPath, entry->name);
				try {
					fs::create_directories(dirPath);
					LogInfo("[ProcessFileW] Directory created: " + dirPath.string());
					return 0;
				} catch (...) {
					LogError("[ProcessFileW] Failed to create directory: " + dirPath.string());
					return E_EWRITE;
				}
			} else {
				if (isViewer || isFolderCopy) {
					if (isFolderCopy && g_EnableSmartExtract) {
						LogInfo("[ProcessFileW][DEBUG] SmartExtract is ENABLED but SKIPPED because FolderCopy is active.");
					}

					std::string directFilePath = fullTargetPath.string();
					LogInfo("[ProcessFileW][DEBUG] Direct/Forced extraction path: " + directFilePath);

					success = currentArchive->ExtractFile(entryIndex, directFilePath);
					finalLogPath = directFilePath;
				} else {
					if (g_EnableSmartExtract) {
						std::unordered_set<std::string> processed;
						finalLogPath = PakArchive::BuildFinalPath(baseDestPath, entry->name).string();

						success = SmartExtractor::ExtractWithDependencies(currentArchive, entryIndex, finalLogPath, processed);

						if (!success) {
							LogInfo("[ProcessFileW] SmartExtractor failed, falling back to direct extraction.");
							success = currentArchive->ExtractFile(entryIndex, baseDestPath);
							finalLogPath = PakArchive::BuildFinalPath(baseDestPath, entry->name).string();
						}
					} else {
						success = currentArchive->ExtractFile(entryIndex, baseDestPath);
						finalLogPath = PakArchive::BuildFinalPath(baseDestPath, entry->name).string();
					}
				}
			}

			if (!success) {
				LogError("[ProcessFileW] Extraction failed for: " + entry->name);
				return E_EWRITE;
			}

			LogInfo("[ProcessFileW] Successfully extracted: " + finalLogPath);
			return 0;
		}

		if (Operation == PK_SKIP) {
			return 0;
		}

		return 0;
	}
	catch (const std::exception& ex) {
		LogError("[ProcessFileW] EXCEPTION: " + std::string(ex.what()));
		return E_EWRITE;
	}
	catch (...) {
		LogError("[ProcessFileW] Unknown EXCEPTION caught.");
		return E_EWRITE;
	}
}

int __stdcall CloseArchive(HANDLE hArcData) {
	PakArchive* archiveToClose = GetArchive(hArcData);
	if (!archiveToClose) {
		LogError("Invalid archive handle in CloseArchive.");
		return E_ECLOSE;
	}

	g_ExtractOptionsShown = false;

	try {
		delete archiveToClose;
		LogInfo("Archive successfully closed and resources released.");
	}
	catch (const std::exception& ex) {
		LogError(std::string("CloseArchive EXCEPTION during delete: ") + ex.what());
		return E_ECLOSE;
	}
	catch (...) {
		LogError("CloseArchive Unknown EXCEPTION during delete.");
		return E_ECLOSE;
	}

	return 0;
}

void __stdcall SetChangeVolProc(HANDLE hArcData, tChangeVolProc pChangeVolProc) {}

void __stdcall SetProcessDataProc(HANDLE hArcData, tProcessDataProc pProcessDataProc) {
	PakArchive* currentArchive = GetArchive(hArcData);
	if (currentArchive) {
		currentArchive->SetProcessDataProc(pProcessDataProc);
	}
}

extern "C" __declspec(dllexport) void __stdcall SetSearchText(const char* SearchString) {
	std::lock_guard<std::mutex> lock(g_SearchTextMutex);
	if (SearchString) {
		SearchTextW = UTF8ToWString(SearchString);
		LogInfo("[SetSearchText] Search pattern set (A): " + WStringToUTF8(SearchTextW));
	}
	else {
		SearchTextW.clear();
		LogInfo("[SetSearchText] Search pattern cleared (A).");
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

static INT_PTR CALLBACK AboutDialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
	switch (message) {
	case WM_INITDIALOG: {
		HBITMAP hBmp = LoadBitmap(g_hModule, MAKEINTRESOURCE(IDB_LOGO));
		if (hBmp) {
			SendDlgItemMessage(hDlg, IDC_ABOUT_LOGO, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)hBmp);
		}
		else {
			LogError("Failed to load About dialog bitmap IDB_LOGO.");
		}

		std::wstring versionText = L"ARMA PAK Plugin v" + UTF8ToWString(PLUGIN_VERSION_STRING);
		SetDlgItemTextW(hDlg, IDC_ABOUT_VERSION_TEXT, versionText.c_str());
		SetDlgItemTextW(hDlg, IDC_ABOUT_AUTHOR_TEXT, L"by Icebird");
		SetDlgItemTextW(hDlg, IDC_ABOUT_SUPPORT_TEXT, L"\u00A9 2026 Icebird. All rights reserved.");

		return TRUE;
	}
	case WM_COMMAND: {
		if (LOWORD(wParam) == IDOK) {
			EndDialog(hDlg, LOWORD(wParam));
			return TRUE;
		}
		break;
	}
	case WM_DESTROY: {
		HBITMAP hBmp = (HBITMAP)SendDlgItemMessage(hDlg, IDC_ABOUT_LOGO, STM_GETIMAGE, IMAGE_BITMAP, 0);
		if (hBmp) {
			DeleteObject(hBmp);
		}
		return TRUE;
	}
	}
	return FALSE;
}

static INT_PTR CALLBACK SettingsDialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
	switch (message) {
	case WM_INITDIALOG: {
		HWND hLogoCtrl = GetDlgItem(hDlg, IDC_LOGO);
		if (hLogoCtrl) {
			HBITMAP hBmp = LoadBitmap(g_hModule, MAKEINTRESOURCE(IDB_LOGO));
			if (hBmp) {
				SendMessage(hLogoCtrl, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)hBmp);
			} else {
				LogError("Failed to load settings dialog bitmap IDB_LOGO.");
			}
		}

		CheckDlgButton(hDlg, IDC_ENABLE_EDDS_CONVERSION, g_EnableEddsConversion ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(hDlg, IDC_ENABLE_SMART_EXTRACT, g_EnableSmartExtract ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(hDlg, IDC_KEEP_STRUCT, g_KeepDirectoryStructure ? BST_CHECKED : BST_UNCHECKED);

		if (GetDlgItem(hDlg, IDC_ENABLE_LOG_INFO)) {
			CheckDlgButton(hDlg, IDC_ENABLE_LOG_INFO, g_EnableLogInfo ? BST_CHECKED : BST_UNCHECKED);
		}

		if (GetDlgItem(hDlg, IDC_SHOW_EXTRACT_PROMPT)) {
			CheckDlgButton(hDlg, IDC_SHOW_EXTRACT_PROMPT, g_ShowExtractPrompt ? BST_CHECKED : BST_UNCHECKED);
		}

		return TRUE;
	}
	case WM_COMMAND: {
		switch (LOWORD(wParam)) {
		case IDOK: {
			g_EnableEddsConversion = (IsDlgButtonChecked(hDlg, IDC_ENABLE_EDDS_CONVERSION) == BST_CHECKED);
			g_EnableSmartExtract = (IsDlgButtonChecked(hDlg, IDC_ENABLE_SMART_EXTRACT) == BST_CHECKED);
			g_KeepDirectoryStructure = (IsDlgButtonChecked(hDlg, IDC_KEEP_STRUCT) == BST_CHECKED);

			if (GetDlgItem(hDlg, IDC_ENABLE_LOG_INFO)) {
				g_EnableLogInfo = (IsDlgButtonChecked(hDlg, IDC_ENABLE_LOG_INFO) == BST_CHECKED);
			}

			if (GetDlgItem(hDlg, IDC_SHOW_EXTRACT_PROMPT)) {
				g_ShowExtractPrompt = (IsDlgButtonChecked(hDlg, IDC_SHOW_EXTRACT_PROMPT) == BST_CHECKED);
			}

			SaveSettings();

			LogInfo("EDDS to DDS conversion setting: " + std::string(g_EnableEddsConversion ? "Enabled" : "Disabled"));
			LogInfo("Log Info messages setting: " + std::string(g_EnableLogInfo ? "Enabled" : "Disabled"));
			LogInfo("Smart Extract setting: " + std::string(g_EnableSmartExtract ? "Enabled" : "Disabled"));
			LogInfo("Keep Directory Structure setting: " + std::string(g_KeepDirectoryStructure ? "Enabled" : "Disabled"));
			LogInfo("Show Extract Prompt setting: " + std::string(g_ShowExtractPrompt ? "Enabled" : "Disabled"));

			EndDialog(hDlg, LOWORD(wParam));
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
			if (hBmp) {
				DeleteObject(hBmp);
			}
		}
		return TRUE;
	}
	}
	return FALSE;
}

extern "C" __declspec(dllexport) int __stdcall ConfigurePacker(HWND Parent, HINSTANCE DllInstance) {
	DialogBoxParam(DllInstance, MAKEINTRESOURCE(IDD_ARMAPAK_SETTINGS), Parent, SettingsDialogProc, 0);
	return 0;
}

extern "C" __declspec(dllexport) void __stdcall About(HWND Parent) {
	DialogBoxParam(g_hModule, MAKEINTRESOURCE(IDD_ABOUTBOX), Parent, AboutDialogProc, 0);
}

extern "C" __declspec(dllexport) int __stdcall GetPackerCaps() {
	return PK_CAPS_MULTIPLE | PK_CAPS_BY_CONTENT | PK_CAPS_OPTIONS | PK_CAPS_SEARCHTEXT | PK_CAPS_MEMPACK;
}

static BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
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
				debugLog << " - EDDS Conversion: " << (g_EnableEddsConversion ? "Enabled" : "Disabled") << "\n";
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

								std::string realExt = fs::path(depEntry->name).extension().string();
								std::transform(realExt.begin(), realExt.end(), realExt.begin(), ::tolower);

								if (realExt == ".edds" && g_EnableEddsConversion) {
									subDest.replace_extension(".dds");
								}

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
				activeTasks.push_back(g_ThreadPool->enqueue([src = current.sourceArchive, idx = current.entryIndex, p = current.targetFullPath]() {

					if (fs::exists(p)) {
						LogInfo("[SKIP] Already exists: " + p);
						return true;
					}

					std::mutex* fLock = nullptr;
					{
						std::lock_guard<std::mutex> lock(g_FileWriteLocksMutex);
						if (g_FileWriteLocks.find(p) == g_FileWriteLocks.end()) {
							g_FileWriteLocks[p] = std::make_unique<std::mutex>();
						}
						fLock = g_FileWriteLocks[p].get();
					}

					std::lock_guard<std::mutex> lock(*fLock);

					if (fs::exists(p)) {
						LogInfo("[SKIP AFTER LOCK] Already exists: " + p);
						return true;
					}

					return src->ExtractFile(idx, p);
				}));
			}
			else {
				std::lock_guard<std::mutex> lock(g_FileWriteMutex);

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
