#include <git/shadowrepo.h>
#include "gitraii.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace ovc::git {

namespace {

const char kGitattributes[] = "*.osu merge=osu -text\n* -text\n";

bool oidFromHex(const QByteArray& hex, git_oid* out)
{
    return !hex.isEmpty() && git_oid_fromstr(out, hex.constData()) == 0;
}

QByteArray oidToHex(const git_oid* oid)
{
    char buf[GIT_OID_MAX_HEXSIZE + 1] = {0};
    git_oid_tostr(buf, sizeof(buf), oid);
    return QByteArray(buf);
}

void setErr(QString* err)
{
    if (err) *err = lastGitError();
}

// Message tail after a blank line, as "Key: value" trailer lines.
QMap<QString, QString> parseTrailers(const QString& message)
{
    QMap<QString, QString> out;
    const qsizetype split = message.lastIndexOf(QStringLiteral("\n\n"));
    if (split < 0) return out;
    const QStringList lines = message.mid(split + 2).split('\n', Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        const qsizetype colon = line.indexOf(QStringLiteral(": "));
        if (colon <= 0) continue;
        const QString key = line.left(colon);
        if (key.contains(' ')) continue;
        out.insert(key, line.mid(colon + 2));
    }
    return out;
}

// Resolve a hex oid (commit or tree) to a tree.
int lookupTree(git_repository* repo, const QByteArray& hex, git_tree** out)
{
    git_oid oid;
    if (!oidFromHex(hex, &oid)) return GIT_ENOTFOUND;
    Ptr<git_object, git_object_free> obj;
    if (int rc = git_object_lookup(obj.out(), repo, &oid, GIT_OBJECT_ANY); rc != 0) return rc;
    Ptr<git_object, git_object_free> peeled;
    if (int rc = git_object_peel(peeled.out(), obj, GIT_OBJECT_TREE); rc != 0) return rc;
    *out = reinterpret_cast<git_tree*>(peeled.release()); // ownership moves to caller
    return 0;
}

} // namespace

ShadowRepo::ShadowRepo(ShadowRepo&& o) noexcept : m_repo(o.m_repo), m_dir(std::move(o.m_dir))
{
    o.m_repo = nullptr;
}

ShadowRepo& ShadowRepo::operator=(ShadowRepo&& o) noexcept
{
    if (this != &o) {
        if (m_repo) git_repository_free(m_repo);
        m_repo = o.m_repo;
        m_dir = std::move(o.m_dir);
        o.m_repo = nullptr;
    }
    return *this;
}

ShadowRepo::~ShadowRepo()
{
    if (m_repo) git_repository_free(m_repo);
}

bool ShadowRepo::create(const QString& dir, QString* err)
{
    git_repository_init_options opts = GIT_REPOSITORY_INIT_OPTIONS_INIT;
    opts.flags = GIT_REPOSITORY_INIT_MKPATH;
    opts.initial_head = "main";

    RepoPtr repo;
    if (git_repository_init_ext(repo.out(), QDir::toNativeSeparators(dir).toUtf8(), &opts) != 0) {
        setErr(err);
        return false;
    }
    QFile attrs(dir + QStringLiteral("/.gitattributes"));
    if (!attrs.open(QIODevice::WriteOnly) || attrs.write(kGitattributes) < 0) {
        if (err) *err = QStringLiteral("cannot write .gitattributes");
        return false;
    }
    return true;
}

std::optional<ShadowRepo> ShadowRepo::open(const QString& dir)
{
    ShadowRepo r;
    if (git_repository_open(&r.m_repo, QDir::toNativeSeparators(dir).toUtf8()) != 0)
        return std::nullopt;
    r.m_dir = dir;
    return r;
}

std::optional<QByteArray> ShadowRepo::stageAll(QString* err)
{
    IndexPtr index;
    if (git_repository_index(index.out(), m_repo) != 0) {
        setErr(err);
        return std::nullopt;
    }
    // add_all picks up new files, update_all re-stats tracked ones and drops
    // deletions; libgit2's stat cache means unchanged files are not re-hashed.
    git_strarray all{nullptr, 0};
    if (git_index_add_all(index, &all, GIT_INDEX_ADD_DEFAULT, nullptr, nullptr) != 0 ||
        git_index_update_all(index, &all, nullptr, nullptr) != 0) {
        setErr(err);
        return std::nullopt;
    }
    git_oid treeOid;
    if (git_index_write_tree(&treeOid, index) != 0 || git_index_write(index) != 0) {
        setErr(err);
        return std::nullopt;
    }
    const QByteArray hex = oidToHex(&treeOid);
    if (hex == headTreeOid()) return std::nullopt; // nothing changed
    return hex;
}

