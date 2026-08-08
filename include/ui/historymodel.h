#pragma once
#include <git/shadowrepo.h>
#include <QAbstractListModel>
#include <QStyledItemDelegate>

namespace ovc::ui {

class HistoryModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        SubjectRole = Qt::UserRole,
        TriggerRole,
        WhenRole,
        OidRole,
        ParentOidRole,
        DifficultyRole
    };

    explicit HistoryModel(QObject* parent = nullptr);

    void reload(const QList<ovc::git::ShadowRepo::CommitInfo>& commits);
    void clear();

    int rowCount(const QModelIndex& = {}) const override { return int(m_commits.size()); }
    QVariant data(const QModelIndex& index, int role) const override;

private:
    QList<ovc::git::ShadowRepo::CommitInfo> m_commits;
};

// Subject line + muted "relative time · trigger · difficulty" line.
class HistoryDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& opt,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex& index) const override;
};

} // namespace ovc::ui
