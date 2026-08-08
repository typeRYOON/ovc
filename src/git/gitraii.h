#pragma once
#include <QString>
#include <git2.h>

// Private header: libgit2 types stay inside src/git so a future libgit2 v2.0
// port touches only this module.
namespace ovc::git {

template <class T, void (*Free)(T*)>
class Ptr {
public:
    Ptr() = default;
    ~Ptr() { reset(); }
    Ptr(const Ptr&) = delete;
    Ptr& operator=(const Ptr&) = delete;
    Ptr(Ptr&& o) noexcept : m_p(o.m_p) { o.m_p = nullptr; }
    Ptr& operator=(Ptr&& o) noexcept
    {
        if (this != &o) {
            reset();
            m_p = o.m_p;
            o.m_p = nullptr;
        }
        return *this;
    }

    T** out()
    {
        reset();
        return &m_p;
    }
    T* get() const { return m_p; }
    T* release()
    {
        T* p = m_p;
        m_p = nullptr;
        return p;
    }
    operator T*() const { return m_p; }
    explicit operator bool() const { return m_p != nullptr; }
    void reset()
    {
        if (m_p) {
            Free(m_p);
            m_p = nullptr;
        }
    }

private:
    T* m_p = nullptr;
};

using RepoPtr = Ptr<git_repository, git_repository_free>;
using IndexPtr = Ptr<git_index, git_index_free>;
using TreePtr = Ptr<git_tree, git_tree_free>;
using CommitPtr = Ptr<git_commit, git_commit_free>;
using DiffPtr = Ptr<git_diff, git_diff_free>;
using RefPtr = Ptr<git_reference, git_reference_free>;
using SigPtr = Ptr<git_signature, git_signature_free>;

inline QString lastGitError()
{
    const git_error* e = git_error_last();
    return e && e->message ? QString::fromUtf8(e->message) : QStringLiteral("unknown git error");
}

} // namespace ovc::git
