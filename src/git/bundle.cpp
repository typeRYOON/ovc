#include <git/bundle.h>
#include <git/paths.h>
#include <git/shadowrepo.h>
#include <ovccore/canonical.h>
#include <ovccore/merge.h>
#include <ovccore/parser.h>
#include <ovccore/peek.h>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <cstring>
#include <miniz/miniz.h>

namespace ovc::git {

namespace {

constexpr int kFormatVersion = 1;

bool isInfra(const QString& p)
{
    return p == QStringLiteral(".gitattributes") || p.startsWith(QStringLiteral(".ovc/"));
}

bool isTextPath(const QString& p)
{
    return p.endsWith(QStringLiteral(".osu"), Qt::CaseInsensitive) ||
           p.endsWith(QStringLiteral(".osb"), Qt::CaseInsensitive);
}

bool fail(QString* err, const QString& message)
{
    if (err) *err = message;
    return false;
}

} // namespace

bool exportBundle(const MapsetEntry& entry, const QString& outPath, bool textOnly, QString* err)
{
    auto repo = ShadowRepo::open(entry.repoDir());
    if (!repo) return fail(err, QStringLiteral("repo missing: ") + entry.repoDir());

    auto history = repo->log(100000);
    std::reverse(history.begin(), history.end()); // oldest first

    QJsonArray snapshots;
    QSet<QByteArray> wantBlobs;
    for (const auto& c : history) {
        QJsonArray tree;
        for (const auto& [relPath, blobOid] : repo->listTree(c.oid)) {
            if (isInfra(relPath)) continue;
            tree.append(QJsonObject{{QStringLiteral("path"), relPath},
                                    {QStringLiteral("blob"), QString::fromUtf8(blobOid)},
                                    {QStringLiteral("size"),
                                     double(repo->blobSize(blobOid))}});
            if (!textOnly || isTextPath(relPath)) wantBlobs.insert(blobOid);
        }
        QJsonObject trailers;
        for (auto it = c.trailers.constBegin(); it != c.trailers.constEnd(); ++it)
            trailers.insert(it.key(), it.value());
        snapshots.append(QJsonObject{{QStringLiteral("oid"), QString::fromUtf8(c.oid)},
                                     {QStringLiteral("when"),
                                      c.when.toUTC().toString(Qt::ISODate)},
                                     {QStringLiteral("subject"), c.subject},
                                     {QStringLiteral("label"), c.label},
                                     {QStringLiteral("trailers"), trailers},
                                     {QStringLiteral("tree"), tree}});
    }

    QJsonArray ids;
    for (int id : entry.beatmapIds) ids.append(id);
    const QJsonObject bundleJson{
        {QStringLiteral("formatVersion"), kFormatVersion},
        {QStringLiteral("textOnly"), textOnly},
        {QStringLiteral("mapset"),
         QJsonObject{{QStringLiteral("beatmapSetId"), entry.beatmapSetId},
                     {QStringLiteral("beatmapIds"), ids},
                     {QStringLiteral("folderName"), entry.folderName},
                     {QStringLiteral("title"), entry.title},
                     {QStringLiteral("artist"), entry.artist},
                     {QStringLiteral("creator"), entry.creator}}},
        {QStringLiteral("snapshots"), snapshots}};

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_heap(&zip, 0, 1 << 20))
        return fail(err, QStringLiteral("zip init failed"));

    auto addEntry = [&](const QByteArray& name, const QByteArray& data, bool compress) {
        return mz_zip_writer_add_mem(&zip, name.constData(), data.constData(),
                                     size_t(data.size()),
                                     compress ? MZ_BEST_SPEED : MZ_NO_COMPRESSION) == MZ_TRUE;
    };

    bool ok = addEntry("bundle.json", QJsonDocument(bundleJson).toJson(QJsonDocument::Compact),
                       true);
    for (const QByteArray& blobOid : wantBlobs) {
        if (!ok) break;
        const QByteArray data = repo->readBlob(blobOid);
        // Media is already compressed; only text earns deflate.
        ok = addEntry("blobs/" + blobOid, data, data.size() < 1 << 20);
    }

    void* buf = nullptr;
    size_t bufSize = 0;
    ok = ok && mz_zip_writer_finalize_heap_archive(&zip, &buf, &bufSize) == MZ_TRUE;
    if (!ok) {
        mz_zip_writer_end(&zip);
        return fail(err, QStringLiteral("zip write failed"));
    }

