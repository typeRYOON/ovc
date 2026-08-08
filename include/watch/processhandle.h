#pragma once
#include <QString>
#include <QStringList>
#include <cstdint>
#include <vector>

namespace ovc::watch {

struct MemRegion {
    uintptr_t base = 0;
    size_t size = 0;
};

// Read-only attachment to the osu! process. Windows: OpenProcess(VM_READ) +
// ReadProcessMemory + VirtualQueryEx.
// Linux: /proc discovery + process_vm_readv, for osu! stable under wine.
// Scalar read*() helpers fail soft to 0/"" so pointer walks degrade safely.
class ProcessHandle {
public:
    ProcessHandle() = default;
    ~ProcessHandle();
    ProcessHandle(const ProcessHandle&) = delete;
    ProcessHandle& operator=(const ProcessHandle&) = delete;

    static std::vector<uint32_t> findProcesses(const QStringList& names);

    bool open(uint32_t pid);
    void close();
    bool isOpen() const { return m_open; }
    bool isAlive() const;

    uint32_t pid() const { return m_pid; }
    bool is64bit() const { return m_is64; }
    void setBitness(bool is64) { m_is64 = is64; }
    // Windows path of the osu!.exe image. Under wine this is reconstructed
    // from /proc/<pid>/cmdline and translated to a unix path.
    QString imagePath() const;

    // The wine prefix the TARGET process runs under, read from its
    // /proc/<pid>/environ (authoritative — launchers like osu-winello use a
    // custom prefix, so our own environment says nothing). Empty on Windows
    // or when the process carries no WINEPREFIX.
    QString winePrefix() const;

    bool readBytes(uintptr_t addr, void* out, size_t size) const;

    // True once a memory read failed with EPERM/EACCES (Linux: yama ptrace
    // scope, or a non-dumpable target — umu/bwrap-launched wine). The scan
    // "finding nothing" and "not being allowed to look" need different fixes,
    // so callers surface this distinctly. Always false on Windows.
    bool readDenied() const { return m_readDenied; }

    int32_t readInt(uintptr_t a) const { return read<int32_t>(a); }
    uint32_t readUInt(uintptr_t a) const { return read<uint32_t>(a); }
    float readFloat(uintptr_t a) const { return read<float>(a); }

    uintptr_t readIntPtr(uintptr_t a) const;

    // .NET System.String at `a`: [MethodTable*][int length][UTF-16 chars].
    QString readCsharpString(uintptr_t a) const;
    QString readCsharpStringPtr(uintptr_t fieldAddr) const
    {
        return readCsharpString(readIntPtr(fieldAddr));
    }

    std::vector<MemRegion> queryRegions() const;

    template <class T>
    T read(uintptr_t a) const
    {
        T v{};
        readBytes(a, &v, sizeof(T));
        return v;
    }

private:
    void* m_handle = nullptr; // HANDLE on Windows
    uint32_t m_pid = 0;
    bool m_is64 = false;
    bool m_open = false;
    mutable bool m_readDenied = false;
};

// Translate a Windows path from osu!'s memory/cmdline to a host path. On
// Windows this is the identity; under wine, Z:\ -> / and C:\ -> <prefix>/drive_c.
// `winePrefix` should come from ProcessHandle::winePrefix(); when empty the
// WINEPREFIX env var (then ~/.wine) is the fallback.
QString windowsPathToHost(const QString& winPath, const QString& winePrefix = QString());

} // namespace ovc::watch
