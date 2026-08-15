#include <git/ops.h>
#include <git/setdiff.h>
#include <ovccore/canonical.h>
#include <ovccore/json.h>
#include <ovccore/parser.h>
#include <serve/localserver.h>
#include <QDir>
#include <QHttpServerResponse>
#include <QHttpServerWebSocketUpgradeResponse>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTcpServer>
#include <QTimer>
#include <QUrlQuery>
#include <QWebSocket>
#include <QtConcurrent>

namespace ovc::serve {

using ovc::git::MapsetEntry;
using ovc::git::Registry;
using ovc::git::ShadowRepo;

namespace {

QHttpServerResponse json(const QJsonValue& value,
                         QHttpServerResponse::StatusCode code = QHttpServerResponse::StatusCode::Ok)
{
    const QJsonDocument doc = value.isArray() ? QJsonDocument(value.toArray())
                                              : QJsonDocument(value.toObject());
    return QHttpServerResponse("application/json", doc.toJson(QJsonDocument::Compact), code);
}

QHttpServerResponse error(QHttpServerResponse::StatusCode code, const QString& message)
{
    return json(QJsonObject{{QStringLiteral("error"), message}}, code);
}

std::optional<MapsetEntry> entryById(const QString& repoId)
{
    Registry reg = Registry::load();
    if (const MapsetEntry* e = reg.findByRepoId(repoId)) return *e;
    return std::nullopt;
}

QJsonValue parseCoreJson(const std::string& s)
{
    return QJsonDocument::fromJson(QByteArray::fromStdString(s)).object();
}

QJsonObject preparedMergeToJson(const ovc::git::PreparedMerge& p, bool withConflicts)
{
    QJsonArray files;
    for (const ovc::git::PreparedFile& f : p.files) {
        QJsonObject o{{QStringLiteral("relPath"), f.relPath},
                      {QStringLiteral("version"), f.version},
                      {QStringLiteral("addedByThem"), f.addedByThem},
                      {QStringLiteral("wholeFileConflict"), f.wholeFileConflict},
                      {QStringLiteral("reason"), f.reason},
                      {QStringLiteral("mode"), f.mode},         // 0 std, 1 taiko, 2 catch, 3 mania
                      {QStringLiteral("keyCount"), f.keyCount}, // mania columns (0 otherwise)
                      {QStringLiteral("conflictCount"), int(f.conflicts.size())}};
        if (withConflicts) {
            QJsonArray conflicts;
            for (const ovc::git::MergeConflictInfo& c : f.conflicts)
                conflicts.append(QJsonObject{{QStringLiteral("id"), c.id},
                                             {QStringLiteral("domain"), c.domain},
                                             {QStringLiteral("key"), c.key},
                                             {QStringLiteral("timeMs"), c.timeMs},
                                             {QStringLiteral("column"), c.column},
                                             {QStringLiteral("base"), c.base},
                                             {QStringLiteral("ours"), c.ours},
                                             {QStringLiteral("theirs"), c.theirs}});
            o.insert(QStringLiteral("conflicts"), conflicts);
            // Full .osu text per side — lets the web resolver render the actual
            // notes (ours solid / theirs ghost) by parsing with its WASM engine.
            // Detail route only (the list route stays light). Read-for-display —
            // resolution still sends only per-conflict choices.
            o.insert(QStringLiteral("oursText"), QString::fromUtf8(f.oursText));
            o.insert(QStringLiteral("theirsText"), QString::fromUtf8(f.theirsText));
            o.insert(QStringLiteral("baseText"), QString::fromUtf8(f.baseText));
        }
        files.append(o);
    }
    return {{QStringLiteral("repoId"), p.repoId},
            {QStringLiteral("title"), p.bundleTitle},
            {QStringLiteral("totalConflicts"), p.totalConflicts()},
            {QStringLiteral("files"), files}};
}

const char* fileKindName(ovc::git::FileKind kind)
{
    switch (kind) {
    case ovc::git::FileKind::Difficulty: return "difficulty";
    case ovc::git::FileKind::Storyboard: return "storyboard";
    case ovc::git::FileKind::Audio: return "audio";
    case ovc::git::FileKind::Image: return "image";
    case ovc::git::FileKind::Video: return "video";
    case ovc::git::FileKind::Sample: return "sample";
    default: return "other";
    }
}

const char* fileOpName(ovc::git::FileOp op)
{
    switch (op) {
    case ovc::git::FileOp::Added: return "added";
    case ovc::git::FileOp::Removed: return "removed";
    case ovc::git::FileOp::Renamed: return "renamed";
    default: return "modified";
    }
}

} // namespace

LocalServer::LocalServer(ovc::watch::TrackingService& service, ovc::git::MergeSessionStore& merges,
                         const ovc::utils::Config& cfg, QObject* parent)
    : QObject(parent), m_service(service), m_merges(merges), m_cfg(cfg)
{
    setupRoutes();

    connect(&m_merges, &ovc::git::MergeSessionStore::pendingChanged, this,
            [this](const QString& repoId) {
                broadcast({{QStringLiteral("type"), QStringLiteral("mergePending")},
                           {QStringLiteral("repoId"), repoId},
                           {QStringLiteral("pending"), m_merges.has(repoId)}});
            });

    connect(&m_service, &ovc::watch::TrackingService::snapshotTaken, this,
            [this](const QString& repoId, const QString& subject, const QByteArray& oid) {
                broadcast({{QStringLiteral("type"), QStringLiteral("snapshotTaken")},
                           {QStringLiteral("repoId"), repoId},
                           {QStringLiteral("subject"), subject},
                           {QStringLiteral("oid"), QString::fromUtf8(oid)}});
            });
    connect(&m_service, &ovc::watch::TrackingService::activeMapsetChanged, this,
            [this](const QString& repoId) {
                broadcast({{QStringLiteral("type"), QStringLiteral("activeMapsetChanged")},
                           {QStringLiteral("repoId"), repoId}});
            });
    connect(&m_service, &ovc::watch::TrackingService::trackedListChanged, this, [this]() {
        broadcast({{QStringLiteral("type"), QStringLiteral("trackedListChanged")}});
    });
}

LocalServer::~LocalServer()
{
    stop();
}

void LocalServer::setRestoreConfirmer(RestoreConfirmer confirmer)
{
    m_confirmer = std::move(confirmer);
}

void LocalServer::announceSnapshotLabeled(const QString& repoId, const QString& oid,
                                          const QString& label)
{
    broadcast({{QStringLiteral("type"), QStringLiteral("snapshotLabeled")},
               {QStringLiteral("repoId"), repoId},
               {QStringLiteral("oid"), oid},
               {QStringLiteral("label"), label}});
}

bool LocalServer::authorized(const QHttpServerRequest& request) const
{
    const QByteArray header = request.value("Authorization");
    if (header == QByteArray("Bearer ") + m_cfg.serverToken.toUtf8()) return true;
    // <audio>/<img> tags and WebSocket upgrades cannot send headers.
    return QUrlQuery(request.url()).queryItemValue(QStringLiteral("token")) == m_cfg.serverToken;
}

void LocalServer::broadcast(const QJsonObject& event)
{
    const QString text = QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact));
    for (QWebSocket* ws : m_clients) ws->sendTextMessage(text);
}

