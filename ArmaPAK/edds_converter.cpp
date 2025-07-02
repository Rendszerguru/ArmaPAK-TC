#include "edds_converter.h"

namespace fs = std::filesystem;

void ConvertToDDS(const std::string& eddsPath, const std::string& ddsPath) {
    std::ifstream input(eddsPath, std::ios::binary);
    if (!input) {
        LogError("[ConvertToDDS] Cannot open input .edds file: " + eddsPath);
        return;
    }

    std::vector<uint8_t> ddsHeader(128);
    input.read(reinterpret_cast<char*>(ddsHeader.data()), 128);
    if (input.gcount() != 128) {
        LogError("[ConvertToDDS] Invalid DDS header in file: " + eddsPath + ". Expected 128 bytes, got " + std::to_string(input.gcount()));
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

        if (blockStr != "COPY" && blockStr != "LZ4 ") {
            input.seekg(-8, std::ios::cur);
            break;
        }

        blocks.push_back({ blockStr, size });
    }

    std::vector<uint8_t> decodedData;

    for (size_t i = 0; i < blocks.size(); i++) {
        const auto& block = blocks[i];
        if (block.size <= 0) {
            LogError("[ConvertToDDS] [WARN] Invalid block size " + std::to_string(block.size) + " at block #" + std::to_string(i) + " (type: " + block.type + "). Skipping.");
            continue;
        }

        if (block.type == "COPY") {
            std::vector<uint8_t> buffer(block.size);
            input.read(reinterpret_cast<char*>(buffer.data()), block.size);
            if (input.gcount() != block.size) {
                LogError("[ConvertToDDS] [WARN] Failed to read full COPY block at #" + std::to_string(i) + ". Expected " + std::to_string(block.size) + " bytes, got " + std::to_string(input.gcount()) + ". Aborting block processing.");
                break;
            }
            decodedData.insert(decodedData.begin(), buffer.begin(), buffer.end());
        }
        else if (block.type == "LZ4 ") {
            uint32_t decompressedTotalSize = 0;
            input.read(reinterpret_cast<char*>(&decompressedTotalSize), sizeof(decompressedTotalSize));
            if (!input) {
                LogError("[ConvertToDDS] [ERROR] Cannot read decompressed size for LZ4 block #" + std::to_string(i) + ". Aborting block processing.");
                break;
            }

            size_t compressedBlockActualSize = static_cast<size_t>(block.size) - sizeof(decompressedTotalSize);
            if (static_cast<int>(compressedBlockActualSize) < 0) {
                LogError("[ConvertToDDS] [WARN] LZ4 block size is too small (less than 4 bytes for total decompressed size) at #" + std::to_string(i) + ". Skipping.");
                continue;
            }

            std::vector<char> compressedData(compressedBlockActualSize);
            input.read(compressedData.data(), compressedBlockActualSize);
            if (input.gcount() != static_cast<std::streamsize>(compressedBlockActualSize)) {
                LogError("[ConvertToDDS] [WARN] Failed to read full LZ4 compressed data for block #" + std::to_string(i) + ". Expected " + std::to_string(compressedBlockActualSize) + " bytes, got " + std::to_string(input.gcount()) + ". Aborting block processing.");
                break;
            }

            std::vector<uint8_t> decompressedBuffer(decompressedTotalSize);

            LZ4_streamDecode_t* lz4Stream = LZ4_createStreamDecode();
            if (!lz4Stream) {
                LogError("[ConvertToDDS] [ERROR] Failed to create LZ4 decode stream for block #" + std::to_string(i) + ". Aborting block processing.");
                break;
            }

            size_t outPos = 0;
            size_t readPos = 0;

            while (readPos < compressedBlockActualSize) {
                if (readPos + sizeof(int32_t) > compressedBlockActualSize) {
                    LogError("[ConvertToDDS] [WARN] Unexpected end of compressed LZ4 block data at chunk header for block #" + std::to_string(i) + ".\n");
                    break;
                }

                int32_t rawChunkSize = *reinterpret_cast<int32_t*>(compressedData.data() + readPos);
                readPos += sizeof(int32_t);

                int32_t currentChunkDataSize = rawChunkSize & 0x7FFFFFFF;

                if (currentChunkDataSize <= 0) {
                    LogError("[ConvertToDDS] [WARN] Invalid (non-positive) chunk data size " + std::to_string(currentChunkDataSize) + " at LZ4 block #" + std::to_string(i) + ", offset " + std::to_string(readPos - 4) + ". Skipping chunk.");
                    break;
                }

                if (readPos + static_cast<size_t>(currentChunkDataSize) > compressedBlockActualSize) {
                    LogError("[ConvertToDDS] [WARN] Chunk data size (" + std::to_string(currentChunkDataSize) + ") goes beyond compressed block bounds at LZ4 block #" + std::to_string(i) + ", offset " + std::to_string(readPos - 4) + ".\n");
                    break;
                }

                int bytesProcessed = LZ4_decompress_safe_continue(
                    lz4Stream,
                    compressedData.data() + readPos,
                    reinterpret_cast<char*>(decompressedBuffer.data()) + outPos,
                    currentChunkDataSize,
                    static_cast<int>(decompressedBuffer.size() - outPos)
                );

                if (bytesProcessed < 0) {
                    LogError("[ConvertToDDS] [ERROR] LZ4 decompress error at block #" + std::to_string(i) + ", chunk offset " + std::to_string(readPos - 4) + ". Error code: " + std::to_string(bytesProcessed) + ". Aborting chunk processing.\n");
                    break;
                }

                outPos += bytesProcessed;
                readPos += static_cast<size_t>(currentChunkDataSize);
            }

            LZ4_freeStreamDecode(lz4Stream);

            if (static_cast<uint32_t>(outPos) != decompressedTotalSize) {
                LogError("[ConvertToDDS] [WARN] Decompressed size mismatch for LZ4 block #" + std::to_string(i)
                    + " (expected " + std::to_string(decompressedTotalSize) + ", got " + std::to_string(outPos) + "). This might indicate a parsing issue.");
            }

            decodedData.insert(decodedData.begin(),
                decompressedBuffer.begin(),
                decompressedBuffer.begin() + outPos);
        }
    }

    std::vector<uint8_t> finalDDS;
    finalDDS.insert(finalDDS.end(), ddsHeader.begin(), ddsHeader.end());
    if (!ddsHeaderDx10.empty()) {
        finalDDS.insert(finalDDS.end(), ddsHeaderDx10.begin(), ddsHeaderDx10.end());
    }
    finalDDS.insert(finalDDS.end(), decodedData.begin(), decodedData.end());

    std::ofstream output(ddsPath, std::ios::binary);
    if (!output) {
        LogError("[ConvertToDDS] [ERROR] Cannot write DDS file: " + ddsPath);
        return;
    }
    output.write(reinterpret_cast<char*>(finalDDS.data()), finalDDS.size());
}