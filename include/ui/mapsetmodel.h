#pragma once
#include <git/registry.h>
#include <QAbstractListModel>
#include <QStyledItemDelegate>

namespace ovc::ui {

class MapsetModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role { TitleRole = Qt::UserRole, SubtitleRole, RepoIdRole, EditingRole };

    explicit MapsetModel(QObject* parent = nullptr);

    void reload(const QList<ovc::git::MapsetEntry>& entries);
    void setEditingRepoId(const QString& repoId); // live "editing now" pill

    int rowCount(const QModelIndex& = {}) const override { return int(m_entries.size()); }
    QVariant data(const QModelIndex& index, int role) const override;
    QModelIndex indexOfRepo(const QString& repoId) const;

private:
    QList<ovc::git::MapsetEntry> m_entries;
    QString m_editingRepoId;
};

// Two lines (title / artist — creator) + a right-aligned pill when live.
class MapsetDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& opt,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex& index) const override;
};

} // namespace ovc::ui
