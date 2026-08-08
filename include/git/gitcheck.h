#pragma once
#include <QString>

namespace ovc::git {

// One per process; wraps git_libgit2_init/shutdown. Construct in main().
class LibGit {
public:
    LibGit();
    ~LibGit();
    LibGit(const LibGit&) = delete;
    LibGit& operator=(const LibGit&) = delete;
};

QString libgit2Version();

// M0 smoke: init + open a scratch repo, prove git2 and its deps really link
// and run. Returns true on success, else fills `err`.
bool selfTest(QString* err);

} // namespace ovc::git
