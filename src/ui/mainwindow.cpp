#include <chrome/windowchrome.h>
#include <ui/diffview.h>
#include <utils/qutils.h>
#include <ui/historymodel.h>
#include <ui/mainwindow.h>
#include <ui/mapsetmodel.h>
#include <watch/gamewatcher.h>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace ovc::ui {

using ovc::git::SetDiff;
using ovc::watch::GameWatcher;
using ovc::watch::TrackingService;

MainWindow::MainWindow(TrackingService& service, GameWatcher& watcher, QWidget* parent)
    : QMainWindow(parent), m_service(service), m_watcher(watcher)
{
    setWindowTitle("ovc");
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowMinimizeButtonHint |
                   Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);
    resize(1280, 760);
    setMinimumSize(1080, 620);

    m_chrome = new WindowChrome(this);
    setCentralWidget(m_chrome->frame());

    auto* bodyLayout = new QVBoxLayout(m_chrome->bodyWidget());
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);

    auto* split = new QSplitter(Qt::Horizontal);
    split->setChildrenCollapsible(false);
    split->addWidget(buildMapsetPane());
    split->addWidget(buildHistoryPane());
    split->addWidget(buildDiffPane());
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 0);
    split->setStretchFactor(2, 1);
    split->setSizes({280, 360, 640});
    bodyLayout->addWidget(split);

    // ---- Service / watcher wiring
    connect(&m_service, &TrackingService::trackedListChanged, this, &MainWindow::reloadMapsets);
    connect(&m_service, &TrackingService::activeMapsetChanged, this, [this](const QString& rid) {
        m_mapsetModel->setEditingRepoId(rid);
        refreshDetectCard();
        if (!rid.isEmpty() && !m_mapsetList->currentIndex().isValid()) selectMapset(rid);
    });
    connect(&m_service, &TrackingService::snapshotTaken, this, &MainWindow::onSnapshotTaken);
    connect(&m_service, &TrackingService::snapshotFailed, this,
            [this](const QString&, const QString& reason) {
                statusBar()->showMessage(tr("snapshot failed: %1").arg(reason), 6000);
            });
    connect(&m_watcher, &GameWatcher::attachedChanged, this, [this](bool) { refreshDetectCard(); });
    connect(&m_watcher, &GameWatcher::beatmapChanged, this,
            [this](const ovc::watch::MemBeatmap&) { refreshDetectCard(); });
    connect(&m_watcher, &GameWatcher::beatmapCleared, this, [this]() { refreshDetectCard(); });
    connect(&m_watcher, &GameWatcher::stateChanged, this,
            [this](ovc::watch::GameState) { refreshDetectCard(); });

    connect(&m_diffWatcher, &QFutureWatcher<SetDiff>::finished, this,
            [this]() { m_diffView->showDiff(m_diffWatcher.result()); });

    // ---- Tray
    m_tray = new QSystemTrayIcon(QIcon(":/icons/taskbar.png"), this);
    auto* trayMenu = new QMenu(this);
    trayMenu->addAction(tr("Open ovc"), this, [this]() {
        setWindowOpacity(1.0);
        showNormal();
        raise();
        activateWindow();
    });
    trayMenu->addSeparator();
    trayMenu->addAction(tr("Quit"), qApp, &QApplication::quit);
    m_tray->setContextMenu(trayMenu);
    m_tray->setToolTip("ovc");
    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
        if (r == QSystemTrayIcon::Trigger) {
            setWindowOpacity(1.0);
            showNormal();
            raise();
            activateWindow();
        }
    });
    m_tray->show();

    statusBar()->setSizeGripEnabled(false);
    statusBar()->hide(); // messages only via showMessage auto-show? keep hidden; toasts later

    reloadMapsets();
    refreshDetectCard();
    show();
}

