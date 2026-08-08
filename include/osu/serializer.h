#pragma once
#include <osu/document.h>

namespace ovc::osu {

// Exact reassembly of what parseOsu consumed: serializeOsu(parseOsu(x).doc) == x.
QByteArray serializeOsu(const OsuDocument& doc);

} // namespace ovc::osu
