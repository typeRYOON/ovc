#include <chrome/windowchrome.h>
#include <git/bundle.h>
#include <git/mergesessions.h>
#include <git/ops.h>
#include <git/shadowrepo.h>
#include <serve/localserver.h>
#include <QAbstractButton>
#include <ui/mainwindow.h>
#include <ui/mapsetmodel.h>
#include <ui/snapshotmodel.h>
#include <ui/updatechecker.h>
#include <utils/qutils.h>
#include <watch/gamewatcher.h>
#include <QApplication>
#include <cstdlib> // std::_Exit — hard-exit backstop so close can never hang
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QUrl>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPromise>
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QSplitter>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace ovc::ui {

using ovc::watch::GameWatcher;
using ovc::watch::TrackingService;

MainWindow::MainWindow(TrackingService& service, GameWatcher& watcher,
                       ovc::serve::LocalServer* server, ovc::git::MergeSessionStore& merges,
                       const ovc::utils::Config& cfg, QWidget* parent)
    : QMainWindow(parent), m_service(service), m_watcher(watcher), m_server(server),
      m_merges(merges), m_cfg(cfg)
{
    setWindowTitle("osu! Version Control");
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowMinimizeButtonHint |
                   Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);
    resize(620, 800);
    setMinimumSize(500, 640);

    m_chrome = new WindowChrome(this);
    setCentralWidget(m_chrome->frame());

    auto* v = new QVBoxLayout(m_chrome->bodyWidget());
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    // ---- Detect card
    auto* card = new QWidget;
    card->setObjectName("DetectCard");
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(14, 12, 14, 12);
    cardLayout->setSpacing(4);
    m_attachPill = new QLabel(tr("○ waiting for osu!"));
    m_attachPill->setProperty("pill", "detached");
    m_detectTitle = new QLabel(tr("no beatmap open"));
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

    // ---- Mapsets (top) / snapshots (bottom), splitter between them
    auto* split = new QSplitter(Qt::Vertical);
    split->setObjectName("BodySplit");
    split->setChildrenCollapsible(false);
    split->setHandleWidth(1);

    auto* mapsetPane = new QWidget;
    auto* mp = new QVBoxLayout(mapsetPane);
    mp->setContentsMargins(0, 0, 0, 0);
    mp->setSpacing(0);
    auto* mapsetHeader = new QLabel(tr("Tracked mapsets"));
    mapsetHeader->setObjectName("PaneHeader");
    mp->addWidget(mapsetHeader);
    m_mapsetModel = new MapsetModel(this);
    m_mapsetList = new QListView;
    m_mapsetList->setModel(m_mapsetModel);
    m_mapsetList->setItemDelegate(new MapsetDelegate(this));
    m_mapsetList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_mapsetList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    connect(m_mapsetList->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex&, const QModelIndex&) {
                loadSnapshots();
                refreshActions();
            });
    m_mapsetList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_mapsetList, &QListView::customContextMenuRequested, this,
            &MainWindow::showMapsetMenu);
    mp->addWidget(m_mapsetList, 1);

    // Mapset-level actions.
    auto* mapsetActions = new QWidget;
    mapsetActions->setFixedHeight(50);
    auto* ma = new QHBoxLayout(mapsetActions);
    ma->setContentsMargins(14, 8, 14, 10);
    ma->setSpacing(8);
    m_autoCheck = new QCheckBox(tr("auto"));
    m_autoCheck->setToolTip(tr("Snapshot automatically on every save"));
    connect(m_autoCheck, &QCheckBox::toggled, this, [this](bool on) {
        if (!selectedRepoId().isEmpty()) m_service.setAutoSnapshot(selectedRepoId(), on);
    });
    // git commit -m style: type a name, press Enter (or Snapshot) to save it.
    m_snapshotName = new QLineEdit;
    m_snapshotName->setPlaceholderText(tr("name this snapshot (optional)"));
    m_snapshotName->setClearButtonEnabled(true);
    m_snapshotBtn = new QPushButton(tr("Snapshot"));
    auto doSnapshot = [this]() {
        const QString rid = selectedRepoId();
        if (rid.isEmpty()) return;
        m_service.requestManualSnapshot(rid, m_snapshotName->text());
        m_snapshotName->clear();
    };
    connect(m_snapshotBtn, &QPushButton::clicked, this, doSnapshot);
    connect(m_snapshotName, &QLineEdit::returnPressed, this, doSnapshot);
    m_exportBtn = new QPushButton(tr("Export"));
    connect(m_exportBtn, &QPushButton::clicked, this, &MainWindow::onExportClicked);
    m_mergeBtn = new QPushButton(tr("Merge"));
    m_mergeBtn->setToolTip(tr("Merge a collaborator's bundle into this mapset"));
    connect(m_mergeBtn, &QPushButton::clicked, this, &MainWindow::onMergeBundleClicked);
    ma->addWidget(m_autoCheck);
    ma->addWidget(m_snapshotName, 1);
    ma->addWidget(m_snapshotBtn);
    ma->addWidget(m_exportBtn);
    ma->addWidget(m_mergeBtn);
    mp->addWidget(mapsetActions);
    split->addWidget(mapsetPane);

    auto* snapPane = new QWidget;
    auto* sp = new QVBoxLayout(snapPane);
    sp->setContentsMargins(0, 0, 0, 0);
    sp->setSpacing(0);
    auto* snapHeader = new QLabel(tr("Snapshots"));
    snapHeader->setObjectName("PaneHeader");
    sp->addWidget(snapHeader);
    m_snapshotModel = new SnapshotModel(this);
    m_snapshotList = new QListView;
    m_snapshotList->setModel(m_snapshotModel);
    m_snapshotList->setItemDelegate(new SnapshotDelegate(this));
    m_snapshotList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_snapshotList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    connect(m_snapshotList->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex&, const QModelIndex&) { refreshActions(); });
    connect(m_snapshotList, &QListView::doubleClicked, this,
            [this](const QModelIndex&) { onRenameClicked(); });
    sp->addWidget(m_snapshotList, 1);

    // Snapshot-level actions.
    auto* snapActions = new QWidget;
    snapActions->setFixedHeight(50);
    auto* sa = new QHBoxLayout(snapActions);
    sa->setContentsMargins(14, 8, 14, 10);
    sa->setSpacing(8);
    m_renameBtn = new QPushButton(tr("Rename"));
    m_renameBtn->setToolTip(tr("Give this snapshot a name (double-click a snapshot too)"));
    connect(m_renameBtn, &QPushButton::clicked, this, &MainWindow::onRenameClicked);
    m_restoreBtn = new QPushButton(tr("Restore"));
    connect(m_restoreBtn, &QPushButton::clicked, this, &MainWindow::onRestoreClicked);
    sa->addStretch();
    sa->addWidget(m_renameBtn);
    sa->addWidget(m_restoreBtn);
    sp->addWidget(snapActions);
    split->addWidget(snapPane);

    split->setSizes({240, 360});
    v->addWidget(split, 1);

    // ---- Server footer
    auto* footer = new QWidget;
    footer->setObjectName("DetectCard");
    footer->setFixedHeight(56);
    auto* f = new QHBoxLayout(footer);
    f->setContentsMargins(14, 10, 14, 10);
    f->setSpacing(8);
    m_serverPill = new QLabel;
    m_updatePill = new QLabel;
    m_updatePill->setObjectName("UpdatePill");
    m_updatePill->setTextFormat(Qt::RichText);
    m_updatePill->setOpenExternalLinks(true); // clicking the link opens the release page
    m_updatePill->hide();                      // shown only once an update is found
    m_viewerBtn = new QPushButton(tr("Open viewer"));
    m_viewerBtn->setObjectName("TrackButton");
    connect(m_viewerBtn, &QPushButton::clicked, this, &MainWindow::openViewer);
    auto* copyBtn = new QPushButton(tr("Copy link"));
    copyBtn->setToolTip(tr("Copy the pairing link (contains your access token)"));
    connect(copyBtn, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(
            QStringLiteral("%1#token=%2&port=%3")
                .arg(m_cfg.viewerUrl, m_cfg.serverToken, QString::number(m_cfg.serverPort)));
    });
    f->addWidget(m_serverPill, 1);
    f->addWidget(m_updatePill);
    f->addWidget(copyBtn);
    f->addWidget(m_viewerBtn);
    v->addWidget(footer);

    // ---- Wiring
    connect(&m_service, &TrackingService::trackedListChanged, this, &MainWindow::reloadMapsets);
    connect(&m_service, &TrackingService::activeMapsetChanged, this, [this](const QString& rid) {
        m_mapsetModel->setEditingRepoId(rid);
        refreshDetectCard();
        if (!rid.isEmpty() && !m_mapsetList->currentIndex().isValid()) selectMapset(rid);
    });
    connect(&m_service, &TrackingService::snapshotTaken, this,
            [this](const QString& repoId, const QString& subject, const QByteArray&) {
                m_tray->setToolTip(QStringLiteral("osu! Version Control — %1").arg(subject));
                if (repoId == selectedRepoId()) loadSnapshots();
            });
    connect(&m_service, &TrackingService::snapshotFailed, this,
            [this](const QString&, const QString& reason) {
                m_tray->showMessage(tr("Snapshot failed"), reason, QSystemTrayIcon::Warning,
                                    5000);
            });
    connect(&m_service, &TrackingService::snapshotClean, this, [this](const QString& repoId) {
        if (repoId != selectedRepoId()) return;
        m_snapshotBtn->setText(tr("no changes"));
        QTimer::singleShot(1400, this, [this]() { m_snapshotBtn->setText(tr("Snapshot")); });
    });
    connect(&m_watcher, &GameWatcher::attachedChanged, this, [this](bool) { refreshDetectCard(); });
    connect(&m_watcher, &GameWatcher::beatmapChanged, this,
            [this](const ovc::watch::MemBeatmap&) { refreshDetectCard(); });
    connect(&m_watcher, &GameWatcher::beatmapCleared, this, [this]() { refreshDetectCard(); });
    connect(&m_watcher, &GameWatcher::stateChanged, this,
            [this](ovc::watch::GameState) { refreshDetectCard(); });
    if (m_server) {
        connect(m_server, &ovc::serve::LocalServer::clientCountChanged, this,
                [this](int) { refreshServerPill(); });
        connect(m_server, &ovc::serve::LocalServer::runningChanged, this,
                [this](bool) { refreshServerPill(); });
        // A write made from the web viewer (rename / restore / merge) → mirror it
        // in the desktop's snapshot list so both stay in sync.
        connect(m_server, &ovc::serve::LocalServer::repoChanged, this,
                [this](const QString& repoId) {
                    if (repoId == selectedRepoId()) loadSnapshots();
                });
    }
    // A pending merge resolved (or cancelled) in the web viewer → refresh the
    // snapshot list if it was for the selected mapset.
    connect(&m_merges, &ovc::git::MergeSessionStore::pendingChanged, this,
            [this](const QString& repoId) {
                if (repoId == selectedRepoId()) loadSnapshots();
            });

    // ---- Tray
    m_tray = new QSystemTrayIcon(QIcon(":/icons/std.svg"), this);
    auto* trayMenu = new QMenu(this);
    trayMenu->addAction(tr("Open ovc"), this, [this]() {
        setWindowOpacity(1.0);
        showNormal();
        raise();
        activateWindow();
    });
    trayMenu->addAction(tr("Open viewer"), this, &MainWindow::openViewer);
    trayMenu->addSeparator();
    trayMenu->addAction(tr("Quit"), qApp, &QApplication::quit);
    m_tray->setContextMenu(trayMenu);
    m_tray->setToolTip("osu! Version Control");
    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
        if (r == QSystemTrayIcon::Trigger) {
            setWindowOpacity(1.0);
            showNormal();
            raise();
            activateWindow();
        }
    });
    m_tray->show();

    // ---- Update check (notify-only; GitHub Releases)
    connect(m_tray, &QSystemTrayIcon::messageClicked, this, [this]() {
        if (!m_updateUrl.isEmpty()) QDesktopServices::openUrl(QUrl(m_updateUrl));
    });
    m_updateChecker = new UpdateChecker(this);
    connect(m_updateChecker, &UpdateChecker::updateAvailable, this,
            [this](const QString& version, const QString& url) {
                m_updateUrl = url;
                m_updatePill->setText(
                    QStringLiteral("<a href=\"%1\" style=\"color:#c8a050;text-decoration:none;\">"
                                   "&#8593; Update available (%2)</a>")
                        .arg(url, version));
                m_updatePill->show();
                if (m_notifiedVersion != version) { // balloon once per version, not per re-check
                    m_notifiedVersion = version;
                    m_tray->showMessage(tr("ovc update available"),
                                        tr("%1 is out - click to view the release.").arg(version),
                                        QSystemTrayIcon::Information);
                }
            });
    if (m_cfg.updateCheckEnabled) m_updateChecker->start();

    // ---- Keybinds
    auto* esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(esc, &QShortcut::activated, this, [this]() {
        if (!isFullScreen()) return;
        // Fade out, drop to normal, fade back in. Timers sequence it, and a
        // fallback restores full opacity no matter what, so Esc can never leave
        // the window stranded invisible.
        ovc::utils::propertyAnimate(this, "windowOpacity", windowOpacity(), 0.0, 150,
                                    QEasingCurve::InOutSine);
        QTimer::singleShot(160, this, [this]() {
            showNormal();
            ovc::utils::propertyAnimate(this, "windowOpacity", windowOpacity(), 1.0, 150,
                                        QEasingCurve::InOutSine);
        });
        QTimer::singleShot(500, this, [this]() { setWindowOpacity(1.0); });
    });
    auto* quit = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Q")), this);
    connect(quit, &QShortcut::activated, qApp, &QApplication::quit);
    auto* closeKey = new QShortcut(QKeySequence(QStringLiteral("Ctrl+W")), this);
    connect(closeKey, &QShortcut::activated, this, &QWidget::close);

    reloadMapsets();
    refreshDetectCard();
    refreshServerPill();
    refreshActions();
    // Fade in on first show instead of popping in. Startup only — never touches
    // the close/quit path. The fallback timer guarantees the window can't get
    // stuck invisible if the animation is ever interrupted.
    setWindowOpacity(0.0);
    show();
    ovc::utils::propertyAnimate(this, "windowOpacity", 0.0, 1.0, 220, QEasingCurve::InOutSine);
    QTimer::singleShot(300, this, [this]() { setWindowOpacity(1.0); });
}

