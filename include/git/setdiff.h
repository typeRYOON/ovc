#pragma once
#include <git/shadowrepo.h>
#include <ovccore/diff.h>
#include <QString>
#include <optional>

namespace ovc::git {

enum class FileOp : quint8 { Added, Removed, Modified, Renamed };
enum class FileKind : quint8 { Difficulty, Storyboard, Audio, Image, Video, Sample, Other };

FileKind classifyPath(QStringView relPath);

struct FileChange {
    FileOp op = FileOp::Modified;
    QString relPath;
    QString oldRelPath; // set when op == Renamed
    FileKind kind = FileKind::Other;
    QByteArray oldOid, newOid; // blob hex oids; empty when absent
    qint64 oldSize = 0, newSize = 0;
    std::optional<ovc::core::BeatmapDiff> semantic; // filled for changed/renamed .osu
};

struct SetDiff {
    QList<FileChange> files;
    bool isEmpty() const { return files.isEmpty(); }
    QString subjectLine() const; // commit-subject content, e.g. "Lagrange Blossom: +12 −3 notes"
};

// Diff two commits/trees ("" = empty tree). .osu renames are matched by
// BeatmapID (peeked from blobs), falling back to identical content.
SetDiff diffTrees(const ShadowRepo& repo, const QByteArray& oidA, const QByteArray& oidB);

} // namespace ovc::git
