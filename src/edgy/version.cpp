#include <edgy/version.hpp>

namespace edgy {
namespace {

#if defined(EDGY_GIT_HASH)
char const* const kGitHash = EDGY_GIT_HASH;
#else
char const* const kGitHash = "";
#endif

}  // namespace

std::string
versionString()
{
    std::string v{kVersionBase};
    std::string meta;
    if (kGitHash != nullptr && kGitHash[0] != '\0')
        meta += kGitHash;
#if !defined(NDEBUG)
    if (!meta.empty())
        meta += '.';
    meta += "DEBUG";
#endif
    if (!meta.empty())
    {
        v += '+';
        v += meta;
    }
    return v;
}

}  // namespace edgy
