#include "WorkbenchModule.h"
#include <windows.h>
#include <string>
#include <filesystem>
#include <vector>
#include <fstream>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <atomic>
#include <tlhelp32.h>
#include <functional>

namespace fs = std::filesystem;

struct PakEntry {
	uint32_t timestamp = 0;
	std::string name;
	uint32_t offset = 0;
	uint32_t size = 0;
	uint32_t originalSize = 0;
	uint32_t compression = 0;
	bool isDirectory = false;
	std::vector<std::shared_ptr<PakEntry>> children;
};

class PakArchive {
public:
	virtual int GetEntryCount() const = 0;
	virtual const PakEntry* GetEntry(int index) const = 0;
	virtual int GetLastIndex() const = 0;
};

extern void LogError(const std::string& message);
extern void LogInfo(const std::string& message);
extern PakEntry g_CurrentEntryForDialog;

static std::string ws2s(const std::wstring& wstr) {
	if (wstr.empty()) return "";
	int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
	std::string strTo(size_needed, 0);
	WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
	return strTo;
}

namespace SmartExtractor {
	bool ExtractWithDependencies(PakArchive* arc, int entryIdx, const std::string& outPath, std::unordered_set<std::string>& processed) {
		return true;
	}
}

// =====================================================
// 🛠️ WORKBENCH MODULE IMPLEMENTATION
// =====================================================

namespace WorkbenchModule {

const std::wstring WORKBENCH_EXE_NAME = L"ArmaReforgerWorkbenchSteamDiag.exe";
const std::wstring WORKBENCH_FALLBACK_EXE = L"ArmaReforgerWorkbenchSteam.exe";

static std::wstring GetWorkbenchPathFromRegistry() {
	HKEY hKey;
	std::wstring exePath = L"";
	LPCWSTR subkey = L"SOFTWARE\\Bohemia Interactive\\Arma Reforger Tools";

	struct RootKey {
		HKEY hRoot;
		const char* name;
	} roots[] = {
		{HKEY_LOCAL_MACHINE, "HKLM"},
		{HKEY_CURRENT_USER,  "HKCU"}
	};

	LogInfo("[Debug] --- Registry Deep Search Start ---");

	for (auto& root : roots) {
		LogInfo("[Debug] Checking " + std::string(root.name) + "...");

		DWORD flags[] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY, 0 };

		for (DWORD flag : flags) {
			if (RegOpenKeyExW(root.hRoot, subkey, 0, KEY_READ | flag, &hKey) == ERROR_SUCCESS) {
				LogInfo("[Debug] SUCCESS: Key opened in " + std::string(root.name) + " with flags: " + std::to_string(flag));

				wchar_t buffer[MAX_PATH];
				DWORD bufferSize = sizeof(buffer);

				if (RegQueryValueExW(hKey, L"exe", NULL, NULL, (LPBYTE)buffer, &bufferSize) == ERROR_SUCCESS) {
					exePath = buffer;
					LogInfo("[Debug] 'exe' value found: " + ws2s(exePath));
				} else {
					bufferSize = sizeof(buffer);
					if (RegQueryValueExW(hKey, L"path", NULL, NULL, (LPBYTE)buffer, &bufferSize) == ERROR_SUCCESS) {
						exePath = std::wstring(buffer) + L"\\" + WORKBENCH_EXE_NAME;
						LogInfo("[Debug] 'path' value found, constructed default exe path.");
					}
				}

				RegCloseKey(hKey);
				if (!exePath.empty()) break;
			}
		}
		if (!exePath.empty()) break;
	}

