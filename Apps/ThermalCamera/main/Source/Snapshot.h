/**
 * @file Snapshot.h
 * @brief Saving of thermal images and radiometric data to storage.
 */
#pragma once

#include "Settings.h"

#include <cstdint>

/** Extra information stored alongside the raw temperatures. */
struct SnapshotMetadata {
    float emissivity = 0.95f;
    float reflectedTemperature = 22.0f;
    float ambientTemperature = 25.0f;
    float minimum = 0.0f;
    float maximum = 0.0f;
    float average = 0.0f;
    const char* paletteName = "Iron";
};

/**
 * @brief Build a base name for a snapshot pair, e.g. "20260820_143012".
 *
 * Falls back to a running index when the system clock has not been set.
 *
 * @param[out] buffer receives the name
 * @param[in] bufferSize size of @p buffer including the terminator
 */
void snapshotMakeBaseName(char* buffer, size_t bufferSize);

/**
 * @brief Write an RGB565 image as a 24-bit Windows bitmap.
 * @param path full path of the file to create
 * @param pixels width * height RGB565 pixels, top row first
 * @return true when the whole file was written
 */
bool snapshotWriteBmp(const char* path, const uint16_t* pixels, int width, int height);

/**
 * @brief Write the raw 32x24 temperature array as CSV, with a metadata header.
 *
 * Temperatures are always written in degrees Celsius so the file is
 * independent of the display unit.
 */
bool snapshotWriteCsv(const char* path, const float* frame, const SnapshotMetadata& metadata);