QByteArray ShadowRepo::commitStaged(const QByteArray& treeOid, const QString& subject,
                                    const QMap<QString, QString>& trailers, QString* err,
                                    const QDateTime& when)
{
    git_oid tree;
    if (!oidFromHex(treeOid, &tree)) {
        if (err) *err = QStringLiteral("bad tree oid");
        return {};
    }
    TreePtr treeObj;
    if (git_tree_lookup(treeObj.out(), m_repo, &tree) != 0) {
        setErr(err);
        return {};
    }

    QString message = subject;
    if (!trailers.isEmpty()) {
        message += QStringLiteral("\n\n");
        for (auto it = trailers.constBegin(); it != trailers.constEnd(); ++it)
            message += it.key() + QStringLiteral(": ") + it.value() + QLatin1Char('\n');
    }

    SigPtr sig;
    const int sigRc = when.isValid()
                          ? git_signature_new(sig.out(), "ovc", "ovc@local",
                                              when.toSecsSinceEpoch(), 0)
                          : git_signature_now(sig.out(), "ovc", "ovc@local");
    if (sigRc != 0) {
        setErr(err);
        return {};
    }

    CommitPtr parent;
    git_oid parentOid;
    const git_commit* parents[1] = {nullptr};
    int parentCount = 0;
    if (git_reference_name_to_id(&parentOid, m_repo, "HEAD") == 0 &&
        git_commit_lookup(parent.out(), m_repo, &parentOid) == 0) {
        parents[0] = parent.get();
        parentCount = 1;
    }

    git_oid commitOid;
    if (git_commit_create(&commitOid, m_repo, "HEAD", sig, sig, nullptr,
                          message.toUtf8().constData(), treeObj, parentCount, parents) != 0) {
        setErr(err);
        return {};
    }
    return oidToHex(&commitOid);
}

std::optional<QByteArray> ShadowRepo::commitAll(const QString& subject,
                                                const QMap<QString, QString>& trailers,
                                                QString* err)
{
    const auto tree = stageAll(err);
    if (!tree) return std::nullopt;
    const QByteArray oid = commitStaged(*tree, subject, trailers, err);
    if (oid.isEmpty()) return std::nullopt;
    return oid;
}

QByteArray ShadowRepo::headOid() const
{
    git_oid oid;
    if (git_reference_name_to_id(&oid, m_repo, "HEAD") != 0) return {};
    return oidToHex(&oid);
}

QByteArray ShadowRepo::headTreeOid() const
{
    git_oid oid;
    if (git_reference_name_to_id(&oid, m_repo, "HEAD") != 0) return {};
    CommitPtr commit;
    if (git_commit_lookup(commit.out(), m_repo, &oid) != 0) return {};
    return oidToHex(git_commit_tree_id(commit));
}

QList<ShadowRepo::CommitInfo> ShadowRepo::log(int limit) const
{
    QList<CommitInfo> out;
    Ptr<git_revwalk, git_revwalk_free> walk;
    if (git_revwalk_new(walk.out(), m_repo) != 0) return out;
    git_revwalk_simplify_first_parent(walk);
    git_revwalk_sorting(walk, GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME);
    if (git_revwalk_push_head(walk) != 0) return out; // unborn: empty history

    const QMap<QByteArray, QString> labelMap = labels();
    git_oid oid;
    while (out.size() < limit && git_revwalk_next(&oid, walk) == 0) {
        CommitPtr commit;
        if (git_commit_lookup(commit.out(), m_repo, &oid) != 0) continue;
        CommitInfo info;
        info.oid = oidToHex(&oid);
        if (git_commit_parentcount(commit) > 0)
            info.parentOid = oidToHex(git_commit_parent_id(commit, 0));
        info.when = QDateTime::fromSecsSinceEpoch(git_commit_time(commit));
        const QString message = QString::fromUtf8(git_commit_message(commit));
        info.subject = message.section('\n', 0, 0);
        info.label = labelMap.value(info.oid);
        info.trailers = parseTrailers(message);
        out.append(info);
    }
    return out;
}

