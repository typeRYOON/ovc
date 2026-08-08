#pragma once
#include <QDateTime>
#include <QList>
#include <QString>

namespace ovc::git {

struct MapsetEntry {
    QString repoId; // repo dir name under reposRoot(); never renames
    int beatmapSetId = -1;
    QList<int> beatmapIds;
    QString folderName; // Songs subfolder name (display / matching)
    QString songsPath;  // absolute path of the mapset folder
    QString title, artist, creator;
    QDateTime trackedSince;
    bool autoSnapshot = true;

    QString repoDir() const;
};

// index.json: the mapset registry. Small; load-modify-save whole.
class Registry {
public:
    static Registry load();
    bool save(QString* err = nullptr) const;

    MapsetEntry* findByRepoId(const QString& idOrPrefix);
    MapsetEntry* findBySongsPath(const QString& path);
    MapsetEntry* findBySetId(int setId);
    MapsetEntry* findByBeatmapId(int beatmapId);

    QString newRepoId() const; // sortable timestamp + random suffix

    QList<MapsetEntry> entries;
};

} // namespace ovc::git
