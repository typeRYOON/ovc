#include <git/gitcheck.h>
#include <git/ops.h>
#include <git/paths.h>
#include <git/mergesessions.h>
#include <serve/localserver.h>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace ovc::git;
using namespace ovc::serve;

namespace {

QByteArray osuFixture(const char* extra = "")
{
    return QByteArray("osu file format v14\n\n[General]\nMode: 3\nAudioFilename: audio.mp3\n\n"
                      "[Metadata]\nTitle:T\nArtist:A\nCreator:C\nVersion:VA\nBeatmapID:11\n"
                      "BeatmapSetID:99\n\n[Difficulty]\nHPDrainRate:8\nCircleSize:7\n"
                      "OverallDifficulty:8\nApproachRate:5\nSliderMultiplier:1.4\n"
                      "SliderTickRate:1\n\n[TimingPoints]\n1000,300,4,2,1,70,1,0\n\n"
                      "[HitObjects]\n36,192,1000,1,0,0:0:0:0:\n") +
           extra;
}

bool writeFile(const QString& path, const QByteArray& content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(content) == content.size();
}

} // namespace

class TestServer : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void infoIsPublic();
    void authRequired();
    void mapsetsAndSnapshots();
    void treeBlobDiffMap();
    void corsHeaders();
    void preflightGrantsCorsAndPna();
    void restoreConfirmerGate();
    void snapshotLabelFlow();
    void mergeResolveFlow();

private:
    QByteArray get(const QString& path, bool auth = true, int* status = nullptr,
                   const QString& origin = {});
    QByteArray post(const QString& path, bool auth, int* status, const QByteArray& body = {});

    QTemporaryDir m_songs;
    MapsetEntry m_entry;
    ovc::utils::Config m_cfg;
    std::unique_ptr<ovc::watch::TrackingService> m_service;
    ovc::git::MergeSessionStore m_merges;
    std::unique_ptr<LocalServer> m_server;
    QNetworkAccessManager m_nam;
    QByteArray m_snapshots; // cached snapshot list JSON
};

QByteArray TestServer::get(const QString& path, bool auth, int* status, const QString& origin)
{
    QNetworkRequest req(QUrl(QStringLiteral("http://127.0.0.1:%1%2")
                                 .arg(m_cfg.serverPort)
                                 .arg(path)));
    if (auth) req.setRawHeader("Authorization", "Bearer " + m_cfg.serverToken.toUtf8());
    if (!origin.isEmpty()) req.setRawHeader("Origin", origin.toUtf8());
    QNetworkReply* reply = m_nam.get(req);
    QSignalSpy done(reply, &QNetworkReply::finished);
    done.wait(15000);
    if (status)
        *status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    if (!origin.isEmpty() && status && *status == 200) {
        // stash CORS header presence marker for corsHeaders()
        if (reply->rawHeader("Access-Control-Allow-Origin") != origin.toUtf8() && status)
            *status = -1;
    }
    reply->deleteLater();
    return body;
}

QByteArray TestServer::post(const QString& path, bool auth, int* status, const QByteArray& body)
{
    QNetworkRequest req(QUrl(QStringLiteral("http://127.0.0.1:%1%2")
                                 .arg(m_cfg.serverPort)
                                 .arg(path)));
    if (auth) req.setRawHeader("Authorization", "Bearer " + m_cfg.serverToken.toUtf8());
    if (!body.isEmpty()) req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply* reply = m_nam.post(req, body);
    QSignalSpy done(reply, &QNetworkReply::finished);
    done.wait(15000);
    if (status)
        *status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray responseBody = reply->readAll();
    reply->deleteLater();
    return responseBody;
}

void TestServer::initTestCase()
{
    QCoreApplication::setApplicationName("ovc");
    QStandardPaths::setTestModeEnabled(true);
    static LibGit s_libgit;
    QDir(dataRoot()).removeRecursively();

    const QString mapset = m_songs.path() + "/99 T - A";
    QVERIFY(writeFile(mapset + "/T - A (C) [VA].osu", osuFixture()));
    QVERIFY(writeFile(mapset + "/audio.mp3", QByteArray(50000, 'x')));
    QString err;
    const auto entry = trackMapset(mapset, &err);
    QVERIFY2(entry.has_value(), qPrintable(err));
    m_entry = *entry;
    QTest::qWait(15);
    QVERIFY(writeFile(mapset + "/T - A (C) [VA].osu",
                      osuFixture("256,192,1500,1,0,0:0:0:0:\n")));
    QVERIFY(snapshotMapset(m_entry, "manual", {}, &err).has_value());

    m_cfg.serverPort = 27912; // out of the way of a real instance
    m_cfg.serverToken = QStringLiteral("test-token-1234");
    m_cfg.corsOrigins = {QStringLiteral("https://ryoon.moe")};
    m_service = std::make_unique<ovc::watch::TrackingService>();
    m_server = std::make_unique<LocalServer>(*m_service, m_merges, m_cfg);
    QVERIFY2(m_server->start(&err), qPrintable(err));
}

