#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

namespace ovc::core {

// osu! editor style: m:ss.mmm (matches the editor's copy-timestamp format).
inline std::string msToClock(int64_t ms)
{
    const bool neg = ms < 0;
    if (neg) ms = -ms;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s%lld:%02lld.%03lld", neg ? "-" : "",
                  (long long)(ms / 60000), (long long)(ms / 1000 % 60), (long long)(ms % 1000));
    return buf;
}

} // namespace ovc::core
