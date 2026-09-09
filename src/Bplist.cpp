#include "Bplist.h"

#include <QtEndian>

namespace Bplist {
namespace {

void appendU16(QByteArray &out, quint16 v)
{
    quint16 be = qToBigEndian(v);
    out.append(reinterpret_cast<const char *>(&be), 2);
}
void appendU32(QByteArray &out, quint32 v)
{
    quint32 be = qToBigEndian(v);
    out.append(reinterpret_cast<const char *>(&be), 4);
}
void appendU64(QByteArray &out, quint64 v)
{
    quint64 be = qToBigEndian(v);
    out.append(reinterpret_cast<const char *>(&be), 8);
}
void appendDouble(QByteArray &out, double v)
{
    quint64 u;
    memcpy(&u, &v, 8);
    appendU64(out, u);
}

struct Writer {
    QByteArray objects; // concatenated object blobs
    QList<int> offsets; // byte offset of each object
    int refSize = 1;

    int addObject(const QByteArray &blob)
    {
        const int idx = offsets.size();
        offsets.append(objects.size());
        objects.append(blob);
        return idx;
    }
    // Marker with a possibly-long count (0xFN + int object), like Apple.
    static void appendMarker(QByteArray &out, quint8 base, quint64 count, Writer *w)
    {
        if (count < 15) {
            out.append(char(base | quint8(count)));
            return;
        }
        out.append(char(base | 0x0F));
        // Minimal int that holds count.
        QByteArray intObj;
        if (count <= 0xFF) {
            intObj.append(char(0x10));
            intObj.append(char(count));
        } else if (count <= 0xFFFF) {
            intObj.append(char(0x11));
            appendU16(intObj, quint16(count));
        } else {
            intObj.append(char(0x13));
            appendU64(intObj, count);
        }
        Q_UNUSED(w);
        out.append(intObj);
    }
    void appendRef(QByteArray &out, int idx)
    {
        if (refSize == 1) {
            out.append(char(idx & 0xFF));
        } else {
            appendU16(out, quint16(idx));
        }
    }
    int encodeAny(const QVariant &v)
    {
        switch (v.typeId()) {
        case QMetaType::QVariantMap: {
            const QVariantMap m = v.toMap();
            QByteArray blob;
            appendMarker(blob, 0xD0, quint64(m.size()), this);
            QList<int> krefs, vrefs;
            for (auto it = m.constBegin(); it != m.constEnd(); ++it) {
                krefs.append(encodeAny(it.key()));
                vrefs.append(encodeAny(it.value()));
            }
            for (int r : krefs)
                appendRef(blob, r);
            for (int r : vrefs)
                appendRef(blob, r);
            return addObject(blob);
        }
        case QMetaType::QVariantList: {
            const QVariantList l = v.toList();
            QByteArray blob;
            appendMarker(blob, 0xA0, quint64(l.size()), this);
            QList<int> refs;
            for (const QVariant &e : l)
                refs.append(encodeAny(e));
            for (int r : refs)
                appendRef(blob, r);
            return addObject(blob);
        }
        case QMetaType::QString: {
            const QString s = v.toString();
            QByteArray blob;
            bool ascii = true;
            for (QChar c : s) {
                if (c.unicode() > 0x7F) {
                    ascii = false;
                    break;
                }
            }
            if (ascii) {
                const QByteArray raw = s.toLatin1();
                appendMarker(blob, 0x50, quint64(raw.size()), this);
                blob.append(raw);
            } else {
                appendMarker(blob, 0x60, quint64(s.size()), this);
                for (QChar c : s)
                    appendU16(blob, c.unicode());
            }
            return addObject(blob);
        }
        case QMetaType::QByteArray: {
            const QByteArray raw = v.toByteArray();
            QByteArray blob;
            appendMarker(blob, 0x40, quint64(raw.size()), this);
            blob.append(raw);
            return addObject(blob);
        }
        case QMetaType::Bool: {
            QByteArray blob;
            blob.append(v.toBool() ? char(0x09) : char(0x08));
            return addObject(blob);
        }
        case QMetaType::Double:
        case QMetaType::Float: {
            QByteArray blob;
            blob.append(char(0x23)); // 8-byte real
            appendDouble(blob, v.toDouble());
            return addObject(blob);
        }
        default: {
            // Integers (int/uint/qlonglong/...): smallest fitting size.
            const qlonglong n = v.toLongLong();
            QByteArray blob;
            if (n >= -128 && n <= 127) {
                blob.append(char(0x10));
                blob.append(char(n & 0xFF));
            } else if (n >= -32768 && n <= 32767) {
                blob.append(char(0x11));
                appendU16(blob, quint16(n & 0xFFFF));
            } else if (n >= -2147483648LL && n <= 2147483647LL) {
                blob.append(char(0x12));
                appendU32(blob, quint32(n & 0xFFFFFFFF));
            } else {
                blob.append(char(0x13));
                appendU64(blob, quint64(n));
            }
            return addObject(blob);
        }
        }
    }
};

struct Reader {
    const QByteArray &data;
    QList<quint64> offsets;
    quint64 numObjects = 0;
    int offSize = 0, refSize = 0;
    bool ok = false;

