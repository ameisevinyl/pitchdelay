#pragma once

namespace pitchdelay::ids
{
inline constexpr const char* mode = "mode";
inline constexpr const char* fraction = "fraction";
inline constexpr const char* calibration = "calibration";
inline constexpr const char* calibrationLevel = "calibrationLevel";
inline constexpr const char* bypass = "bypass";

// First-release parameters: read only to migrate old sessions, never created.
inline constexpr const char* legacyCustomUnit = "customUnit";
inline constexpr const char* legacyCustomMs = "customMs";
inline constexpr const char* legacyCustomSamples = "customSamples";
}
