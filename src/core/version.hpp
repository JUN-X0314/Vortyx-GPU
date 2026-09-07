#pragma once

#define VORTYX_VERSION_MAJOR 0
#define VORTYX_VERSION_MINOR 17
#define VORTYX_VERSION_PATCH 0

#define VORTYX_VERSION_STRING "0.17.0"

inline constexpr int vortyx_version_major() { return VORTYX_VERSION_MAJOR; }
inline constexpr int vortyx_version_minor() { return VORTYX_VERSION_MINOR; }
inline constexpr int vortyx_version_patch() { return VORTYX_VERSION_PATCH; }
