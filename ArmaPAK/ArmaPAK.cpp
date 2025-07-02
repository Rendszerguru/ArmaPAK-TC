#include <windows.h>
const int PK_CAPS_OPTIONS_DIALOG = 16;
const int CUSTOM_PK_CAPS_ABOUT = 0x00000004;
#include <vector>
#include <string>
#include <fstream>
#include <algorithm>
#include <stdexcept>
#include <zlib.h>
#include <memory>
#include <map>
#include <ctime>
#include "wcxhead.h"
#include <assert.h>
#include <filesystem>
#include <cstdint>
#include <cstring>

#include "edds_converter.h"
#include "resource.h"

namespace fs = std::filesystem;

const char* PLUGIN_VERSION_STRING = "0.9.7";

class PakArchive;

void LogError(const std::string& message);
static void LogInfo(const std::string& message);

static std::ofstream debugLog;
static bool logInitialized = false;
static tProcessDataProc processDataProc = nullptr;
static std::unique_ptr<PakArchive> currentArchive;
static int currentIndex = 0;

static bool g_EnableEddsConversion = true;

const char* const INI_FILE_NAME = "pak_plugin.ini";
const char* const INI_SECTION_NAME = "Settings";
const char* const INI_KEY_EDDS_CONVERSION = "EnableEddsConversion";
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
        debugLog << "Plugin version: " << PLUGIN_VERSION_STRING << " (with refined logging)\n";
        logInitialized = true;
    }

    debugLog << "[ERROR] " << message << "\n";
    debugLog.flush();
}

static void LogInfo(const std::string& message) {
    if (logInitialized) {
        debugLog << "[INFO] " << message << "\n";
        debugLog.flush();
    }
}

static void LoadSettings() {
    std::string iniPath = GetIniPath();
    g_EnableEddsConversion = GetPrivateProfileIntA(INI_SECTION_NAME, INI_KEY_EDDS_CONVERSION, 1, iniPath.c_str()) != 0;
}