void TestServer::infoIsPublic()
{
    int status = 0;
    const auto body = get("/v1/info", false, &status);
    QCOMPARE(status, 200);
    QCOMPARE(QJsonDocument::fromJson(body).object().value("app").toString(),
             QStringLiteral("ovc"));
}

void TestServer::authRequired()
{
    int status = 0;
    get("/v1/mapsets", false, &status);
    QCOMPARE(status, 401);
    get("/v1/mapsets", true, &status);
    QCOMPARE(status, 200);
    // Query-token fallback (audio tags / WS can't send headers).
    get(QStringLiteral("/v1/mapsets?token=") + m_cfg.serverToken, false, &status);
    QCOMPARE(status, 200);
}

void TestServer::mapsetsAndSnapshots()
{
    int status = 0;
    const auto mapsets = QJsonDocument::fromJson(get("/v1/mapsets", true, &status)).array();
    QCOMPARE(status, 200);
    QCOMPARE(mapsets.size(), 1);
    QCOMPARE(mapsets[0].toObject().value("repoId").toString(), m_entry.repoId);

    m_snapshots = get(QStringLiteral("/v1/mapsets/%1/snapshots").arg(m_entry.repoId), true,
                      &status);
    QCOMPARE(status, 200);
    const auto arr = QJsonDocument::fromJson(m_snapshots).array();
    QCOMPARE(arr.size(), 2);
    QVERIFY(arr[1].toObject().value("subject").toString().startsWith("[import]"));
}

void TestServer::treeBlobDiffMap()
{
    const auto arr = QJsonDocument::fromJson(m_snapshots).array();
    const QString head = arr[0].toObject().value("oid").toString();
    const QString parent = arr[0].toObject().value("parentOid").toString();

    int status = 0;
    const auto tree = QJsonDocument::fromJson(
                          get(QStringLiteral("/v1/snapshots/%1/%2/tree")
                                  .arg(m_entry.repoId, head),
                              true, &status))
                          .array();
    QCOMPARE(status, 200);
    QString osuBlob;
    for (const auto& e : tree)
        if (e.toObject().value("path").toString().endsWith(".osu"))
            osuBlob = e.toObject().value("oid").toString();
    QVERIFY(!osuBlob.isEmpty());

    const auto blob = get(QStringLiteral("/v1/blobs/%1/%2").arg(m_entry.repoId, osuBlob), true,
                          &status);
    QCOMPARE(status, 200);
    QVERIFY(blob.startsWith("osu file format v14"));

    const auto diff = QJsonDocument::fromJson(
                          get(QStringLiteral("/v1/diff/%1?from=%2&to=%3")
                                  .arg(m_entry.repoId, parent, head),
                              true, &status))
                          .object();
    QCOMPARE(status, 200);
    const auto files = diff.value("files").toArray();
    QCOMPARE(files.size(), 1);
    const auto semantic = files[0].toObject().value("semantic").toObject();
    QCOMPARE(semantic.value("notes").toArray().size(), 1);

    const auto map = QJsonDocument::fromJson(
                         get(QStringLiteral("/v1/map/%1/%2").arg(m_entry.repoId, osuBlob), true,
                             &status))
                         .object();
    QCOMPARE(status, 200);
    QCOMPARE(map.value("keyCount").toInt(), 7);
    QCOMPARE(map.value("notes").toArray().size(), 2);
}

void TestServer::corsHeaders()
{
    int status = 0;
    get("/v1/mapsets", true, &status, QStringLiteral("https://ryoon.moe"));
    QCOMPARE(status, 200); // helper downgrades to -1 when the CORS header is missing
    get("/v1/mapsets", true, &status, QStringLiteral("https://evil.example"));
    QCOMPARE(status, -1); // no CORS header for a foreign origin
}

