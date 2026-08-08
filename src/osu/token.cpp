#include <osu/token.h>

namespace ovc::osu {

bool Token::numEquals(const Token& o) const
{
    if (raw == o.raw) return true;
    bool okA = false, okB = false;
    const double a = toDouble(&okA);
    const double b = o.toDouble(&okB);
    return okA && okB && a == b;
}

} // namespace ovc::osu