	if (exePath.empty()) {
		LogInfo("[Debug] Still not found, checking HKEY_CLASSES_ROOT\\enfusion...");
		LPCWSTR uriKey = L"enfusion\\shell\\open\\command";
		if (RegOpenKeyExW(HKEY_CLASSES_ROOT, uriKey, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
			wchar_t buffer[MAX_PATH * 2];
			DWORD bufferSize = sizeof(buffer);
			if (RegQueryValueExW(hKey, NULL, NULL, NULL, (LPBYTE)buffer, &bufferSize) == ERROR_SUCCESS) {
				std::wstring fullCmd = buffer;
				size_t firstQuote = fullCmd.find(L"\"");
				size_t secondQuote = fullCmd.find(L"\"", firstQuote + 1);
				if (firstQuote != std::wstring::npos && secondQuote != std::wstring::npos) {
					exePath = fullCmd.substr(firstQuote + 1, secondQuote - firstQuote - 1);
					LogInfo("[Debug] Found path in Enfusion URI handler: " + ws2s(exePath));
				}
			}
			RegCloseKey(hKey);
		}
	}

	if (!exePath.empty()) {
		fs::path p(exePath);
		std::vector<std::wstring> candidates;

		candidates.push_back(p.wstring());

		if (p.has_parent_path()) {
			fs::path root = p.parent_path();

			candidates.push_back((root / WORKBENCH_EXE_NAME).wstring());
			candidates.push_back((root / L"Workbench" / WORKBENCH_EXE_NAME).wstring());

			if (WORKBENCH_EXE_NAME != WORKBENCH_FALLBACK_EXE) {
				candidates.push_back((root / WORKBENCH_FALLBACK_EXE).wstring());
				candidates.push_back((root / L"Workbench" / WORKBENCH_FALLBACK_EXE).wstring());
			}
		}

		bool found = false;
		for (const auto& cand : candidates) {
			if (!cand.empty() && fs::exists(cand) && !fs::is_directory(cand)) {
				exePath = cand;
				LogInfo("[Debug] Physical file VERIFIED: " + ws2s(exePath));
				found = true;
				break;
			}
		}

		if (!found) {
			LogError("[Debug] Path found in registry, but physical file not found: " + ws2s(exePath));
			exePath = L"";
		}
	}

	if (exePath.empty()) {
		LogError("[Debug] ALL Registry locations and physical checks failed.");
	}

	LogInfo("[Debug] --- Registry Deep Search End ---");
	return exePath;
}

struct WorkbenchContext {
	std::string internalPath;
	fs::path sandboxPath;
	fs::path targetFilePath;
	std::string ext;
	std::string GUID_PROJECT = "A1B2C3D4E5F60789";
	std::string projectName = "PAKViewer";
	std::string wbModule = "resourceManager";
	std::string loadResource;
};

using ExtensionProcessor = std::function<void(WorkbenchContext&)>;
static std::unordered_map<std::string, ExtensionProcessor> g_ExtensionLogic;

struct ExtensionAutoReg {
	ExtensionAutoReg() {
		g_ExtensionLogic[".xob"] = [](WorkbenchContext& ctx) {
			ctx.projectName = "XOBViewer";
			ctx.wbModule = "resourceManager";

			std::string GUID_XOB        = "D887766554433221";
			std::string GUID_ENTITY     = "B9876543210FEDCB";
			std::string GUID_COMPONENT  = "C112233445566778";
			std::string GUID_PREFAB     = "E554433221100998";

			fs::path prefabDir = ctx.sandboxPath / "Prefabs";
			fs::create_directories(prefabDir);

			try {
				std::ofstream xobMeta(ctx.targetFilePath.string() + ".meta");
				xobMeta << "MetaFileClass {\n Name \"{" << GUID_XOB << "}" << ctx.internalPath
						<< "\"\n Configurations {\n  StaticModelResourceClass PC {}\n }\n}";
				xobMeta.close();

				std::ofstream et(prefabDir / "preview.et");
				et << "GenericEntity {\n ID \"" << GUID_ENTITY << "\"\n components {\n  MeshObject \"" << GUID_COMPONENT << "\" {\n    Object \"{" << GUID_XOB << "}" << ctx.internalPath << "\"\n  }\n }\n}";
				et.close();

				std::ofstream etMeta(prefabDir / "preview.et.meta");
				etMeta << "MetaFileClass {\n Name \"{" << GUID_PREFAB << "}Prefabs/preview.et\"\n Configurations {\n  EntityTemplateResourceClass PC {}\n }\n}";
				etMeta.close();

				ctx.loadResource = "{" + GUID_PREFAB + "}Prefabs/preview.et";

			} catch (...) {
				LogInfo("[XOB Processor] Critical error while writing files!");
				ctx.loadResource = "{" + ctx.GUID_PROJECT + "}" + ctx.internalPath;
			}
		};
	}
};

//static ExtensionAutoReg g_DoReg;
bool IsWorkbenchSupported(const std::string& extension) {
	if (g_ExtensionLogic.count(extension)) {
		return true;
	}

static const std::unordered_set<std::string> supportedExtensions = {
 //        ".et", ".ent", ".layer", ".c", ".acp", ".sig", ".afm", ".snd", ".wav",
 //        ".agf", ".agr", ".anm", ".asi", ".ast", ".aw", ".ae", ".asy", ".txa",
 //        ".bt", ".ptc", ".layout", ".styles", ".imageset", ".emat", ".gamemat",
 //        ".physmat", ".st", ".nmn", ".pap", ".siga", ".conf",
 //        ".gproj", ".meta", ".pre", ".fnt", ".ttf"
		  ".xob", ".c", ".edds"
	};

	return supportedExtensions.count(extension) > 0;
}

int CountProcesses(const std::wstring& exeName) {
	HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (hSnap == INVALID_HANDLE_VALUE) return 0;

	PROCESSENTRY32W pe;
	pe.dwSize = sizeof(pe);
	int count = 0;

	if (Process32FirstW(hSnap, &pe)) {
		do {
			if (_wcsicmp(pe.szExeFile, exeName.c_str()) == 0) {
				count++;
			}
		} while (Process32NextW(hSnap, &pe));
	}

	CloseHandle(hSnap);
	return count;
}

bool WaitForSteamReady(int maxWaitSeconds) {
	LogInfo("[Steam] Monitoring WebHelper count for initialization...");

	int stableTicks = 0;
	int lastCount = 0;

	for (int i = 0; i < maxWaitSeconds; ++i) {
		int currentCount = CountProcesses(L"steamwebhelper.exe");

		if (currentCount >= 7 && currentCount == lastCount) {
			stableTicks++;
		} else {
			stableTicks = 0;
		}

		lastCount = currentCount;

		if (stableTicks >= 3) {
			LogInfo("[Steam] Ready! WebHelper count stable at: " + std::to_string(currentCount));
			return true;
		}

		std::this_thread::sleep_for(std::chrono::seconds(1));
		if (i % 5 == 0) LogInfo("[Steam] Waiting... Current WebHelpers: " + std::to_string(currentCount));
	}

	LogInfo("[Steam] Timeout reached, continuing launch attempt anyway...");
	return false;
}

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
	DWORD pid;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == (DWORD)lParam) {
		PostMessage(hwnd, WM_CLOSE, 0, 0);
	}
	return TRUE;
}

