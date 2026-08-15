#pragma once
#include <QString>
#include <QStringList>

namespace ovc::utils {

// %LOCALAPPDATA%/ovc/config.json. Load-modify-save whole; tiny.
struct Config {
    int serverPort = 27500;
    bool serveEnabled = true;
    bool updateCheckEnabled = true; // poll GitHub Releases on launch (notify only)
    QString serverToken;   // minted on first run; pairs the web viewer
    QString viewerUrl = QStringLiteral("https://ryoon.moe/ovc");
    QStringList corsOrigins{QStringLiteral("https://ryoon.moe"),
                            QStringLiteral("http://localhost:4321")}; // astro dev

    static Config load();
    bool save(QString* err = nullptr) const;
};

} // namespace ovc::utils