void TestServer::preflightGrantsCorsAndPna()
{
    // A browser on https://ryoon.moe reaching http://127.0.0.1 sends a CORS +
    // Private-Network-Access preflight; without these headers the real request
    // is blocked (this is why curl worked but the site couldn't pair).
    QNetworkRequest req(QUrl(QStringLiteral("http://127.0.0.1:%1/v1/mapsets").arg(m_cfg.serverPort)));
    req.setRawHeader("Origin", "https://ryoon.moe");
    req.setRawHeader("Access-Control-Request-Method", "GET");
    req.setRawHeader("Access-Control-Request-Headers", "authorization");
    req.setRawHeader("Access-Control-Request-Private-Network", "true");
    QNetworkReply* reply = m_nam.sendCustomRequest(req, "OPTIONS");
    QSignalSpy done(reply, &QNetworkReply::finished);
    QVERIFY(done.wait(15000));
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 204);
    QCOMPARE(reply->rawHeader("Access-Control-Allow-Origin"), QByteArray("https://ryoon.moe"));
    QCOMPARE(reply->rawHeader("Access-Control-Allow-Private-Network"), QByteArray("true"));
    QVERIFY(reply->rawHeader("Access-Control-Allow-Headers").contains("Authorization"));
    reply->deleteLater();

    // Foreign origin gets no grant.
    QNetworkRequest bad(QUrl(QStringLiteral("http://127.0.0.1:%1/v1/mapsets").arg(m_cfg.serverPort)));
    bad.setRawHeader("Origin", "https://evil.example");
    QNetworkReply* r2 = m_nam.sendCustomRequest(bad, "OPTIONS");
    QSignalSpy d2(r2, &QNetworkReply::finished);
    QVERIFY(d2.wait(15000));
    QVERIFY(r2->rawHeader("Access-Control-Allow-Origin").isEmpty());
    r2->deleteLater();
}

void TestServer::restoreConfirmerGate()
{
    const auto arr = QJsonDocument::fromJson(m_snapshots).array();
    const QString import = arr[1].toObject().value("oid").toString();

    int status = 0;
    // No confirmer registered → not found/declined path.
    post(QStringLiteral("/v1/restore/%1/%2").arg(m_entry.repoId, import), true, &status);
    QCOMPARE(status, 404);

    m_server->setRestoreConfirmer([](QString, QString) {
        return QtFuture::makeReadyValueFuture(false); // decline
    });
    post(QStringLiteral("/v1/restore/%1/%2").arg(m_entry.repoId, import), true, &status);
    QCOMPARE(status, 403);

    m_server->setRestoreConfirmer([](QString, QString) {
        return QtFuture::makeReadyValueFuture(true); // approve
    });
    const auto body = post(QStringLiteral("/v1/restore/%1/%2").arg(m_entry.repoId, import), true,
                           &status);
    QCOMPARE(status, 200);
    QVERIFY(QJsonDocument::fromJson(body).object().value("restored").toBool());
    QVERIFY(QFile::exists(m_entry.songsPath + "/T - A (C) [VA].osu"));
}

void TestServer::snapshotLabelFlow()
{
    int status = 0;
    const auto snaps = QJsonDocument::fromJson(
        get(QStringLiteral("/v1/mapsets/%1/snapshots").arg(m_entry.repoId), true, &status)).array();
    QCOMPARE(status, 200);
    QVERIFY(!snaps.isEmpty());
    const QString oid = snaps[0].toObject().value("oid").toString();

    // Set a label — trimmed and echoed back.
    const QByteArray set =
        QJsonDocument(QJsonObject{{QStringLiteral("label"), QStringLiteral("  kiai redone  ")}})
            .toJson(QJsonDocument::Compact);
    const auto body = QJsonDocument::fromJson(
        post(QStringLiteral("/v1/snapshots/%1/%2/label").arg(m_entry.repoId, oid), true, &status,
             set)).object();
    QCOMPARE(status, 200);
    QVERIFY(body.value("labeled").toBool());
    QCOMPARE(body.value("label").toString(), QStringLiteral("kiai redone"));

    // It surfaces on the snapshot list.
    const auto after = QJsonDocument::fromJson(
        get(QStringLiteral("/v1/mapsets/%1/snapshots").arg(m_entry.repoId), true, &status)).array();
    bool found = false;
    for (const auto& s : after)
        if (s.toObject().value("oid").toString() == oid) {
            QCOMPARE(s.toObject().value("label").toString(), QStringLiteral("kiai redone"));
            found = true;
        }
    QVERIFY(found);

    // Empty label clears it.
    const QByteArray clear =
        QJsonDocument(QJsonObject{{QStringLiteral("label"), QString()}}).toJson(QJsonDocument::Compact);
    const auto cleared = QJsonDocument::fromJson(
        post(QStringLiteral("/v1/snapshots/%1/%2/label").arg(m_entry.repoId, oid), true, &status,
             clear)).object();
    QCOMPARE(status, 200);
    QVERIFY(cleared.value("label").toString().isEmpty());

    // Unknown snapshot → 404; missing token → 401.
    post(QStringLiteral("/v1/snapshots/%1/%2/label")
             .arg(m_entry.repoId, QStringLiteral("deadbeefdeadbeefdeadbeefdeadbeefdeadbeef")),
         true, &status, set);
    QCOMPARE(status, 404);
    post(QStringLiteral("/v1/snapshots/%1/%2/label").arg(m_entry.repoId, oid), false, &status, set);
    QCOMPARE(status, 401);
}

