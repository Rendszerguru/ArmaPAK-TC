#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <cstdint>
#include <cstring>
#include <optional>

extern void LogError(const std::string& message);

#include "lz4.h"

namespace fs = std::filesystem;

void ConvertToDDS(const std::string& eddsPath, const std::string& ddsPath);

