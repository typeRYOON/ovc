#include <ui/snapshotmodel.h>
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

// Auto subjects carry a redundant "[trigger] " prefix — the chip already says it.
QString cleanSubject(const QString& subject)
{
    if (subject.startsWith('[')) {
        const qsizetype close = subject.indexOf(QStringLiteral("] "));
        if (close > 0) return subject.mid(close + 2);
    }
    return subject;
}

} // namespace

SnapshotModel::SnapshotModel(QObject* parent) : QAbstractListModel(parent) {}

void SnapshotModel::reload(const QList<ovc::git::ShadowRepo::CommitInfo>& commits)
{
    beginResetModel();
    m_commits = commits;
    endResetModel();
}

void SnapshotModel::clear()
{
    beginResetModel();
    m_commits.clear();
    endResetModel();
}

QByteArray SnapshotModel::oidAt(const QModelIndex& index) const
{
    if (!index.isValid() || index.row() >= m_commits.size()) return {};
    return m_commits[index.row()].oid;
}

QVariant SnapshotModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_commits.size()) return {};
    const auto& c = m_commits[index.row()];
    switch (role) {
    case Qt::DisplayRole:
    case SubjectRole: return cleanSubject(c.subject);
    case LabelRole: return c.label;
    case TriggerRole: return c.trailers.value(QStringLiteral("Ovc-Trigger"));
    case WhenRole: return relativeTime(c.when);
    case OidRole: return c.oid;
    case ParentOidRole: return c.parentOid;
    }
    return {};
}

void SnapshotDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt,
                             const QModelIndex& index) const
{
    QStyledItemDelegate::paint(p, opt, QModelIndex());
    p->save();

    const QRect r = opt.rect.adjusted(10, 6, -10, -6);
    QFont main = opt.font;
    QFont sub = opt.font;
    sub.setPointSizeF(sub.pointSizeF() * 0.8);

    const QString label = index.data(SnapshotModel::LabelRole).toString();
    const QString subject = index.data(SnapshotModel::SubjectRole).toString();

    // Title line: the label (amber) when present, else the auto subject.
    p->setFont(main);
    if (!label.isEmpty()) {
        p->setPen(QColor(0xc8, 0xa0, 0x50));
        const QString title =
            QFontMetrics(main).elidedText(label, Qt::ElideRight, r.width());
        p->drawText(r, Qt::AlignTop | Qt::AlignLeft, title);
    }
    else {
        p->setPen(QColor(0xd0, 0xd0, 0xd0));
        const QString title =
            QFontMetrics(main).elidedText(subject, Qt::ElideRight, r.width());
        p->drawText(r, Qt::AlignTop | Qt::AlignLeft, title);
    }

    // Second line: relative time, then the trigger word in its own color. When
    // labeled, the auto subject rides along here so nothing is lost.
    const QString trigger = index.data(SnapshotModel::TriggerRole).toString();
    const QString when = index.data(SnapshotModel::WhenRole).toString();
    p->setFont(sub);
    p->setPen(QColor(0x66, 0x66, 0x66));
    QString lead = trigger.isEmpty() ? when : when + QStringLiteral("  ·  ");
    if (!label.isEmpty() && !subject.isEmpty())
        lead = when + QStringLiteral("  ·  ") + subject + QStringLiteral("  ·  ");
    const QString leadElided = QFontMetrics(sub).elidedText(
        lead, Qt::ElideRight, r.width() - (trigger.isEmpty() ? 0 : 60));
    p->drawText(r, Qt::AlignBottom | Qt::AlignLeft, leadElided);
    if (!trigger.isEmpty() && leadElided == lead) {
        const int leadWidth = QFontMetrics(sub).horizontalAdvance(lead);
        p->setPen(triggerColor(trigger));
        p->drawText(r.adjusted(leadWidth, 0, 0, 0), Qt::AlignBottom | Qt::AlignLeft, trigger);
    }

    p->restore();
}

QSize SnapshotDelegate::sizeHint(const QStyleOptionViewItem& opt, const QModelIndex&) const
{
    return {opt.rect.width(), QFontMetrics(opt.font).height() * 2 + 16};
}

} // namespace ovc::ui
