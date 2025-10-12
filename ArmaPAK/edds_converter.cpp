#include "edds_converter.h"

#include <algorithm>
#include <memory>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace fs = std::filesystem;

#define DDSD_CAPS         0x00000001
#define DDSD_HEIGHT       0x00000002
#define DDSD_WIDTH        0x00000004
#define DDSD_PITCH        0x00000008
#define DDSD_PIXELFORMAT  0x00000020
#define DDSD_MIPMAPCOUNT  0x00020000
#define DDSD_LINEARSIZE   0x00080000

#define DDPF_FOURCC       0x00000004
#define DDPF_RGB          0x00000040

#define DDSCAPS_COMPLEX   0x00000008
#define DDSCAPS_MIPMAP    0x00000040
#define DDSCAPS_TEXTURE   0x00001000

#define FOURCC_DXT1 0x31545844
#define FOURCC_DXT5 0x35545844
#define FOURCC_DX10 0x30315844

#pragma pack(push, 1)
typedef struct {
	uint32_t dwSize;
	uint32_t dwFlags;
	uint32_t dwFourCC;
	uint32_t dwRGBBitCount;
	uint32_t dwRBitMask;
	uint32_t dwGBitMask;
	uint32_t dwBBitMask;
	uint32_t dwABitMask;
} DDS_PIXELFORMAT;

typedef struct {
	uint32_t dwSize;
	uint32_t dwFlags;
	uint32_t dwHeight;
	uint32_t dwWidth;
	uint32_t dwPitchOrLinearSize;
	uint32_t dwDepth;
	uint32_t dwMipMapCount;
	uint32_t dwReserved1[11];
	DDS_PIXELFORMAT ddspf;
	uint32_t dwCaps;
	uint32_t dwCaps2;
	uint32_t dwCaps3;
	uint32_t dwCaps4;
	uint32_t dwReserved2;
} DDS_HEADER;
#pragma pack(pop)

namespace {
	uint32_t SwapEndian(uint32_t value) {
		return ((value & 0x000000FF) << 24) |
			((value & 0x0000FF00) << 8) |
			((value & 0x00FF0000) >> 8) |
			((value & 0xFF000000) >> 24);
	}

	size_t GetFirstMipmapSize(const DDS_HEADER& header) {
		if (header.ddspf.dwFlags & DDPF_FOURCC) {
			uint32_t fourCC = header.ddspf.dwFourCC;
			uint32_t blockSize = 0;

			if (fourCC == FOURCC_DXT1) {
				blockSize = 8;
			}
			else if (fourCC == FOURCC_DXT5 || fourCC == FOURCC_DX10) {
				blockSize = 16;
			}
			else {
				if (header.dwFlags & DDSD_LINEARSIZE) {
					return header.dwPitchOrLinearSize;
				}
				return 0;
			}

			size_t numBlocksWide = std::max(1u, (header.dwWidth + 3) / 4);
			size_t numBlocksHigh = std::max(1u, (header.dwHeight + 3) / 4);

			return numBlocksWide * numBlocksHigh * blockSize;

		}
		else if (header.ddspf.dwFlags & DDPF_RGB) {
			if (header.ddspf.dwRGBBitCount % 8 != 0) return 0;
			return static_cast<size_t>(header.dwWidth) * header.dwHeight * (header.ddspf.dwRGBBitCount / 8);
		}

		return 0;
	}
}

