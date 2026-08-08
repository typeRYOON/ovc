#include <git/gitcheck.h>
#include <ui/mainwindow.h>
#include <watch/gamewatcher.h>
#include <watch/trackingservice.h>
#include <QApplication>
#include <QFile>
#include <QFontDatabase>
#include <QProxyStyle>

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
    QCoreApplication::setApplicationName("ovc");
    QApplication::setWindowIcon(QIcon(":/icons/taskbar.png"));
    QApplication::setQuitOnLastWindowClosed(false); // lives in the tray

    app.setStyle(new NoFocusRectStyle(app.style()));
    QFontDatabase::addApplicationFont(":/fonts/Hiragino Maru Gothic ProN W4.otf");
    QFile qss(":/styles/app.qss");
    if (qss.open(QIODevice::ReadOnly)) app.setStyleSheet(QString::fromUtf8(qss.readAll()));

    ovc::git::LibGit libgit;

    ovc::watch::GameWatcher watcher;
    ovc::watch::TrackingService service;
    QObject::connect(&watcher, &ovc::watch::GameWatcher::beatmapChanged, &service,
                     &ovc::watch::TrackingService::onBeatmapChanged);
    QObject::connect(&watcher, &ovc::watch::GameWatcher::beatmapCleared, &service,
                     &ovc::watch::TrackingService::onBeatmapCleared);
    QObject::connect(&watcher, &ovc::watch::GameWatcher::stateChanged, &service,
                     &ovc::watch::TrackingService::onStateChanged);
    service.setEditorTimeProvider([&watcher]() { return watcher.editorTimeMs(); });

    ovc::ui::MainWindow window(service, watcher);
    return QApplication::exec();
}
