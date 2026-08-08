#include <git/gitcheck.h>
#include <ui/mainwindow.h>
#include <watch/gamewatcher.h>
#include <QApplication>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("ovc");
    ovc::git::LibGit libgit;

    ovc::watch::GameWatcher watcher;
    ovc::ui::MainWindow window(watcher);
    window.show();
    return app.exec();
}
