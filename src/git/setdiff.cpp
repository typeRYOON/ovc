#include <git/setdiff.h>
#include <ovccore/canonical.h>
#include <ovccore/parser.h>
#include <ovccore/peek.h>
#include <QHash>

namespace ovc::git {

namespace {

std::string_view view(const QByteArray& b)
{
    return {b.constData(), size_t(b.size())};
}

core::CanonicalMap canonFromBlob(const ShadowRepo& repo, const QByteArray& blobOid)
{
    const QByteArray bytes = repo.readBlob(blobOid);
    return core::canonicalize(core::parseOsu(view(bytes)).doc);
}

int beatmapIdOfBlob(const ShadowRepo& repo, const QByteArray& blobOid)
{
    const QByteArray head = repo.readBlob(blobOid).left(8192);
    const auto h = core::peekOsuHeader(view(head));
    return h ? h->beatmapId : -1;
}

} // namespace

FileKind classifyPath(QStringView relPath)
{
    const qsizetype dot = relPath.lastIndexOf('.');
    const QString ext = dot >= 0 ? relPath.mid(dot + 1).toString().toLower() : QString();
    const qsizetype slash = relPath.lastIndexOf('/');
    const QString base = relPath.mid(slash + 1).toString().toLower();

    if (ext == "osu") return FileKind::Difficulty;
    if (ext == "osb") return FileKind::Storyboard;
    if (ext == "mp3" || ext == "m4a") return FileKind::Audio;
    if (ext == "ogg") // the song is usually "audio.ogg"; other oggs are keysounds
        return base.startsWith(QStringLiteral("audio")) ? FileKind::Audio : FileKind::Sample;
    if (ext == "wav") return FileKind::Sample;
    if (ext == "jpg" || ext == "jpeg" || ext == "png") return FileKind::Image;
    if (ext == "mp4" || ext == "avi" || ext == "flv" || ext == "wmv") return FileKind::Video;
    return FileKind::Other;
}

SetDiff diffTrees(const ShadowRepo& repo, const QByteArray& oidA, const QByteArray& oidB)
{
    SetDiff diff;

    // Repo infrastructure never surfaces as content changes.
    auto isInfra = [](const QString& p) {
        return p == QStringLiteral(".gitattributes") || p.startsWith(QStringLiteral(".ovc/"));
    };
    QHash<QString, QByteArray> before, after;
    for (const auto& [path, blob] : repo.listTree(oidA))
        if (!isInfra(path)) before.insert(path, blob);
    for (const auto& [path, blob] : repo.listTree(oidB))
        if (!isInfra(path)) after.insert(path, blob);

    QList<FileChange> removed;
    for (auto it = before.constBegin(); it != before.constEnd(); ++it) {
        const auto inAfter = after.constFind(it.key());
        if (inAfter == after.constEnd()) {
            FileChange c;
            c.op = FileOp::Removed;
            c.relPath = it.key();
            c.kind = classifyPath(it.key());
            c.oldOid = it.value();
            c.oldSize = repo.blobSize(it.value());
            removed.append(c);
        }
        else if (it.value() != inAfter.value()) {
            FileChange c;
            c.op = FileOp::Modified;
            c.relPath = it.key();
            c.kind = classifyPath(it.key());
            c.oldOid = it.value();
            c.newOid = inAfter.value();
            c.oldSize = repo.blobSize(c.oldOid);
            c.newSize = repo.blobSize(c.newOid);
            diff.files.append(c);
        }
    }
    QList<FileChange> added;
    for (auto it = after.constBegin(); it != after.constEnd(); ++it) {
        if (!before.contains(it.key())) {
            FileChange c;
            c.op = FileOp::Added;
            c.relPath = it.key();
            c.kind = classifyPath(it.key());
            c.newOid = it.value();
            c.newSize = repo.blobSize(it.value());
            added.append(c);
        }
    }

    // Pair removed+added .osu into renames: same BeatmapID (>0), else same blob.
    for (FileChange& rem : removed) {
        if (rem.op != FileOp::Removed || rem.kind != FileKind::Difficulty) continue;
        const int remId = beatmapIdOfBlob(repo, rem.oldOid);
        for (FileChange& add : added) {
            if (add.op != FileOp::Added || add.kind != FileKind::Difficulty) continue;
            const bool idMatch = remId > 0 && beatmapIdOfBlob(repo, add.newOid) == remId;
            const bool contentMatch = add.newOid == rem.oldOid;
            if (idMatch || contentMatch) {
                add.op = FileOp::Renamed;
                add.oldRelPath = rem.relPath;
                add.oldOid = rem.oldOid;
                add.oldSize = rem.oldSize;
                rem.op = FileOp::Modified; // sentinel: consumed by the rename
                rem.relPath.clear();
                break;
            }
        }
    }
    for (const FileChange& c : removed)
        if (!c.relPath.isEmpty()) diff.files.append(c);
    for (const FileChange& c : added) diff.files.append(c);

    // Semantic diff for every .osu with both sides present.
    for (FileChange& c : diff.files) {
        if (c.kind != FileKind::Difficulty) continue;
        if ((c.op == FileOp::Modified || c.op == FileOp::Renamed) && c.oldOid != c.newOid)
            c.semantic = core::diffBeatmaps(canonFromBlob(repo, c.oldOid),
                                            canonFromBlob(repo, c.newOid));
    }

    std::stable_sort(diff.files.begin(), diff.files.end(),
                     [](const FileChange& a, const FileChange& b) {
                         return QPair(int(a.kind), a.relPath) < QPair(int(b.kind), b.relPath);
                     });
    return diff;
}

QString SetDiff::subjectLine() const
{
    QStringList parts;
    QStringList mediaNames; // basenames of the first few changed media files
    int mediaAdded = 0, mediaRemoved = 0, mediaModified = 0;
    for (const FileChange& c : files) {
        if (c.kind == FileKind::Difficulty) {
            const QString name = c.relPath.section('[', -1).section(']', 0, 0);
            switch (c.op) {
            case FileOp::Added: parts << QStringLiteral("+[%1]").arg(name); break;
            case FileOp::Removed: parts << QStringLiteral("-[%1]").arg(name); break;
            case FileOp::Renamed:
            case FileOp::Modified:
                if (c.semantic && !c.semantic->empty())
                    parts << QStringLiteral("%1: %2").arg(
                        name, QString::fromStdString(c.semantic->summary()));
                else if (c.op == FileOp::Renamed)
                    parts << QStringLiteral("renamed [%1]").arg(name);
                else
                    parts << QStringLiteral("[%1] resaved").arg(name); // blob changed, no semantic delta
                break;
            }
        }
        else {
            const QString base = c.relPath.section('/', -1);
            const char* verb = c.op == FileOp::Added     ? "added"
                               : c.op == FileOp::Removed ? "removed"
                                                         : "changed";
            if (mediaNames.size() < 3) mediaNames << base + QLatin1Char(' ') + verb;
            switch (c.op) {
            case FileOp::Added: ++mediaAdded; break;
            case FileOp::Removed: ++mediaRemoved; break;
            default: ++mediaModified; break;
            }
        }
    }
    const int mediaTotal = mediaAdded + mediaRemoved + mediaModified;
    if (mediaTotal > 0) {
        // Name the files when there are only a few; fall back to a count otherwise.
        if (mediaTotal <= 3) {
            parts << mediaNames.join(QStringLiteral(", "));
        }
        else {
            QStringList m;
            if (mediaAdded) m << QStringLiteral("+%1").arg(mediaAdded);
            if (mediaRemoved) m << QStringLiteral("-%1").arg(mediaRemoved);
            if (mediaModified) m << QStringLiteral("~%1").arg(mediaModified);
            parts << QStringLiteral("%1 media files (%2)").arg(mediaTotal).arg(m.join(' '));
        }
    }
    QString line = parts.join(QStringLiteral(" · "));
    if (line.size() > 100) line = line.left(97) + QStringLiteral("…");
    return line.isEmpty() ? QStringLiteral("no changes") : line;
}

} // namespace ovc::git
