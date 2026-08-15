#include <ovccore/canonical.h>
#include <algorithm>
#include <cmath>

namespace ovc::core {

namespace {

const Token kEmptyToken{};

std::vector<Token> splitFields(std::string_view raw)
{
    std::vector<Token> out;
    size_t start = 0;
    while (true) {
        const size_t comma = raw.find(',', start);
        if (comma == std::string_view::npos) {
            out.push_back(Token{trimCopy(raw.substr(start))});
            return out;
        }
        out.push_back(Token{trimCopy(raw.substr(start, comma - start))});
        start = comma + 1;
    }
}

// First `maxParts - 1` colons split; the remainder (which may itself contain
// ':') becomes the last part — filenames are the only free-text field.
std::vector<Token> splitColonCapped(std::string_view raw, size_t maxParts)
{
    std::vector<Token> out;
    size_t start = 0;
    while (out.size() < maxParts - 1) {
        const size_t colon = raw.find(':', start);
        if (colon == std::string_view::npos) break;
        out.push_back(Token{trimCopy(raw.substr(start, colon - start))});
        start = colon + 1;
    }
    out.push_back(Token{trimCopy(raw.substr(start))});
    return out;
}

void warn(std::vector<ParseWarning>* warnings, std::string msg)
{
    if (warnings) warnings->push_back({0, std::move(msg)});
}

} // namespace

const Token& TimingPoint::field(int i) const
{
    return i >= 0 && size_t(i) < fields.size() ? fields[i] : kEmptyToken;
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

std::vector<Token> CanonicalNote::samplesNoEnd() const
{
    if (isHold && !samples.empty()) return {samples.begin() + 1, samples.end()};
    return samples;
}

void deriveNoteFields(CanonicalNote& n, int keyCount)
{
    // Keep in sync with the [HitObjects] parse loop below.
    n.timeMs = int(std::lround(n.time.toDouble()));
    n.typeBits = n.type.toInt();
    n.isHold = n.typeBits & 128;
    n.samples.clear();
    const std::string_view tailTrimmed = trimView(n.tail);
    if (!tailTrimmed.empty())
        n.samples = splitColonCapped(tailTrimmed, n.isHold ? 6 : 5);
    n.endTimeMs = n.timeMs;
    if (n.isHold && !n.samples.empty() && n.samples[0].toDouble() > 0)
        n.endTimeMs = int(std::lround(n.samples[0].toDouble()));
    n.column = 0;
    if (keyCount > 0) {
        const int col = int(std::floor(n.x.toDouble() * keyCount / 512.0));
        n.column = std::clamp(col, 0, keyCount - 1);
    }
    n.key.timeMs = n.timeMs;
    n.key.column = n.column;
}

const Token* CanonicalMap::kv(SectionId section, std::string_view key) const
{
    const std::vector<std::pair<std::string, Token>>* list = nullptr;
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

CanonicalMap canonicalize(const OsuDocument& doc, std::vector<ParseWarning>* warnings)
{
    CanonicalMap map;
    map.formatVersion = doc.formatVersion;

    // KV sections first: Mode and CircleSize steer hitobject interpretation.
    for (const Section& sec : doc.sections) {
        std::vector<std::pair<std::string, Token>>* target = nullptr;
        switch (sec.id) {
        case SectionId::General: target = &map.general; break;
        case SectionId::Editor: target = &map.editor; break;
        case SectionId::Metadata: target = &map.metadata; break;
        case SectionId::Difficulty: target = &map.difficulty; break;
        default: continue;
        }
        for (const RawLine& line : sec.lines) {
            if (line.kind != LineKind::KeyValue) continue;
            const size_t colon = line.raw.find(':'); // first colon: Tags may contain ':'
            const std::string key = trimCopy(std::string_view(line.raw).substr(0, colon));
            Token value{trimCopy(std::string_view(line.raw).substr(colon + 1))};

            if (sec.id == SectionId::Editor && key == "Bookmarks") {
                for (const Token& t : splitFields(value.raw))
                    if (!t.empty()) map.bookmarks.push_back(int64_t(std::llround(t.toDouble())));
                std::sort(map.bookmarks.begin(), map.bookmarks.end());
                continue;
            }
            if (sec.id == SectionId::Metadata && key == "Tags") {
                size_t start = 0;
                while (start <= value.raw.size()) {
                    const size_t space = value.raw.find(' ', start);
                    const size_t end = space == std::string::npos ? value.raw.size() : space;
                    if (end > start) map.tagList.push_back(value.raw.substr(start, end - start));
                    if (space == std::string::npos) break;
                    start = space + 1;
                }
                continue;
            }
            target->emplace_back(key, std::move(value));
        }
    }

    if (const Token* mode = map.kv(SectionId::General, "Mode")) map.mode = mode->toInt();
    if (map.mode == 3) {
        double cs = 5;
        if (const Token* t = map.kv(SectionId::Difficulty, "CircleSize")) cs = t->toDouble();
        map.keyCount = std::max(1, int(std::lround(cs)));
    }

    for (const Section& sec : doc.sections) {
        if (sec.id == SectionId::Events) {
            for (const RawLine& line : sec.lines) {
                if (line.kind != LineKind::Data) continue;
                const std::vector<Token> f = splitFields(line.raw);
                const std::string& kind = f[0].raw;
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
                    b.startMs = int64_t(std::llround(b.start.toDouble()));
                    b.endMs = int64_t(std::llround(b.end.toDouble()));
                    map.breaks.push_back(std::move(b));
                }
                else {
                    map.storyboardLines.push_back(line.raw);
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
                tp.key.timeQ = int64_t(std::llround(tp.timeMs * 1000.0));
                tp.key.redRank = tp.uninherited ? 0 : 1;
                map.timing.push_back(std::move(tp));
            }
            std::stable_sort(map.timing.begin(), map.timing.end(),
                             [](const TimingPoint& a, const TimingPoint& b) {
                                 return std::tie(a.key.timeQ, a.key.redRank) <
                                        std::tie(b.key.timeQ, b.key.redRank);
                             });
            for (size_t i = 1; i < map.timing.size(); ++i) {
                TimingPoint& cur = map.timing[i];
                const TimingPoint& prev = map.timing[i - 1];
                if (cur.key.timeQ == prev.key.timeQ && cur.key.redRank == prev.key.redRank) {
                    cur.key.occurrence = prev.key.occurrence + 1;
                    warn(warnings,
                         "duplicate timing point at " + std::to_string(cur.timeMs) + "ms");
                }
            }
        }
        else if (sec.id == SectionId::HitObjects) {
            for (const RawLine& line : sec.lines) {
                if (line.kind != LineKind::Data) continue;
                CanonicalNote n;
                const std::string_view rest = line.raw;
                Token* fieldSlot[] = {&n.x, &n.y, &n.time, &n.type, &n.hitSound};
                size_t start = 0;
                int part = 0;
                for (; part < 5; ++part) {
                    const size_t comma = rest.find(',', start);
                    if (comma == std::string_view::npos) break;
                    *fieldSlot[part] = Token{trimCopy(rest.substr(start, comma - start))};
                    start = comma + 1;
                }
                if (part < 5) // short line (corrupt or ancient): rest fills the next slot
                    *fieldSlot[part] = Token{trimCopy(rest.substr(start))};
                else
                    n.tail = std::string(rest.substr(start));

                n.timeMs = int(std::lround(n.time.toDouble()));
                n.typeBits = n.type.toInt();
                n.isHold = n.typeBits & 128;
                const std::string_view tailTrimmed = trimView(n.tail);
                if (!tailTrimmed.empty())
                    n.samples = splitColonCapped(tailTrimmed, n.isHold ? 6 : 5);
                n.endTimeMs = n.timeMs;
                if (n.isHold) {
                    if (!n.samples.empty() && n.samples[0].toDouble() > 0)
                        n.endTimeMs = int(std::lround(n.samples[0].toDouble()));
                    else
                        warn(warnings, "hold note at " + std::to_string(n.timeMs) +
                                           "ms without end time");
                }

                if (map.keyCount > 0) {
                    const int col = int(std::floor(n.x.toDouble() * map.keyCount / 512.0));
                    n.column = std::clamp(col, 0, map.keyCount - 1);
                }
                n.key.timeMs = n.timeMs;
                n.key.column = n.column;
                map.notes.push_back(std::move(n));
            }
            std::stable_sort(map.notes.begin(), map.notes.end(),
                             [](const CanonicalNote& a, const CanonicalNote& b) {
                                 return std::tie(a.key.timeMs, a.key.column) <
                                        std::tie(b.key.timeMs, b.key.column);
                             });
            for (size_t i = 1; i < map.notes.size(); ++i) {
                CanonicalNote& cur = map.notes[i];
                const CanonicalNote& prev = map.notes[i - 1];
                if (cur.key.timeMs == prev.key.timeMs && cur.key.column == prev.key.column) {
                    cur.key.occurrence = prev.key.occurrence + 1;
                    if (map.mode == 3)
                        warn(warnings, "duplicate note at " + std::to_string(cur.timeMs) +
                                           "ms column " + std::to_string(cur.column));
                }
            }
        }
    }

    return map;
}

} // namespace ovc::core
