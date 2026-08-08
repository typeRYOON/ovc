#pragma once
#include <watch/processhandle.h>
#include <QByteArray>
#include <QHash>
#include <QString>
#include <cstdint>

namespace ovc::watch {

// Parsed AOB signature: `bytes` fixed values (wildcards zeroed), `mask` 1=fixed
// per position, `offset` added to a match address.
struct Signature {
    QByteArray bytes;
    QByteArray mask;
    int offset = 0;
};

Signature parseSignature(const QString& pattern, int offset = 0);

// One sweep over the process's scannable regions for all signatures. Returns
// name -> absolute address (+offset), 0 for not found.
QHash<QString, uintptr_t> batchScan(const ProcessHandle& proc,
                                    const QHash<QString, Signature>& sigs);

} // namespace ovc::watch