void LocalServer::setupRoutes()
{
    using Method = QHttpServerRequest::Method;

    // Friendly landing page for humans who open the port directly — otherwise
    // "/" hits the 404 missing-handler and looks broken. No token here (the
    // app's Open viewer / Copy link buttons handle pairing); a foreign page
    // can't read this body anyway (CORS).
    m_http.route("/", Method::Get, [this](const QHttpServerRequest&) {
        const QString html =
            QStringLiteral(
                "<!doctype html><meta charset=utf-8><title>ovc local API</title>"
                "<style>body{background:#0d0d0d;color:#e0e0e0;font:15px sans-serif;"
                "max-width:34rem;margin:4rem auto;padding:0 1rem;line-height:1.6}"
                "code{background:#1a1a1a;padding:2px 6px;border-radius:4px}"
                "a{color:#8fbf8f}</style>"
                "<h2>ovc &mdash; local API</h2>"
                "<p>This is the local API the ovc web viewer talks to. "
                "It&rsquo;s running correctly &mdash; there&rsquo;s no page to view here.</p>"
                "<p>Open the actual viewer from the <b>Open viewer</b> button in the ovc "
                "app, or visit <a href=\"%1\">%1</a> and pair with the app.</p>"
                "<p style=\"color:#888\">Endpoints live under <code>/v1</code>; e.g. "
                "<code>/v1/info</code>. This server binds to localhost only.</p>")
                .arg(m_cfg.viewerUrl.toHtmlEscaped());
        return QHttpServerResponse("text/html", html.toUtf8());
    });

    // Presence probe: unauthenticated but content-free beyond identification.
    m_http.route("/v1/info", Method::Get, [](const QHttpServerRequest&) {
        return json(QJsonObject{{QStringLiteral("app"), QStringLiteral("ovc")},
                                {QStringLiteral("api"), 1}});
    });

    m_http.route("/v1/mapsets", Method::Get, [this](const QHttpServerRequest& req) {
        if (!authorized(req)) return error(QHttpServerResponse::StatusCode::Unauthorized, "token");
        QJsonArray arr;
        for (const MapsetEntry& e : Registry::load().entries) {
            arr.append(QJsonObject{{QStringLiteral("repoId"), e.repoId},
                                   {QStringLiteral("title"), e.title},
                                   {QStringLiteral("artist"), e.artist},
                                   {QStringLiteral("creator"), e.creator},
                                   {QStringLiteral("folderName"), e.folderName},
                                   {QStringLiteral("beatmapSetId"), e.beatmapSetId},
                                   {QStringLiteral("autoSnapshot"), e.autoSnapshot},
                                   {QStringLiteral("editingNow"),
                                    e.repoId == m_service.activeRepoId()}});
        }
        return json(arr);
    });

    m_http.route(
        "/v1/mapsets/<arg>/snapshots", Method::Get,
        [this](const QString& repoId, const QHttpServerRequest& req) {
            if (!authorized(req))
                return error(QHttpServerResponse::StatusCode::Unauthorized, "token");
            const auto entry = entryById(repoId);
            if (!entry) return error(QHttpServerResponse::StatusCode::NotFound, "unknown mapset");
            auto repo = ShadowRepo::open(entry->repoDir());
            if (!repo) return error(QHttpServerResponse::StatusCode::NotFound, "repo missing");
            QJsonArray arr;
            for (const auto& c : repo->log(1000)) {
                QJsonObject trailers;
                for (auto it = c.trailers.constBegin(); it != c.trailers.constEnd(); ++it)
                    trailers.insert(it.key(), it.value());
                arr.append(QJsonObject{{QStringLiteral("oid"), QString::fromUtf8(c.oid)},
                                       {QStringLiteral("parentOid"),
                                        QString::fromUtf8(c.parentOid)},
                                       {QStringLiteral("when"),
                                        c.when.toUTC().toString(Qt::ISODate)},
                                       {QStringLiteral("subject"), c.subject},
                                       {QStringLiteral("label"), c.label},
                                       {QStringLiteral("trailers"), trailers}});
            }
            return json(arr);
        });

    m_http.route(
        "/v1/snapshots/<arg>/<arg>/tree", Method::Get,
        [this](const QString& repoId, const QString& oid, const QHttpServerRequest& req) {
            if (!authorized(req))
                return error(QHttpServerResponse::StatusCode::Unauthorized, "token");
            const auto entry = entryById(repoId);
            if (!entry) return error(QHttpServerResponse::StatusCode::NotFound, "unknown mapset");
            auto repo = ShadowRepo::open(entry->repoDir());
            if (!repo) return error(QHttpServerResponse::StatusCode::NotFound, "repo missing");
            QJsonArray arr;
            for (const auto& [relPath, blobOid] : repo->listTree(oid.toUtf8())) {
                arr.append(QJsonObject{{QStringLiteral("path"), relPath},
                                       {QStringLiteral("oid"), QString::fromUtf8(blobOid)},
                                       {QStringLiteral("size"),
                                        double(repo->blobSize(blobOid))}});
            }
            return json(arr);
        });

    m_http.route(
        "/v1/blobs/<arg>/<arg>", Method::Get,
        [this](const QString& repoId, const QString& blobOid, const QHttpServerRequest& req) {
            if (!authorized(req))
                return QtConcurrent::run([] {
                    return error(QHttpServerResponse::StatusCode::Unauthorized, "token");
                });
            return QtConcurrent::run([repoId, blobOid]() -> QHttpServerResponse {
                const auto entry = entryById(repoId);
                if (!entry)
                    return error(QHttpServerResponse::StatusCode::NotFound, "unknown mapset");
                auto repo = ShadowRepo::open(entry->repoDir());
                if (!repo) return error(QHttpServerResponse::StatusCode::NotFound, "repo missing");
                const QByteArray bytes = repo->readBlob(blobOid.toUtf8());
                if (bytes.isEmpty() && repo->blobSize(blobOid.toUtf8()) != 0)
                    return error(QHttpServerResponse::StatusCode::NotFound, "unknown blob");
                return QHttpServerResponse("application/octet-stream", bytes);
            });
        });

    m_http.route(
        "/v1/diff/<arg>", Method::Get,
        [this](const QString& repoId, const QHttpServerRequest& req) {
            if (!authorized(req))
                return QtConcurrent::run([] {
                    return error(QHttpServerResponse::StatusCode::Unauthorized, "token");
                });
            const QUrlQuery q(req.url());
            const QByteArray from = q.queryItemValue(QStringLiteral("from")).toUtf8();
            const QByteArray to = q.queryItemValue(QStringLiteral("to")).toUtf8();
            return QtConcurrent::run([repoId, from, to]() -> QHttpServerResponse {
                const auto entry = entryById(repoId);
                if (!entry)
                    return error(QHttpServerResponse::StatusCode::NotFound, "unknown mapset");
                auto repo = ShadowRepo::open(entry->repoDir());
                if (!repo) return error(QHttpServerResponse::StatusCode::NotFound, "repo missing");
                const ovc::git::SetDiff diff = ovc::git::diffTrees(*repo, from, to);
                QJsonArray files;
                for (const ovc::git::FileChange& c : diff.files) {
                    QJsonObject o{{QStringLiteral("op"), fileOpName(c.op)},
                                  {QStringLiteral("path"), c.relPath},
                                  {QStringLiteral("kind"), fileKindName(c.kind)},
                                  {QStringLiteral("oldSize"), double(c.oldSize)},
                                  {QStringLiteral("newSize"), double(c.newSize)}};
                    if (!c.oldRelPath.isEmpty())
                        o.insert(QStringLiteral("oldPath"), c.oldRelPath);
                    if (c.semantic)
                        o.insert(QStringLiteral("semantic"),
                                 parseCoreJson(ovc::core::diffToJson(*c.semantic)));
                    files.append(o);
                }
                return json(QJsonObject{{QStringLiteral("files"), files},
                                        {QStringLiteral("subject"), diff.subjectLine()}});
            });
        });

    m_http.route(
        "/v1/map/<arg>/<arg>", Method::Get,
        [this](const QString& repoId, const QString& blobOid, const QHttpServerRequest& req) {
            if (!authorized(req))
                return QtConcurrent::run([] {
                    return error(QHttpServerResponse::StatusCode::Unauthorized, "token");
                });
            return QtConcurrent::run([repoId, blobOid]() -> QHttpServerResponse {
                const auto entry = entryById(repoId);
                if (!entry)
                    return error(QHttpServerResponse::StatusCode::NotFound, "unknown mapset");
                auto repo = ShadowRepo::open(entry->repoDir());
                if (!repo) return error(QHttpServerResponse::StatusCode::NotFound, "repo missing");
                const QByteArray bytes = repo->readBlob(blobOid.toUtf8());
                const auto map = ovc::core::canonicalize(
                    ovc::core::parseOsu({bytes.constData(), size_t(bytes.size())}).doc);
                return json(parseCoreJson(ovc::core::mapToJson(map)));
            });
        });

    m_http.route(
        "/v1/restore/<arg>/<arg>", Method::Post,
        [this](const QString& repoId, const QString& oid, const QHttpServerRequest& req) {
            if (!authorized(req))
                return QtConcurrent::run([] {
                    return error(QHttpServerResponse::StatusCode::Unauthorized, "token");
                });
            const auto entry = entryById(repoId);
            if (!entry || !m_confirmer)
                return QtConcurrent::run([] {
                    return error(QHttpServerResponse::StatusCode::NotFound, "unknown mapset");
                });
            const auto pf = m_service.preflightRestore(repoId);
            if (!pf.allowed) {
                const QString reason = pf.reason;
                return QtConcurrent::run([reason] {
                    return error(QHttpServerResponse::StatusCode::Conflict, reason);
                });
            }
            auto repo = ShadowRepo::open(entry->repoDir());
            const auto info = repo ? repo->commitInfo(oid.toUtf8()) : std::nullopt;
            if (!info)
                return QtConcurrent::run([] {
                    return error(QHttpServerResponse::StatusCode::NotFound, "unknown snapshot");
                });

            const MapsetEntry entryCopy = *entry;
            const QByteArray oidBytes = oid.toUtf8();
            // Desktop-side confirmation gate, then the restore.
            return m_confirmer(entryCopy.title, info->subject)
                .then([this, entryCopy, oidBytes, repoId](bool approved) -> QHttpServerResponse {
                    if (!approved)
                        return error(QHttpServerResponse::StatusCode::Forbidden, "declined");
                    QString err;
                    const auto res = ovc::git::restoreMapset(entryCopy, oidBytes, &err);
                    if (!res && !err.isEmpty())
                        return error(QHttpServerResponse::StatusCode::InternalServerError, err);
                    // A restore lands as new commits straight through git, not the
                    // tracker, so no snapshotTaken fires on its own — announce it so
                    // connected viewers follow their history to the new head.
                    if (res) {
                        const QByteArray newOid = res->commitOid;
                        QMetaObject::invokeMethod(this, [this, repoId, newOid] {
                            broadcast({{QStringLiteral("type"), QStringLiteral("snapshotTaken")},
                                       {QStringLiteral("repoId"), repoId},
                                       {QStringLiteral("subject"), QStringLiteral("restore")},
                                       {QStringLiteral("oid"), QString::fromUtf8(newOid)}});
                        });
                    }
                    return json(QJsonObject{
                        {QStringLiteral("restored"), true},
                        {QStringLiteral("newOid"),
                         res ? QString::fromUtf8(res->commitOid) : QString()}});
                });
        });

    // Rename a snapshot: set (or clear, with "") its user label. The web
    // viewer's rename control drives this; the desktop stays the writer of the
    // label file, and every viewer gets the snapshotLabeled push.
    m_http.route(
        "/v1/snapshots/<arg>/<arg>/label", Method::Post,
        [this](const QString& repoId, const QString& oid, const QHttpServerRequest& req) {
            if (!authorized(req))
                return error(QHttpServerResponse::StatusCode::Unauthorized, "token");
            const auto entry = entryById(repoId);
            if (!entry) return error(QHttpServerResponse::StatusCode::NotFound, "unknown mapset");
            auto repo = ShadowRepo::open(entry->repoDir());
            if (!repo) return error(QHttpServerResponse::StatusCode::NotFound, "repo missing");
            if (!repo->commitInfo(oid.toUtf8()))
                return error(QHttpServerResponse::StatusCode::NotFound, "unknown snapshot");
            const QString label = QJsonDocument::fromJson(req.body())
                                      .object()
                                      .value(QStringLiteral("label"))
                                      .toString();
            QString err;
            if (!repo->setLabel(oid.toUtf8(), label, &err))
                return error(QHttpServerResponse::StatusCode::InternalServerError,
                             err.isEmpty() ? QStringLiteral("could not set label") : err);
            const QString stored = repo->labelFor(oid.toUtf8()); // setLabel trims
            announceSnapshotLabeled(repoId, oid, stored);
            return json(QJsonObject{{QStringLiteral("labeled"), true},
                                    {QStringLiteral("label"), stored}});
        });

    // ---- pending merges (web resolver) ----

    m_http.route("/v1/merges", Method::Get, [this](const QHttpServerRequest& req) {
        if (!authorized(req)) return error(QHttpServerResponse::StatusCode::Unauthorized, "token");
        QJsonArray arr;
        for (const ovc::git::PreparedMerge& p : m_merges.all())
            arr.append(preparedMergeToJson(p, false)); // summary: no per-conflict detail
        return json(arr);
    });

    m_http.route(
        "/v1/merges/<arg>", Method::Get,
        [this](const QString& repoId, const QHttpServerRequest& req) {
            if (!authorized(req))
                return error(QHttpServerResponse::StatusCode::Unauthorized, "token");
            const ovc::git::PreparedMerge* p = m_merges.get(repoId);
            if (!p) return error(QHttpServerResponse::StatusCode::NotFound, "no pending merge");
            return json(preparedMergeToJson(*p, true));
        });

    m_http.route(
        "/v1/merges/<arg>/resolve", Method::Post,
        [this](const QString& repoId, const QHttpServerRequest& req) {
            if (!authorized(req))
                return QtConcurrent::run(
                    [] { return error(QHttpServerResponse::StatusCode::Unauthorized, "token"); });
            const ovc::git::PreparedMerge* p = m_merges.get(repoId);
            const auto entry = entryById(repoId);
            if (!p || !entry)
                return QtConcurrent::run(
                    [] { return error(QHttpServerResponse::StatusCode::NotFound, "no pending merge"); });
            const auto pf = m_service.preflightRestore(repoId);
            if (!pf.allowed) {
                const QString reason = pf.reason;
                return QtConcurrent::run([reason] {
                    return error(QHttpServerResponse::StatusCode::Conflict, reason);
                });
            }
            // { "resolutions": { "<relPath>": { "<conflict id>": "ours"|"theirs" } } }
            // Per-file so ids that repeat across difficulties resolve independently.
            ovc::git::FileResolutions resolutions;
            const QJsonObject body = QJsonDocument::fromJson(req.body()).object();
            const QJsonObject res = body.value(QStringLiteral("resolutions")).toObject();
            for (auto it = res.constBegin(); it != res.constEnd(); ++it) {
                QMap<QString, QString> fileRes;
                const QJsonObject ids = it.value().toObject();
                for (auto jt = ids.constBegin(); jt != ids.constEnd(); ++jt)
                    fileRes.insert(jt.key(), jt.value().toString());
                resolutions.insert(it.key(), fileRes);
            }

            const ovc::git::PreparedMerge prepared = *p;
            const ovc::git::MapsetEntry entryCopy = *entry;
            return QtConcurrent::run([this, entryCopy, prepared, resolutions,
                                      repoId]() -> QHttpServerResponse {
                QString err;
                const auto out = ovc::git::applyBundleMerge(entryCopy, prepared, resolutions, &err);
                if (!out)
                    return error(QHttpServerResponse::StatusCode::InternalServerError,
                                 err.isEmpty() ? QStringLiteral("merge failed") : err);
                // The store lives on the main thread and is read by other routes
                // there; mutate it back on that thread, not this worker. The merge
                // committed via git (not the tracker), so announce the snapshot
                // ourselves — otherwise connected viewers never refresh their
                // history after a resolve.
                const QByteArray oid = out->snapshotOid;
                QMetaObject::invokeMethod(this, [this, repoId, oid] {
                    m_merges.remove(repoId);
                    if (!oid.isEmpty())
                        broadcast({{QStringLiteral("type"), QStringLiteral("snapshotTaken")},
                                   {QStringLiteral("repoId"), repoId},
                                   {QStringLiteral("subject"), QStringLiteral("merge")},
                                   {QStringLiteral("oid"), QString::fromUtf8(oid)}});
                });
                return json(QJsonObject{{QStringLiteral("applied"), true},
                                        {QStringLiteral("snapshotOid"),
                                         QString::fromUtf8(out->snapshotOid)}});
            });
        });

    m_http.route(
        "/v1/merges/<arg>/cancel", Method::Post,
        [this](const QString& repoId, const QHttpServerRequest& req) {
            if (!authorized(req))
                return error(QHttpServerResponse::StatusCode::Unauthorized, "token");
            if (!m_merges.has(repoId))
                return error(QHttpServerResponse::StatusCode::NotFound, "no pending merge");
            m_merges.remove(repoId);
            return json(QJsonObject{{QStringLiteral("cancelled"), true}});
        });

    // Open the mapset's tracked Songs folder in the OS file browser. Only ever
    // opens a *tracked* mapset's own folder, so it can't reach arbitrary paths.
    // Shell out (QtCore only — the serve target has no QtGui) so `ovc-cli serve`
    // stays headless-buildable.
    m_http.route(
        "/v1/reveal/<arg>", Method::Post,
        [this](const QString& repoId, const QHttpServerRequest& req) {
            if (!authorized(req))
                return error(QHttpServerResponse::StatusCode::Unauthorized, "token");
            const auto entry = entryById(repoId);
            if (!entry || entry->songsPath.isEmpty())
                return error(QHttpServerResponse::StatusCode::NotFound,
                             "no local folder for this mapset");
            const QString path = entry->songsPath;
#if defined(Q_OS_WIN)
            QProcess::startDetached(QStringLiteral("explorer.exe"),
                                    {QDir::toNativeSeparators(path)});
#elif defined(Q_OS_MACOS)
            QProcess::startDetached(QStringLiteral("open"), {path});
#else
            QProcess::startDetached(QStringLiteral("xdg-open"), {path});
#endif
            return json(QJsonObject{{QStringLiteral("revealed"), true}});
        });

    // CORS on every response when the Origin is allowlisted.
    m_http.addAfterRequestHandler(this, [this](const QHttpServerRequest& req,
                                               QHttpServerResponse& resp) {
        const QByteArray origin = req.value("Origin");
        if (!origin.isEmpty() && m_cfg.corsOrigins.contains(QString::fromUtf8(origin))) {
            auto headers = resp.headers();
            headers.append(QHttpHeaders::WellKnownHeader::AccessControlAllowOrigin, origin);
            resp.setHeaders(std::move(headers));
        }
    });

    // Router misses land here — including every CORS/PNA preflight, since we
    // register no OPTIONS routes. A public HTTPS page (ryoon.moe) reaching a
    // loopback address triggers BOTH a standard CORS preflight (the
    // Authorization header isn't safelisted) AND Chrome's Private Network
    // Access preflight; the browser blocks the real request unless this reply
    // carries the matching headers. The after-request hook above doesn't run
    // for responder-written replies, so the preflight sets its own.
    m_http.setMissingHandler(this, [this](const QHttpServerRequest& req,
                                          QHttpServerResponder& responder) {
        if (req.method() == Method::Options) {
            QHttpServerResponse resp(QHttpServerResponse::StatusCode::NoContent);
            const QByteArray origin = req.value("Origin");
            if (!origin.isEmpty() && m_cfg.corsOrigins.contains(QString::fromUtf8(origin))) {
                QHttpHeaders h = resp.headers();
                h.append(QHttpHeaders::WellKnownHeader::AccessControlAllowOrigin, origin);
                h.append(QHttpHeaders::WellKnownHeader::AccessControlAllowMethods,
                         "GET, POST, OPTIONS");
                h.append(QHttpHeaders::WellKnownHeader::AccessControlAllowHeaders,
                         "Authorization, Content-Type");
                // Private Network Access: no WellKnownHeader enum for it yet.
                h.append("Access-Control-Allow-Private-Network", "true");
                h.append(QHttpHeaders::WellKnownHeader::AccessControlMaxAge, "600");
                resp.setHeaders(std::move(h));
            }
            responder.sendResponse(resp);
            return;
        }
        responder.write(QJsonDocument(QJsonObject{{QStringLiteral("error"),
                                                   QStringLiteral("not found")}}),
                        QHttpServerResponder::StatusCode::NotFound);
    });

    // WebSocket /v1/events (token via query — browsers can't set WS headers).
    m_http.addWebSocketUpgradeVerifier(this, [this](const QHttpServerRequest& req) {
        if (req.url().path() == QStringLiteral("/v1/events") && authorized(req))
            return QHttpServerWebSocketUpgradeResponse::accept();
        return QHttpServerWebSocketUpgradeResponse::deny();
    });
    connect(&m_http, &QAbstractHttpServer::newWebSocketConnection, this, [this]() {
        while (m_http.hasPendingWebSocketConnections()) {
            QWebSocket* ws = m_http.nextPendingWebSocketConnection().release();
            ws->setParent(this);
            m_clients.append(ws);
            connect(ws, &QWebSocket::disconnected, this, [this, ws]() {
                m_clients.removeOne(ws);
                ws->deleteLater();
                emit clientCountChanged(int(m_clients.size()));
            });
            emit clientCountChanged(int(m_clients.size()));
        }
    });
}

