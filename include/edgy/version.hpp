#pragma once

#include <string>

namespace edgy {

// SemVer. Edit this for each tagged release, same as rippled BuildInfo.
// After a tag, bump develop to the next -bN prerelease (e.g. 0.1.7-b0).
inline constexpr char const* kVersionBase = "0.1.8-b50";

[[nodiscard]] std::string
versionString();

}  // namespace edgy
