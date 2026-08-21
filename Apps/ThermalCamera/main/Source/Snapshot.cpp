#include "Snapshot.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>

namespace {

/** Incremented for every snapshot, used when the clock is not set. */
uint32_t snapshotCounter = 0;

void writeLittleEndian32(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value & 0xFF);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    out[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

void writeLittleEndian16(uint8_t* out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value & 0xFF);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

} // namespace

void snapshotMakeBaseName(char* buffer, size_t bufferSize) {
    if (buffer == nullptr || bufferSize == 0) return;

    const time_t now = time(nullptr);
    struct tm parts = {};
    localtime_r(&now, &parts);

    // tm_year counts from 1900; anything before 2020 means the clock never got set.
    if (parts.tm_year > 120) {
        snprintf(
            buffer,
            bufferSize,
            "%04d%02d%02d_%02d%02d%02d",
            parts.tm_year + 1900,
            parts.tm_mon + 1,
            parts.tm_mday,
            parts.tm_hour,
            parts.tm_min,
            parts.tm_sec
        );
    } else {
        snprintf(buffer, bufferSize, "thermal_%04u", static_cast<unsigned>(snapshotCounter));
    }
    snapshotCounter++;
}

bool snapshotWriteBmp(const char* path, const uint16_t* pixels, int width, int height) {
    if (path == nullptr || pixels == nullptr || width <= 0 || height <= 0) return false;

    const int rowBytes = width * 3;
    const int padding = (4 - (rowBytes % 4)) % 4;
    const int paddedRowBytes = rowBytes + padding;
    const uint32_t imageSize = static_cast<uint32_t>(paddedRowBytes) * static_cast<uint32_t>(height);
    const uint32_t fileSize = 54u + imageSize;

    uint8_t header[54];
    memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    writeLittleEndian32(header + 2, fileSize);
    writeLittleEndian32(header + 10, 54); // pixel data offset
    writeLittleEndian32(header + 14, 40); // DIB header size
    writeLittleEndian32(header + 18, static_cast<uint32_t>(width));
    writeLittleEndian32(header + 22, static_cast<uint32_t>(height));
    writeLittleEndian16(header + 26, 1);  // colour planes
    writeLittleEndian16(header + 28, 24); // bits per pixel
    writeLittleEndian32(header + 34, imageSize);
    writeLittleEndian32(header + 38, 2835); // 72 DPI in pixels per metre
    writeLittleEndian32(header + 42, 2835);

    auto* row = new (std::nothrow) uint8_t[paddedRowBytes];
    if (row == nullptr) return false;
    memset(row, 0, paddedRowBytes);

    FILE* file = fopen(path, "wb");
    if (file == nullptr) {
        delete[] row;
        return false;
    }

    bool success = fwrite(header, 1, sizeof(header), file) == sizeof(header);

    // Bitmaps store the bottom row first.
    for (int y = height - 1; success && y >= 0; y--) {
        const uint16_t* source = pixels + static_cast<size_t>(y) * static_cast<size_t>(width);
        for (int x = 0; x < width; x++) {
            const uint16_t color = source[x];
            const uint8_t r = static_cast<uint8_t>(((color >> 11) & 0x1F) * 255 / 31);
            const uint8_t g = static_cast<uint8_t>(((color >> 5) & 0x3F) * 255 / 63);
            const uint8_t b = static_cast<uint8_t>((color & 0x1F) * 255 / 31);
            row[x * 3 + 0] = b;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
        success = fwrite(row, 1, static_cast<size_t>(paddedRowBytes), file) ==
                  static_cast<size_t>(paddedRowBytes);
    }

    if (fclose(file) != 0) success = false;
    delete[] row;
    return success;
}

bool snapshotWriteCsv(const char* path, const float* frame, const SnapshotMetadata& metadata) {
    if (path == nullptr || frame == nullptr) return false;

    FILE* file = fopen(path, "w");
    if (file == nullptr) return false;

    bool success = fprintf(
                       file,
                       "# MLX90640 radiometric data, degrees Celsius\n"
                       "# sensor=MLX90640,width=%d,height=%d\n"
                       "# emissivity=%.2f,reflected=%.1f,ambient=%.1f\n"
                       "# min=%.2f,max=%.2f,avg=%.2f,palette=%s\n",
                       MLX_WIDTH,
                       MLX_HEIGHT,
                       static_cast<double>(metadata.emissivity),
                       static_cast<double>(metadata.reflectedTemperature),
                       static_cast<double>(metadata.ambientTemperature),
                       static_cast<double>(metadata.minimum),
                       static_cast<double>(metadata.maximum),
                       static_cast<double>(metadata.average),
                       metadata.paletteName != nullptr ? metadata.paletteName : ""
                   ) > 0;

    for (int y = 0; success && y < MLX_HEIGHT; y++) {
        for (int x = 0; x < MLX_WIDTH; x++) {
            const int written = fprintf(
                file,
                x + 1 < MLX_WIDTH ? "%.2f," : "%.2f\n",
                static_cast<double>(frame[y * MLX_WIDTH + x])
            );
            if (written <= 0) {
                success = false;
                break;
            }
        }
    }

    if (fclose(file) != 0) success = false;
    return success;
}