static void SaveSettings() {
    std::string iniPath = GetIniPath();
    WritePrivateProfileStringA(INI_SECTION_NAME, INI_KEY_EDDS_CONVERSION, g_EnableEddsConversion ? "1" : "0", iniPath.c_str());
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

class PakEntry {
public:
    enum class CompressionType {
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
    std::ifstream file;
    std::shared_ptr<PakEntry> root;
    std::vector<std::shared_ptr<PakEntry>> flatEntries;
    std::string filename;
    bool initialized = false;

    uint32_t ReadU32BE() {
        uint32_t value = 0;
        file.read(reinterpret_cast<char*>(&value), sizeof(value));
        return _byteswap_ulong(value);
    }

    uint32_t ReadU32LE() {
        uint32_t value = 0;
        file.read(reinterpret_cast<char*>(&value), sizeof(value));
        return value;
    }

    uint8_t ReadU8() {
        uint8_t value = 0;
        file.read(reinterpret_cast<char*>(&value), sizeof(value));
        return value;
    }

    std::string ReadString(uint8_t length) {
        std::string str(length, '\0');
        file.read(&str[0], length);
        return str;
    }

    bool ProcessFormChunk() {
        char form[4] = { 0 };
        file.read(form, 4);
        if (strncmp(form, "FORM", 4) != 0) {
            LogError("PAK file does not start with 'FORM' signature.");
            return false;
        }

        uint32_t formSize = ReadU32BE();
        char formType[4] = { 0 };
        file.read(formType, 4);
        if (strncmp(formType, "PAC1", 4) != 0) {
            LogError("PAK file FORM chunk type is not 'PAC1'.");
            return false;
        }
        return true;
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
        std::string fullPath = path.empty() ? entry->name : path + "\\" + entry->name;

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
    PakArchive(const std::string& filename) : filename(filename) {
        file.open(filename, std::ios::binary);
        if (!file.is_open()) {
            LogError("Failed to open PAK file: " + filename);
            throw std::runtime_error("Failed to open PAK file");
        }

        initialized = ProcessFormChunk();
        if (initialized) {
            root = std::make_shared<PakEntry>();
            root->name = "";
            root->isDirectory = true;

            while (file) {
                char chunkId[4] = { 0 };
                file.read(chunkId, 4);
                if (!file) break;

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

    bool ExtractFile(int index, const std::string& destPath) {
        if (index < 0 || index >= static_cast<int>(flatEntries.size())) {
            LogError("[ExtractFile] Invalid file index: " + std::to_string(index));
            return false;
        }

        const PakEntry* entry = flatEntries[index].get();
        if (!entry || entry->isDirectory) {
            LogError("[ExtractFile] Entry is null or a directory at index " + std::to_string(index));
            return false;
        }

        std::ifstream input(filename, std::ios::binary);
        if (!input.is_open()) {
            LogError("[ExtractFile] Cannot reopen archive: " + filename);
            return false;
        }

        try {
            input.seekg(0, std::ios::end);
            size_t fileSize = static_cast<size_t>(input.tellg());
            if (entry->offset + entry->size > fileSize) {
                LogError("[ExtractFile] Entry data goes beyond archive bounds: " + entry->name);
                return false;
            }

            input.seekg(static_cast<std::streamoff>(entry->offset), std::ios::beg);

            std::vector<uint8_t> rawFileContent(entry->size);
            input.read(reinterpret_cast<char*>(rawFileContent.data()), entry->size);
            if (input.gcount() != static_cast<std::streamsize>(entry->size)) {
                LogError("[ExtractFile] Incomplete read from PAK: got " + std::to_string(input.gcount()) +
                    " bytes, expected " + std::to_string(entry->size) + " for file " + entry->name);
            }

            std::vector<uint8_t> processedContent;
            if (entry->compression == PakEntry::CompressionType::Zlib) {
                processedContent.resize(entry->originalSize);
                uLongf destLen = entry->originalSize;
                int zResult = uncompress(
                    reinterpret_cast<Bytef*>(processedContent.data()), &destLen,
                    reinterpret_cast<const Bytef*>(rawFileContent.data()), entry->size);

                if (zResult != Z_OK || destLen != entry->originalSize) {
                    LogError("[ExtractFile] Zlib decompression failed for " + entry->name + ": code=" + std::to_string(zResult) +
                        ", decompressed=" + std::to_string(destLen) +
                        ", expected=" + std::to_string(entry->originalSize));
                    return false;
                }
            }
            else {
                processedContent = rawFileContent;
            }

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
                processDataProc(const_cast<char*>(entry->name.c_str()), entry->size);
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
        currentArchive = std::make_unique<PakArchive>(ArchiveData->ArcName);
        if (!currentArchive || !currentArchive->IsInitialized()) {
            currentArchive.reset();
            ArchiveData->OpenResult = E_EOPEN;
            return nullptr;
        }

        currentIndex = 0;
        ArchiveData->OpenResult = 0;
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

int __stdcall ReadHeader(HANDLE hArcData, tHeaderData* HeaderData) {
    if (!currentArchive || hArcData != reinterpret_cast<HANDLE>(1)) {
        LogError("Invalid archive handle in ReadHeader.");
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
    if (g_EnableEddsConversion && (displayName.length() >= 5)) {
        std::string ext = displayName.substr(displayName.length() - 5);
        if (ext == ".edds" || ext == ".EDDS") {
            displayName = displayName.substr(0, displayName.length() - 5) + ".dds";
        }
    }
    strncpy_s(HeaderData->FileName, displayName.c_str(), sizeof(HeaderData->FileName) - 1);
    HeaderData->FileName[sizeof(HeaderData->FileName) - 1] = '\0';

    HeaderData->PackSize = entry->isDirectory ? 0 : entry->size;
    HeaderData->UnpSize = entry->isDirectory ? 0 : entry->originalSize;
    HeaderData->FileAttr = entry->isDirectory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;

    SYSTEMTIME st;
    GetSystemTime(&st);
    HeaderData->FileTime = SystemTimeToDosDateTime(st);

    currentIndex++;
    return 0;
}

int __stdcall ProcessFile(HANDLE hArcData, int Operation, char* DestPath, char* DestName) {
    if (!currentArchive || hArcData != reinterpret_cast<HANDLE>(1)) {
        LogError("Invalid archive handle in ProcessFile.");
        return E_BAD_ARCHIVE;
    }

    if (Operation == PK_SKIP) {
        return 0;
    }

    int entryIndex = currentIndex - 1;
    if (entryIndex < 0 || entryIndex >= currentArchive->GetEntryCount()) {
        LogError("Entry index out of range in ProcessFile: " + std::to_string(entryIndex));
        return E_NO_FILES;
    }

    const PakEntry* entry = currentArchive->GetEntry(entryIndex);
    if (!entry) {
        LogError("Failed to get entry at index " + std::to_string(entryIndex) + " in ProcessFile.");
        return E_NO_FILES;
    }

    if (Operation == PK_TEST) {
        return 0;
    }

    if (Operation == PK_EXTRACT) {
        if (!DestPath && !DestName) {
            LogError("Both DestPath and DestName are NULL for EXTRACT operation.");
            return E_BAD_DATA;
        }

        std::string fullDestPath;
        if (!DestPath && DestName) {
            fullDestPath = DestName;
        }
        else if (DestPath && DestName) {
            fullDestPath = std::string(DestPath) + "\\" + DestName;
        }
        else {
            LogError("Invalid parameter combination for EXTRACT operation (DestPath: " + (DestPath ? std::string(DestPath) : "NULL") + ", DestName: " + (DestName ? std::string(DestName) : "NULL") + ")");
            return E_BAD_DATA;
        }

        if (entry->isDirectory) {
            if (!CreateDirectoryA(fullDestPath.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
                LogError("Failed to create directory: " + fullDestPath);
                return E_EWRITE;
            }
            return 0;
        }

        if (!currentArchive->ExtractFile(entryIndex, fullDestPath)) {
            return E_EWRITE;
        }

        return 0;
    }

    return 0;
}

int __stdcall CloseArchive(HANDLE hArcData) {
    if (!currentArchive || hArcData != reinterpret_cast<HANDLE>(1)) {
        LogError("Invalid archive handle or archive not open in CloseArchive.");
        return E_ECLOSE;
    }
    currentArchive.reset();
    currentIndex = 0;
    return 0;
}

void __stdcall SetChangeVolProc(HANDLE hArcData, tChangeVolProc pChangeVolProc) {
}

void __stdcall SetProcessDataProc(HANDLE hArcData, tProcessDataProc pProcessDataProc) {
    processDataProc = pProcessDataProc;
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
        return TRUE;
    }
    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case IDOK: {
            g_EnableEddsConversion = (IsDlgButtonChecked(hDlg, IDC_ENABLE_EDDS_CONVERSION) == BST_CHECKED);
            SaveSettings();
            LogInfo("EDDS to DDS conversion setting: " + std::string(g_EnableEddsConversion ? "Enabled" : "Disabled"));
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
    return PK_CAPS_MULTIPLE | PK_CAPS_BY_CONTENT | PK_CAPS_OPTIONS_DIALOG;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        g_hModule = hModule;
        LoadSettings();
        break;
    case DLL_PROCESS_DETACH:
        if (debugLog.is_open()) {
            LogInfo("=== Session end ===");
            debugLog.close();
        }
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}
