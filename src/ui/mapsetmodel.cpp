#include <ui/mapsetmodel.h>
#include <QPainter>

namespace ovc::ui {

MapsetModel::MapsetModel(QObject* parent) : QAbstractListModel(parent) {}

void MapsetModel::reload(const QList<ovc::git::MapsetEntry>& entries)
{
    beginResetModel();
    m_entries = entries;
    endResetModel();
}

void MapsetModel::setEditingRepoId(const QString& repoId)
{
    if (m_editingRepoId == repoId) return;
    m_editingRepoId = repoId;
    if (!m_entries.isEmpty())
        emit dataChanged(index(0), index(int(m_entries.size()) - 1), {EditingRole});
}

QVariant MapsetModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_entries.size()) return {};
    const auto& e = m_entries[index.row()];
    switch (role) {
    case Qt::DisplayRole:
    case TitleRole: return e.title.isEmpty() ? e.folderName : e.title;
    case SubtitleRole:
        return e.artist.isEmpty() ? QString() : e.artist + QStringLiteral("  —  ") + e.creator;
    case RepoIdRole: return e.repoId;
    case EditingRole: return e.repoId == m_editingRepoId;
    }
    return {};
}

QModelIndex MapsetModel::indexOfRepo(const QString& repoId) const
{
    for (int i = 0; i < m_entries.size(); ++i)
        if (m_entries[i].repoId == repoId) return index(i);
    return {};
}

void MapsetDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt,
                           const QModelIndex& index) const
{
    QStyledItemDelegate::paint(p, opt, QModelIndex()); // background/selection only
    p->save();

    const QRect r = opt.rect.adjusted(10, 6, -10, -6);
    QFont title = opt.font;
    QFont sub = opt.font;
    sub.setPointSizeF(sub.pointSizeF() * 0.82);

    const bool editing = index.data(MapsetModel::EditingRole).toBool();
    if (editing) {
        p->setFont(sub);
        p->setPen(QColor(0x66, 0xaa, 0x66));
        p->drawText(r, Qt::AlignTop | Qt::AlignRight, QStringLiteral("● editing"));
    }

    p->setFont(title);
    p->setPen(QColor(0xe0, 0xe0, 0xe0));
    QString t = index.data(MapsetModel::TitleRole).toString();
    const int titleWidth = r.width() - (editing ? 70 : 0);
    t = QFontMetrics(title).elidedText(t, Qt::ElideRight, titleWidth);
    p->drawText(r, Qt::AlignTop | Qt::AlignLeft, t);

    p->setFont(sub);
    p->setPen(QColor(0x77, 0x77, 0x77));
    QString s = index.data(MapsetModel::SubtitleRole).toString();
    s = QFontMetrics(sub).elidedText(s, Qt::ElideRight, r.width());
    p->drawText(r, Qt::AlignBottom | Qt::AlignLeft, s);

    p->restore();
}

QSize MapsetDelegate::sizeHint(const QStyleOptionViewItem& opt, const QModelIndex&) const
{
    return {opt.rect.width(), QFontMetrics(opt.font).height() * 2 + 18};
}

} // namespace ovc::ui
