#include <ovccore/token.h>
#include <cstdlib>

namespace ovc::core {

namespace {

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

} // namespace

std::string_view trimView(std::string_view s)
{
    size_t b = 0, e = s.size();
    while (b < e && isSpace(s[b])) ++b;
    while (e > b && isSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::string trimCopy(std::string_view s)
{
    return std::string(trimView(s));
}

double Token::toDouble(bool* ok) const
{
    const std::string_view t = trimView(raw);
    if (t.empty()) {
        if (ok) *ok = false;
        return 0;
    }
    const std::string tmp(t); // strtod needs NUL termination
    char* end = nullptr;
    const double v = std::strtod(tmp.c_str(), &end);
    const bool fullParse = end == tmp.c_str() + tmp.size();
    if (ok) *ok = fullParse;
    return fullParse ? v : 0;
}

int Token::toInt(bool* ok) const
{
    const std::string_view t = trimView(raw);
    if (t.empty()) {
        if (ok) *ok = false;
        return 0;
    }
    const std::string tmp(t);
    char* end = nullptr;
    const long v = std::strtol(tmp.c_str(), &end, 10);
    const bool fullParse = end == tmp.c_str() + tmp.size();
    if (ok) *ok = fullParse;
    return fullParse ? int(v) : 0;
}

bool Token::numEquals(const Token& o) const
{
    if (raw == o.raw) return true;
    bool okA = false, okB = false;
    const double a = toDouble(&okA);
    const double b = o.toDouble(&okB);
    return okA && okB && a == b;
}

} // namespace ovc::core