    QFile out(outPath);
    const bool written =
        out.open(QIODevice::WriteOnly) && out.write(static_cast<const char*>(buf),
                                                    qint64(bufSize)) == qint64(bufSize);
    mz_free(buf);
    mz_zip_writer_end(&zip);
    return written ? true : fail(err, QStringLiteral("cannot write ") + outPath);
}

namespace {

struct OpenedBundle {
    QByteArray zipBytes;
    mz_zip_archive zip{};
    QJsonObject bundleJson;
    ~OpenedBundle() { mz_zip_reader_end(&zip); }
};

bool openBundle(const QString& path, OpenedBundle& out, QString* err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        fail(err, QStringLiteral("cannot read ") + path);
        return false;
    }
    out.zipBytes = f.readAll();
    if (!mz_zip_reader_init_mem(&out.zip, out.zipBytes.constData(), size_t(out.zipBytes.size()),
                                0)) {
        fail(err, QStringLiteral("not a bundle (zip open failed)"));
        return false;
    }
    const int idx = mz_zip_reader_locate_file(&out.zip, "bundle.json", nullptr, 0);
    if (idx < 0) {
        fail(err, QStringLiteral("not a bundle (bundle.json missing)"));
        return false;
    }
    size_t size = 0;
    void* data = mz_zip_reader_extract_to_heap(&out.zip, mz_uint(idx), &size, 0);
    if (!data) {
        fail(err, QStringLiteral("bundle.json extract failed"));
        return false;
    }
    out.bundleJson =
        QJsonDocument::fromJson(QByteArray(static_cast<const char*>(data), qsizetype(size)))
            .object();
    mz_free(data);
    if (out.bundleJson.value(QStringLiteral("formatVersion")).toInt() != kFormatVersion) {
        fail(err, QStringLiteral("unsupported bundle version"));
        return false;
    }
    return true;
}

QByteArray extractBlob(mz_zip_archive& zip, const QByteArray& blobOid)
{
    const int idx = mz_zip_reader_locate_file(&zip, ("blobs/" + blobOid).constData(), nullptr, 0);
    if (idx < 0) return {};
    size_t size = 0;
    void* data = mz_zip_reader_extract_to_heap(&zip, mz_uint(idx), &size, 0);
    if (!data) return {};
    QByteArray out(static_cast<const char*>(data), qsizetype(size));
    mz_free(data);
    return out;
}

} // namespace

std::optional<BundleInfo> peekBundle(const QString& bundlePath, QString* err)
{
    OpenedBundle b;
    if (!openBundle(bundlePath, b, err)) return std::nullopt;
    const QJsonObject mapset = b.bundleJson.value(QStringLiteral("mapset")).toObject();
    BundleInfo info;
    info.title = mapset.value(QStringLiteral("title")).toString();
    info.artist = mapset.value(QStringLiteral("artist")).toString();
    info.creator = mapset.value(QStringLiteral("creator")).toString();
    info.snapshotCount = b.bundleJson.value(QStringLiteral("snapshots")).toArray().size();
    info.textOnly = b.bundleJson.value(QStringLiteral("textOnly")).toBool();
    return info;
}

