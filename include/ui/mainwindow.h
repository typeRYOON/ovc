#pragma once
#include <git/setdiff.h>
#include <watch/trackingservice.h>
#include <QFutureWatcher>
#include <QMainWindow>
#include <QSystemTrayIcon>

class QCheckBox;
class QLabel;
class QListView;
class QPushButton;

namespace ovc::watch {
class GameWatcher;
}

namespace ovc::ui {

class DiffView;
class HistoryModel;
class MapsetModel;
class WindowChrome;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(ovc::watch::TrackingService& service, ovc::watch::GameWatcher& watcher,
               QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    QWidget* buildMapsetPane();
    QWidget* buildHistoryPane();
    QWidget* buildDiffPane();
    void reloadMapsets();
    void refreshDetectCard();
    void selectMapset(const QString& repoId);
    void loadHistory();          // for the selected mapset
    void loadDiffForSelection(); // selected commit → async SetDiff
    void onSnapshotTaken(const QString& repoId, const QString& subject, const QByteArray& oid);
    void onRestoreClicked();
    QString selectedRepoId() const;
    QByteArray selectedOid() const;

    ovc::watch::TrackingService& m_service;
    ovc::watch::GameWatcher& m_watcher;
    WindowChrome* m_chrome = nullptr;

    MapsetModel* m_mapsetModel = nullptr;
    HistoryModel* m_historyModel = nullptr;
    QListView* m_mapsetList = nullptr;
    QListView* m_historyList = nullptr;
    DiffView* m_diffView = nullptr;

    QLabel* m_detectTitle = nullptr;
    QLabel* m_detectSub = nullptr;
    QLabel* m_attachPill = nullptr;
    QPushButton* m_trackBtn = nullptr;
    QPushButton* m_snapshotBtn = nullptr;
    QPushButton* m_restoreBtn = nullptr;
    QCheckBox* m_autoCheck = nullptr;

    QFutureWatcher<ovc::git::SetDiff> m_diffWatcher;
    QSystemTrayIcon* m_tray = nullptr;
    bool m_trayMessageShown = false;
};

} // namespace ovc::ui
