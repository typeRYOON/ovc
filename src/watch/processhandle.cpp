#include <watch/processhandle.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#else
#include <sys/uio.h>
#include <unistd.h>
#include <cerrno>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#endif

namespace ovc::watch {

#ifdef _WIN32

ProcessHandle::~ProcessHandle()
{
    close();
}

std::vector<uint32_t> ProcessHandle::findProcesses(const QStringList& names)
{
    std::vector<uint32_t> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
        do {
            const QString exe = QString::fromWCharArray(entry.szExeFile);
            for (const QString& name : names) {
                if (exe.compare(name, Qt::CaseInsensitive) == 0) {
                    out.push_back(entry.th32ProcessID);
                    break;
                }
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return out;
}

bool ProcessHandle::open(uint32_t pid)
{
    close();
    HANDLE h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) return false;
    m_handle = h;
    m_pid = pid;
    m_open = true;

    BOOL wow64 = FALSE;
    IsWow64Process(h, &wow64);
    m_is64 = (wow64 == FALSE);
    return true;
}

void ProcessHandle::close()
{
    if (m_handle) {
        CloseHandle(static_cast<HANDLE>(m_handle));
        m_handle = nullptr;
    }
    m_pid = 0;
    m_open = false;
}

bool ProcessHandle::isAlive() const
{
    if (!m_handle) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(static_cast<HANDLE>(m_handle), &code)) return false;
    return code == STILL_ACTIVE;
}

QString ProcessHandle::imagePath() const
{
    if (!m_handle) return {};
    wchar_t buf[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameExW(static_cast<HANDLE>(m_handle), nullptr, buf, MAX_PATH);
    return n ? QString::fromWCharArray(buf, n) : QString();
}

bool ProcessHandle::readBytes(uintptr_t addr, void* out, size_t size) const
{
    if (!m_handle || !addr) return false;
    SIZE_T read = 0;
    const BOOL ok = ReadProcessMemory(static_cast<HANDLE>(m_handle),
                                      reinterpret_cast<LPCVOID>(addr), out, size, &read);
    return ok && read == size;
}

std::vector<MemRegion> ProcessHandle::queryRegions() const
{
    std::vector<MemRegion> out;
    if (!m_handle) return out;

    MEMORY_BASIC_INFORMATION info;
    uintptr_t addr = 0;
    while (VirtualQueryEx(static_cast<HANDLE>(m_handle), reinterpret_cast<LPCVOID>(addr), &info,
                          sizeof(info)) == sizeof(info)) {
        const uintptr_t regionBase = reinterpret_cast<uintptr_t>(info.BaseAddress);
        const size_t regionSize = info.RegionSize;

        // tosu's scannable set: committed RW (objects) + RWX (JITted .NET code).
        const DWORD prot = info.Protect;
        const bool scannable = (info.State & MEM_COMMIT) &&
                               (prot & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE));
        if (scannable && regionSize > 0) out.push_back({regionBase, regionSize});

        const uintptr_t next = regionBase + regionSize;
        if (next <= addr) break;
        addr = next;
    }
    return out;
}

QString ProcessHandle::winePrefix() const
{
    return {};
}

QString windowsPathToHost(const QString& winPath, const QString& winePrefix)
{
    Q_UNUSED(winePrefix);
    return winPath;
}

#else // Linux (osu! stable under wine)

ProcessHandle::~ProcessHandle()
{
    close();
}

std::vector<uint32_t> ProcessHandle::findProcesses(const QStringList& names)
{
    std::vector<uint32_t> out;
    QDirIterator it("/proc", QDir::Dirs | QDir::NoDotAndDotDot);
    while (it.hasNext()) {
        const QString dir = it.next();
        bool numeric = false;
        const uint32_t pid = it.fileName().toUInt(&numeric);
        if (!numeric) continue;
        QFile comm(dir + "/comm");
        if (!comm.open(QIODevice::ReadOnly)) continue;
        const QString name = QString::fromUtf8(comm.readAll()).trimmed();
        for (const QString& want : names) {
            // /proc comm is truncated to 15 chars.
            if (name.compare(want.left(15), Qt::CaseInsensitive) == 0) {
                out.push_back(pid);
                break;
            }
        }
    }
    return out;
}

bool ProcessHandle::open(uint32_t pid)
{
    close();
    if (!QFile::exists(QStringLiteral("/proc/%1").arg(pid))) return false;
    m_pid = pid;
    m_open = true;
    m_is64 = false; // stable under wine is 32-bit; lazer is out of scope
    m_readDenied = false;
    return true;
}

void ProcessHandle::close()
{
    m_pid = 0;
    m_open = false;
}

bool ProcessHandle::isAlive() const
{
    return m_open && QFile::exists(QStringLiteral("/proc/%1").arg(m_pid));
}

QString ProcessHandle::imagePath() const
{
    if (!m_open) return {};
    // wine runs osu! via wine-preloader; the Windows exe path is argv[0/1].
    QFile cmdline(QStringLiteral("/proc/%1/cmdline").arg(m_pid));
    if (cmdline.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> args = cmdline.readAll().split('\0');
        for (const QByteArray& a : args) {
            const QString s = QString::fromUtf8(a);
            if (s.endsWith("osu!.exe", Qt::CaseInsensitive)) return s;
        }
    }
    return {};
}

bool ProcessHandle::readBytes(uintptr_t addr, void* out, size_t size) const
{
    if (!m_open || !addr) return false;
    iovec local{out, size};
    iovec remote{reinterpret_cast<void*>(addr), size};
    const ssize_t got = process_vm_readv(static_cast<pid_t>(m_pid), &local, 1, &remote, 1, 0);
    if (got < 0 && (errno == EPERM || errno == EACCES)) m_readDenied = true;
    return got == static_cast<ssize_t>(size);
}

std::vector<MemRegion> ProcessHandle::queryRegions() const
{
    std::vector<MemRegion> out;
    QFile maps(QStringLiteral("/proc/%1/maps").arg(m_pid));
    if (!maps.open(QIODevice::ReadOnly)) return out;
    // Read via readAll(), not an atEnd()/readLine() loop: /proc files report
    // st_size == 0, so QFile::atEnd() (size-based) reports EOF immediately and
    // the loop never runs — 0 regions, the whole Linux scan silently empty.
    // readAll()'s chunked fallback handles unknown-size files correctly.
    const QList<QByteArray> lines = maps.readAll().split('\n');
    for (const QByteArray& line : lines) {
        const int sp = line.indexOf(' ');
        if (sp < 0 || line.size() < sp + 3) continue;
        // Any readable region. The .NET JIT pages the signatures live in are
        // r-x (not rw) on some wine builds, so an rw-only filter never finds
        // them — cosutrainer scans all r* for the same reason.
        if (line[sp + 1] != 'r') continue;
        const int dash = line.indexOf('-');
        if (dash < 0 || dash > sp) continue;
        bool ok1 = false, ok2 = false;
        const uintptr_t lo = line.left(dash).toULongLong(&ok1, 16);
        const uintptr_t hi = line.mid(dash + 1, sp - dash - 1).toULongLong(&ok2, 16);
        if (ok1 && ok2 && hi > lo) out.push_back({lo, hi - lo});
    }
    return out;
}

QString ProcessHandle::winePrefix() const
{
    if (!m_open) return {};
    QFile environ(QStringLiteral("/proc/%1/environ").arg(m_pid));
    if (!environ.open(QIODevice::ReadOnly)) return {};
    const QList<QByteArray> vars = environ.readAll().split('\0');
    for (const QByteArray& v : vars) {
        if (v.startsWith("WINEPREFIX=")) return QString::fromUtf8(v.mid(11));
    }
    return {};
}

QString windowsPathToHost(const QString& winPath, const QString& winePrefix)
{
    QString p = winPath;
    p.replace('\\', '/');
    if (p.size() >= 2 && p[1] == ':') {
        const QChar drive = p[0].toLower();
        QString rest = p.mid(2); // keeps the leading '/'

        QString prefix = winePrefix;
        if (prefix.isEmpty()) {
            prefix = QProcessEnvironment::systemEnvironment().value(
                "WINEPREFIX", QDir::homePath() + "/.wine");
        }
        // Wine maps every drive letter through <prefix>/dosdevices/<letter>: —
        // a symlink to the true host location (osu-wine points d: at the
        // install folder, the default z: at /). Resolve that symlink; the
        // literal <prefix>/drive_<letter> layout is a last-ditch fallback and
        // only ever exists for c:.
        const QString dosdevice = prefix + "/dosdevices/" + drive + ':';
        QString base = QFileInfo(dosdevice).canonicalFilePath();
        if (base.isEmpty()) base = QFileInfo(dosdevice).symLinkTarget();
        if (base.isEmpty()) base = (drive == 'z') ? QString() : prefix + "/drive_" + drive;
        if (base.endsWith('/')) base.chop(1); // avoid // when base resolves to "/"
        return base + rest;
    }
    return p;
}

#endif

uintptr_t ProcessHandle::readIntPtr(uintptr_t a) const
{
    if (m_is64) return static_cast<uintptr_t>(read<uint64_t>(a));
    return static_cast<uintptr_t>(read<uint32_t>(a));
}

QString ProcessHandle::readCsharpString(uintptr_t a) const
{
    if (!a) return {};
    const uintptr_t skip = m_is64 ? 8 : 4; // MethodTable pointer width
    const int32_t length = read<int32_t>(a + skip);
    if (length <= 0 || length >= 4096) return {};

    std::vector<char16_t> buf(static_cast<size_t>(length));
    if (!readBytes(a + skip + 4, buf.data(), static_cast<size_t>(length) * 2)) return {};
    return QString::fromUtf16(buf.data(), length);
}

} // namespace ovc::watch
