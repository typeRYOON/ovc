#include <ui/mainwindow.h>
#include <watch/gamewatcher.h>
#include <QLabel>
#include <QVBoxLayout>

namespace ovc::ui {

MainWindow::MainWindow(ovc::watch::GameWatcher& watcher, QWidget* parent)
    : QMainWindow(parent), m_watcher(watcher)
{
    setWindowTitle("ovc");
    resize(720, 480);

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);

    auto* status = new QLabel(tr("waiting for osu!…"), central);
    auto* map = new QLabel(tr("no beatmap"), central);
    map->setWordWrap(true);
    layout->addWidget(status);
    layout->addWidget(map);
    layout->addStretch();
    setCentralWidget(central);

    using ovc::watch::GameWatcher;
    using ovc::watch::MemBeatmap;
    connect(&m_watcher, &GameWatcher::attachedChanged, this, [status](bool on) {
        status->setText(on ? tr("osu! connected") : tr("waiting for osu!…"));
    });
    connect(&m_watcher, &GameWatcher::beatmapChanged, this, [map](const MemBeatmap& m) {
        map->setText(QStringLiteral("%1 - %2 [%3]\n%4")
                         .arg(m.artist, m.title, m.version, m.osuPath));
    });
    connect(&m_watcher, &GameWatcher::beatmapCleared, this,
            [map]() { map->setText(tr("no beatmap")); });
}

} // namespace ovc::ui
