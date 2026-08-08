#include <git/mirror.h>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QThread>

namespace ovc::git {

namespace {

bool isJunk(const QString& fileName)
{
    const QString lower = fileName.toLower();
    return lower.endsWith(QStringLiteral(".tmp")) || lower.startsWith('~') ||
           lower == QStringLiteral("desktop.ini") || lower == QStringLiteral("thumbs.db");
}

bool isReservedRoot(const QString& relPath)
{
    return relPath == QStringLiteral(".gitattributes") ||
           relPath.startsWith(QStringLiteral(".git/")) ||
           relPath.startsWith(QStringLiteral(".ovc/")) || relPath == QStringLiteral(".git") ||
           relPath == QStringLiteral(".ovc");
}

QString manifestPath(const QString& repoDir)
{
    return repoDir + QStringLiteral("/.git/ovc/manifest.json");
}

QJsonObject loadManifest(const QString& repoDir)
{
    QFile f(manifestPath(repoDir));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

bool saveManifest(const QString& repoDir, const QJsonObject& manifest)
{
    QDir().mkpath(repoDir + QStringLiteral("/.git/ovc"));
    QFile f(manifestPath(repoDir));
    if (!f.open(QIODevice::WriteOnly)) return false;
    return f.write(QJsonDocument(manifest).toJson(QJsonDocument::Compact)) >= 0;
}

} // namespace

bool mirrorIntoRepo(const QString& songsDir, const QString& repoDir, MirrorStats* stats,
                    QString* err)
{
    MirrorStats local;
    MirrorStats* s = stats ? stats : &local;

    const QDir source(songsDir);
    if (!source.exists()) {
        if (err) *err = QStringLiteral("source folder does not exist: ") + songsDir;
        return false;
    }

    QJsonObject manifest = loadManifest(repoDir);
    QJsonObject nextManifest;
    QSet<QString> sourcePaths;

    QDirIterator it(songsDir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        if (isJunk(info.fileName())) continue;
        const QString relPath = source.relativeFilePath(info.absoluteFilePath());
        if (isReservedRoot(relPath)) continue; // paranoia: never import git metadata
        sourcePaths.insert(relPath);

        const qint64 size = info.size();
        const qint64 mtime = info.lastModified().toMSecsSinceEpoch();
        const QString destPath = repoDir + QLatin1Char('/') + relPath;

        const QJsonObject prev = manifest.value(relPath).toObject();
        const bool unchanged = prev.value(QStringLiteral("size")).toDouble() == double(size) &&
                               prev.value(QStringLiteral("mtime")).toDouble() == double(mtime) &&
                               QFile::exists(destPath);
        if (unchanged) {
            ++s->statSkipped;
            nextManifest.insert(relPath, prev);
            continue;
        }

        QDir().mkpath(QFileInfo(destPath).absolutePath());
        // Source may be mid-write by osu!; retry briefly on open failure.
        bool copied = false;
        for (int attempt = 0; attempt < 3 && !copied; ++attempt) {
            if (attempt) QThread::msleep(150);
            QFile in(info.absoluteFilePath());
            if (!in.open(QIODevice::ReadOnly)) continue;
            QFile out(destPath);
            if (!out.open(QIODevice::WriteOnly)) break;
            copied = out.write(in.readAll()) >= 0;
        }
        if (!copied) {
            if (err) *err = QStringLiteral("cannot copy ") + relPath;
            return false;
        }
        ++s->copied;
        nextManifest.insert(relPath,
                            QJsonObject{{QStringLiteral("size"), double(size)},
                                        {QStringLiteral("mtime"), double(mtime)}});
    }

    // Remove workdir files whose source vanished.
    QDirIterator wt(repoDir, QDir::Files, QDirIterator::Subdirectories);
    const QDir repo(repoDir);
    while (wt.hasNext()) {
        wt.next();
        const QString relPath = repo.relativeFilePath(wt.fileInfo().absoluteFilePath());
        if (isReservedRoot(relPath)) continue;
        if (!sourcePaths.contains(relPath)) {
            QFile::remove(wt.fileInfo().absoluteFilePath());
            ++s->deleted;
        }
    }

    if (!saveManifest(repoDir, nextManifest)) {
        if (err) *err = QStringLiteral("cannot write manifest");
        return false;
    }
    return true;
}

} // namespace ovc::git
