#define NOMINMAX

#include "wcxhead.h"
#include "edds_converter.h"

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
#include <codecvt>
#include <locale>

namespace fs = std::filesystem;

const char* PLUGIN_VERSION_STRING = "1.0.0";

class PakArchive;
class PakEntry;

void LogError(const std::string& message);
static void LogInfo(const std::string& message);
static void LogHexDump(const std::vector<uint8_t>& data, const std::string& prefix = "");

static std::ofstream debugLog;
static bool logInitialized = false;
static tProcessDataProc processDataProc = nullptr;
static std::unique_ptr<PakArchive> currentArchive;
static int currentIndex = 0;

static bool g_EnableEddsConversion = true;
static bool g_EnableLogInfo = false;
static std::wstring SearchTextW;

const char* const INI_FILE_NAME = "pak_plugin.ini";
const char* const INI_SECTION_NAME = "Settings";
const char* const INI_KEY_EDDS_CONVERSION = "EnableEddsConversion";
const char* const INI_KEY_LOG_INFO = "EnableLogInfo";
const char* const LOG_FILE_NAME = "pak_plugin.log";

static HMODULE g_hModule = NULL;


static std::string GetPluginPath() {
	char path[MAX_PATH];
	GetModuleFileNameA(g_hModule, path, MAX_PATH);
	fs::path plugin_path(path);
	return plugin_path.parent_path().string();
}

static std::string GetLogPath() {
	return GetPluginPath() + "\\" + LOG_FILE_NAME;
}

static std::string GetIniPath() {
	return GetPluginPath() + "\\" + INI_FILE_NAME;
}

void LogError(const std::string& message) {
	if (!logInitialized) {
		std::string logFilePath = GetLogPath();
		debugLog.open(logFilePath, std::ios::out | std::ios::app);
		if (!debugLog.is_open()) {
			std::string msg = "Failed to create log file! Check permissions. Log path: " + logFilePath;
			MessageBoxA(NULL, msg.c_str(), "Error", MB_ICONERROR);
			return;
		}

		time_t now = time(nullptr);
		char timeStr[26];
		ctime_s(timeStr, sizeof(timeStr), &now);
		if (timeStr[strlen(timeStr) - 1] == '\n') {
			timeStr[strlen(timeStr) - 1] = '\0';
		}
		debugLog << "\n\n=== New session === " << timeStr << "\n";
		debugLog << "Plugin version: " << PLUGIN_VERSION_STRING << "\n";
		logInitialized = true;
	}

	debugLog << "[ERROR] " << message << "\n";
	debugLog.flush();
}

static void LogInfo(const std::string& message) {
	if (!g_EnableLogInfo) return;
	if (logInitialized) {
		debugLog << "[INFO] " << message << "\n";
		debugLog.flush();
	}
}

static void LogHexDump(const std::vector<uint8_t>& data, const std::string& prefix) {
	if (!logInitialized || data.empty() || !g_EnableLogInfo) return;

	std::stringstream ss;
	ss << prefix << "First " << std::min((size_t)32, data.size()) << " bytes HEX: ";
	for (size_t i = 0; i < 32 && i < data.size(); ++i) {
		ss << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << (int)data[i] << " ";
	}
	LogInfo(ss.str());
}

static void LoadSettings() {
	std::string iniPath = GetIniPath();
	g_EnableEddsConversion = GetPrivateProfileIntA(INI_SECTION_NAME, INI_KEY_EDDS_CONVERSION, 1, iniPath.c_str()) != 0;
	g_EnableLogInfo = GetPrivateProfileIntA(INI_SECTION_NAME, INI_KEY_LOG_INFO, 0, iniPath.c_str()) != 0;
}

static void SaveSettings() {
	std::string iniPath = GetIniPath();
	WritePrivateProfileStringA(INI_SECTION_NAME, INI_KEY_EDDS_CONVERSION, g_EnableEddsConversion ? "1" : "0", iniPath.c_str());
	WritePrivateProfileStringA(INI_SECTION_NAME, INI_KEY_LOG_INFO, g_EnableLogInfo ? "1" : "0", iniPath.c_str());
}

