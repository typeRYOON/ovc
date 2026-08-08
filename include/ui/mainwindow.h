#pragma once
#include <QMainWindow>

namespace ovc::watch {
class GameWatcher;
}

namespace ovc::ui {

// Shell only until M5; shows live watcher state so the skeleton is testable.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(ovc::watch::GameWatcher& watcher, QWidget* parent = nullptr);

private:
    ovc::watch::GameWatcher& m_watcher;
};

} // namespace ovc::ui