std::optional<MapsetEntry> importBundle(const QString& bundlePath, const QString& intoSongsFolder,
                                        QString* err)
{
    OpenedBundle b;
    if (!openBundle(bundlePath, b, err)) return std::nullopt;

    const QJsonObject mapset = b.bundleJson.value(QStringLiteral("mapset")).toObject();
    Registry reg = Registry::load();

    MapsetEntry entry;
    entry.repoId = reg.newRepoId();
    entry.beatmapSetId = mapset.value(QStringLiteral("beatmapSetId")).toInt(-1);
    for (const auto& id : mapset.value(QStringLiteral("beatmapIds")).toArray())
        entry.beatmapIds.append(id.toInt());
    entry.folderName = mapset.value(QStringLiteral("folderName")).toString();
    entry.title = mapset.value(QStringLiteral("title")).toString();
    entry.artist = mapset.value(QStringLiteral("artist")).toString();
    entry.creator = mapset.value(QStringLiteral("creator")).toString();
    entry.songsPath = QDir::cleanPath(intoSongsFolder); // may be empty: view-only archive
    entry.trackedSince = QDateTime::currentDateTimeUtc();
    entry.autoSnapshot = false; // imported history; local saves shouldn't auto-append

    const QString repoDir = entry.repoDir();
    if (!ShadowRepo::create(repoDir, err)) return std::nullopt;
    auto repo = ShadowRepo::open(repoDir);
    if (!repo) {
        fail(err, QStringLiteral("cannot open repo ") + repoDir);
        return std::nullopt;
    }

    // Replay snapshots oldest→newest, touching only paths whose blob changed.
    QHash<QString, QByteArray> workdirState; // relPath -> blob oid
    const QJsonArray snapshots = b.bundleJson.value(QStringLiteral("snapshots")).toArray();
    for (const auto& sv : snapshots) {
        const QJsonObject snap = sv.toObject();
        QHash<QString, QByteArray> target;
        for (const auto& tv : snap.value(QStringLiteral("tree")).toArray()) {
            const QJsonObject t = tv.toObject();
            target.insert(t.value(QStringLiteral("path")).toString(),
                          t.value(QStringLiteral("blob")).toString().toUtf8());
        }

        for (auto it = workdirState.constBegin(); it != workdirState.constEnd(); ++it)
            if (!target.contains(it.key())) QFile::remove(repoDir + '/' + it.key());
        for (auto it = target.constBegin(); it != target.constEnd(); ++it) {
            if (workdirState.value(it.key()) == it.value()) continue;
            const QByteArray data = extractBlob(b.zip, it.value());
            if (data.isEmpty() && !isTextPath(it.key())) continue; // text-only bundle: media absent
            QDir().mkpath(QFileInfo(repoDir + '/' + it.key()).absolutePath());
            QFile out(repoDir + '/' + it.key());
            if (!out.open(QIODevice::WriteOnly)) {
                fail(err, QStringLiteral("cannot write ") + it.key());
                return std::nullopt;
            }
            out.write(data);
        }
        workdirState = target;

        QMap<QString, QString> trailers;
        const QJsonObject trailersJson = snap.value(QStringLiteral("trailers")).toObject();
        for (auto it = trailersJson.constBegin(); it != trailersJson.constEnd(); ++it)
            trailers.insert(it.key(), it.value().toString());
        trailers.insert(QStringLiteral("Ovc-Bundle-Oid"),
                        snap.value(QStringLiteral("oid")).toString());

        const auto tree = repo->stageAll(err);
        const QDateTime when = QDateTime::fromString(
            snap.value(QStringLiteral("when")).toString(), Qt::ISODate);
        if (tree) {
            const QByteArray newOid =
                repo->commitStaged(*tree, snap.value(QStringLiteral("subject")).toString(),
                                   trailers, err, when);
            if (newOid.isEmpty()) return std::nullopt;
            // Re-key the label onto the new commit (import mints fresh oids).
            const QString label = snap.value(QStringLiteral("label")).toString();
            if (!label.isEmpty()) repo->setLabel(newOid, label);
        }
    }

    reg.entries.append(entry);
    if (!reg.save(err)) return std::nullopt;
    return entry;
}

int BundleMergeReport::totalConflicts() const
{
    int n = 0;
    for (const FileMergeResult& f : files) n += f.conflictCount();
    return n;
}

int BundleMergeReport::mediaWritten() const
{
    int n = 0;
    for (const MediaMergeResult& m : media)
        if (m.op == MediaMergeResult::Op::Added || m.op == MediaMergeResult::Op::Updated) ++n;
    return n;
}

int BundleMergeReport::mediaKeptOurs() const
{
    int n = 0;
    for (const MediaMergeResult& m : media)
        if (m.op == MediaMergeResult::Op::KeptOurs) ++n;
    return n;
}

bool BundleMergeReport::anyChange() const
{
    for (const FileMergeResult& f : files)
        if (f.changed) return true;
    return mediaWritten() > 0;
}