bool LocalServer::start(QString* err)
{
    if (m_running) return true;
    m_tcp = new QTcpServer(this);
    if (!m_tcp->listen(QHostAddress::LocalHost, quint16(m_cfg.serverPort))) {
        m_lastError = QStringLiteral("port %1 unavailable: %2")
                          .arg(m_cfg.serverPort)
                          .arg(m_tcp->errorString());
        if (err) *err = m_lastError;
        m_tcp->deleteLater();
        m_tcp = nullptr;
        return false;
    }
    if (!m_http.bind(m_tcp)) {
        m_lastError = QStringLiteral("http bind failed");
        if (err) *err = m_lastError;
        m_tcp->deleteLater();
        m_tcp = nullptr;
        return false;
    }
    m_lastError.clear();
    m_running = true;
    emit runningChanged(true);
    return true;
}

void LocalServer::startAutoRetry()
{
    if (start()) return;
    // Usually a just-closed instance is still releasing the port; poll until
    // it frees rather than staying dead until the app is restarted.
    if (!m_retryTimer) {
        m_retryTimer = new QTimer(this);
        m_retryTimer->setInterval(3000);
        connect(m_retryTimer, &QTimer::timeout, this, [this]() {
            if (start()) m_retryTimer->stop();
        });
    }
    m_retryTimer->start();
}

void LocalServer::stop()
{
    if (m_retryTimer) m_retryTimer->stop();
    if (!m_running) return;
    for (QWebSocket* ws : m_clients) ws->close();
    m_clients.clear();
    if (m_tcp) m_tcp->close();
    m_running = false;
    emit runningChanged(false);
}

} // namespace ovc::serve