void TestServer::mergeResolveFlow()
{
    // End-to-end web-resolver contract: desktop parks a conflicted merge, the
    // site lists it, reads the conflict, and POSTs a *choice* — never file
    // content. The desktop re-merges natively and is the only writer of Songs.
    QTemporaryDir theirSongs;
    const QString theirMapset = theirSongs.path() + "/collab";
    QVERIFY(writeFile(theirMapset + "/T - A (C) [VA].osu", osuFixture()));
    QVERIFY(writeFile(theirMapset + "/audio.mp3", QByteArray(1000, 'x')));
    QString err;
    const auto theirs = trackMapset(theirMapset, &err);
    QVERIFY2(theirs.has_value(), qPrintable(err));
    QTest::qWait(15);
    QByteArray theirEdit = osuFixture();
    theirEdit.replace("OverallDifficulty:8", "OverallDifficulty:6");
    QVERIFY(writeFile(theirMapset + "/T - A (C) [VA].osu", theirEdit));
    QVERIFY(snapshotMapset(*theirs, "manual", {}, &err).has_value());
    const QString bundle = theirSongs.path() + "/theirs.ovcz";
    QVERIFY(exportBundle(*theirs, bundle, false, &err));

    // Ours changes the same field differently, on disk.
    const QString osu = m_entry.songsPath + "/T - A (C) [VA].osu";
    QByteArray ourEdit = osuFixture();
    ourEdit.replace("OverallDifficulty:8", "OverallDifficulty:9");
    QVERIFY(writeFile(osu, ourEdit));

    // Desktop prepares the merge and parks it pending (writes nothing yet).
    const auto prepared = prepareBundleMerge(m_entry, bundle, &err);
    QVERIFY2(prepared.has_value(), qPrintable(err));
    QVERIFY(prepared->hasConflicts());
    m_merges.put(*prepared);

    // Site lists pending merges.
    int status = 0;
    const auto list = QJsonDocument::fromJson(get("/v1/merges", true, &status)).array();
    QCOMPARE(status, 200);
    QCOMPARE(list.size(), 1);
    QCOMPARE(list[0].toObject().value("repoId").toString(), m_entry.repoId);
    QCOMPARE(list[0].toObject().value("totalConflicts").toInt(), 1);

    // Site fetches conflict detail, including the stable id it echoes back.
    const auto detail = QJsonDocument::fromJson(
        get(QStringLiteral("/v1/merges/%1").arg(m_entry.repoId), true, &status)).object();
    QCOMPARE(status, 200);
    const auto files = detail.value("files").toArray();
    QCOMPARE(files.size(), 1);
    const auto file0 = files[0].toObject();
    const auto conflicts = file0.value("conflicts").toArray();
    QCOMPARE(conflicts.size(), 1);
    const QString cid = conflicts[0].toObject().value("id").toString();
    QCOMPARE(cid, QStringLiteral("kv:Difficulty:OverallDifficulty"));

    // Detail carries per-file mode + the full .osu text per side, so the web
    // resolver can render the notes spatially (parse client-side).
    QCOMPARE(file0.value("mode").toInt(), 3);      // mania
    QCOMPARE(file0.value("keyCount").toInt(), 7);
    QVERIFY(file0.value("oursText").toString().contains("OverallDifficulty:9"));   // ours' live edit
    QVERIFY(file0.value("theirsText").toString().contains("OverallDifficulty:6")); // theirs
    // The list route stays light — no texts there.
    QVERIFY(!list[0].toObject().value("files").toArray()[0].toObject().contains("oursText"));

    // Site resolves it as "theirs". Resolutions are per-file: { relPath: { id: side } }.
    const QString relPath = file0.value("relPath").toString();
    const QByteArray body =
        QJsonDocument(QJsonObject{{QStringLiteral("resolutions"),
                                   QJsonObject{{relPath, QJsonObject{{cid, QStringLiteral("theirs")}}}}}})
            .toJson(QJsonDocument::Compact);
    const auto applied = QJsonDocument::fromJson(
        post(QStringLiteral("/v1/merges/%1/resolve").arg(m_entry.repoId), true, &status, body))
        .object();
    QCOMPARE(status, 200);
    QVERIFY(applied.value("applied").toBool());
    QVERIFY(!applied.value("snapshotOid").toString().isEmpty());

    // Session consumed; the choice landed on disk as theirs' OD 6.
    QTRY_COMPARE(m_merges.count(), 0); // removal is marshalled to the main thread
    QByteArray after;
    {
        QFile f(osu);
        QVERIFY(f.open(QIODevice::ReadOnly));
        after = f.readAll();
    }
    QVERIFY(after.contains("OverallDifficulty:6"));
    QVERIFY(!after.contains("OverallDifficulty:9"));
}

QTEST_GUILESS_MAIN(TestServer)
#include "test_server.moc"
