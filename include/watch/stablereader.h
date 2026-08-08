#pragma once
#include <watch/osustate.h>
#include <watch/processhandle.h>
#include <QHash>

namespace ovc::watch {

struct MemBeatmap {
    QString md5;
    QString folder;
    QString filename;
    QString artist;
    QString title;
    QString artistUnicode; // TitleUnicode/ArtistUnicode ('' when absent)
    QString titleUnicode;
    QString version;
    QString osuPath; // host filesystem path to the .osu
    QString songsDir;
};

// Required: the currently selected beatmap + game status + songs folder.
class StableReader {
public:
    explicit StableReader(ProcessHandle& proc) : m_proc(proc) {}

    bool resolve();
    bool valid() const;

    GameState status() const;
    // Cheap per-tick probe: just the md5 string of the selected map.
    QString currentMd5() const;
    bool readBeatmap(MemBeatmap& out) const;
    // Audio clock in ms — the editor's position while status() == Edit.
    // -1 when the playTime signature didn't resolve.
    int editorTimeMs() const;

private:
    uintptr_t p(uintptr_t a) const { return m_proc.readUInt(a); }
    uintptr_t pat(const char* name) const { return m_addr.value(QString::fromLatin1(name), 0); }
    QString songsFolder() const;

    ProcessHandle& m_proc;
    QHash<QString, uintptr_t> m_addr;
};

} // namespace ovc::watch
