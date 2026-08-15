#pragma once
#include <git/bundle.h>
#include <QHash>
#include <QObject>

namespace ovc::git {

// In-memory holder for merges that are computed but waiting on the user to
// resolve conflicts (in the web viewer). One pending merge per mapset. Shared
// by the local API server (exposes/resolves them) and the desktop window
// (creates them, offers a "keep mine" fallback).
class MergeSessionStore : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

    void put(const PreparedMerge& prepared); // replaces any existing for its repoId
    void remove(const QString& repoId);
    bool has(const QString& repoId) const { return m_sessions.contains(repoId); }
    const PreparedMerge* get(const QString& repoId) const;
    QList<PreparedMerge> all() const { return m_sessions.values(); }
    int count() const { return int(m_sessions.size()); }

signals:
    // Fired when a pending merge is added, resolved, or cancelled.
    void pendingChanged(const QString& repoId);

private:
    QHash<QString, PreparedMerge> m_sessions;
};

} // namespace ovc::git