namespace {

std::string_view sview(const QByteArray& b)
{
    return {b.constData(), size_t(b.size())};
}

// theirs' blob for `path` at snapshot index `i`, or empty.
QByteArray blobInSnapshot(const QJsonArray& snaps, int i, const QString& path)
{
    for (const auto& tv : snaps[i].toObject().value(QStringLiteral("tree")).toArray()) {
        const QJsonObject t = tv.toObject();
        if (t.value(QStringLiteral("path")).toString() == path)
            return t.value(QStringLiteral("blob")).toString().toUtf8();
    }
    return {};
}

bool writeSongsFile(const QString& songsPath, const QString& relPath, const QByteArray& bytes)
{
    const QString dest = songsPath + QLatin1Char('/') + relPath;
    QDir().mkpath(QFileInfo(dest).absolutePath());
    QFile f(dest);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

QByteArray readSongsFile(const QString& songsPath, const QString& relPath)
{
    QFile f(songsPath + QLatin1Char('/') + relPath);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

int PreparedMerge::totalConflicts() const
{
    int n = 0;
    for (const PreparedFile& f : files) n += int(f.conflicts.size());
    return n;
}

bool PreparedMerge::hasConflicts() const
{
    for (const PreparedFile& f : files)
        if (!f.conflicts.isEmpty()) return true;
    return false;
}

bool PreparedMerge::anyMergeable() const
{
    for (const PreparedFile& f : files)
        if (f.addedByThem || (!f.wholeFileConflict && (f.oursText != f.theirsText))) return true;
    for (const PreparedMedia& m : media)
        if (m.op != PreparedMedia::Op::Conflict) return true; // added/updated media
    return false;
}

std::optional<PreparedMerge> prepareBundleMerge(const MapsetEntry& ours, const QString& bundlePath,
                                                QString* err)
{
    if (ours.songsPath.isEmpty()) {
        fail(err, QStringLiteral("no local folder linked to this mapset"));
        return std::nullopt;
    }
    OpenedBundle b;
    if (!openBundle(bundlePath, b, err)) return std::nullopt;
    auto repo = ShadowRepo::open(ours.repoDir());
    if (!repo) {
        fail(err, QStringLiteral("repo missing: ") + ours.repoDir());
        return std::nullopt;
    }

    PreparedMerge prepared;
    prepared.repoId = ours.repoId;
    prepared.bundlePath = bundlePath;
    prepared.bundleTitle = b.bundleJson.value(QStringLiteral("mapset"))
                               .toObject()
                               .value(QStringLiteral("title"))
                               .toString();

    QHash<QString, QByteArray> oursHead;
    for (const auto& [p, oid] : repo->listTree(repo->headOid())) oursHead.insert(p, oid);
    // Blob history per path — the content-addressed merge-base for both the .osu
    // merge and the media sync (a shared blob oid = shared content).
    QHash<QString, QSet<QByteArray>> oursHist;
    for (const auto& c : repo->log(100000))
        for (const auto& [p, oid] : repo->listTree(c.oid))
            if (!isInfra(p)) oursHist[p].insert(oid);

    const QJsonArray snaps = b.bundleJson.value(QStringLiteral("snapshots")).toArray();
    if (snaps.isEmpty()) return prepared;

    QHash<QString, QByteArray> theirsHead;
    for (const auto& tv : snaps.last().toObject().value(QStringLiteral("tree")).toArray()) {
        const QJsonObject t = tv.toObject();
        const QString p = t.value(QStringLiteral("path")).toString();
        if (p.endsWith(QStringLiteral(".osu"), Qt::CaseInsensitive))
            theirsHead.insert(p, t.value(QStringLiteral("blob")).toString().toUtf8());
    }

    for (auto it = theirsHead.constBegin(); it != theirsHead.constEnd(); ++it) {
        const QString path = it.key();
        const QByteArray theirsBlob = it.value();
        const QByteArray theirsText = extractBlob(b.zip, theirsBlob);

        if (!oursHead.contains(path)) { // a difficulty they added
            if (theirsText.isEmpty()) continue;
            PreparedFile f;
            f.relPath = path;
            f.addedByThem = true;
            f.theirsText = theirsText;
            const auto h = ovc::core::peekOsuHeader(sview(theirsText.left(8192)));
            f.version = h ? QString::fromStdString(h->version) : path;
            f.mode = h ? h->mode : -1; // keyCount stays 0 — an added diff has no conflicts to render
            prepared.files.append(f);
            continue;
        }

        const QByteArray oursBlob = oursHead.value(path);
        // Ours' side is the live working file: the mapper may have edits that
        // aren't snapshotted yet, and the merge must preserve them. (apply takes
        // a pre-merge snapshot of this same state first, so it stays reversible.)
        // Fall back to HEAD if the file is gone from disk.
        QByteArray oursText = readSongsFile(ours.songsPath, path);
        if (oursText.isEmpty()) oursText = repo->readBlob(oursBlob);
        if (oursText == theirsText) continue; // already identical → nothing to merge

        QByteArray baseBlob;
        for (int i = snaps.size() - 1; i >= 0 && baseBlob.isEmpty(); --i) {
            const QByteArray candidate = blobInSnapshot(snaps, i, path);
            if (!candidate.isEmpty() && oursHist.value(path).contains(candidate))
                baseBlob = candidate;
        }

        PreparedFile f;
        f.relPath = path;
        f.oursText = oursText;
        f.theirsText = theirsText;
        f.baseText = baseBlob.isEmpty() ? QByteArray() : repo->readBlob(baseBlob);

        const auto oursMap = ovc::core::canonicalize(ovc::core::parseOsu(sview(f.oursText)).doc);
        if (const auto* v = oursMap.kv(ovc::core::SectionId::Metadata, "Version"))
            f.version = QString::fromStdString(v->raw);
        f.mode = oursMap.mode;
        f.keyCount = oursMap.keyCount;
        const auto baseMap = ovc::core::canonicalize(ovc::core::parseOsu(sview(f.baseText)).doc);
        const auto theirsMap =
            ovc::core::canonicalize(ovc::core::parseOsu(sview(f.theirsText)).doc);
        const ovc::core::MergeResult mr = ovc::core::merge3(baseMap, oursMap, theirsMap);
        if (mr.wholeFileConflict) {
            f.wholeFileConflict = true;
            f.reason = QString::fromStdString(mr.reason);
            prepared.files.append(f);
            continue;
        }
        for (const ovc::core::Conflict& c : mr.conflicts) {
            MergeConflictInfo ci;
            ci.id = QString::fromStdString(c.id);
            ci.key = QString::fromStdString(c.key);
            ci.timeMs = c.timeMs;
            ci.column = c.column;
            ci.base = QString::fromStdString(c.base);
            ci.ours = QString::fromStdString(c.ours);
            ci.theirs = QString::fromStdString(c.theirs);
            switch (c.domain) {
            case ovc::core::MergeDomain::Kv: ci.domain = QStringLiteral("kv"); break;
            case ovc::core::MergeDomain::Timing: ci.domain = QStringLiteral("timing"); break;
            case ovc::core::MergeDomain::Breaks: ci.domain = QStringLiteral("break"); break;
            case ovc::core::MergeDomain::Storyboard: ci.domain = QStringLiteral("storyboard"); break;
            default: ci.domain = QStringLiteral("note"); break;
            }
            f.conflicts.append(ci);
        }
        prepared.files.append(f);
    }

    // ---- media: whole-file sync (binary can't 3-way; blob-oid identity decides).
    // Skipped for text-only bundles, whose media bytes were never shipped. ----
    if (!b.bundleJson.value(QStringLiteral("textOnly")).toBool()) {
        for (const auto& tv : snaps.last().toObject().value(QStringLiteral("tree")).toArray()) {
            const QJsonObject t = tv.toObject();
            const QString path = t.value(QStringLiteral("path")).toString();
            if (isTextPath(path) || isInfra(path)) continue; // media only
            const QByteArray theirsBlob = t.value(QStringLiteral("blob")).toString().toUtf8();
            // Fast path: theirs already matches ours at HEAD → theirs has nothing
            // new for this file (if our disk diverged, that's an ours-only edit we
            // keep), so skip without reading the media off disk.
            if (oursHead.value(path) == theirsBlob) continue;
            // Otherwise hash ours' content live from disk (like the .osu path) so
            // an unsnapshotted media swap isn't silently overwritten by theirs;
            // fall back to HEAD when the file isn't on disk.
            const QByteArray oursDisk = readSongsFile(ours.songsPath, path);
            const QByteArray oursBlob =
                oursDisk.isEmpty() ? oursHead.value(path) : ShadowRepo::hashBlob(oursDisk);
            if (oursBlob == theirsBlob) continue; // ours' live content matches theirs

            QByteArray baseBlob;
            for (int i = snaps.size() - 1; i >= 0 && baseBlob.isEmpty(); --i) {
                const QByteArray candidate = blobInSnapshot(snaps, i, path);
                if (!candidate.isEmpty() && oursHist.value(path).contains(candidate))
                    baseBlob = candidate;
            }

            PreparedMedia m;
            m.relPath = path;
            m.theirsBlob = theirsBlob;
            m.theirsSize = qint64(t.value(QStringLiteral("size")).toDouble());
            if (oursBlob.isEmpty())
                m.op = PreparedMedia::Op::Add; // ours lacks it → pull in
            else if (oursBlob == baseBlob)
                m.op = PreparedMedia::Op::Update; // ours unchanged, theirs did → take theirs
            else if (theirsBlob == baseBlob)
                continue; // theirs unchanged, ours diverged → keep ours, nothing to do
            else
                m.op = PreparedMedia::Op::Conflict; // both diverged → keep ours, report
            prepared.media.append(m);
        }
    }
    return prepared;
}

BundleMergeReport applyPreparedMerge(const MapsetEntry& ours, const PreparedMerge& prepared,
                                     const FileResolutions& resolutions, QString* err)
{
    BundleMergeReport report;
    report.bundleTitle = prepared.bundleTitle;

    for (const PreparedFile& pf : prepared.files) {
        FileMergeResult f;
        f.relPath = pf.relPath;
        f.version = pf.version;

        if (pf.addedByThem) {
            f.changed = writeSongsFile(ours.songsPath, pf.relPath, pf.theirsText);
            report.files.append(f);
            continue;
        }
        if (pf.wholeFileConflict) {
            f.wholeFileConflict = true;
            f.reason = pf.reason;
            report.files.append(f);
            continue;
        }

        // Conflict ids are only unique within a file (kv:…, note:…, "storyboard",
        // etc. repeat across difficulties), so resolutions are scoped per relPath;
        // build this file's map from its own bucket. Absent → the caller kept ours.
        ovc::core::ResolutionMap res;
        const QMap<QString, QString> fileRes = resolutions.value(pf.relPath);
        for (auto it = fileRes.constBegin(); it != fileRes.constEnd(); ++it)
            res[it.key().toStdString()] =
                it.value() == QStringLiteral("theirs") ? ovc::core::ResolveSide::Theirs
                                                       : ovc::core::ResolveSide::Ours;

        const auto baseMap = ovc::core::canonicalize(ovc::core::parseOsu(sview(pf.baseText)).doc);
        const auto oursMap = ovc::core::canonicalize(ovc::core::parseOsu(sview(pf.oursText)).doc);
        const auto theirsMap =
            ovc::core::canonicalize(ovc::core::parseOsu(sview(pf.theirsText)).doc);
        const ovc::core::MergeResult mr = ovc::core::merge3(baseMap, oursMap, theirsMap, res);
        for (const ovc::core::Conflict& c : mr.conflicts)
            f.conflictKeys << QStringLiteral("%1 (%2 vs %3)")
                                  .arg(QString::fromStdString(c.key),
                                       QString::fromStdString(c.ours),
                                       QString::fromStdString(c.theirs));

        const std::string merged = ovc::core::emitCanonical(mr.merged);
        const QByteArray mergedBytes(merged.data(), qsizetype(merged.size()));
        f.changed = mergedBytes != pf.oursText;
        if (f.changed && !writeSongsFile(ours.songsPath, pf.relPath, mergedBytes)) {
            if (err) *err = QStringLiteral("cannot write ") + pf.relPath;
        }
        report.files.append(f);
    }

    // Media sync: copy in added/updated files (and conflicts the caller resolved
    // to theirs); other binary conflicts stay ours. Bytes come from the bundle
    // now — the prepared plan holds only oids, so it never carried media in RAM.
    if (!prepared.media.isEmpty()) {
        OpenedBundle b;
        const bool opened = openBundle(prepared.bundlePath, b, nullptr);
        for (const PreparedMedia& pm : prepared.media) {
            MediaMergeResult r;
            r.relPath = pm.relPath;
            r.size = pm.theirsSize;
            // Binary conflicts aren't resolvable from the web (no per-byte UI), so
            // a conflicted media file always keeps ours; added/updated files come in.
            const bool takeTheirs = pm.op != PreparedMedia::Op::Conflict;
            if (!takeTheirs) {
                r.op = MediaMergeResult::Op::KeptOurs;
                report.media.append(r);
                continue;
            }
            const QByteArray bytes = opened ? extractBlob(b.zip, pm.theirsBlob) : QByteArray();
            if (bytes.isEmpty()) {
                r.op = MediaMergeResult::Op::Skipped;
                r.reason = opened ? QStringLiteral("blob missing from bundle")
                                  : QStringLiteral("cannot read bundle");
            }
            else if (writeSongsFile(ours.songsPath, pm.relPath, bytes)) {
                r.op = pm.op == PreparedMedia::Op::Add ? MediaMergeResult::Op::Added
                                                       : MediaMergeResult::Op::Updated;
                r.size = bytes.size();
            }
            else {
                r.op = MediaMergeResult::Op::Skipped;
                r.reason = QStringLiteral("cannot write ") + pm.relPath;
            }
            report.media.append(r);
        }
    }
    return report;
}

} // namespace ovc::git