QFuture<bool> MainWindow::confirmRestore(const QString& title, const QString& subject)
{
    setWindowOpacity(1.0);
    showNormal();
    raise();
    activateWindow();

    auto promise = std::make_shared<QPromise<bool>>();
    promise->start();
    auto* box = new QMessageBox(QMessageBox::Question, tr("Restore from viewer"),
                                tr("The web viewer asks to restore\n\n%1\n\"%2\"\n\ninto the "
                                   "Songs folder. Proceed?")
                                    .arg(title, subject),
                                QMessageBox::Yes | QMessageBox::No, this);
    connect(box, &QMessageBox::finished, this, [promise, box](int result) {
        promise->addResult(result == QMessageBox::Yes);
        promise->finish();
        box->deleteLater();
    });
    box->open(); // non-modal: never blocks the server's event loop
    return promise->future();
}

void MainWindow::openViewer()
{
    QDesktopServices::openUrl(
        QUrl(QStringLiteral("%1#token=%2&port=%3")
                 .arg(m_cfg.viewerUrl, m_cfg.serverToken, QString::number(m_cfg.serverPort))));
}

void MainWindow::reloadMapsets()
{
    const QString keep = selectedRepoId();
    m_mapsetModel->reload(m_service.tracked());
    m_mapsetModel->setEditingRepoId(m_service.activeRepoId());
    if (!keep.isEmpty()) selectMapset(keep);
    refreshActions();
}

