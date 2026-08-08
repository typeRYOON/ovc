#include <watch/memoryscan.h>
#include <vector>

namespace ovc::watch {

Signature parseSignature(const QString& pattern, int offset)
{
    Signature sig;
    sig.offset = offset;
    const QStringList tokens = pattern.split(' ', Qt::SkipEmptyParts);
    sig.bytes.reserve(tokens.size());
    sig.mask.reserve(tokens.size());
    for (const QString& tok : tokens) {
        if (tok == "??" || tok == "?") {
            sig.bytes.append('\0');
            sig.mask.append('\0');
        }
        else {
            sig.bytes.append(static_cast<char>(tok.toUInt(nullptr, 16) & 0xff));
            sig.mask.append('\x01');
        }
    }
    return sig;
}

namespace {

qsizetype findInBuffer(const QByteArray& buf, const Signature& sig)
{
    const int n = static_cast<int>(sig.bytes.size());
    if (n == 0 || buf.size() < n) return -1;
    const char* b = buf.constData();
    const char* pat = sig.bytes.constData();
    const char* msk = sig.mask.constData();
    const qsizetype limit = buf.size() - n;

    for (qsizetype i = 0; i <= limit; ++i) {
        bool ok = true;
        for (int j = 0; j < n; ++j) {
            if (msk[j] && b[i + j] != pat[j]) {
                ok = false;
                break;
            }
        }
        if (ok) return i;
    }
    return -1;
}

} // namespace

QHash<QString, uintptr_t> batchScan(const ProcessHandle& proc,
                                    const QHash<QString, Signature>& sigs)
{
    QHash<QString, uintptr_t> result;
    for (auto it = sigs.constBegin(); it != sigs.constEnd(); ++it)
        result.insert(it.key(), 0);

    // Regions are read in fixed chunks rather than whole: wine maps
    // multi-GB readable regions a single QByteArray shouldn't hold, and one
    // unreadable page inside a region must not void the rest of it (a whole-
    // region readv stops at the first hole). Chunks overlap by more than the
    // longest signature so a match straddling a boundary is still seen.
    constexpr size_t kChunk = 4u << 20;
    constexpr size_t kOverlap = 64;

    const std::vector<MemRegion> regions = proc.queryRegions();
    QByteArray buf;
    int remaining = static_cast<int>(sigs.size());

    for (const MemRegion& region : regions) {
        if (remaining == 0) break;
        const uintptr_t end = region.base + region.size;
        uintptr_t at = region.base;
        while (at < end && remaining != 0) {
            const size_t len = static_cast<size_t>(qMin<uintptr_t>(kChunk, end - at));
            buf.resize(static_cast<qsizetype>(len));
            if (proc.readBytes(at, buf.data(), len)) {
                for (auto it = sigs.constBegin(); it != sigs.constEnd(); ++it) {
                    if (result.value(it.key()) != 0) continue;
                    const qsizetype off = findInBuffer(buf, it.value());
                    if (off >= 0) {
                        result[it.key()] =
                            at + static_cast<uintptr_t>(off) + it.value().offset;
                        --remaining;
                    }
                }
            }
            if (len < kChunk) break;      // region tail reached
            at += kChunk - kOverlap;
        }
    }
    return result;
}

} // namespace ovc::watch
