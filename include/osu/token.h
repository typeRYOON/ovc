#pragma once
#include <QByteArray>
#include <QString>

namespace ovc::osu {

// A scalar value as it appeared in the file, byte-exact. .osu numbers like
// "315.789473684211" or "3.799999" must survive round-trips untouched, so the
// raw text is the value; numeric interpretation is derived on demand.
struct Token {
    QByteArray raw;

    bool isEmpty() const { return raw.isEmpty(); }
    QString text() const { return QString::fromUtf8(raw); }

    double toDouble(bool* ok = nullptr) const { return raw.toDouble(ok); } // C locale
    int toInt(bool* ok = nullptr) const { return raw.toInt(ok); }

    bool operator==(const Token& o) const { return raw == o.raw; }

    // Diff equality: byte-equal, or both parse fully as doubles that compare
    // exactly equal ("70" == "70.0", but "3.8" != "3.799999").
    bool numEquals(const Token& o) const;
};

} // namespace ovc::osu
