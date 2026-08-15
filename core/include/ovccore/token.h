#pragma once
#include <string>
#include <string_view>

namespace ovc::core {

// A scalar value as it appeared in the file, byte-exact. .osu numbers like
// "315.789473684211" or "3.799999" must survive round-trips untouched, so the
// raw text is the value; numeric interpretation is derived on demand.
struct Token {
    std::string raw;

    bool empty() const { return raw.empty(); }

    // Full-string strict parses, C locale (matches osu!'s own parsing).
    double toDouble(bool* ok = nullptr) const;
    int toInt(bool* ok = nullptr) const;

    bool operator==(const Token& o) const { return raw == o.raw; }

    // Diff equality: byte-equal, or both parse fully as doubles that compare
    // exactly equal ("70" == "70.0", but "3.8" != "3.799999").
    bool numEquals(const Token& o) const;
};

// ASCII trim/split helpers shared across the core.
std::string_view trimView(std::string_view s);
std::string trimCopy(std::string_view s);

} // namespace ovc::core