bool IsSteamRunning() {
	HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (hSnap == INVALID_HANDLE_VALUE) return false;
	PROCESSENTRY32W pe = { sizeof(pe) };
	bool running = false;
	if (Process32FirstW(hSnap, &pe)) {
		do {
			if (_wcsicmp(pe.szExeFile, L"steam.exe") == 0) {
				running = true;
				break;
			}
		} while (Process32NextW(hSnap, &pe));
	}
	CloseHandle(hSnap);
	return running;
}

bool IsProcessRunning(const std::wstring& exeName) {
	HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (hSnap == INVALID_HANDLE_VALUE) return false;

	PROCESSENTRY32W pe{ sizeof(pe) };
	bool running = false;

	for (BOOL ok = Process32FirstW(hSnap, &pe); ok; ok = Process32NextW(hSnap, &pe)) {
		if (_wcsicmp(pe.szExeFile, exeName.c_str()) == 0) {
			running = true;
			break;
		}
	}

	CloseHandle(hSnap);
	return running;
}

bool IsWorkbenchRunning() {
	return IsProcessRunning(WORKBENCH_EXE_NAME) || IsProcessRunning(WORKBENCH_FALLBACK_EXE);
}

bool LaunchWorkbenchWithRetry(const std::wstring& cmd, const std::wstring& workDir, const fs::path& sandboxPath) {
	const int maxRetries = 5;

	for (int i = 0; i < maxRetries; ++i) {
		if (!IsSteamRunning()) {
			LogInfo("[Workbench] Steam not running, launching...");
			ShellExecuteW(NULL, L"open", L"steam://open/main", NULL, NULL, SW_SHOWNORMAL);
			WaitForSteamReady(45);
		} else {
			if (CountProcesses(L"steamwebhelper.exe") < 7) {
				LogInfo("[Workbench] Steam running but WebHelpers starting, waiting...");
				WaitForSteamReady(10);
			}
		}

		LogInfo("[Workbench] Launch attempt #" + std::to_string(i + 1));

		STARTUPINFOW si{ sizeof(si) };
		si.dwFlags = STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_SHOWNORMAL;

		PROCESS_INFORMATION pi{};
		std::vector<wchar_t> buffer(cmd.begin(), cmd.end());
		buffer.push_back(0);

		if (!CreateProcessW(NULL, buffer.data(), NULL, NULL, FALSE, 0, NULL, workDir.c_str(), &si, &pi)) {
			LogInfo("[Workbench] CreateProcessW failed: " + std::to_string(GetLastError()));
			std::this_thread::sleep_for(std::chrono::seconds(3));
			continue;
		}

		HANDLE hProcess = pi.hProcess;
		DWORD pid = pi.dwProcessId;
		CloseHandle(pi.hThread);

		bool success = false;
		for (int check = 0; check < 15; ++check) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));

			DWORD currentExitCode;
			if (!GetExitCodeProcess(hProcess, &currentExitCode)) break;

			if (currentExitCode != STILL_ACTIVE) {
				success = false;
				break;
			}

			if (check >= 10) {
				success = true;
				break;
			}
		}

		if (success) {
			LogInfo("[Workbench] Process running (PID: " + std::to_string(pid) + ")");

			std::thread([hProcess, sandboxPath, pid]() {
				WaitForSingleObject(hProcess, INFINITE);

				DWORD finalExit = 0;
				GetExitCodeProcess(hProcess, &finalExit);
				CloseHandle(hProcess);

				std::this_thread::sleep_for(std::chrono::seconds(1));

				if (finalExit == 0 && fs::exists(sandboxPath)) {
					try {
						fs::remove_all(sandboxPath);
						LogInfo("[Cleanup] Sandbox deleted after Workbench (PID: " + std::to_string(pid) + ") closed.");
					} catch (...) {
						LogInfo("[Cleanup] Failed to delete sandbox (folder may be locked).");
					}
				} else if (finalExit == 0) {
					LogInfo("[Cleanup] Sandbox folder already gone or inaccessible, skipping.");
				} else {
					LogInfo("[Cleanup] Process " + std::to_string(pid) + " closed via code or error (Exit: " + std::to_string(finalExit) + "), skipping cleanup.");
				}
			}).detach();

			return true;
		}

		LogInfo("[Workbench] Process exited prematurely.");
		CloseHandle(hProcess);
		std::this_thread::sleep_for(std::chrono::seconds(3));
	}

	return false;
}

