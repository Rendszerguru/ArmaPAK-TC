#pragma once
#include <windows.h>
#include <string>
#include <filesystem>
#include <vector>

class PakArchive;

namespace WorkbenchModule {
    bool IsWorkbenchSupported(const std::string& extension);
    bool IsWorkbenchRunning();
    bool OpenInWorkbench(const std::string& pakEntryPath, const std::string& extractedRoot);
    void TerminateWorkbench();
}