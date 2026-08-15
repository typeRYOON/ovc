#include <ui/updatechecker.h>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#ifndef OVC_VERSION
#define OVC_VERSION "0.0.0" // filled in by CMake (PROJECT_VERSION); fallback keeps this buildable
#endif

namespace ovc::ui {

namespace {
// GitHub "owner/repo" whose Releases we watch. Change if the repo is renamed.
constexpr auto kRepo = "typeRYOON/ovc";
constexpr int kIntervalMs = 24 * 60 * 60 * 1000; // once a day

// Parse "1.2.3" / "v1.2.3" into three ints. false if it isn't plain semver
// (a tag we can't compare just means "don't nag").
bool parseVer(const QString& s, int out[3])
{
    QString t = s.trimmed();
    if (t.startsWith(QLatin1Char('v')) || t.startsWith(QLatin1Char('V'))) t = t.mid(1);
    const QStringList parts = t.split(QLatin1Char('.'));
    if (parts.isEmpty() || parts.size() > 3) return false;
    out[0] = out[1] = out[2] = 0;
    for (int i = 0; i < parts.size(); ++i) {
        bool ok = false;
        out[i] = parts[i].toInt(&ok);
        if (!ok) return false;
    }
    return true;
}

bool isNewer(const QString& remote, const QString& current)
{
    int r[3], c[3];
    if (!parseVer(remote, r) || !parseVer(current, c)) return false;
    for (int i = 0; i < 3; ++i)
        if (r[i] != c[i]) return r[i] > c[i];
    return false;
}
} // namespace

UpdateChecker::UpdateChecker(QObject* parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this))
{
}

void UpdateChecker::start()
{
    check();
    auto* timer = new QTimer(this);
    timer->setInterval(kIntervalMs);
    connect(timer, &QTimer::timeout, this, &UpdateChecker::check);
    timer->start();
}

void UpdateChecker::check()
{
    QNetworkRequest req(QUrl(QStringLiteral("https://api.github.com/repos/%1/releases/latest")
                                 .arg(QLatin1String(kRepo))));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("User-Agent", "ovc"); // GitHub rejects requests without a UA
    QNetworkReply* reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        // Offline, rate-limited, or no releases published yet: stay quiet.
        if (reply->error() != QNetworkReply::NoError) return;
        const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
        const QString tag = o.value(QStringLiteral("tag_name")).toString();
        if (tag.isEmpty() || !isNewer(tag, QStringLiteral(OVC_VERSION))) return;
        QString url = o.value(QStringLiteral("html_url")).toString();
        if (url.isEmpty())
            url = QStringLiteral("https://github.com/%1/releases/latest").arg(QLatin1String(kRepo));
        emit updateAvailable(tag, url);
    });
}

} // namespace ovc::ui
