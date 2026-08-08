#pragma once
#include <git/setdiff.h>
#include <QScrollArea>

class QVBoxLayout;

namespace ovc::ui {

// Right pane: a scroll of collapsible sections built from a SetDiff — media
// file changes plus per-difficulty semantic groups (KV / timing / notes /
// breaks / bookmarks / tags).
class DiffView : public QScrollArea {
    Q_OBJECT
public:
    explicit DiffView(QWidget* parent = nullptr);

    void showDiff(const ovc::git::SetDiff& diff);
    void showPlaceholder(const QString& text);

private:
    void clearBody();
    QWidget* makeSection(const QString& title, QWidget* content, bool expanded);
    QWidget* buildSemantic(const ovc::osu::BeatmapDiff& d);
    QWidget* buildFilesSection(const QList<ovc::git::FileChange>& media);

    QWidget* m_body = nullptr;
    QVBoxLayout* m_layout = nullptr;
};

} // namespace ovc::ui
