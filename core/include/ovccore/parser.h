#pragma once
#include <ovccore/document.h>
#include <string_view>

namespace ovc::core {

// Never throws; arbitrary bytes degrade to classified raw lines.
ParseResult parseOsu(std::string_view bytes);

// Exact reassembly: serializeOsu(parseOsu(x).doc) == x.
std::string serializeOsu(const OsuDocument& doc);

} // namespace ovc::core