static unsigned int SystemTimeToDosDateTime(const SYSTEMTIME& st) {
	unsigned int dosTime = 0;

	dosTime = ((st.wYear - 1980) << 25)
		| (st.wMonth << 21)
		| (st.wDay << 16)
		| (st.wHour << 11)
		| (st.wMinute << 5)
		| (st.wSecond / 2);

	return dosTime;
}

static std::string WStringToUTF8(const std::wstring& ws) {
	if (ws.empty()) return {};
	int size_needed = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
	std::string result(size_needed, 0);
	WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), &result[0], size_needed, nullptr, nullptr);
	return result;
}

static std::string WCharToUTF8(const WCHAR* ws) {
	if (!ws) return {};
	int size_needed = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
	if (size_needed == 0) return {};
	std::string result(size_needed - 1, 0);
	WideCharToMultiByte(CP_UTF8, 0, ws, -1, &result[0], size_needed, nullptr, nullptr);
	return result;
}

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

class PakArchive {
private:
	std::ifstream file;
	std::shared_ptr<PakEntry> root;
	std::vector<std::shared_ptr<PakEntry>> flatEntries;
	std::string filename;
	bool initialized = false;
	long long actualFileSize = 0;

	uint32_t ReadU32BE() {
		uint32_t value = 0;
		if (!file.read(reinterpret_cast<char*>(&value), sizeof(value))) {
			throw std::runtime_error("Failed to read U32BE");
		}
		return _byteswap_ulong(value);
	}

	uint32_t ReadU32LE() {
		uint32_t value = 0;
		if (!file.read(reinterpret_cast<char*>(&value), sizeof(value))) {
			throw std::runtime_error("Failed to read U32LE");
		}
		return value;
	}

	uint8_t ReadU8() {
		uint8_t value = 0;
		if (!file.read(reinterpret_cast<char*>(&value), sizeof(value))) {
			throw std::runtime_error("Failed to read U8");
		}
		return value;
	}

	std::string ReadString(uint8_t length) {
		if (length == 0) return "";
		std::string str(length, '\0');
		if (!file.read(&str[0], length)) {
			throw std::runtime_error("Failed to read string of length " + std::to_string(length));
		}
		return str;
	}

	bool ProcessHeadChunk(uint32_t length) {
		file.seekg(static_cast<std::streamoff>(length), std::ios::cur);
		return true;
	}

