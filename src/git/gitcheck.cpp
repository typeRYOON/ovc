#include <git/gitcheck.h>
#include "gitraii.h"
#include <QDir>
#include <QTemporaryDir>
#include <git2/sys/merge.h> // git_merge_driver_register — the v0.3 semantic merge hook

namespace ovc::git {

LibGit::LibGit()
{
    git_libgit2_init();
}

LibGit::~LibGit()
{
    git_libgit2_shutdown();
}

QString libgit2Version()
{
    int major = 0, minor = 0, rev = 0;
    git_libgit2_version(&major, &minor, &rev);
    return QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(rev);
}

// Referenced (not called) so the linker proves the merge-driver API exists in
// the lib we built against.
[[maybe_unused]] static const auto kMergeDriverApi = &git_merge_driver_register;

bool selfTest(QString* err)
{
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        if (err) *err = QStringLiteral("no temp dir");
        return false;
    }
    const QByteArray path = QDir::toNativeSeparators(tmp.path()).toUtf8();

    RepoPtr repo;
    if (git_repository_init(repo.out(), path.constData(), false) != 0) {
        if (err) *err = lastGitError();
        return false;
    }
    RepoPtr reopened;
    if (git_repository_open(reopened.out(), path.constData()) != 0) {
        if (err) *err = lastGitError();
        return false;
    }
    return true;
}

} // namespace ovc::git
