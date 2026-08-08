#pragma once
#include <QString>

namespace ovc::git {

struct MirrorStats {
    int copied = 0;      // files (re)copied into the workdir
    int deleted = 0;     // workdir files removed (gone from source)
    int statSkipped = 0; // unchanged per manifest — not copied, not re-read
};

// One-way sync Songs/<mapset>/ → repo working tree. Reserved repo files
// (.git/, .ovc/, .gitattributes) are never touched; junk (*.tmp, ~*,
// desktop.ini, thumbs.db) is never copied. A stat manifest at
// .git/ovc/manifest.json (size + mtime per file) makes unchanged media a no-op
// — the 8 MB mp3 is not re-read on every save.
bool mirrorIntoRepo(const QString& songsDir, const QString& repoDir, MirrorStats* stats,
                    QString* err);

} // namespace ovc::git
