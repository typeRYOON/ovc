#include <git/paths.h>
#include <git/registry.h>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>

namespace ovc::git {

QString MapsetEntry::repoDir() const
{
    return reposRoot() + QLatin1Char('/') + repoId;
}

Registry Registry::load()
{
    Registry reg;
    QFile f(indexPath());
    if (!f.open(QIODevice::ReadOnly)) return reg;
    const QJsonArray arr = QJsonDocument::fromJson(f.readAll())
                               .object()
                               .value(QStringLiteral("mapsets"))
                               .toArray();
    for (const auto& v : arr) {
        const QJsonObject o = v.toObject();
        MapsetEntry e;
        e.repoId = o.value(QStringLiteral("repoId")).toString();
        e.beatmapSetId = o.value(QStringLiteral("beatmapSetId")).toInt(-1);
        for (const auto& id : o.value(QStringLiteral("beatmapIds")).toArray())
            e.beatmapIds.append(id.toInt());
        e.folderName = o.value(QStringLiteral("folderName")).toString();
        e.songsPath = o.value(QStringLiteral("songsPath")).toString();
        e.title = o.value(QStringLiteral("title")).toString();
        e.artist = o.value(QStringLiteral("artist")).toString();
        e.creator = o.value(QStringLiteral("creator")).toString();
        e.trackedSince =
            QDateTime::fromString(o.value(QStringLiteral("trackedSince")).toString(), Qt::ISODate);
        e.autoSnapshot = o.value(QStringLiteral("autoSnapshot")).toBool(true);
        if (!e.repoId.isEmpty()) reg.entries.append(e);
    }
    return reg;
}

bool Registry::save(QString* err) const
{
    QJsonArray arr;
    for (const MapsetEntry& e : entries) {
        QJsonArray ids;
        for (int id : e.beatmapIds) ids.append(id);
        arr.append(QJsonObject{
            {QStringLiteral("repoId"), e.repoId},
            {QStringLiteral("beatmapSetId"), e.beatmapSetId},
            {QStringLiteral("beatmapIds"), ids},
            {QStringLiteral("folderName"), e.folderName},
            {QStringLiteral("songsPath"), e.songsPath},
            {QStringLiteral("title"), e.title},
            {QStringLiteral("artist"), e.artist},
            {QStringLiteral("creator"), e.creator},
            {QStringLiteral("trackedSince"), e.trackedSince.toString(Qt::ISODate)},
            {QStringLiteral("autoSnapshot"), e.autoSnapshot},
        });
    }
    QDir().mkpath(dataRoot());
    QFile f(indexPath());
    if (!f.open(QIODevice::WriteOnly)) {
        if (err) *err = QStringLiteral("cannot write ") + indexPath();
        return false;
    }
    f.write(QJsonDocument(QJsonObject{{QStringLiteral("mapsets"), arr}}).toJson());
    return true;
}

MapsetEntry* Registry::findByRepoId(const QString& idOrPrefix)
{
    MapsetEntry* prefixHit = nullptr;
    int prefixHits = 0;
    for (MapsetEntry& e : entries) {
        if (e.repoId == idOrPrefix) return &e;
        if (e.repoId.startsWith(idOrPrefix)) {
            prefixHit = &e;
            ++prefixHits;
        }
    }
    return prefixHits == 1 ? prefixHit : nullptr;
}

MapsetEntry* Registry::findBySongsPath(const QString& path)
{
    const QString clean = QDir::cleanPath(path);
    for (MapsetEntry& e : entries)
        if (QDir::cleanPath(e.songsPath).compare(clean, Qt::CaseInsensitive) == 0) return &e;
    return nullptr;
}

MapsetEntry* Registry::findBySetId(int setId)
{
    if (setId <= 0) return nullptr;
    for (MapsetEntry& e : entries)
        if (e.beatmapSetId == setId) return &e;
    return nullptr;
}

MapsetEntry* Registry::findByBeatmapId(int beatmapId)
{
    if (beatmapId <= 0) return nullptr;
    for (MapsetEntry& e : entries)
        if (e.beatmapIds.contains(beatmapId)) return &e;
    return nullptr;
}

bool Registry::removeByRepoId(const QString& repoId)
{
    return entries.removeIf([&](const MapsetEntry& e) { return e.repoId == repoId; }) > 0;
}

QString Registry::newRepoId() const
{
    QString id;
    do {
        id = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss")) +
             QStringLiteral("-%1").arg(QRandomGenerator::global()->bounded(0x1000000), 6, 16,
                                       QLatin1Char('0'));
    } while (std::any_of(entries.cbegin(), entries.cend(),
                         [&](const MapsetEntry& e) { return e.repoId == id; }));
    return id;
}

} // namespace ovc::git
