#include <ui/diffview.h>
#include <utils/timefmt.h>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

namespace ovc::ui {

using namespace ovc::git;
using namespace ovc::osu;
using ovc::utils::msToClock;

namespace {

constexpr int kMaxNoteRows = 200;
const char* kAdded = "#66aa66";
const char* kRemoved = "#cc6666";
const char* kModified = "#c8a050";

QString esc(const QString& s)
{
    return s.toHtmlEscaped();
}

QLabel* row(const QString& html)
{
    auto* l = new QLabel(html);
    l->setObjectName("DiffRow");
    l->setTextFormat(Qt::RichText);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return l;
}

QString opSpan(ChangeOp op)
{
    switch (op) {
    case ChangeOp::Added: return QStringLiteral("<span style='color:%1'>+</span>").arg(kAdded);
    case ChangeOp::Removed: return QStringLiteral("<span style='color:%1'>−</span>").arg(kRemoved);
    default: return QStringLiteral("<span style='color:%1'>~</span>").arg(kModified);
    }
}

QString fileKindName(FileKind kind)
{
    switch (kind) {
    case FileKind::Difficulty: return QStringLiteral("diff");
    case FileKind::Storyboard: return QStringLiteral("storyboard");
    case FileKind::Audio: return QStringLiteral("audio");
    case FileKind::Image: return QStringLiteral("image");
    case FileKind::Video: return QStringLiteral("video");
    case FileKind::Sample: return QStringLiteral("sample");
    default: return QStringLiteral("file");
    }
}

QString sizeText(qint64 bytes)
{
    if (bytes >= 1024 * 1024)
        return QStringLiteral("%1 MiB").arg(double(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
    if (bytes >= 1024) return QStringLiteral("%1 KiB").arg(bytes / 1024);
    return QStringLiteral("%1 B").arg(bytes);
}

} // namespace

DiffView::DiffView(QWidget* parent) : QScrollArea(parent)
{
    setWidgetResizable(true);
    m_body = new QWidget;
    m_body->setObjectName("DiffBody");
    m_layout = new QVBoxLayout(m_body);
    m_layout->setContentsMargins(12, 8, 12, 12);
    m_layout->setSpacing(6);
    m_layout->addStretch();
    setWidget(m_body);
}

void DiffView::clearBody()
{
    while (m_layout->count() > 1) {
        QLayoutItem* item = m_layout->takeAt(0);
        delete item->widget();
        delete item;
    }
}

void DiffView::showPlaceholder(const QString& text)
{
    clearBody();
    auto* hint = new QLabel(text);
    hint->setObjectName("EmptyHint");
    hint->setAlignment(Qt::AlignCenter);
    m_layout->insertWidget(0, hint);
}

QWidget* DiffView::makeSection(const QString& title, QWidget* content, bool expanded)
{
    auto* wrap = new QWidget;
    auto* v = new QVBoxLayout(wrap);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);

    auto* header = new QToolButton;
    header->setObjectName("SectionHeader");
    header->setText(title);
    header->setCheckable(true);
    header->setChecked(expanded);
    header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    content->setVisible(expanded);
    connect(header, &QToolButton::toggled, content, [header, content](bool on) {
        content->setVisible(on);
        header->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    });

    v->addWidget(header);
    v->addWidget(content);
    return wrap;
}

QWidget* DiffView::buildFilesSection(const QList<FileChange>& media)
{
    auto* content = new QWidget;
    auto* v = new QVBoxLayout(content);
    v->setContentsMargins(8, 2, 0, 2);
    v->setSpacing(1);
    for (const FileChange& c : media) {
        QString text = opSpan(c.op == FileOp::Added      ? ChangeOp::Added
                              : c.op == FileOp::Removed  ? ChangeOp::Removed
                                                         : ChangeOp::Modified);
        text += QStringLiteral(" %1 <span style='color:#666'>(%2, %3)</span>")
                    .arg(esc(c.relPath), fileKindName(c.kind),
                         sizeText(c.op == FileOp::Removed ? c.oldSize : c.newSize));
        if (c.op == FileOp::Renamed)
            text += QStringLiteral(" <span style='color:#666'>was %1</span>").arg(esc(c.oldRelPath));
        v->addWidget(row(text));
    }
    return content;
}

QWidget* DiffView::buildSemantic(const BeatmapDiff& d)
{
    auto* content = new QWidget;
    auto* v = new QVBoxLayout(content);
    v->setContentsMargins(8, 2, 0, 2);
    v->setSpacing(1);

    if (d.modeChanged) v->addWidget(row(QStringLiteral("<i>game mode changed — object diff skipped</i>")));
    if (d.keyCountChanged)
        v->addWidget(row(QStringLiteral("<i>%1K → %2K — every column remaps, object diff skipped</i>")
                             .arg(d.keyCountBefore)
                             .arg(d.keyCountAfter)));

    static const QHash<int, QString> kSecName = {
        {int(SectionId::General), QStringLiteral("General")},
        {int(SectionId::Editor), QStringLiteral("Editor")},
        {int(SectionId::Metadata), QStringLiteral("Metadata")},
        {int(SectionId::Difficulty), QStringLiteral("Difficulty")},
    };
    for (const KvDiff& sec : d.kv) {
        for (const FieldChange& f : sec.changes) {
            QString text;
            if (f.before.isEmpty())
                text = opSpan(ChangeOp::Added) + QStringLiteral(" %1 · %2: %3");
            else if (f.after.isEmpty())
                text = opSpan(ChangeOp::Removed) + QStringLiteral(" %1 · %2: %3");
            else
                text = opSpan(ChangeOp::Modified) +
                       QStringLiteral(" %1 · %2: %3 <span style='color:#666'>→</span> %4");
            text = text.arg(kSecName.value(int(sec.section)), esc(QString::fromUtf8(f.key)),
                            esc(f.before.isEmpty() ? f.after.text() : f.before.text()));
            if (!f.before.isEmpty() && !f.after.isEmpty()) text = text.arg(esc(f.after.text()));
            v->addWidget(row(text));
        }
    }

    for (const TimingChange& t : d.timing) {
        const TimingPoint& tp = t.op == ChangeOp::Removed ? t.before : t.after;
        QString desc = tp.uninherited
                           ? QStringLiteral("%1 BPM").arg(tp.bpm(), 0, 'g', 6)
                           : QStringLiteral("SV %1×").arg(tp.sv(), 0, 'g', 4);
        if (tp.kiai()) desc += QStringLiteral(" <span style='color:#c8a050'>kiai</span>");
        QString text = opSpan(t.op) + QStringLiteral(" %1  %2").arg(msToClock(t.timeQ / 1000), desc);
        if (t.op == ChangeOp::Modified) {
            QStringList bits;
            for (const FieldChange& f : t.fields)
                bits << QStringLiteral("%1 %2 <span style='color:#666'>→</span> %3")
                            .arg(esc(QString::fromUtf8(f.key)), esc(f.before.text()),
                                 esc(f.after.text()));
            text = opSpan(t.op) + QStringLiteral(" %1  %2").arg(msToClock(t.timeQ / 1000),
                                                                bits.join(QStringLiteral(", ")));
        }
        v->addWidget(row(text));
    }

    int shown = 0, visible = 0;
    for (const NoteChange& n : d.notes)
        if (!n.moveSuppressed) ++visible;
    for (const NoteChange& n : d.notes) {
        if (n.moveSuppressed) continue;
        if (shown == kMaxNoteRows) {
            v->addWidget(row(QStringLiteral("<span style='color:#666'>… and %1 more</span>")
                                 .arg(visible - kMaxNoteRows)));
            break;
        }
        ++shown;
        if (n.movedFromColumn >= 0) {
            v->addWidget(row(QStringLiteral("<span style='color:#77aadd'>→</span> %1  col %2→%3 "
                                            "<span style='color:#666'>moved</span>")
                                 .arg(msToClock(n.timeMs))
                                 .arg(n.movedFromColumn)
                                 .arg(n.column)));
            continue;
        }
        const CanonicalNote& note = n.op == ChangeOp::Removed ? n.before : n.after;
        QString text = opSpan(n.op) +
                       QStringLiteral(" %1  col %2").arg(msToClock(n.timeMs)).arg(n.column);
        if (note.isHold)
            text += QStringLiteral("  <span style='color:#888'>hold →%1</span>")
                        .arg(msToClock(note.endTimeMs));
        for (const FieldChange& f : n.fields)
            text += QStringLiteral("  <span style='color:#888'>%1</span> %2 "
                                   "<span style='color:#666'>→</span> %3")
                        .arg(esc(QString::fromUtf8(f.key)), esc(f.before.text()),
                             esc(f.after.text()));
        v->addWidget(row(text));
    }

    for (const BreakChange& b : d.events.breaks) {
        const BreakPeriod& br = b.op == ChangeOp::Removed ? b.before : b.after;
        QString text = opSpan(b.op) + QStringLiteral(" break %1–%2")
                                          .arg(msToClock(br.startMs), msToClock(br.endMs));
        if (b.op == ChangeOp::Modified)
            text = opSpan(b.op) + QStringLiteral(" break %1  end %2 <span style='color:#666'>→</span> %3")
                                      .arg(msToClock(b.before.startMs), msToClock(b.before.endMs),
                                           msToClock(b.after.endMs));
        v->addWidget(row(text));
    }
    if (d.events.background)
        v->addWidget(row(opSpan(ChangeOp::Modified) + QStringLiteral(" background %1 <span style='color:#666'>→</span> %2")
                                                          .arg(esc(d.events.background->before.text()),
                                                               esc(d.events.background->after.text()))));
    if (d.events.storyboardChanged)
        v->addWidget(row(QStringLiteral("storyboard lines <span style='color:%1'>+%2</span> "
                                        "<span style='color:%3'>−%4</span>")
                             .arg(kAdded)
                             .arg(d.events.sbLinesAdded)
                             .arg(kRemoved)
                             .arg(d.events.sbLinesRemoved)));

    auto listRow = [&](const QString& name, const ListDiff& l) {
        if (l.isEmpty()) return;
        QStringList bits;
        for (const QByteArray& a : l.added)
            bits << QStringLiteral("<span style='color:%1'>+%2</span>").arg(kAdded, esc(QString::fromUtf8(a)));
        for (const QByteArray& r2 : l.removed)
            bits << QStringLiteral("<span style='color:%1'>−%2</span>").arg(kRemoved, esc(QString::fromUtf8(r2)));
        v->addWidget(row(name + QStringLiteral("  ") + bits.join(QStringLiteral("  "))));
    };
    listRow(QStringLiteral("bookmarks"), d.bookmarks);
    listRow(QStringLiteral("tags"), d.tags);

    return content;
}

void DiffView::showDiff(const SetDiff& diff)
{
    clearBody();
    if (diff.isEmpty()) {
        showPlaceholder(tr("no changes in this snapshot"));
        return;
    }

    int at = 0;
    QList<FileChange> media;
    for (const FileChange& c : diff.files) {
        if (c.kind == FileKind::Difficulty && c.semantic && !c.semantic->isEmpty()) {
            const QString title = QStringLiteral("[%1]  %2")
                                      .arg(c.semantic->version, c.semantic->summary());
            m_layout->insertWidget(at++, makeSection(title, buildSemantic(*c.semantic), true));
        }
        else if (c.kind == FileKind::Difficulty) {
            const QString name = c.relPath.section('[', -1).section(']', 0, 0);
            QString title;
            switch (c.op) {
            case FileOp::Added: title = QStringLiteral("[%1]  new difficulty").arg(name); break;
            case FileOp::Removed: title = QStringLiteral("[%1]  removed").arg(name); break;
            case FileOp::Renamed: title = QStringLiteral("[%1]  renamed").arg(name); break;
            default: title = QStringLiteral("[%1]").arg(name); break;
            }
            auto* empty = new QWidget;
            empty->setFixedHeight(0);
            m_layout->insertWidget(at++, makeSection(title, empty, false));
        }
        else {
            media.append(c);
        }
    }
    if (!media.isEmpty()) {
        m_layout->insertWidget(at++, makeSection(QStringLiteral("Files  (%1)").arg(media.size()),
                                                 buildFilesSection(media), media.size() <= 8));
    }
}

} // namespace ovc::ui