	bool ProcessFileChunk(uint32_t length) {
		uint8_t entryType = ReadU8();
		uint8_t nameLength = ReadU8();
		std::string name = ReadString(nameLength);

		if (entryType == 0) {
			auto entry = ReadDirectoryEntry(name);
			root->children.push_back(entry);
		}
		else {
			auto entry = ReadFileEntry(name);
			root->children.push_back(entry);
		}

		return true;
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

			if (childType == 0) {
				auto childEntry = ReadDirectoryEntry(childName);
				entry->children.push_back(childEntry);
			}
			else {
				auto childEntry = ReadFileEntry(childName);
				entry->children.push_back(childEntry);
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
		file.seekg(4, std::ios::cur);
		entry->compression = static_cast<PakEntry::CompressionType>(ReadU32BE());
		file.seekg(4, std::ios::cur);
		return entry;
	}

	void FlattenEntries(const std::shared_ptr<PakEntry>& entry, const std::string& path = "") {
		std::string fullPath = path;
		if (entry.get() != root.get()) {
			if (!fullPath.empty()) {
				fullPath += "\\";
			}
			fullPath += entry->name;
		}

		if (entry.get() != root.get()) {
			auto flatEntry = std::make_shared<PakEntry>(*entry);
			flatEntry->name = fullPath;
			flatEntries.push_back(flatEntry);
		}

		for (const auto& child : entry->children) {
			FlattenEntries(child, fullPath);
		}
	}

public:
	std::vector<uint8_t> DecompressEntryData(const PakEntry* entry) {
		if (!entry || entry->isDirectory) {
			throw std::runtime_error("Invalid or directory entry for decompression.");
		}

		std::ifstream input(filename, std::ios::binary);
		if (!input.is_open()) {
			throw std::runtime_error("Cannot reopen archive: " + filename);
		}

		input.seekg(0, std::ios::end);
		size_t fileSize = static_cast<size_t>(input.tellg());
		if (entry->offset + entry->size > fileSize) {
			LogError("[DecompressEntryData] Entry data goes beyond archive bounds: " + entry->name);
			throw std::runtime_error("Entry data out of bounds.");
		}

		input.seekg(static_cast<std::streamoff>(entry->offset), std::ios::beg);

		std::vector<uint8_t> rawFileContent(entry->size);
		input.read(reinterpret_cast<char*>(rawFileContent.data()), entry->size);
		if (input.gcount() != static_cast<std::streamsize>(entry->size)) {
			LogError("[DecompressEntryData] Incomplete read from PAK.");
			throw std::runtime_error("Incomplete read from PAK.");
		}

		if (entry->compression == PakEntry::CompressionType::Zlib) {
			std::vector<uint8_t> processedContent(entry->originalSize);
			uLongf destLen = entry->originalSize;
			int zResult = uncompress(
				reinterpret_cast<Bytef*>(processedContent.data()), &destLen,
				reinterpret_cast<const Bytef*>(rawFileContent.data()), entry->size);

			if (zResult != Z_OK || destLen != entry->originalSize) {
				LogError("[DecompressEntryData] Zlib decompression failed for " + entry->name);
				throw std::runtime_error("Zlib decompression failed.");
			}
			return processedContent;
		}
		else {
			return rawFileContent;
		}
	}


	PakArchive(const std::string& filename) : filename(filename) {

		try {
			file.open(filename, std::ios::binary);
			if (!file.is_open()) {
				LogError("Failed to open PAK file: " + filename);
				throw std::runtime_error("Failed to open PAK file");
			}

			file.seekg(0, std::ios::end);
			actualFileSize = file.tellg();
			file.seekg(0, std::ios::beg);

			char form[4] = { 0 };
			if (!file.read(form, 4) || strncmp(form, "FORM", 4) != 0) {
				LogError("PAK file does not start with 'FORM' signature.");
				throw std::runtime_error("Invalid PAK signature.");
			}

			uint32_t formSize = ReadU32BE();

			uint64_t expectedTotalSize = 8 + formSize;

			if (actualFileSize != static_cast<long long>(expectedTotalSize)) {
				LogError("Archive header size mismatch. Expected: " + std::to_string(expectedTotalSize) +
					", Actual: " + std::to_string(actualFileSize));

				throw std::runtime_error("Archive size mismatch (corrupt or truncated).");
			}

			char formType[4] = { 0 };
			if (!file.read(formType, 4) || strncmp(formType, "PAC1", 4) != 0) {
				LogError("PAK file FORM chunk type is not 'PAC1'.");
				throw std::runtime_error("Invalid PAK type.");
			}

			initialized = true;

			root = std::make_shared<PakEntry>();
			root->name = "";
			root->isDirectory = true;

			while (file.peek() != EOF) {
				char chunkId[4] = { 0 };
				if (!file.read(chunkId, 4)) break;

				uint32_t chunkSize = ReadU32BE();

				if (strncmp(chunkId, "HEAD", 4) == 0) {
					ProcessHeadChunk(chunkSize);
				}
				else if (strncmp(chunkId, "FILE", 4) == 0) {
					ProcessFileChunk(chunkSize);
				}
				else {
					file.seekg(static_cast<std::streamoff>(chunkSize), std::ios::cur);
				}

			}

			if (root) {
				FlattenEntries(root);
			}
		}
		catch (const std::exception& ex) {
			LogError(std::string("PakArchive construction EXCEPTION: ") + ex.what());
			initialized = false;
		}
	}

	~PakArchive() {
		if (file.is_open()) {
			file.close();
		}
	}

	bool IsInitialized() const { return initialized; }
	int GetEntryCount() const { return static_cast<int>(flatEntries.size()); }

	const PakEntry* GetEntry(int index) const {
		if (index < 0 || index >= static_cast<int>(flatEntries.size())) return nullptr;
		return flatEntries[index].get();
	}

	std::string GetFilename() const { return filename; }

	bool ExtractFile(int index, const std::string& destPath) {
		const PakEntry* entry = GetEntry(index);

		if (!entry || entry->isDirectory) {
			LogError("[ExtractFile] Invalid file index or is directory: " + std::to_string(index));
			return false;
		}

		try {
			std::vector<uint8_t> processedContent = DecompressEntryData(entry);

			fs::path originalEntryPath(entry->name);
			std::string newFileName = originalEntryPath.filename().string();
			std::string outputExtension = originalEntryPath.extension().string();

			fs::path finalDestDirPath = fs::path(destPath).parent_path();
			if (!finalDestDirPath.empty()) {
				if (!fs::create_directories(finalDestDirPath) && !fs::exists(finalDestDirPath)) {
					LogError("[ExtractFile] Cannot create output directory: " + finalDestDirPath.string());
					return false;
				}
			}

			std::string finalOutputPath = destPath;
			bool isEDDS = (outputExtension == ".edds" || outputExtension == ".EDDS");

			if (isEDDS && g_EnableEddsConversion) {
				finalOutputPath = (fs::path(destPath).parent_path() / fs::path(newFileName).replace_extension(".dds")).string();

				fs::path tempEddsPath = fs::temp_directory_path() / (fs::path(newFileName).stem().string() + "_tc_temp.edds");
				std::string tempEddsPathStr = tempEddsPath.string();

				std::ofstream tempEddsFile(tempEddsPathStr, std::ios::binary);
				if (!tempEddsFile.is_open()) {
					LogError("[ExtractFile] Failed to create temporary .edds file for conversion: " + tempEddsPathStr);
					return false;
				}
				tempEddsFile.write(reinterpret_cast<char*>(processedContent.data()), processedContent.size());
				tempEddsFile.close();

				ConvertToDDS(tempEddsPathStr, finalOutputPath);

				try {
					fs::remove(tempEddsPath);
				}
				catch (const fs::filesystem_error& e) {
					LogError("[ExtractFile] Failed to delete temporary .edds file: " + tempEddsPathStr + " - " + e.what());
				}

			}
			else {
				std::ofstream outFile(finalOutputPath, std::ios::binary);
				if (!outFile.is_open()) {
					LogError("[ExtractFile] Cannot open output file: " + finalOutputPath);
					return false;
				}
				outFile.write(reinterpret_cast<char*>(processedContent.data()), processedContent.size());
				outFile.close();
			}

			if (processDataProc) {
				processDataProc(const_cast<char*>(entry->name.c_str()), entry->originalSize);
			}

			return true;
		}
		catch (const std::exception& ex) {
			LogError(std::string("[ExtractFile] EXCEPTION: ") + ex.what());
			return false;
		}
		catch (...) {
			LogError("[ExtractFile] Unknown EXCEPTION caught.");
			return false;
		}
	}
};

HANDLE __stdcall OpenArchive(tOpenArchiveData* ArchiveData) {
	try {
		if (!ArchiveData || !ArchiveData->ArcName) {
			LogError("[OpenArchive] Invalid archive data or missing filename.");
			ArchiveData->OpenResult = E_BAD_ARCHIVE;
			return nullptr;
		}

		LogInfo("[OpenArchive] Opening archive: " + std::string(ArchiveData->ArcName));
		currentArchive = std::make_unique<PakArchive>(ArchiveData->ArcName);

		if (!currentArchive || !currentArchive->IsInitialized()) {
			currentArchive.reset();
			ArchiveData->OpenResult = E_EOPEN;
			return nullptr;
		}

		currentIndex = 0;
		ArchiveData->OpenResult = 0;
		LogInfo("Successfully opened archive: " + std::string(ArchiveData->ArcName));

		return reinterpret_cast<HANDLE>(1);
	}
	catch (const std::exception& ex) {
		LogError(std::string("OpenArchive EXCEPTION: ") + ex.what());
		currentArchive.reset();
		ArchiveData->OpenResult = E_EOPEN;
		return nullptr;
	}
	catch (...) {
		LogError("OpenArchive Unknown EXCEPTION caught.");
		currentArchive.reset();
		ArchiveData->OpenResult = E_EOPEN;
		return nullptr;
	}
}

HANDLE __stdcall OpenArchiveW(tOpenArchiveDataW* ArchiveDataW) {
	if (!ArchiveDataW || !ArchiveDataW->ArcName) {
		LogError("[OpenArchiveW] Invalid archive data or missing filename.");
		ArchiveDataW->OpenResult = E_BAD_ARCHIVE;
		return nullptr;
	}

	std::string arcName;
	if (ArchiveDataW->ArcName[0] != L'\0') {
		int size_needed = WideCharToMultiByte(CP_ACP, 0, ArchiveDataW->ArcName, -1, nullptr, 0, nullptr, nullptr);
		if (size_needed > 0) {
			arcName.resize(size_needed);
			WideCharToMultiByte(CP_ACP, 0, ArchiveDataW->ArcName, -1, arcName.data(), size_needed, nullptr, nullptr);
			arcName.resize(size_needed - 1);
		}
	}

	try {
		LogInfo("[OpenArchiveW] Opening archive: " + arcName);
		currentArchive = std::make_unique<PakArchive>(arcName);

		if (!currentArchive || !currentArchive->IsInitialized()) {
			currentArchive.reset();
			ArchiveDataW->OpenResult = E_EOPEN;
			return nullptr;
		}

		currentIndex = 0;
		ArchiveDataW->OpenResult = 0;
		LogInfo("Successfully opened archive: " + arcName);

		return reinterpret_cast<HANDLE>(1);
	}
	catch (const std::exception& ex) {
		LogError(std::string("OpenArchiveW EXCEPTION: ") + ex.what());
		currentArchive.reset();
		ArchiveDataW->OpenResult = E_EOPEN;
		return nullptr;
	}
	catch (...) {
		LogError("OpenArchiveW Unknown EXCEPTION caught.");
		currentArchive.reset();
		ArchiveDataW->OpenResult = E_EOPEN;
		return nullptr;
	}
}

int __stdcall ReadHeader(HANDLE hArcData, tHeaderData* HeaderData) {
	if (!currentArchive || hArcData != reinterpret_cast<HANDLE>(1)) {
		LogError("Invalid archive handle or archive not open in ReadHeader.");
		return E_BAD_ARCHIVE;
	}

	if (currentIndex >= currentArchive->GetEntryCount()) {
		return E_END_ARCHIVE;
	}

	const PakEntry* entry = currentArchive->GetEntry(currentIndex);
	if (!entry) {
		LogError("Failed to get entry at index " + std::to_string(currentIndex) + " in ReadHeader.");
		return E_BAD_DATA;
	}

	memset(HeaderData, 0, sizeof(tHeaderData));

	std::string displayName = entry->name;
	if (g_EnableEddsConversion) {
		fs::path entryPath(displayName);
		std::string ext = entryPath.extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
		if (ext == ".edds") displayName = entryPath.replace_extension(".dds").string();
	}

	strncpy_s(HeaderData->FileName, displayName.c_str(), sizeof(HeaderData->FileName) - 1);
	HeaderData->PackSize = entry->isDirectory ? 0 : entry->size;
	HeaderData->UnpSize = entry->isDirectory ? 0 : entry->originalSize;
	HeaderData->FileAttr = entry->isDirectory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;

	SYSTEMTIME st;
	GetSystemTime(&st);
	HeaderData->FileTime = SystemTimeToDosDateTime(st);

	currentIndex++;
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
	tHeaderDataEx headerEx{};
	int res = ReadHeaderEx(hArcData, &headerEx);
	if (res != 0) return res;

	if (headerEx.FileName[0] != '\0') {
		MultiByteToWideChar(CP_ACP, 0, headerEx.FileName, -1, HeaderDataExW->FileName, MAX_PATH);
	}
	else {
		HeaderDataExW->FileName[0] = L'\0';
	}

	HeaderDataExW->PackSize = headerEx.PackSize;
	HeaderDataExW->UnpSize = headerEx.UnpSize;
	HeaderDataExW->FileAttr = headerEx.FileAttr;
	HeaderDataExW->FileTime = headerEx.FileTime;

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

int __stdcall ProcessFileW(HANDLE hArcData, int Operation, const wchar_t* DestPath, const wchar_t* DestName)
{
	try {
		if (!currentArchive || hArcData != reinterpret_cast<HANDLE>(1)) {
			LogError("[ProcessFileW] Invalid archive handle or archive not open.");
			return E_BAD_ARCHIVE;
		}

		int entryIndex = currentIndex - 1;

		if (entryIndex < 0 || entryIndex >= static_cast<int>(currentArchive->GetEntryCount())) {
			LogError("[ProcessFileW] Entry index out of range: " + std::to_string(entryIndex));
			return E_NO_FILES;
		}

		const PakEntry* entry = currentArchive->GetEntry(entryIndex);
		if (!entry) {
			LogError("[ProcessFileW] Failed to get entry at index " + std::to_string(entryIndex));
			return E_NO_FILES;
		}

		LogInfo("[ProcessFileW] Processing entry: " + entry->name +
			", size=" + std::to_string(entry->size) +
			", originalSize=" + std::to_string(entry->originalSize) +
			", compression=" + std::to_string(static_cast<int>(entry->compression)) +
			", isDirectory=" + std::to_string(entry->isDirectory) +
			", Operation=" + std::to_string(Operation));



		if (Operation == PK_TEST) {
			if (entry->isDirectory) return 0;

			std::vector<uint8_t> processedContent = currentArchive->DecompressEntryData(entry);

			const size_t CHUNK_SIZE = 16384;
			size_t pos = 0;
			int result = 1;

			LogInfo("[ProcessFileW] PK_TEST: Streaming " + std::to_string(processedContent.size()) + " bytes to TC callback.");

			while (pos < processedContent.size()) {
				size_t chunkSize = std::min(CHUNK_SIZE, processedContent.size() - pos);

				if (processDataProc) {
					result = processDataProc(const_cast<char*>(entry->name.c_str()), (int)chunkSize);
				}

				if (result == 0) {
					LogError("[ProcessFileW] PK_TEST streaming aborted by user.");
					return E_EABORTED;
				}

				pos += chunkSize;
			}
			LogInfo("[ProcessFileW] PK_TEST streaming finished.");
			return 0;
		}

		if (Operation == PK_EXTRACT) {
			std::wstring fullDestPath = DestPath ? DestPath : L"";
			if (DestName) {
				if (!fullDestPath.empty() && fullDestPath.back() != L'\\' && fullDestPath.back() != L'/')
					fullDestPath += L"\\";
				fullDestPath += DestName;
			}

			if (entry->isDirectory) {
				if (!CreateDirectoryW(fullDestPath.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
					LogError("[ProcessFileW] Failed to create directory: " + WStringToUTF8(fullDestPath));
					return E_EWRITE;
				}
				LogInfo("[ProcessFileW] Created directory: " + WStringToUTF8(fullDestPath));
				return 0;
			}

			if (!currentArchive->ExtractFile(entryIndex, WStringToUTF8(fullDestPath))) {
				LogError("[ProcessFileW] Extraction failed for: " + entry->name);
				return E_EWRITE;
			}

			LogInfo("[ProcessFileW] Successfully extracted file: " + WStringToUTF8(fullDestPath));
			return 0;
		}

		if (Operation == PK_SKIP) {
			LogInfo("[ProcessFileW] Operation SKIP, skipping entry: " + entry->name);
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
	if (!currentArchive || hArcData != reinterpret_cast<HANDLE>(1)) {
		LogError("Invalid archive handle or archive not open in CloseArchive.");
		return E_ECLOSE;
	}
	currentArchive.reset();
	currentIndex = 0;
	LogInfo("Archive successfully closed.");
	return 0;
}

void __stdcall SetChangeVolProc(HANDLE hArcData, tChangeVolProc pChangeVolProc) {
}

void __stdcall SetProcessDataProc(HANDLE hArcData, tProcessDataProc pProcessDataProc) {
	processDataProc = pProcessDataProc;
}

extern "C" __declspec(dllexport) void __stdcall SetSearchText(const char* SearchString) {
	if (SearchString) {
		int len = MultiByteToWideChar(CP_ACP, 0, SearchString, -1, nullptr, 0);
		if (len > 0) {
			SearchTextW.resize(len - 1);
			MultiByteToWideChar(CP_ACP, 0, SearchString, -1, SearchTextW.data(), len);
		}
		LogInfo("[SetSearchText] Search pattern set (A): " + WStringToUTF8(SearchTextW));
	} else {
		SearchTextW.clear();
		LogInfo("[SetSearchText] Search pattern cleared (A).");
	}
}

extern "C" __declspec(dllexport) void __stdcall SetSearchTextW(const WCHAR* SearchString) {
	if (SearchString) {
		SearchTextW = SearchString;
		LogInfo("[SetSearchTextW] Search pattern set (W): " + WStringToUTF8(SearchTextW));
	} else {
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
		SetDlgItemTextA(hDlg, IDC_ABOUT_VERSION_TEXT, ("ARMA PAK Plugin v" + std::string(PLUGIN_VERSION_STRING)).c_str());
		SetDlgItemTextA(hDlg, IDC_ABOUT_AUTHOR_TEXT, "by Icebird");
		SetDlgItemTextA(hDlg, IDC_ABOUT_SUPPORT_TEXT, "© 2025 Icebird. All rights reserved.");
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
		HBITMAP hBmp = LoadBitmap(g_hModule, MAKEINTRESOURCE(IDB_LOGO));
		if (hBmp) {
			SendDlgItemMessage(hDlg, IDC_LOGO, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)hBmp);
		}
		else {
			LogError("Failed to load settings dialog bitmap IDB_LOGO.");
		}
		CheckDlgButton(hDlg, IDC_ENABLE_EDDS_CONVERSION, g_EnableEddsConversion ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(hDlg, IDC_ENABLE_LOG_INFO, g_EnableLogInfo ? BST_CHECKED : BST_UNCHECKED);
		return TRUE;
	}
	case WM_COMMAND: {
		switch (LOWORD(wParam)) {
		case IDOK: {
			g_EnableEddsConversion = (IsDlgButtonChecked(hDlg, IDC_ENABLE_EDDS_CONVERSION) == BST_CHECKED);
			g_EnableLogInfo = (IsDlgButtonChecked(hDlg, IDC_ENABLE_LOG_INFO) == BST_CHECKED);
			SaveSettings();
			LogInfo("EDDS to DDS conversion setting: " + std::string(g_EnableEddsConversion ? "Enabled" : "Disabled"));
			LogInfo("Log Info messages setting: " + std::string(g_EnableLogInfo ? "Enabled" : "Disabled"));
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
		HBITMAP hBmp = (HBITMAP)SendDlgItemMessage(hDlg, IDC_LOGO, STM_GETIMAGE, IMAGE_BITMAP, 0);
		if (hBmp) {
			DeleteObject(hBmp);
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
		LoadSettings();
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
				debugLog.flush();
			}
		}
		break;
	case DLL_PROCESS_DETACH:
		if (debugLog.is_open()) {
			LogInfo("=== DLL DETACH: Session end ===");
			debugLog.close();
		}
		break;
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
		break;
	}
	return TRUE;
}
