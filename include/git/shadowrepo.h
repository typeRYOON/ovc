#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QMap>
#include <QPair>
#include <QString>
#include <memory>
#include <optional>

struct git_repository;

namespace ovc::git {

// One mapset's git repo. Not thread-safe: open one instance per operation on
// the calling thread — opening is microseconds. Public API exposes hex oids
// and Qt types only; libgit2 stays inside src/git.
class ShadowRepo {
public:
    struct CommitInfo {
        QByteArray oid, parentOid; // hex; parent empty on the root commit
        QDateTime when;
        QString subject;
        QString label; // user-given name, if any (mutable; see setLabel)
        QMap<QString, QString> trailers;
    };

    // init with branch "main" + committed .gitattributes (merge=osu wiring).
    static bool create(const QString& dir, QString* err);
    static std::optional<ShadowRepo> open(const QString& dir);

    ShadowRepo(ShadowRepo&&) noexcept;
    ShadowRepo& operator=(ShadowRepo&&) noexcept;
    ~ShadowRepo();

    QString dir() const { return m_dir; }

    // Stage the whole working tree; returns the tree oid, or nullopt when it
    // equals HEAD's tree (no-op save — nothing to commit).
    std::optional<QByteArray> stageAll(QString* err = nullptr);
    // Commit a tree from stageAll. Trailers append as "Key: value" lines.
    // `when` overrides the commit timestamp (bundle import replays history).
    QByteArray commitStaged(const QByteArray& treeOid, const QString& subject,
                            const QMap<QString, QString>& trailers, QString* err = nullptr,
                            const QDateTime& when = {});
    std::optional<QByteArray> commitAll(const QString& subject,
                                        const QMap<QString, QString>& trailers,
                                        QString* err = nullptr);

    QByteArray headOid() const;     // hex commit oid; empty on unborn
    QByteArray headTreeOid() const; // hex tree oid; empty on unborn
    QList<CommitInfo> log(int limit = 1000) const; // first-parent, newest first
    std::optional<CommitInfo> commitInfo(const QByteArray& commitOid) const;

    // Human names for snapshots. Commits are immutable (the oid is their hash),
    // so labels live beside the repo in .git/ovc/labels.json — settable and
    // clearable anytime without rewriting history or changing any oid. An empty
    // name removes the label.
    QMap<QByteArray, QString> labels() const;
    QString labelFor(const QByteArray& commitOid) const;
    bool setLabel(const QByteArray& commitOid, const QString& name, QString* err = nullptr);

    // Accepts a commit or tree oid. relPath -> blob hex oid.
    QList<QPair<QString, QByteArray>> listTree(const QByteArray& oid) const;
    QByteArray readBlob(const QByteArray& blobOid) const;
    qint64 blobSize(const QByteArray& blobOid) const;
    // Content-addressed hex oid for arbitrary bytes (no write to the odb), so a
    // live working file can be compared to tree blob oids without staging it.
    static QByteArray hashBlob(const QByteArray& bytes);

    // Force the working tree (and index) to a commit's tree. HEAD stays put —
    // committing afterwards records the restored state as a NEW commit.
    bool checkoutTree(const QByteArray& commitOid, QString* err = nullptr);

private:
    ShadowRepo() = default;
    git_repository* m_repo = nullptr;
    QString m_dir;
};

} // namespace ovc::git