void ConvertToDDS(const std::string& eddsPath, const std::string& ddsPath) {
	std::ifstream input(eddsPath, std::ios::binary);
	if (!input) {
		LogError("[ConvertToDDS] Cannot open input .edds file: " + eddsPath);
		return;
	}

	try {
		std::vector<uint8_t> ddsHeader(128);
		input.read(reinterpret_cast<char*>(ddsHeader.data()), 128);
		if (input.gcount() != 128) {
			LogError("[ConvertToDDS] Invalid DDS header in file: " + eddsPath + ". Expected 128 bytes, got " + std::to_string(input.gcount()));
			return;
		}
		if (std::memcmp(ddsHeader.data(), "DDS ", 4) != 0) {
			LogError("[ConvertToDDS] DDS magic number not found. Invalid EDDS file.");
			return;
		}


		std::vector<uint8_t> ddsHeaderDx10;
		if (ddsHeader[84] == 'D' && ddsHeader[85] == 'X' &&
			ddsHeader[86] == '1' && ddsHeader[87] == '0') {
			ddsHeaderDx10.resize(20);
			input.read(reinterpret_cast<char*>(ddsHeaderDx10.data()), 20);
			if (input.gcount() != 20) {
				LogError("[ConvertToDDS] Invalid DX10 header in file: " + eddsPath + ". Expected 20 bytes, got " + std::to_string(input.gcount()));
				return;
			}
		}

		struct Block {
			std::string type;
			int32_t size;
		};
		std::vector<Block> blocks;

		while (true) {
			char blockName[4] = { 0 };
			int32_t size = 0;

			input.read(blockName, 4);
			if (input.gcount() != 4) {
				break;
			}

			input.read(reinterpret_cast<char*>(&size), 4);
			if (input.gcount() != 4) {
				break;
			}

			std::string blockStr(blockName, 4);
			int32_t original_size = size;

			if (blockStr != "COPY" && blockStr != "LZ4 ") {
				input.seekg(-8, std::ios::cur);
				break;
			}

			if (size < 0 || static_cast<uint32_t>(size) > 100 * 1024 * 1024) {
				size = static_cast<int32_t>(SwapEndian(static_cast<uint32_t>(original_size)));
				if (size < 0 || static_cast<uint32_t>(size) > 100 * 1024 * 1024) {
					LogError("[ConvertToDDS] Invalid block size even after byte order swap for block type '" + blockStr + "'. Aborting block search.");
					break;
				}
			}

			blocks.push_back({ blockStr, size });
		}

		if (blocks.empty()) {
			LogError("[ConvertToDDS] No data block (COPY/LZ4) found.");
			return;
		}

		std::vector<std::vector<uint8_t>> decodedBlockContents;
		decodedBlockContents.reserve(blocks.size());

		for (size_t i = 0; i < blocks.size(); i++) {
			const auto& block = blocks[i];
			std::vector<uint8_t> currentBlockData;

			if (block.size <= 0) {
				LogError("[ConvertToDDS] [WARN] Invalid block size " + std::to_string(block.size) + " at block #" + std::to_string(i) + " (type: " + block.type + "). Skipping.");
				continue;
			}

			if (block.type == "COPY") {
				currentBlockData.resize(static_cast<size_t>(block.size));
				input.read(reinterpret_cast<char*>(currentBlockData.data()), block.size);
				if (input.gcount() != block.size) {
					LogError("[ConvertToDDS] [WARN] Failed to read full COPY block at #" + std::to_string(i) + ". Expected " + std::to_string(block.size) + " bytes, got " + std::to_string(input.gcount()) + ". Aborting block processing.");
					break;
				}
			}
			else if (block.type == "LZ4 ") {
				uint32_t decompressedTotalSize = 0;
				input.read(reinterpret_cast<char*>(&decompressedTotalSize), sizeof(decompressedTotalSize));
				if (!input) {
					LogError("[ConvertToDDS] [ERROR] Cannot read decompressed size for LZ4 block #" + std::to_string(i) + ". Aborting block processing.");
					break;
				}

				size_t compressedBlockActualSize = static_cast<size_t>(block.size) - sizeof(decompressedTotalSize);
				if (static_cast<int>(compressedBlockActualSize) <= 0) {
					LogError("[ConvertToDDS] [WARN] LZ4 block size is too small (less than 4 bytes for total decompressed size) at #" + std::to_string(i) + ". Skipping.");
					continue;
				}

				std::vector<char> compressedData(compressedBlockActualSize);
				input.read(compressedData.data(), compressedBlockActualSize);
				if (input.gcount() != static_cast<std::streamsize>(compressedBlockActualSize)) {
					LogError("[ConvertToDDS] [WARN] Failed to read full LZ4 compressed data for block #" + std::to_string(i) + ". Expected " + std::to_string(compressedBlockActualSize) + " bytes, got " + std::to_string(input.gcount()) + ". Aborting block processing.");
					break;
				}

				currentBlockData.resize(decompressedTotalSize);

				std::unique_ptr<LZ4_streamDecode_t, decltype(&LZ4_freeStreamDecode)> lz4Stream(
					LZ4_createStreamDecode(), &LZ4_freeStreamDecode);

				if (!lz4Stream) {
					LogError("[ConvertToDDS] [ERROR] Failed to create LZ4 decode stream for block #" + std::to_string(i) + ". Aborting block processing.");
					break;
				}

				size_t outPos = 0;
				size_t readPos = 0;

				while (readPos < compressedBlockActualSize) {
					if (readPos + sizeof(int32_t) > compressedBlockActualSize) {
						LogError("[ConvertToDDS] [WARN] Unexpected end of compressed LZ4 block data at chunk header for block #" + std::to_string(i) + ".\n");
						currentBlockData.clear();
						break;
					}

					int32_t rawChunkSize;
					std::memcpy(&rawChunkSize, compressedData.data() + readPos, sizeof(int32_t));
					readPos += sizeof(int32_t);

					int32_t currentChunkDataSize = rawChunkSize & 0x7FFFFFFF;

					if (currentChunkDataSize == 0 || readPos + static_cast<size_t>(currentChunkDataSize) > compressedBlockActualSize) {
						LogError("[ConvertToDDS] [WARN] Invalid chunk data size/bounds for chunk at LZ4 block #" + std::to_string(i) + ", offset " + std::to_string(readPos - 4) + ". Skipping chunk.");
						currentBlockData.clear();
						break;
					}

					int bytesProcessed = LZ4_decompress_safe_continue(
						lz4Stream.get(),
						compressedData.data() + readPos,
						reinterpret_cast<char*>(currentBlockData.data()) + outPos,
						currentChunkDataSize,
						static_cast<int>(currentBlockData.size() - outPos)
					);

					if (bytesProcessed <= 0) {
						LogError("[ConvertToDDS] [ERROR] LZ4 decompress error at block #" + std::to_string(i) + ", chunk offset " + std::to_string(readPos - 4) + ". Error code: " + std::to_string(bytesProcessed) + ". Aborting chunk processing.\n");
						currentBlockData.clear();
						break;
					}

					outPos += bytesProcessed;
					readPos += static_cast<size_t>(currentChunkDataSize);
				}

				if (!currentBlockData.empty() && static_cast<uint32_t>(outPos) != decompressedTotalSize) {
					LogError("[ConvertToDDS] [WARN] Decompressed size mismatch for LZ4 block #" + std::to_string(i)
						+ " (expected " + std::to_string(decompressedTotalSize) + ", got " + std::to_string(outPos) + "). This might indicate a parsing issue.");
				}

				if (currentBlockData.empty()) continue;

			}

			decodedBlockContents.push_back(std::move(currentBlockData));
		}


		std::vector<uint8_t> decodedData;
		size_t totalDataSize = 0;
		for (const auto& content : decodedBlockContents) {
			totalDataSize += content.size();
		}
		decodedData.reserve(totalDataSize);

		for (auto it = decodedBlockContents.rbegin(); it != decodedBlockContents.rend(); ++it) {
			decodedData.insert(decodedData.end(), it->begin(), it->end());
		}

		std::ofstream output(ddsPath, std::ios::binary);
		if (!output) {
			LogError("[ConvertToDDS] [ERROR] Cannot write DDS file: " + ddsPath);
			return;
		}

		DDS_HEADER* pHeader = reinterpret_cast<DDS_HEADER*>(ddsHeader.data() + 4);
		bool headerFixed = false;

		const size_t firstMipMapSize = GetFirstMipmapSize(*pHeader);

		if (firstMipMapSize > 0 && decodedData.size() >= firstMipMapSize) {
			pHeader->dwMipMapCount = 1;
			pHeader->dwCaps &= ~DDSCAPS_MIPMAP;
			pHeader->dwCaps &= ~DDSCAPS_COMPLEX;
			pHeader->dwCaps |= DDSCAPS_TEXTURE;
			headerFixed = true;
		}


		output.write(reinterpret_cast<const char*>(ddsHeader.data()), static_cast<std::streamsize>(ddsHeader.size()));
		if (!ddsHeaderDx10.empty()) {
			output.write(reinterpret_cast<const char*>(ddsHeaderDx10.data()), static_cast<std::streamsize>(ddsHeaderDx10.size()));
		}

		if (headerFixed) {
			output.write(reinterpret_cast<const char*>(decodedData.data()), static_cast<std::streamsize>(firstMipMapSize));
		}
		else {
			output.write(reinterpret_cast<const char*>(decodedData.data()), static_cast<std::streamsize>(decodedData.size()));
		}

		if (!output.good()) {
			LogError("[ConvertToDDS] Failed to finalize output file writing.");
		}
	}
	catch (const std::exception& ex) {
		LogError(std::string("[ConvertToDDS] General EXCEPTION: ") + ex.what());
	}
	catch (...) {
		LogError("[ConvertToDDS] Unknown EXCEPTION during conversion.");
	}
}