QWidget* MainWindow::buildMapsetPane()
{
    auto* pane = new QWidget;
    pane->setObjectName("MapsetPane");
    pane->setMinimumWidth(230);
    auto* v = new QVBoxLayout(pane);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    auto* header = new QLabel(tr("Mapsets"));
    header->setObjectName("PaneHeader");
    v->addWidget(header);

    m_mapsetModel = new MapsetModel(this);
    m_mapsetList = new QListView;
    m_mapsetList->setModel(m_mapsetModel);
    m_mapsetList->setItemDelegate(new MapsetDelegate(this));
    m_mapsetList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_mapsetList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    connect(m_mapsetList->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex&, const QModelIndex&) { loadHistory(); });
    v->addWidget(m_mapsetList, 1);

    auto* card = new QWidget;
    card->setObjectName("DetectCard");
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(12, 10, 12, 10);
    cardLayout->setSpacing(4);

    m_attachPill = new QLabel(tr("○ waiting for osu!"));
    m_attachPill->setProperty("pill", "detached");
    m_detectTitle = new QLabel(tr("no beatmap"));
    m_detectTitle->setObjectName("DetectTitle");
    m_detectTitle->setWordWrap(true);
    m_detectSub = new QLabel;
    m_detectSub->setObjectName("DetectSub");
    m_detectSub->setWordWrap(true);
    m_trackBtn = new QPushButton(tr("Track this mapset"));
    m_trackBtn->setObjectName("TrackButton");
    m_trackBtn->hide();
    connect(m_trackBtn, &QPushButton::clicked, this, [this]() {
        QString err;
        const auto entry = m_service.trackCurrentMapset(&err);
        if (!entry)
            QMessageBox::warning(this, tr("Track failed"), err);
        else
            selectMapset(entry->repoId);
    });

    cardLayout->addWidget(m_attachPill);
    cardLayout->addWidget(m_detectTitle);
    cardLayout->addWidget(m_detectSub);
    cardLayout->addWidget(m_trackBtn);
    v->addWidget(card);
    return pane;
}

QWidget* MainWindow::buildHistoryPane()
{
    auto* pane = new QWidget;
    pane->setObjectName("HistoryPane");
    pane->setMinimumWidth(300);
    auto* v = new QVBoxLayout(pane);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    auto* headerRow = new QWidget;
    auto* h = new QHBoxLayout(headerRow);
    h->setContentsMargins(0, 0, 12, 0);
    auto* header = new QLabel(tr("History"));
    header->setObjectName("PaneHeader");
    h->addWidget(header);
    h->addStretch();
    m_autoCheck = new QCheckBox(tr("auto"));
    m_autoCheck->setToolTip(tr("Snapshot automatically on every save"));
    connect(m_autoCheck, &QCheckBox::toggled, this, [this](bool on) {
        if (!selectedRepoId().isEmpty()) m_service.setAutoSnapshot(selectedRepoId(), on);
    });
    h->addWidget(m_autoCheck);
    m_snapshotBtn = new QPushButton(tr("Snapshot now"));
    connect(m_snapshotBtn, &QPushButton::clicked, this, [this]() {
        if (!selectedRepoId().isEmpty()) m_service.requestManualSnapshot(selectedRepoId());
    });
    h->addWidget(m_snapshotBtn);
    v->addWidget(headerRow);

    m_historyModel = new HistoryModel(this);
    m_historyList = new QListView;
    m_historyList->setModel(m_historyModel);
    m_historyList->setItemDelegate(new HistoryDelegate(this));
    m_historyList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_historyList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    connect(m_historyList->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex&, const QModelIndex&) { loadDiffForSelection(); });
    v->addWidget(m_historyList, 1);

    auto* footer = new QWidget;
    auto* f = new QHBoxLayout(footer);
    f->setContentsMargins(12, 8, 12, 10);
    m_restoreBtn = new QPushButton(tr("Restore this version"));
    m_restoreBtn->setEnabled(false);
    connect(m_restoreBtn, &QPushButton::clicked, this, &MainWindow::onRestoreClicked);
    f->addWidget(m_restoreBtn);
    v->addWidget(footer);
    return pane;
}

QWidget* MainWindow::buildDiffPane()
{
    auto* pane = new QWidget;
    pane->setObjectName("DiffPane");
    auto* v = new QVBoxLayout(pane);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    auto* header = new QLabel(tr("Changes"));
    header->setObjectName("PaneHeader");
    v->addWidget(header);

    m_diffView = new DiffView;
    m_diffView->showPlaceholder(tr("select a snapshot"));
    v->addWidget(m_diffView, 1);
    return pane;
}

void MainWindow::reloadMapsets()
{
    const QString keep = selectedRepoId();
    m_mapsetModel->reload(m_service.tracked());
    m_mapsetModel->setEditingRepoId(m_service.activeRepoId());
    if (!keep.isEmpty()) selectMapset(keep);
}

void MainWindow::refreshDetectCard()
{
    const bool attached = m_watcher.attached();
    const bool editing = m_watcher.gameState() == ovc::watch::GameState::Edit;
    m_attachPill->setText(!attached      ? tr("○ waiting for osu!")
                          : editing      ? tr("● osu! — in editor")
                                         : tr("● osu! connected"));
    m_attachPill->setProperty("pill", !attached ? "detached" : editing ? "editing" : "attached");
    m_attachPill->style()->unpolish(m_attachPill);
    m_attachPill->style()->polish(m_attachPill);

    const auto& m = m_watcher.beatmap();
    if (m.md5.isEmpty()) {
        m_detectTitle->setText(tr("no beatmap open"));
        m_detectSub->clear();
        m_trackBtn->hide();
        return;
    }
    m_detectTitle->setText(QStringLiteral("%1 - %2").arg(m.artist, m.title));
    m_detectSub->setText(QStringLiteral("[%1]  ·  %2").arg(m.version, m.folder));
    m_trackBtn->setVisible(m_service.activeRepoId().isEmpty());
}

