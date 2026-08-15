#pragma once
#include <git/mergesessions.h>
#include <utils/config.h>
#include <watch/trackingservice.h>
#include <QFuture>
#include <QHttpServer>
#include <QObject>
#include <functional>
#include <memory>

class QTcpServer;
class QTimer;
class QWebSocket;

namespace ovc::serve {

// Loopback-only API the web viewer talks to (see PLAN.md deliverable B).
// Every /v1 route except /v1/info requires the bearer token from config;
// CORS is pinned to the configured origins. Restore requests resolve through
// a UI-provided confirmer so only the desktop can approve writes.
class LocalServer : public QObject {
    Q_OBJECT
public:
    LocalServer(ovc::watch::TrackingService& service, ovc::git::MergeSessionStore& merges,
                const ovc::utils::Config& cfg, QObject* parent = nullptr);
    ~LocalServer() override;

    // Bind now; on failure (usually the port is briefly held by an exiting
    // instance) keep retrying in the background until it frees. runningChanged
    // fires when it finally binds.
    void startAutoRetry();
    bool start(QString* err = nullptr); // single attempt
    void stop();
    bool running() const { return m_running; }
    QString lastError() const { return m_lastError; }
    quint16 port() const { return quint16(m_cfg.serverPort); }

    // Returns a future resolving to the user's decision. Default: reject.
    using RestoreConfirmer = std::function<QFuture<bool>(QString title, QString subject)>;
    void setRestoreConfirmer(RestoreConfirmer confirmer);

    // Push a snapshotLabeled event to connected viewers. Called by the label
    // route and by the desktop's own Rename action so a rename made anywhere
    // updates every open viewer live.
    void announceSnapshotLabeled(const QString& repoId, const QString& oid, const QString& label);

signals:
    void clientCountChanged(int count);
    void runningChanged(bool running);
    // A web-triggered write (label / restore / merge-resolve) landed on this
    // repo — the desktop UI listens so it reflects changes made from the viewer.
    void repoChanged(const QString& repoId);

private:
    bool authorized(const QHttpServerRequest& request) const;
    void broadcast(const QJsonObject& event);
    void setupRoutes();

    ovc::watch::TrackingService& m_service;
    ovc::git::MergeSessionStore& m_merges;
    ovc::utils::Config m_cfg;
    QHttpServer m_http;
    QTcpServer* m_tcp = nullptr;
    QList<QWebSocket*> m_clients;
    RestoreConfirmer m_confirmer;
    QTimer* m_retryTimer = nullptr;
    QString m_lastError;
    bool m_running = false;
};

} // namespace ovc::serve