std::optional<ShadowRepo::CommitInfo> ShadowRepo::commitInfo(const QByteArray& commitOid) const
{
    git_oid oid;
    if (!oidFromHex(commitOid, &oid)) return std::nullopt;
    CommitPtr commit;
    if (git_commit_lookup(commit.out(), m_repo, &oid) != 0) return std::nullopt;
    CommitInfo info;
    info.oid = commitOid;
    if (git_commit_parentcount(commit) > 0)
        info.parentOid = oidToHex(git_commit_parent_id(commit, 0));
    info.when = QDateTime::fromSecsSinceEpoch(git_commit_time(commit));
    const QString message = QString::fromUtf8(git_commit_message(commit));
    info.subject = message.section('\n', 0, 0);
    info.label = labelFor(commitOid);
    info.trailers = parseTrailers(message);
    return info;
}

QMap<QByteArray, QString> ShadowRepo::labels() const
{
    QMap<QByteArray, QString> out;
    QFile f(m_dir + QStringLiteral("/.git/ovc/labels.json"));
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    for (auto it = o.constBegin(); it != o.constEnd(); ++it)
        out.insert(it.key().toUtf8(), it.value().toString());
    return out;
}

QString ShadowRepo::labelFor(const QByteArray& commitOid) const
{
    return labels().value(commitOid);
}

bool ShadowRepo::setLabel(const QByteArray& commitOid, const QString& name, QString* err)
{
    QMap<QByteArray, QString> map = labels();
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        map.remove(commitOid);
    else
        map.insert(commitOid, trimmed);

    QJsonObject o;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it)
        o.insert(QString::fromUtf8(it.key()), it.value());

    QDir().mkpath(m_dir + QStringLiteral("/.git/ovc"));
    QFile f(m_dir + QStringLiteral("/.git/ovc/labels.json"));
    if (!f.open(QIODevice::WriteOnly)) {
        if (err) *err = QStringLiteral("cannot write labels.json");
        return false;
    }
    f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    return true;
}

QList<QPair<QString, QByteArray>> ShadowRepo::listTree(const QByteArray& oid) const
{
    QList<QPair<QString, QByteArray>> out;
    if (oid.isEmpty()) return out;
    git_tree* rawTree = nullptr;
    if (lookupTree(m_repo, oid, &rawTree) != 0) return out;
    TreePtr tree;
    *tree.out() = rawTree;

    struct Ctx {
        QList<QPair<QString, QByteArray>>* out;
    } ctx{&out};
    git_tree_walk(
        tree, GIT_TREEWALK_PRE,
        [](const char* root, const git_tree_entry* entry, void* payload) -> int {
            if (git_tree_entry_type(entry) == GIT_OBJECT_BLOB) {
                auto* c = static_cast<Ctx*>(payload);
                c->out->append({QString::fromUtf8(root) + QString::fromUtf8(git_tree_entry_name(entry)),
                                oidToHex(git_tree_entry_id(entry))});
            }
            return 0;
        },
        &ctx);
    return out;
}

QByteArray ShadowRepo::readBlob(const QByteArray& blobOid) const
{
    git_oid oid;
    if (!oidFromHex(blobOid, &oid)) return {};
    Ptr<git_blob, git_blob_free> blob;
    if (git_blob_lookup(blob.out(), m_repo, &oid) != 0) return {};
    return QByteArray(static_cast<const char*>(git_blob_rawcontent(blob)),
                      static_cast<qsizetype>(git_blob_rawsize(blob)));
}

qint64 ShadowRepo::blobSize(const QByteArray& blobOid) const
{
    git_oid oid;
    if (!oidFromHex(blobOid, &oid)) return 0;
    Ptr<git_blob, git_blob_free> blob;
    if (git_blob_lookup(blob.out(), m_repo, &oid) != 0) return 0;
    return static_cast<qint64>(git_blob_rawsize(blob));
}

QByteArray ShadowRepo::hashBlob(const QByteArray& bytes)
{
    git_oid oid;
    if (git_odb_hash(&oid, bytes.constData(), size_t(bytes.size()), GIT_OBJECT_BLOB) != 0) return {};
    return oidToHex(&oid);
}

bool ShadowRepo::checkoutTree(const QByteArray& commitOid, QString* err)
{
    git_oid oid;
    if (!oidFromHex(commitOid, &oid)) {
        if (err) *err = QStringLiteral("bad oid");
        return false;
    }
    Ptr<git_object, git_object_free> obj;
    if (git_object_lookup(obj.out(), m_repo, &oid, GIT_OBJECT_ANY) != 0) {
        setErr(err);
        return false;
    }
    git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
    opts.checkout_strategy = GIT_CHECKOUT_FORCE | GIT_CHECKOUT_REMOVE_UNTRACKED;
    if (git_checkout_tree(m_repo, obj, &opts) != 0) {
        setErr(err);
        return false;
    }
    return true;
}

} // namespace ovc::git