void MainWindow::selectMapset(const QString& repoId)
{
    const QModelIndex idx = m_mapsetModel->indexOfRepo(repoId);
    if (idx.isValid()) m_mapsetList->setCurrentIndex(idx);
}

QString MainWindow::selectedRepoId() const
{
    return m_mapsetList->currentIndex().data(MapsetModel::RepoIdRole).toString();
}

QByteArray MainWindow::selectedOid() const
{
    return m_historyList->currentIndex().data(HistoryModel::OidRole).toByteArray();
}

void MainWindow::loadHistory()
{
    const QString rid = selectedRepoId();
    if (rid.isEmpty()) {
        m_historyModel->clear();
        m_diffView->showPlaceholder(tr("select a mapset"));
        return;
    }
    for (const auto& e : m_service.tracked()) {
        if (e.repoId == rid) {
            QSignalBlocker block(m_autoCheck);
            m_autoCheck->setChecked(e.autoSnapshot);
            auto repo = ovc::git::ShadowRepo::open(e.repoDir());
            m_historyModel->reload(repo ? repo->log(500)
                                        : QList<ovc::git::ShadowRepo::CommitInfo>{});
            break;
        }
    }
    if (m_historyModel->rowCount() > 0)
        m_historyList->setCurrentIndex(m_historyModel->index(0));
    else
        m_diffView->showPlaceholder(tr("no snapshots yet"));
}

void MainWindow::loadDiffForSelection()
{
    const QByteArray oid = selectedOid();
    m_restoreBtn->setEnabled(!oid.isEmpty());
    if (oid.isEmpty()) return;
    const QByteArray parent =
        m_historyList->currentIndex().data(HistoryModel::ParentOidRole).toByteArray();

    QString repoDir;
    for (const auto& e : m_service.tracked())
        if (e.repoId == selectedRepoId()) repoDir = e.repoDir();
    if (repoDir.isEmpty()) return;

    m_diffView->showPlaceholder(tr("…"));
    m_diffWatcher.setFuture(QtConcurrent::run([repoDir, parent, oid]() -> SetDiff {
        auto repo = ovc::git::ShadowRepo::open(repoDir);
        if (!repo) return {};
        return ovc::git::diffTrees(*repo, parent, oid); // "" parent = import diff vs empty
    }));
}

void MainWindow::onSnapshotTaken(const QString& repoId, const QString& subject, const QByteArray&)
{
    m_tray->setToolTip(QStringLiteral("ovc — %1").arg(subject));
    if (repoId == selectedRepoId())
        loadHistory(); // newest row auto-selected → diff pane follows
    else if (selectedRepoId().isEmpty())
        selectMapset(repoId);
}

void MainWindow::onRestoreClicked()
{
    const QString rid = selectedRepoId();
    const QByteArray oid = selectedOid();
    if (rid.isEmpty() || oid.isEmpty()) return;

    const auto pf = m_service.preflightRestore(rid);
    if (!pf.allowed) {
        QMessageBox::warning(this, tr("Cannot restore now"), pf.reason);
        return;
    }
    const QString subject = m_historyList->currentIndex().data(HistoryModel::SubjectRole).toString();
    const auto answer = QMessageBox::question(
        this, tr("Restore snapshot"),
        tr("Write \"%1\" back into the Songs folder?\n\nYour current state is snapshotted "
           "first, so nothing is lost. Press F5 in song select afterwards.")
            .arg(subject));
    if (answer != QMessageBox::Yes) return;

    QString err;
    const auto res = m_service.restore(rid, oid, &err);
    if (!res && !err.isEmpty()) QMessageBox::warning(this, tr("Restore failed"), err);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // Tracking continues in the tray; Quit lives in the tray menu.
    event->ignore();
    hide();
    if (!m_trayMessageShown) {
        m_trayMessageShown = true;
        m_tray->showMessage(tr("ovc keeps tracking"),
                            tr("Snapshots continue in the background. Right-click the tray "
                               "icon to quit."),
                            QSystemTrayIcon::Information, 4000);
    }
}

void MainWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        if (m_chrome) m_chrome->onWindowStateChanged();
        // The titlebar fades the window out before minimizing; fade back in on
        // restore or the window comes back invisible.
        if (!(windowState() & Qt::WindowMinimized) && windowOpacity() < 1.0)
            ovc::utils::propertyAnimate(this, "windowOpacity", windowOpacity(), 1.0, 200,
                                        QEasingCurve::InOutSine);
    }
}

} // namespace ovc::ui
