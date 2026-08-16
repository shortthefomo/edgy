#pragma once

#include <string>

namespace edgy {

// SemVer. Edit this for each tagged release, same as rippled BuildInfo.
// develop / unreleased work stays on a -bN prerelease (e.g. 0.1.0-b0).
inline constexpr char const* kVersionBase = "0.1.0-b0";

[[nodiscard]] std::string
versionString();

}  // namespace edgy