void TerminateWorkbench() {
	HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (hSnap == INVALID_HANDLE_VALUE) return;

	PROCESSENTRY32W pe;
	pe.dwSize = sizeof(pe);

	if (Process32FirstW(hSnap, &pe)) {
		do {
			std::wstring exeName = pe.szExeFile;
			if (_wcsicmp(exeName.c_str(), WORKBENCH_EXE_NAME.c_str()) == 0 || _wcsicmp(exeName.c_str(), WORKBENCH_FALLBACK_EXE.c_str()) == 0) {

				DWORD pid = pe.th32ProcessID;

				LogInfo("[Workbench] Existing process found (PID: " + std::to_string(pid) + "), sending close message...");

				EnumWindows(EnumWindowsProc, pid);

				HANDLE hProcess = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);

				if (hProcess) {
					DWORD wait = WaitForSingleObject(hProcess, 5000);

					if (wait == WAIT_TIMEOUT) {
						LogInfo("[Workbench] Graceful close timeout, forcing termination (PID: " + std::to_string(pid) + ")");
						TerminateProcess(hProcess, 0);
						WaitForSingleObject(hProcess, 2000);
					} else {
						LogInfo("[Workbench] Process exited cleanly (PID: " + std::to_string(pid) + ")");
					}

					CloseHandle(hProcess);
				} else {
					LogInfo("[Workbench] Failed to open process handle (PID: " + std::to_string(pid) + ")");
				}
			}
		} while (Process32NextW(hSnap, &pe));
	}

	CloseHandle(hSnap);

	std::this_thread::sleep_for(std::chrono::milliseconds(300));
}