    static quint64 be(const QByteArray &d, int pos, int n)
    {
        quint64 v = 0;
        for (int i = 0; i < n; ++i)
            v = (v << 8) | quint8(d.at(pos + i));
        return v;
    }
    bool parseTrailer()
    {
        if (data.size() < 40 || !data.startsWith("bplist00"))
            return false;
        const int t = data.size() - 32;
        offSize = quint8(data.at(t + 6));
        refSize = quint8(data.at(t + 7));
        numObjects = be(data, t + 8, 8);
        const quint64 top = be(data, t + 16, 8);
        const quint64 tableOff = be(data, t + 24, 8);
        if (offSize < 1 || offSize > 8 || refSize < 1 || refSize > 8)
            return false;
        if (tableOff + numObjects * quint64(offSize) > quint64(data.size()))
            return false;
        for (quint64 i = 0; i < numObjects; ++i)
            offsets.append(be(data, int(tableOff + i * quint64(offSize)), offSize));
        Q_UNUSED(top);
        return !offsets.isEmpty();
    }
    // (object index, length) for containers/strings/data with long counts.
    QPair<int, quint64> counted(int pos, quint8 low)
    {
        if (low != 0x0F)
            return {pos, low};
        if (pos >= data.size())
            return {-1, 0};
        const quint8 m = quint8(data.at(pos));
        if ((m & 0xF0) != 0x10)
            return {-1, 0}; // must be an int object
        const int n = 1 << (m & 0x0F);
        if (pos + 1 + n > data.size())
            return {-1, 0};
        return {pos + 1 + n, be(data, pos + 1, n)};
    }
    QVariant resolve(quint64 idx)
    {
        if (idx >= quint64(offsets.size()))
            return {};
        const int pos = int(offsets.at(int(idx)));
        if (pos >= data.size())
            return {};
        const quint8 marker = quint8(data.at(pos));
        const quint8 type = marker & 0xF0, low = marker & 0x0F;
        switch (type) {
        case 0x00:
            if (marker == 0x08)
                return false;
            if (marker == 0x09)
                return true;
            return {};
        case 0x10: { // int: 1/2/4-byte unsigned, 8-byte signed
            // (matches CPython plistlib — TVs stuff ports >32767 into
            // 2-byte ints, e.g. eventPort 42477).
            const int n = 1 << low; // 1,2,4,8
            if (low > 3 || pos + 1 + n > data.size())
                return {};
            const quint64 u = be(data, pos + 1, n);
            if (n == 8)
                return (qlonglong)u;
            return u;
        }
        case 0x20: { // real
            if (low == 2 && pos + 5 <= data.size()) {
                quint32 u = quint32(be(data, pos + 1, 4));
                float f;
                memcpy(&f, &u, 4);
                // BE→host for float: manual swap.
                QByteArray tmp = data.mid(pos + 1, 4);
                quint32 be32 = (quint32(quint8(tmp[0])) << 24)
                    | (quint32(quint8(tmp[1])) << 16)
                    | (quint32(quint8(tmp[2])) << 8) | quint32(quint8(tmp[3]));
                memcpy(&f, &be32, 4);
                return double(f);
            }
            if (low == 3 && pos + 9 <= data.size()) {
                quint64 u = be(data, pos + 1, 8);
                double d;
                memcpy(&d, &u, 8);
                return d;
            }
            return {};
        }
        case 0x40: { // data
            auto [np, len] = counted(pos + 1, low);
            if (np < 0 || np + int(len) > data.size())
                return {};
            return data.mid(np, int(len));
        }
        case 0x50: { // ascii string
            auto [np, len] = counted(pos + 1, low);
            if (np < 0 || np + int(len) > data.size())
                return {};
            return QString::fromLatin1(data.mid(np, int(len)));
        }
        case 0x60: { // utf-16 string (count = units)
            auto [np, len] = counted(pos + 1, low);
            if (np < 0 || np + int(len) * 2 > data.size())
                return {};
            QString s;
            s.reserve(int(len));
            for (quint64 i = 0; i < len; ++i)
                s.append(QChar(quint16(be(data, np + int(i) * 2, 2))));
            return s;
        }
        case 0xA0: { // array
            auto [np, len] = counted(pos + 1, low);
            if (np < 0 || np + int(len) * refSize > data.size())
                return {};
            QVariantList out;
            for (quint64 i = 0; i < len; ++i)
                out.append(resolve(be(data, np + int(i) * refSize, refSize)));
            return out;
        }
        case 0xD0: { // dict
            auto [np, len] = counted(pos + 1, low);
            if (np < 0 || np + int(len) * 2 * refSize > data.size())
                return {};
            QVariantMap out;
            for (quint64 i = 0; i < len; ++i) {
                const QVariant k =
                    resolve(be(data, np + int(i) * refSize, refSize));
                const QVariant val = resolve(
                    be(data, np + int(len + i) * refSize, refSize));
                out.insert(k.toString(), val);
            }
            return out;
        }
        default:
            return {};
        }
    }
};

} // namespace

QByteArray encode(const QVariant &v)
{
    Writer w;
    const int top = w.encodeAny(v);
    w.refSize = (w.offsets.size() > 256) ? 2 : 1;
    // NOTE: refs were already emitted with refSize=1; objects >256 refs are
    // far beyond our AirPlay payloads (a dozen objects), so keep it simple.
    Q_ASSERT(w.offsets.size() <= 256);
    QByteArray out("bplist00", 8);
    // Re-emit with correct offsets: objects already concatenated in order.
    Q_UNUSED(top);
    out.append(w.objects);
    const int tableOff = out.size();
    int offSize = 1;
    for (int o : w.offsets) {
        const int abs = o + 8;
        if (abs > 0xFFFF)
            offSize = 4;
        else if (abs > 0xFF && offSize < 2)
            offSize = 2;
    }
    for (int o : w.offsets) {
        const int abs = o + 8; // offsets count from file start (past header)
        for (int i = offSize - 1; i >= 0; --i)
            out.append(char((abs >> (8 * i)) & 0xFF));
    }
    out.append(QByteArray(6, 0));
    out.append(char(offSize));
    out.append(char(w.refSize));
    appendU64(out, quint64(w.offsets.size()));
    appendU64(out, quint64(top));
    appendU64(out, quint64(tableOff));
    return out;
}

QVariant decode(const QByteArray &data)
{
    Reader r{data};
    if (!r.parseTrailer())
        return {};
    // Top object index is at trailer+16; re-read it here.
    const int t = data.size() - 32;
    const quint64 top = Reader::be(data, t + 16, 8);
    return r.resolve(top);
}

} // namespace Bplist
