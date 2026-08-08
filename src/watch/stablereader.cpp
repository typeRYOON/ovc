#include <watch/stablereader.h>
#include <watch/memoryscan.h>
#include <QDir>
#include <QFileInfo>

namespace ovc::watch {

namespace {

bool isAbsoluteAnyOs(const QString& s)
{
    return s.startsWith('/') || (s.size() >= 2 && s[1] == ':');
}

} // namespace

bool StableReader::resolve()
{
    m_proc.setBitness(false); // stable is always 32-bit

    // Patterns verbatim from tosu's stable.ts scanPatterns.
    QHash<QString, Signature> sigs;
    sigs.insert("baseAddr", parseSignature("F8 01 74 04 83 65"));
    sigs.insert("statusPtr", parseSignature("48 83 F8 04 73 1E", -0x4));
    sigs.insert("settingsClassAddr", parseSignature("83 E0 20 85 C0 7E 2F"));
    sigs.insert("playTimeAddr", parseSignature("5E 5F 5D C3 A1 ?? ?? ?? ?? 89 ?? 04"));

    m_addr = batchScan(m_proc, sigs);
    return valid();
}

bool StableReader::valid() const
{
    // settingsClassAddr/playTimeAddr are best-effort (songs folder falls back
    // to "Songs"; editor time reads -1).
    return m_proc.isAlive() && pat("baseAddr") && pat("statusPtr");
}

GameState StableReader::status() const
{
    const uintptr_t sp = pat("statusPtr");
    if (!sp) return GameState::Unknown;
    return static_cast<GameState>(m_proc.readInt(p(sp)));
}

QString StableReader::currentMd5() const
{
    const uintptr_t beatmapAddr = p(p(pat("baseAddr") - 0xc));
    if (!beatmapAddr) return {};
    return m_proc.readCsharpStringPtr(beatmapAddr + 0x6c);
}

int StableReader::editorTimeMs() const
{
    const uintptr_t pt = pat("playTimeAddr");
    if (!pt) return -1;
    return m_proc.readInt(p(pt + 0x5));
}

QString StableReader::songsFolder() const
{
    const uintptr_t sc = pat("settingsClassAddr");
    if (!sc) return {};
    const uintptr_t p1 = p(sc + 0x8);
    const uintptr_t p2 = p(p1 + 0xb8);
    return m_proc.readCsharpStringPtr(p2 + 0x4);
}

bool StableReader::readBeatmap(MemBeatmap& out) const
{
    const uintptr_t beatmapAddr = p(p(pat("baseAddr") - 0xc));
    if (!beatmapAddr) return false;

    out.filename = m_proc.readCsharpStringPtr(beatmapAddr + 0x90);
    if (!out.filename.endsWith(".osu", Qt::CaseInsensitive)) return false;

    out.md5 = m_proc.readCsharpStringPtr(beatmapAddr + 0x6c);
    out.folder = m_proc.readCsharpStringPtr(beatmapAddr + 0x78);
    out.artist = m_proc.readCsharpStringPtr(beatmapAddr + 0x18);
    out.artistUnicode = m_proc.readCsharpStringPtr(beatmapAddr + 0x1c);
    out.title = m_proc.readCsharpStringPtr(beatmapAddr + 0x24);
    out.titleUnicode = m_proc.readCsharpStringPtr(beatmapAddr + 0x28);
    out.version = m_proc.readCsharpStringPtr(beatmapAddr + 0xac);
    out.mapId = m_proc.readInt(beatmapAddr + 0xc8);
    out.setId = m_proc.readInt(beatmapAddr + 0xcc);

    // Songs is stored relative to the install dir by default (literally
    // "Songs"); users can point it anywhere absolute. Path translation uses
    // the osu! process's own wine prefix (osu-winello etc. run custom ones).
    const QString prefix = m_proc.winePrefix();
    QString songs = songsFolder();
    if (songs.isEmpty()) songs = QStringLiteral("Songs");
    if (isAbsoluteAnyOs(songs)) {
        songs = windowsPathToHost(songs, prefix);
    }
    else {
        const QString osuDir =
            QFileInfo(windowsPathToHost(m_proc.imagePath(), prefix)).absolutePath();
        songs = osuDir + "/" + songs;
    }
    out.songsDir = QDir::cleanPath(songs);
    out.osuPath = QDir::cleanPath(songs + "/" + out.folder + "/" + out.filename);
    return true;
}

} // namespace ovc::watch