// =====================================================
// 🔥 WORKBENCH LAUNCH
// =====================================================
bool OpenInWorkbench(const std::string& pakEntryPath, const std::string& extractedRoot) {
	TerminateWorkbench();

	for (int i = 0; i < 20; ++i) {
		if (!IsWorkbenchRunning()) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	LogInfo("[Workbench] Launching via Retry system...");

	std::wstring wbExe = GetWorkbenchPathFromRegistry();
	if (wbExe.empty()) return false;

	fs::path exePath(wbExe);
	fs::path binDir = exePath.parent_path();

	fs::path diagPath = binDir / WORKBENCH_EXE_NAME;
	if (fs::exists(diagPath)) wbExe = diagPath.wstring();

	fs::path toolsDir = binDir.parent_path();
	fs::path commonDir = toolsDir.parent_path();
	fs::path gameData = commonDir / L"Arma Reforger" / L"addons" / L"data";
	fs::path sandboxPath = fs::path(extractedRoot);

	WorkbenchContext ctx;
	ctx.internalPath = pakEntryPath;

	std::replace(ctx.internalPath.begin(), ctx.internalPath.end(), '\\', '/');
	if (!ctx.internalPath.empty() && ctx.internalPath[0] == '/') ctx.internalPath.erase(0, 1);

	ctx.sandboxPath = sandboxPath;
	ctx.targetFilePath = sandboxPath / ctx.internalPath;
	ctx.ext = ctx.targetFilePath.extension().string();
	std::transform(ctx.ext.begin(), ctx.ext.end(), ctx.ext.begin(), ::tolower);

	ctx.loadResource = "{" + ctx.GUID_PROJECT + "}" + ctx.internalPath;

	fs::create_directories(ctx.targetFilePath.parent_path());

	// =====================================================
	// 🛠️ EXTENSION LOGIC
	// =====================================================
	if (g_ExtensionLogic.count(ctx.ext)) {
		g_ExtensionLogic[ctx.ext](ctx);
	} else {
		const std::string& ext = ctx.ext;
		if (ext == ".xob") { ctx.projectName = "XOBViewer"; ctx.wbModule = "resourceManager"; }
		else if (ext == ".et" || ext == ".ent" || ext == ".layer") { ctx.projectName = "WorldEditor"; ctx.wbModule = "worldEditor"; }
		else if (ext == ".c") { ctx.projectName = "ScriptEditor"; ctx.wbModule = "resourceManager"; }
		else if (ext == ".acp" || ext == ".sig" || ext == ".afm" || ext == ".snd" || ext == ".wav") { ctx.projectName = "AudioEditor"; ctx.wbModule = "audioEditor"; }
		else if (ext == ".agf" || ext == ".agr" || ext == ".anm" || ext == ".asi" || ext == ".ast" || ext == ".aw" || ext == ".ae" || ext == ".asy" || ext == ".txa") { ctx.projectName = "AnimationEditor"; ctx.wbModule = "animEditor"; }
		else if (ext == ".bt") { ctx.projectName = "BehaviorEditor"; ctx.wbModule = "behaviorEditor"; }
		else if (ext == ".ptc") { ctx.projectName = "ParticleEditor"; ctx.wbModule = "particleEditor"; }
		else if (ext == ".layout" || ext == ".styles" || ext == ".imageset") { ctx.projectName = "LayoutEditor"; ctx.wbModule = "resourceManager"; }
		else if (ext == ".emat" || ext == ".gamemat" || ext == ".physmat") { ctx.projectName = "MaterialEditor"; ctx.wbModule = "resourceManager"; }

		else if (ext == ".edds") { ctx.projectName = "ResourceManager"; ctx.wbModule = "resourceManager"; }

		else if (ext == ".st") { ctx.projectName = "LocalizationEditor"; ctx.wbModule = "localizationEditor"; }
		else if (ext == ".nmn") { ctx.projectName = "NavmeshGenerator"; ctx.wbModule = "navmeshGeneratorMain"; }
		else if (ext == ".pap" || ext == ".siga") { ctx.projectName = "ProcAnimEditor"; ctx.wbModule = "procAnimEditor"; }
		else if (ext == ".conf" || ext == ".gproj" || ext == ".meta" || ext == ".pre" || ext == ".fnt" || ext == ".ttf") { ctx.projectName = "ConfigViewer"; ctx.wbModule = "resourceManager"; }
	}

	// =====================================================
	// 📂 FILE OPERATIONS
	// =====================================================
	try {
		fs::path sourceInSandbox = sandboxPath / ctx.internalPath;
		if (fs::exists(sourceInSandbox) && sourceInSandbox != ctx.targetFilePath) {
			if (fs::exists(ctx.targetFilePath)) fs::remove(ctx.targetFilePath);
			fs::copy_file(sourceInSandbox, ctx.targetFilePath);
		}
	} catch (...) { return false; }

	// =====================================================
	// 📝 PROJECT GENERATION (Meta & GPROJ)
	// =====================================================
	try {
		std::ofstream meta(ctx.targetFilePath.string() + ".meta");
		meta << "MetaFileClass {\n Name \"{" << ctx.GUID_PROJECT << "}" << ctx.internalPath << "\"\n}\n";
		meta.close();

		std::ofstream gproj(sandboxPath / "addon.gproj");
		gproj << "GameProject {\n"
			  << " ID \"" << ctx.projectName << "\"\n"
			  << " GUID \"" << ctx.GUID_PROJECT << "\"\n"
			  << " TITLE \"" << ctx.projectName << "\"\n"
			  << " Dependencies {\n"
			  << "  \"58D0FB3206B6F859\"\n"
			  << " }\n"
			  << "}";
		gproj.close();
	} catch (...) { return false; }

	// =====================================================
	// 🚀 COMMAND EXECUTION
	// =====================================================
	std::wstring wModule = std::wstring(ctx.wbModule.begin(), ctx.wbModule.end());
	std::wstring wLoad = std::wstring(ctx.loadResource.begin(), ctx.loadResource.end());

	std::wstring openCmd = L"\"" + wbExe + L"\"" +
		L" -gproj \"" + (sandboxPath / "addon.gproj").wstring() + L"\"" +
		L" -addonsDir \"" + sandboxPath.wstring() + L"," + gameData.wstring() + L"\"" +
		L" -wbModule=" + wModule;

	if (ctx.wbModule == "scriptEditor") {
		openCmd += L" -noGameScriptsOnInit";
	}

	openCmd += L" -run -load \"" + wLoad + L"\"";

	LogInfo("[Workbench] Final Launch Command: " + ws2s(openCmd));

	if (!LaunchWorkbenchWithRetry(openCmd, binDir.wstring(), sandboxPath)) {
		MessageBoxW(NULL,
			L"Failed to launch Workbench after multiple attempts.\nPlease ensure Steam is running and logged in.",
			L"Error",
			MB_OK | MB_ICONERROR);
		return false;
	}

	return true;
}

} // namespace WorkbenchModule
