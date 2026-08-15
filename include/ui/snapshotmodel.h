#pragma once
#include <git/shadowrepo.h>
#include <QAbstractListModel>
#include <QStyledItemDelegate>

namespace ovc::ui {

// Snapshots of one mapset. Display prefers a user-given label over the
// auto-generated subject.
class SnapshotModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        SubjectRole = Qt::UserRole,
        LabelRole,
        TriggerRole,
        WhenRole,
        OidRole,
        ParentOidRole
    };

    explicit SnapshotModel(QObject* parent = nullptr);

    void reload(const QList<ovc::git::ShadowRepo::CommitInfo>& commits);
    void clear();
    QByteArray oidAt(const QModelIndex& index) const;

    int rowCount(const QModelIndex& = {}) const override { return int(m_commits.size()); }
    QVariant data(const QModelIndex& index, int role) const override;

private:
    QList<ovc::git::ShadowRepo::CommitInfo> m_commits;
};

// Title line (label in amber, else the subject) + muted "relative time · trigger".
class SnapshotDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& opt,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex& index) const override;
};

} // namespace ovc::ui
