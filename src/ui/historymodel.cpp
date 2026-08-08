#include <ui/historymodel.h>
#include <QDateTime>
#include <QPainter>

namespace ovc::ui {

namespace {

QString relativeTime(const QDateTime& when)
{
    const qint64 secs = when.secsTo(QDateTime::currentDateTime());
    if (secs < 60) return QStringLiteral("just now");
    if (secs < 3600) return QStringLiteral("%1m ago").arg(secs / 60);
    if (secs < 86400) return QStringLiteral("%1h ago").arg(secs / 3600);
    if (secs < 86400 * 7) return QStringLiteral("%1d ago").arg(secs / 86400);
    return when.toLocalTime().toString(QStringLiteral("MMM d, HH:mm"));
}

QColor triggerColor(const QString& trigger)
{
    if (trigger == QStringLiteral("autosave")) return {0x77, 0xaa, 0xdd};
    if (trigger == QStringLiteral("manual")) return {0xc8, 0xa0, 0x50};
    if (trigger == QStringLiteral("import")) return {0x8f, 0xbf, 0x8f};
    if (trigger == QStringLiteral("restore")) return {0xcc, 0x66, 0x66};
    if (trigger == QStringLiteral("pre-restore")) return {0x88, 0x66, 0x44};
    return {0x88, 0x88, 0x88};
}

} // namespace

HistoryModel::HistoryModel(QObject* parent) : QAbstractListModel(parent) {}

void HistoryModel::reload(const QList<ovc::git::ShadowRepo::CommitInfo>& commits)
{
    beginResetModel();
    m_commits = commits;
    endResetModel();
}

void HistoryModel::clear()
{
    beginResetModel();
    m_commits.clear();
    endResetModel();
}

QVariant HistoryModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_commits.size()) return {};
    const auto& c = m_commits[index.row()];
    switch (role) {
    case Qt::DisplayRole:
    case SubjectRole: {
        // The "[trigger] " prefix is redundant with the chip line below.
        QString s = c.subject;
        if (s.startsWith('[')) {
            const qsizetype close = s.indexOf(QStringLiteral("] "));
            if (close > 0) s = s.mid(close + 2);
        }
        return s;
    }
    case TriggerRole: return c.trailers.value(QStringLiteral("Ovc-Trigger"));
    case WhenRole: return relativeTime(c.when);
    case OidRole: return c.oid;
    case ParentOidRole: return c.parentOid;
    case DifficultyRole: return c.trailers.value(QStringLiteral("Ovc-Difficulty"));
    }
    return {};
}

void HistoryDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt,
                            const QModelIndex& index) const
{
    QStyledItemDelegate::paint(p, opt, QModelIndex());
    p->save();

    const QRect r = opt.rect.adjusted(10, 6, -10, -6);
    QFont main = opt.font;
    QFont sub = opt.font;
    sub.setPointSizeF(sub.pointSizeF() * 0.8);

    p->setFont(main);
    p->setPen(QColor(0xd0, 0xd0, 0xd0));
    QString subject = index.data(HistoryModel::SubjectRole).toString();
    subject = QFontMetrics(main).elidedText(subject, Qt::ElideRight, r.width());
    p->drawText(r, Qt::AlignTop | Qt::AlignLeft, subject);

    const QString trigger = index.data(HistoryModel::TriggerRole).toString();
    const QString when = index.data(HistoryModel::WhenRole).toString();
    p->setFont(sub);
    p->setPen(QColor(0x66, 0x66, 0x66));
    const QString lead = trigger.isEmpty() ? when : when + QStringLiteral("  ·  ");
    p->drawText(r, Qt::AlignBottom | Qt::AlignLeft, lead);
    if (!trigger.isEmpty()) {
        const int leadWidth = QFontMetrics(sub).horizontalAdvance(lead);
        p->setPen(triggerColor(trigger));
        p->drawText(r.adjusted(leadWidth, 0, 0, 0), Qt::AlignBottom | Qt::AlignLeft, trigger);
    }

    p->restore();
}

QSize HistoryDelegate::sizeHint(const QStyleOptionViewItem& opt, const QModelIndex&) const
{
    return {opt.rect.width(), QFontMetrics(opt.font).height() * 2 + 16};
}

} // namespace ovc::ui
