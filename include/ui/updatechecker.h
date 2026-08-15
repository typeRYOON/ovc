#pragma once
#include <QObject>
#include <QString>

class QNetworkAccessManager;

namespace ovc::ui {

// Notify-only update check: polls the project's GitHub Releases and emits
// updateAvailable when the latest tag is newer than the built-in version. No
// downloading or installing — just a nudge, so the mapper doesn't keep running
// a build whose stable memory offsets have gone stale after an osu! update.
class UpdateChecker : public QObject {
    Q_OBJECT
public:
    explicit UpdateChecker(QObject* parent = nullptr);
    void start(); // check now, then re-check once a day

signals:
    void updateAvailable(const QString& version, const QString& url);

private:
    void check();
    QNetworkAccessManager* m_nam;
};

} // namespace ovc::ui
