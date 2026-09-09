#pragma once

#include <QByteArray>
#include <QVariant>

// Minimal Apple binary-plist (bplist00) codec — just enough for the AirPlay 2
// control channel (pyatv mirrors: dict/array/string/int/real/bool/data).
//
// Writer: QVariantMap (string keys) / QVariantList / QString / QByteArray
// (data) / integers / double / bool. Reader: the same set back (numbers come
// back as qlonglong/double, matching what we need: eventPort, error dicts).
namespace Bplist {
QByteArray encode(const QVariant &v);
QVariant decode(const QByteArray &data);
} // namespace Bplist