void MainWindow::refreshDetectCard()
{
    const bool attached = m_watcher.attached();
    const bool editing = m_watcher.gameState() == ovc::watch::GameState::Edit;
    m_attachPill->setText(!attached  ? tr("○ waiting for osu!")
                          : editing  ? tr("● osu! — in editor")
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

void MainWindow::refreshServerPill()
{
    if (!m_server || !m_server->running()) {
        m_serverPill->setText(tr("○ local API off"));
        m_serverPill->setProperty("pill", "detached");
        // Say why — usually another ovc instance already holds the port.
        m_serverPill->setToolTip(m_server && !m_server->lastError().isEmpty()
                                     ? m_server->lastError() + tr("\n(retrying…)")
                                     : QString());
    }
    else {
        m_serverPill->setText(tr("● API on :%1").arg(m_server->port()));
        m_serverPill->setProperty("pill", "attached");
        m_serverPill->setToolTip(tr("The web viewer can reach this app."));
    }
    m_serverPill->style()->unpolish(m_serverPill);
    m_serverPill->style()->polish(m_serverPill);
}

void MainWindow::refreshActions()
{
    const ovc::git::MapsetEntry* e = selectedEntry();
    const bool canSnapshot = e && !e->songsPath.isEmpty();
    m_snapshotBtn->setEnabled(canSnapshot);
    m_snapshotName->setEnabled(canSnapshot);
    m_exportBtn->setEnabled(e != nullptr);
    m_mergeBtn->setEnabled(canSnapshot); // merge writes into the Songs folder
    QSignalBlocker block(m_autoCheck);
    m_autoCheck->setEnabled(e && !e->songsPath.isEmpty());
    m_autoCheck->setChecked(e && e->autoSnapshot);

    // Rename works on any snapshot; restore needs a local folder to write into.
    const bool haveSnap = !selectedSnapshotOid().isEmpty();
    m_renameBtn->setEnabled(haveSnap);
    m_restoreBtn->setEnabled(haveSnap && e && !e->songsPath.isEmpty());
}

void MainWindow::loadSnapshots()
{
    const QString rid = selectedRepoId();
    if (rid.isEmpty()) {
        m_snapshotModel->clear();
        return;
    }
    for (const auto& e : m_service.tracked()) {
        if (e.repoId != rid) continue;
        auto repo = ovc::git::ShadowRepo::open(e.repoDir());
        m_snapshotModel->reload(repo ? repo->log(1000)
                                     : QList<ovc::git::ShadowRepo::CommitInfo>{});
        if (m_snapshotModel->rowCount() > 0)
            m_snapshotList->setCurrentIndex(m_snapshotModel->index(0));
        break;
    }
    refreshActions();
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

void MainWindow::showMapsetMenu(const QPoint& pos)
{
    const QModelIndex idx = m_mapsetList->indexAt(pos);
    if (!idx.isValid()) return;
    m_mapsetList->setCurrentIndex(idx); // right-clicking a row acts on that row
    const QString rid = idx.data(MapsetModel::RepoIdRole).toString();
    const QString title = idx.data(MapsetModel::TitleRole).toString();
    if (rid.isEmpty()) return;

    QMenu menu(this);
    QAction* relinkAct = menu.addAction(tr("Relink to folder"));
    relinkAct->setToolTip(tr("Point this set at a renamed/moved folder (e.g. after uploading)"));
    menu.addSeparator();
    QAction* stopAct = menu.addAction(tr("Stop tracking / Remove"));
    QAction* chosen = menu.exec(m_mapsetList->viewport()->mapToGlobal(pos));
    if (!chosen) return;

    if (chosen == relinkAct) {
        const ovc::git::MapsetEntry* e = selectedEntry();
        const QString dir = QFileDialog::getExistingDirectory(
            this, tr("Relink \"%1\" to its folder").arg(title), e ? e->songsPath : QString());
        if (dir.isEmpty()) return;
        QString relErr;
        if (!m_service.relink(rid, dir, &relErr))
            QMessageBox::warning(this, tr("osu! Version Control"), tr("Couldn't relink: %1").arg(relErr));
        return;
    }
    if (chosen != stopAct) return;

    // Deleting the shadow repo is the default; the opt-in checkbox keeps it on
    // disk (orphaned) for manual recovery. The osu! Songs folder is never touched.
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Stop tracking"));
    box.setText(tr("Stop tracking \"%1\"?").arg(title));
    box.setInformativeText(tr("It's removed from ovc and its snapshot history is deleted. "
                              "Your osu! Songs folder is not touched."));
    QCheckBox keep(tr("Keep the snapshot history on disk"));
    box.setCheckBox(&keep);
    box.setStandardButtons(QMessageBox::Cancel | QMessageBox::Yes);
    box.setDefaultButton(QMessageBox::Cancel);
    if (box.exec() != QMessageBox::Yes) return;

    QString err;
    if (!m_service.untrack(rid, !keep.isChecked(), &err))
        QMessageBox::warning(this, tr("osu! Version Control"), tr("Couldn't remove: %1").arg(err));
    // trackedListChanged -> reloadMapsets repopulates the list.
}

QByteArray MainWindow::selectedSnapshotOid() const
{
    return m_snapshotModel->oidAt(m_snapshotList->currentIndex());
}

const ovc::git::MapsetEntry* MainWindow::selectedEntry() const
{
    const QString rid = selectedRepoId();
    if (rid.isEmpty()) return nullptr;
    for (const auto& e : m_service.tracked())
        if (e.repoId == rid) {
            static ovc::git::MapsetEntry copy;
            copy = e;
            return &copy;
        }
    return nullptr;
}

void MainWindow::onExportClicked()
{
    const ovc::git::MapsetEntry* e = selectedEntry();
    if (!e) return;
    QString suggested = QStringLiteral("%1 - %2.ovcz").arg(e->artist, e->title);
    suggested.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")),
                      QStringLiteral("_"));
    const QString path = QFileDialog::getSaveFileName(this, tr("Export bundle"), suggested,
                                                      tr("ovc bundle (*.ovcz)"));
    if (path.isEmpty()) return;
    const auto mode = QMessageBox::question(
        this, tr("Export bundle"),
        tr("Include media (audio/images)?\n\nYes — full bundle, shareable as-is.\nNo — "
           "text-only history: much smaller, media referenced by hash."),
        QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Yes);
    if (mode == QMessageBox::Cancel) return;

    QString err;
    if (!ovc::git::exportBundle(*e, path, mode == QMessageBox::No, &err)) {
        QMessageBox::warning(this, tr("Export failed"), err);
        return;
    }
    m_tray->showMessage(tr("Bundle exported"),
                        QStringLiteral("%1 (%2 KiB)")
                            .arg(QFileInfo(path).fileName())
                            .arg(QFileInfo(path).size() / 1024),
                        QSystemTrayIcon::Information, 4000);
}

void MainWindow::onMergeBundleClicked()
{
    const ovc::git::MapsetEntry* e = selectedEntry();
    if (!e || e->songsPath.isEmpty()) return;
    const QString rid = e->repoId;

    // Same guard as restore: the merge writes into Songs, which osu! would
    // overwrite on its next save if the editor is open on this mapset.
    const auto pf = m_service.preflightRestore(rid);
    if (!pf.allowed) {
        QMessageBox::warning(this, tr("Cannot merge now"), pf.reason);
        return;
    }

    const QString path = QFileDialog::getOpenFileName(this, tr("Merge collaborator's bundle"),
                                                      QString(), tr("ovc bundle (*.ovcz)"));
    if (path.isEmpty()) return;

    QString err;
    const auto prepared = ovc::git::prepareBundleMerge(*e, path, &err);
    if (!prepared) {
        QMessageBox::warning(this, tr("Merge failed"), err.isEmpty() ? tr("unknown error") : err);
        return;
    }
    if (!prepared->anyMergeable()) {
        QMessageBox::information(this, tr("Nothing to merge"),
                                 tr("This bundle has no changes your mapset is missing."));
        return;
    }

    // No overlaps → apply straight away; overlaps → hand off to the web resolver.
    if (!prepared->hasConflicts()) {
        const auto outcome = ovc::git::applyBundleMerge(*e, *prepared, {}, &err);
        loadSnapshots();
        if (!outcome)
            QMessageBox::warning(this, tr("Merge failed"), err);
        else {
            QString msg = tr("%1 merged in.").arg(prepared->bundleTitle);
            if (const int mw = outcome->report.mediaWritten())
                msg += tr(" %1 media file%2 synced.").arg(mw).arg(mw == 1 ? "" : "s");
            msg += tr(" Press F5 in song select.");
            m_tray->showMessage(tr("Merged cleanly"), msg, QSystemTrayIcon::Information, 4000);
        }
        return;
    }

    // Conflicts: hold the merge pending and send the user to the resolver.
    m_merges.put(*prepared);
    const int n = prepared->totalConflicts();
    auto* box = new QMessageBox(
        QMessageBox::Question, tr("Conflicts to resolve"),
        tr("%1 has %2 overlapping edit%3 with your version. Resolve them in the web viewer "
           "(pick yours or theirs per conflict), and ovc applies the result here.\n\nNothing "
           "has changed on disk yet.")
            .arg(prepared->bundleTitle)
            .arg(n)
            .arg(n == 1 ? "" : "s"),
        QMessageBox::NoButton, this);
    QPushButton* openBtn = box->addButton(tr("Open resolver"), QMessageBox::AcceptRole);
    box->addButton(tr("Keep mine (resolve now)"), QMessageBox::DestructiveRole);
    box->addButton(QMessageBox::Cancel);
    box->setAttribute(Qt::WA_DeleteOnClose);
    // Capture rid by value, not the entry pointer: selectedEntry() hands back a
    // pointer into a shared static that a later selection would overwrite.
    connect(box, &QMessageBox::finished, this, [this, box, openBtn, rid](int) {
        QAbstractButton* clicked = box->clickedButton();
        if (clicked == openBtn) {
            openViewer(); // the resolver pops from the mergePending event
            return;
        }
        if (box->buttonRole(clicked) == QMessageBox::DestructiveRole) {
            // Fallback for desktop-only users: keep mine for every conflict.
            const ovc::git::PreparedMerge* p = m_merges.get(rid);
            if (!p) { m_merges.remove(rid); return; }
            const auto prepared2 = *p;
            ovc::git::MapsetEntry entry;
            bool found = false;
            for (const auto& te : m_service.tracked())
                if (te.repoId == rid) { entry = te; found = true; break; }
            m_merges.remove(rid);
            if (!found) return;
            QString err2;
            ovc::git::applyBundleMerge(entry, prepared2, {}, &err2);
            loadSnapshots();
        }
        else {
            m_merges.remove(rid); // cancelled
        }
    });
    box->open();
}

void MainWindow::onRenameClicked()
{
    const ovc::git::MapsetEntry* e = selectedEntry();
    const QByteArray oid = selectedSnapshotOid();
    if (!e || oid.isEmpty()) return;

    auto repo = ovc::git::ShadowRepo::open(e->repoDir());
    if (!repo) return;
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("Name snapshot"),
        tr("A name for this snapshot (blank to clear):"), QLineEdit::Normal,
        repo->labelFor(oid), &ok);
    if (!ok) return;

    QString err;
    if (!repo->setLabel(oid, name, &err))
        QMessageBox::warning(this, tr("Rename failed"), err);
    else {
        loadSnapshots(); // re-read so the list shows the new name
        if (m_server)
            m_server->announceSnapshotLabeled(e->repoId, QString::fromUtf8(oid),
                                              repo->labelFor(oid));
    }
}

void MainWindow::onRestoreClicked()
{
    const ovc::git::MapsetEntry* e = selectedEntry();
    const QByteArray oid = selectedSnapshotOid();
    if (!e || oid.isEmpty()) return;
    const QString rid = e->repoId;

    auto repo = ovc::git::ShadowRepo::open(e->repoDir());
    const auto info = repo ? repo->commitInfo(oid) : std::nullopt;
    if (!info) return;

    const auto pf = m_service.preflightRestore(rid);
    if (!pf.allowed) {
        QMessageBox::warning(this, tr("Cannot restore now"), pf.reason);
        return;
    }
    const QString shown = info->label.isEmpty() ? info->subject : info->label;
    const auto answer = QMessageBox::question(
        this, tr("Restore snapshot"),
        tr("Write \"%1\" back into the Songs folder?\n\nYour current state is snapshotted "
           "first. Press F5 in song select afterwards.")
            .arg(shown));
    if (answer != QMessageBox::Yes) return;

    QString err;
    const auto res = m_service.restore(rid, oid, &err);
    if (!res && !err.isEmpty()) QMessageBox::warning(this, tr("Restore failed"), err);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // Close = quit the app (no close-to-tray); tracking/serving stop with it.
    // Fade out first, then re-close: the second pass runs the plain, proven
    // accept+quit below. Timers drive it (never animation callbacks), and a hard
    // last-resort exit guarantees the app always terminates — the window can
    // never get stuck uncloseable, whatever the fade does.
    if (m_quitting) {
        event->accept();
        qApp->quit();
        return;
    }
    m_quitting = true;
    event->ignore();
    ovc::utils::propertyAnimate(this, "windowOpacity", windowOpacity(), 0.0, 150,
                                QEasingCurve::InOutSine);
    QTimer::singleShot(160, this, [this]() { close(); });
    QTimer::singleShot(1000, qApp, []() { std::_Exit(0); });
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
