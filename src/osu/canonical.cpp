#include <osu/canonical.h>
#include <algorithm>
#include <cmath>

namespace ovc::osu {

namespace {

const Token kEmptyToken{};

QList<Token> splitFields(const QByteArray& raw)
{
    QList<Token> out;
    qsizetype start = 0;
    while (true) {
        const qsizetype comma = raw.indexOf(',', start);
        if (comma < 0) {
            out.append(Token{raw.mid(start).trimmed()});
            return out;
        }
        out.append(Token{raw.mid(start, comma - start).trimmed()});
        start = comma + 1;
    }
}

// First `maxParts - 1` colons split; the remainder (which may itself contain
// ':') becomes the last part — filenames are the only free-text field.
QList<Token> splitColonCapped(const QByteArray& raw, int maxParts)
{
    QList<Token> out;
    qsizetype start = 0;
    while (out.size() < maxParts - 1) {
        const qsizetype colon = raw.indexOf(':', start);
        if (colon < 0) break;
        out.append(Token{raw.mid(start, colon - start).trimmed()});
        start = colon + 1;
    }
    out.append(Token{raw.mid(start).trimmed()});
    return out;
}

void warn(QList<ParseWarning>* warnings, const QString& msg)
{
    if (warnings) warnings->append({0, msg});
}

} // namespace

const Token& TimingPoint::field(int i) const
{
    return i >= 0 && i < fields.size() ? fields[i] : kEmptyToken;
}

double TimingPoint::sv() const
{
    if (uninherited) return 1.0;
    const double bl = beatLength();
    return bl < 0 ? 100.0 / -bl : 1.0;
}

double TimingPoint::bpm() const
{
    if (!uninherited) return 0;
    const double bl = beatLength();
    return bl > 0 ? 60000.0 / bl : 0;
}

QList<Token> CanonicalNote::samplesNoEnd() const
{
    return isHold && !samples.isEmpty() ? samples.mid(1) : samples;
}

const Token* CanonicalMap::kv(SectionId section, QByteArrayView key) const
{
    const QList<QPair<QByteArray, Token>>* list = nullptr;
    switch (section) {
    case SectionId::General: list = &general; break;
    case SectionId::Editor: list = &editor; break;
    case SectionId::Metadata: list = &metadata; break;
    case SectionId::Difficulty: list = &difficulty; break;
    default: return nullptr;
    }
    for (const auto& [k, v] : *list)
        if (k == key) return &v;
    return nullptr;
}

CanonicalMap canonicalize(const OsuDocument& doc, QList<ParseWarning>* warnings)
{
    CanonicalMap map;
    map.formatVersion = doc.formatVersion;

    // KV sections first: Mode and CircleSize steer hitobject interpretation.
    for (const Section& sec : doc.sections) {
        QList<QPair<QByteArray, Token>>* target = nullptr;
        switch (sec.id) {
        case SectionId::General: target = &map.general; break;
        case SectionId::Editor: target = &map.editor; break;
        case SectionId::Metadata: target = &map.metadata; break;
        case SectionId::Difficulty: target = &map.difficulty; break;
        default: continue;
        }
        for (const RawLine& line : sec.lines) {
            if (line.kind != LineKind::KeyValue) continue;
            const qsizetype colon = line.raw.indexOf(':'); // first colon: Tags may contain ':'
            const QByteArray key = line.raw.left(colon).trimmed();
            const Token value{line.raw.mid(colon + 1).trimmed()};

            if (sec.id == SectionId::Editor && key == "Bookmarks") {
                for (const Token& t : splitFields(value.raw))
                    if (!t.isEmpty()) map.bookmarks.append(std::llround(t.toDouble()));
                std::sort(map.bookmarks.begin(), map.bookmarks.end());
                continue;
            }
            if (sec.id == SectionId::Metadata && key == "Tags") {
                for (const QByteArray& tag : value.raw.split(' '))
                    if (!tag.isEmpty()) map.tagList.append(tag);
                continue;
            }
            target->append({key, value});
        }
    }

    if (const Token* mode = map.kv(SectionId::General, "Mode")) map.mode = mode->toInt();
    if (map.mode == 3) {
        double cs = 5;
        if (const Token* t = map.kv(SectionId::Difficulty, "CircleSize")) cs = t->toDouble();
        map.keyCount = std::max(1, static_cast<int>(std::lround(cs)));
    }

    for (const Section& sec : doc.sections) {
        if (sec.id == SectionId::Events) {
            for (const RawLine& line : sec.lines) {
                if (line.kind != LineKind::Data) continue; // boilerplate comments stay physical
                const QList<Token> f = splitFields(line.raw);
                const QByteArray& kind = f[0].raw;
                if (kind == "0" && f.size() >= 3 && !map.backgroundFile) {
                    map.backgroundFile = f[2];
                }
                else if ((kind == "1" || kind == "Video") && f.size() >= 3 && !map.videoFile) {
                    map.videoFile = f[2];
                }
                else if ((kind == "2" || kind == "Break") && f.size() >= 3) {
                    BreakPeriod b;
                    b.start = f[1];
                    b.end = f[2];
                    b.startMs = std::llround(b.start.toDouble());
                    b.endMs = std::llround(b.end.toDouble());
                    map.breaks.append(b);
                }
                else {
                    map.storyboardLines.append(line.raw);
                }
            }
            std::stable_sort(map.breaks.begin(), map.breaks.end(),
                             [](const BreakPeriod& a, const BreakPeriod& b) {
                                 return a.startMs < b.startMs;
                             });
        }
        else if (sec.id == SectionId::TimingPoints) {
            for (const RawLine& line : sec.lines) {
                if (line.kind != LineKind::Data) continue;
                TimingPoint tp;
                tp.fields = splitFields(line.raw);
                tp.timeMs = tp.field(0).toDouble();
                // Field 7 absent in old formats: every point is uninherited then.
                tp.uninherited = tp.fields.size() > 6 ? tp.field(6).toInt() != 0 : true;
                tp.key.timeQ = std::llround(tp.timeMs * 1000.0);
                tp.key.redRank = tp.uninherited ? 0 : 1;
                map.timing.append(tp);
            }
            std::stable_sort(map.timing.begin(), map.timing.end(),
                             [](const TimingPoint& a, const TimingPoint& b) {
                                 return std::tie(a.key.timeQ, a.key.redRank) <
                                        std::tie(b.key.timeQ, b.key.redRank);
                             });
            for (int i = 1; i < map.timing.size(); ++i) {
                TimingPoint& cur = map.timing[i];
                const TimingPoint& prev = map.timing[i - 1];
                if (cur.key.timeQ == prev.key.timeQ && cur.key.redRank == prev.key.redRank) {
                    cur.key.occurrence = prev.key.occurrence + 1;
                    warn(warnings, QStringLiteral("duplicate timing point at %1ms")
                                       .arg(cur.timeMs));
                }
            }
        }
        else if (sec.id == SectionId::HitObjects) {
            for (const RawLine& line : sec.lines) {
                if (line.kind != LineKind::Data) continue;
                CanonicalNote n;
                // First five comma fields; the tail keeps everything else.
                QByteArray rest = line.raw;
                Token* fieldSlot[] = {&n.x, &n.y, &n.time, &n.type, &n.hitSound};
                qsizetype start = 0;
                int part = 0;
                for (; part < 5; ++part) {
                    const qsizetype comma = rest.indexOf(',', start);
                    if (comma < 0) break;
                    *fieldSlot[part] = Token{rest.mid(start, comma - start).trimmed()};
                    start = comma + 1;
                }
                if (part < 5) // short line (corrupt or ancient): rest fills the next slot
                    *fieldSlot[part] = Token{rest.mid(start).trimmed()};
                else
                    n.tail = rest.mid(start);

                n.timeMs = static_cast<int>(std::lround(n.time.toDouble()));
                n.typeBits = n.type.toInt();
                n.isHold = n.typeBits & 128;
                const QByteArray tailTrimmed = n.tail.trimmed();
                if (!tailTrimmed.isEmpty())
                    n.samples = splitColonCapped(tailTrimmed, n.isHold ? 6 : 5);
                n.endTimeMs = n.timeMs;
                if (n.isHold) {
                    if (!n.samples.isEmpty() && n.samples[0].toDouble() > 0)
                        n.endTimeMs = static_cast<int>(std::lround(n.samples[0].toDouble()));
                    else
                        warn(warnings, QStringLiteral("hold note at %1ms without end time")
                                           .arg(n.timeMs));
                }

                if (map.keyCount > 0) {
                    const int col =
                        static_cast<int>(std::floor(n.x.toDouble() * map.keyCount / 512.0));
                    n.column = std::clamp(col, 0, map.keyCount - 1);
                }
                n.key.timeMs = n.timeMs;
                n.key.column = n.column;
                map.notes.append(n);
            }
            std::stable_sort(map.notes.begin(), map.notes.end(),
                             [](const CanonicalNote& a, const CanonicalNote& b) {
                                 return std::tie(a.key.timeMs, a.key.column) <
                                        std::tie(b.key.timeMs, b.key.column);
                             });
            for (int i = 1; i < map.notes.size(); ++i) {
                CanonicalNote& cur = map.notes[i];
                const CanonicalNote& prev = map.notes[i - 1];
                if (cur.key.timeMs == prev.key.timeMs && cur.key.column == prev.key.column) {
                    cur.key.occurrence = prev.key.occurrence + 1;
                    if (map.mode == 3)
                        warn(warnings, QStringLiteral("duplicate note at %1ms column %2")
                                           .arg(cur.timeMs)
                                           .arg(cur.column));
                }
            }
        }
    }

    return map;
}

} // namespace ovc::osu
