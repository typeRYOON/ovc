#include <git/gitcheck.h>
#include <git/mergesessions.h>
#include <serve/localserver.h>
#include <ui/mainwindow.h>
#include <utils/config.h>
#include <watch/gamewatcher.h>
#include <watch/trackingservice.h>
#include <QApplication>
#include <QFile>
#include <QFontDatabase>
#include <QLockFile>
#include <QProxyStyle>
#include <QStandardPaths>

namespace {

// Suppress the dotted focus rectangle app-wide (tagcomposer convention).
class NoFocusRectStyle : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;
    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                       const QWidget* widget) const override
    {
        if (element == PE_FrameFocusRect) return;
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }
};

} // namespace

int main(int argc, char* argv[])
{
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::Floor);
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("ovc"); // identity for %LOCALAPPDATA%/ovc — do not change
    QGuiApplication::setApplicationDisplayName("osu! Version Control");
    QApplication::setWindowIcon(QIcon(":/icons/std.svg"));
    QApplication::setQuitOnLastWindowClosed(false); // lives in the tray

    // One instance only: a second launch would fight the first for the API
    // port and spawn a duplicate tray icon. Bail if we're already running.
    QLockFile lock(QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
                   QStringLiteral("/ovc.lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock()) return 0;

    app.setStyle(new NoFocusRectStyle(app.style()));
    QFontDatabase::addApplicationFont(":/fonts/Hiragino Maru Gothic ProN W4.otf");
    QFile qss(":/styles/app.qss");
    if (qss.open(QIODevice::ReadOnly)) app.setStyleSheet(QString::fromUtf8(qss.readAll()));

    ovc::git::LibGit libgit;
    const ovc::utils::Config cfg = ovc::utils::Config::load();

    ovc::watch::GameWatcher watcher;
    ovc::watch::TrackingService service;
    QObject::connect(&watcher, &ovc::watch::GameWatcher::beatmapChanged, &service,
                     &ovc::watch::TrackingService::onBeatmapChanged);
    QObject::connect(&watcher, &ovc::watch::GameWatcher::beatmapCleared, &service,
                     &ovc::watch::TrackingService::onBeatmapCleared);
    QObject::connect(&watcher, &ovc::watch::GameWatcher::stateChanged, &service,
                     &ovc::watch::TrackingService::onStateChanged);
    service.setEditorTimeProvider([&watcher]() { return watcher.editorTimeMs(); });

    ovc::git::MergeSessionStore merges;
    ovc::serve::LocalServer server(service, merges, cfg);
    if (cfg.serveEnabled) server.startAutoRetry(); // recovers once the port frees

    ovc::ui::MainWindow window(service, watcher, &server, merges, cfg);
    server.setRestoreConfirmer([&window](QString title, QString subject) {
        return window.confirmRestore(title, subject);
    });
    return QApplication::exec();
}
