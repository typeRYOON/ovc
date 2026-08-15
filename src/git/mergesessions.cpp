#include <git/mergesessions.h>

namespace ovc::git {

void MergeSessionStore::put(const PreparedMerge& prepared)
{
    m_sessions.insert(prepared.repoId, prepared);
    emit pendingChanged(prepared.repoId);
}

void MergeSessionStore::remove(const QString& repoId)
{
    if (m_sessions.remove(repoId) > 0) emit pendingChanged(repoId);
}

const PreparedMerge* MergeSessionStore::get(const QString& repoId) const
{
    const auto it = m_sessions.constFind(repoId);
    return it == m_sessions.constEnd() ? nullptr : &it.value();
}

} // namespace ovc::git
