#pragma once
#include <utils/config.h>
#include <watch/trackingservice.h>
#include <QFuture>
#include <QMainWindow>
#include <QSystemTrayIcon>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;

namespace ovc::watch {
class GameWatcher;
}
namespace ovc::serve {
class LocalServer;
}
namespace ovc::git {
class MergeSessionStore;
}

namespace ovc::ui {

class MapsetModel;
class SnapshotModel;
class UpdateChecker;
class WindowChrome;

// The slim tray-first window: tracked list + live status + local actions.
// All visualization lives in the web viewer (PLAN.md).
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(ovc::watch::TrackingService& service, ovc::watch::GameWatcher& watcher,
               ovc::serve::LocalServer* server, ovc::git::MergeSessionStore& merges,
               const ovc::utils::Config& cfg, QWidget* parent = nullptr);

    // Non-blocking approval dialog for site-initiated restores.
    QFuture<bool> confirmRestore(const QString& title, const QString& subject);

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void reloadMapsets();
    void loadSnapshots();
    void refreshDetectCard();
    void refreshServerPill();
    void refreshActions();
    void selectMapset(const QString& repoId);
    void openViewer();
    void onExportClicked();
    void onMergeBundleClicked();
    void onRenameClicked();
    void onRestoreClicked();
    void showMapsetMenu(const QPoint& pos);
    QString selectedRepoId() const;
    QByteArray selectedSnapshotOid() const;
    const ovc::git::MapsetEntry* selectedEntry() const;

    ovc::watch::TrackingService& m_service;
    ovc::watch::GameWatcher& m_watcher;
    ovc::serve::LocalServer* m_server;
    ovc::git::MergeSessionStore& m_merges;
    ovc::utils::Config m_cfg;
    WindowChrome* m_chrome = nullptr;

    MapsetModel* m_mapsetModel = nullptr;
    QListView* m_mapsetList = nullptr;
    SnapshotModel* m_snapshotModel = nullptr;
    QListView* m_snapshotList = nullptr;

    QLabel* m_detectTitle = nullptr;
    QLabel* m_detectSub = nullptr;
    QLabel* m_attachPill = nullptr;
    QPushButton* m_trackBtn = nullptr;

    QCheckBox* m_autoCheck = nullptr;
    QLineEdit* m_snapshotName = nullptr;
    QPushButton* m_snapshotBtn = nullptr;
    QPushButton* m_exportBtn = nullptr;
    QPushButton* m_mergeBtn = nullptr;
    QPushButton* m_renameBtn = nullptr;
    QPushButton* m_restoreBtn = nullptr;

    QLabel* m_serverPill = nullptr;
    QPushButton* m_viewerBtn = nullptr;

    QSystemTrayIcon* m_tray = nullptr;
    bool m_quitting = false; // guards the fade-out-then-quit on close

    QLabel* m_updatePill = nullptr;
    UpdateChecker* m_updateChecker = nullptr;
    QString m_notifiedVersion; // dedupes the tray balloon per version
    QString m_updateUrl;       // release page, opened from the tray balloon
};

} // namespace ovc::ui
