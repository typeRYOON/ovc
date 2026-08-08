#pragma once
#include <osu/document.h>

namespace ovc::osu {

// Never throws; arbitrary bytes degrade to classified raw lines.
ParseResult parseOsu(const QByteArray& bytes);

} // namespace ovc::osu
