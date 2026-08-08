#pragma once
#include <QString>

namespace ovc::utils {

// osu! editor style: m:ss.mmm (matches the editor's copy-timestamp format).
inline QString msToClock(qint64 ms)
{
    const bool neg = ms < 0;
    if (neg) ms = -ms;
    return QStringLiteral("%1%2:%3.%4")
        .arg(neg ? QStringLiteral("-") : QString())
        .arg(ms / 60000)
        .arg(ms / 1000 % 60, 2, 10, QLatin1Char('0'))
        .arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

} // namespace ovc::utils
