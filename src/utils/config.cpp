#include <git/paths.h>
#include <utils/config.h>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

namespace ovc::utils {

Config Config::load()
{
    Config c;
    QFile f(ovc::git::configPath());
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        c.serverPort = o.value(QStringLiteral("serverPort")).toInt(c.serverPort);
        c.serveEnabled = o.value(QStringLiteral("serveEnabled")).toBool(c.serveEnabled);
        c.updateCheckEnabled =
            o.value(QStringLiteral("updateCheckEnabled")).toBool(c.updateCheckEnabled);
        c.serverToken = o.value(QStringLiteral("serverToken")).toString();
        c.viewerUrl = o.value(QStringLiteral("viewerUrl")).toString(c.viewerUrl);
        if (o.contains(QStringLiteral("corsOrigins"))) {
            c.corsOrigins.clear();
            for (const auto& v : o.value(QStringLiteral("corsOrigins")).toArray())
                c.corsOrigins << v.toString();
        }
    }
    if (c.serverToken.isEmpty()) {
        c.serverToken = QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-') +
                        QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-');
        c.save();
    }
    return c;
}

bool Config::save(QString* err) const
{
    QDir().mkpath(ovc::git::dataRoot());
    QFile f(ovc::git::configPath());
    if (!f.open(QIODevice::WriteOnly)) {
        if (err) *err = QStringLiteral("cannot write ") + ovc::git::configPath();
        return false;
    }
    QJsonArray origins;
    for (const QString& o : corsOrigins) origins.append(o);
    f.write(QJsonDocument(QJsonObject{
                              {QStringLiteral("serverPort"), serverPort},
                              {QStringLiteral("serveEnabled"), serveEnabled},
                              {QStringLiteral("updateCheckEnabled"), updateCheckEnabled},
                              {QStringLiteral("serverToken"), serverToken},
                              {QStringLiteral("viewerUrl"), viewerUrl},
                              {QStringLiteral("corsOrigins"), origins},
                          })
                .toJson());
    return true;
}

} // namespace ovc::utils
