#include "AirPlayPairing.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QTextStream>

#include "CastDebug.h"
#include <QTimer>
#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>
#include <QSettings>
#include <QUuid>

#include <openssl/bn.h>
#include <openssl/rand.h>
#include <sodium.h>

namespace {

// RFC 5054 Appendix A.4, 3072-bit group, generator g = 5.
constexpr const char *kSrpN_Hex =
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E08"
    "8A67CC74020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B"
    "302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9"
    "A637ED6B0BFF5CB6F406B7EDEE386BFB5A899FA5AE9F24117C4B1FE6"
    "49286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8"
    "FD24CF5F83655D23DCA3AD961C62F356208552BB9ED529077096966D"
    "670C354E4ABC9804F1746C08CA18217C32905E462E36CE3BE39E772C"
    "180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF695581718"
    "3995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D"
    "04507A33A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7D"
    "B3970F85A6E1E4C7ABF5AE8CDB0933D71E8C94E04A25619DCEE3D226"
    "1AD2EE6BF12FFA06D98A0864D87602733EC86A64521F2B18177B200C"
    "BBE117577A615D6C770988C0BAD946E208E24FA074E5AB3143DB5BFC"
    "E0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF";
constexpr int kSrpG = 5;
constexpr int kPadLen = 384; // N byte length

QByteArray bnToBin(const BIGNUM *bn)
{
    const int n = BN_num_bytes(bn);
    QByteArray out(n, 0);
    BN_bn2bin(bn, reinterpret_cast<unsigned char *>(out.data()));
    return out;
}

QByteArray bnToPadded(const BIGNUM *bn, int len = kPadLen)
{
    QByteArray out(len, 0);
    if (BN_bn2binpad(bn, reinterpret_cast<unsigned char *>(out.data()), len)
        != len)
        return {};
    return out;
}

BIGNUM *bnFromBin(const QByteArray &b)
{
    return BN_bin2bn(reinterpret_cast<const unsigned char *>(b.constData()),
                     b.size(), nullptr);
}

QByteArray sha512(const QByteArray &d)
{
    return QCryptographicHash::hash(d, QCryptographicHash::Sha512);
}

QByteArray hmacSha512(const QByteArray &key, const QByteArray &d)
{
    QMessageAuthenticationCode code(QCryptographicHash::Sha512, key);
    code.addData(d);
    return code.result();
}

} // namespace

QString AirPlayPairing::Credentials::serialize() const
{
    return QStringLiteral("%1:%2:%3:%4")
        .arg(QString::fromLatin1(ltpk.toHex()),
             QString::fromLatin1(ltsk.toHex()),
             QString::fromLatin1(atvId.toHex()),
             QString::fromLatin1(clientId.toHex()));
}

AirPlayPairing::Credentials AirPlayPairing::Credentials::parse(const QString &s)
{
    Credentials c;
    const QStringList parts = s.split(QLatin1Char(':'));
    if (parts.size() != 4)
        return c;
    c.ltpk = QByteArray::fromHex(parts[0].toLatin1());
    c.ltsk = QByteArray::fromHex(parts[1].toLatin1());
    c.atvId = QByteArray::fromHex(parts[2].toLatin1());
    c.clientId = QByteArray::fromHex(parts[3].toLatin1());
    return c;
}

AirPlayPairing::AirPlayPairing(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
{
    static bool sodiumOk = false;
    if (!sodiumOk) {
        sodiumOk = (sodium_init() >= 0);
    }
    Q_UNUSED(sodiumOk);
}

void AirPlayPairing::setDevice(const QString &host, quint16 port,
                               const QString &deviceId)
{
    m_host = host;
    m_port = port ? port : 7000;
    m_deviceId = deviceId;
    QSettings s;
    m_creds = Credentials::parse(
        s.value(QStringLiteral("AirPlay/pairing/") + deviceKey()).toString());
}

QString AirPlayPairing::deviceKey() const
{
    return m_deviceId.isEmpty()
        ? QStringLiteral("%1:%2").arg(m_host).arg(m_port)
        : m_deviceId;
}

void AirPlayPairing::clearPairing()
{
    m_creds = Credentials();
    QSettings s;
    s.remove(QStringLiteral("AirPlay/pairing/") + deviceKey());
}

QMap<int, QByteArray> AirPlayPairing::tlvRead(const QByteArray &data)
{
    QMap<int, QByteArray> out;
    int pos = 0;
    while (pos + 2 <= data.size()) {
        const int tag = quint8(data.at(pos));
        const int len = quint8(data.at(pos + 1));
        if (pos + 2 + len > data.size())
            break;
        out[tag] += data.mid(pos + 2, len); // long values split across tags
        pos += 2 + len;
    }
    return out;
}

QByteArray AirPlayPairing::tlvWrite(const QList<QPair<int, QByteArray>> &data)
{
    QByteArray out;
    for (const auto &kv : data) {
        const QByteArray &v = kv.second;
        int pos = 0, left = v.size();
        do {
            const int chunk = qMin(left, 255);
            out.append(char(kv.first));
            out.append(char(chunk));
            out.append(v.mid(pos, chunk));
            pos += chunk;
            left -= chunk;
        } while (left > 0);
    }
    return out;
}

QByteArray AirPlayPairing::opackName(const QString &name)
{
    const QByteArray key = "name";
    const QByteArray val = name.toUtf8();
    QByteArray out;
    out.append(char(0xE1)); // dict, 1 entry
    out.append(char(0x40 + key.size()));
    out.append(key);
    if (val.size() <= 0x20) {
        out.append(char(0x40 + val.size()));
        out.append(val);
    } else {
        out.append(char(0x61));
        out.append(char(val.size()));
        out.append(val);
    }
    return out;
}

QByteArray AirPlayPairing::sha512(const QByteArray &data)
{
    return ::sha512(data);
}

QByteArray AirPlayPairing::hkdf(const QByteArray &salt, const QByteArray &info,
                                const QByteArray &ikm)
{
    QByteArray realSalt = salt.isEmpty() ? QByteArray(64, 0) : salt;
    const QByteArray prk = hmacSha512(realSalt, ikm);
    QByteArray t;
    QByteArray okm;
    quint8 counter = 1;
    while (okm.size() < 32) {
        QByteArray msg = t + info + QByteArray(1, char(counter));
        t = hmacSha512(prk, msg);
        okm += t;
        ++counter;
    }
    return okm.left(32);
}

QByteArray AirPlayPairing::chachaEncrypt(const QByteArray &key,
                                         const QByteArray &nonce8,
                                         const QByteArray &plain)
{
    QByteArray nonce12(4, 0);
    nonce12 += nonce8.left(8);
    QByteArray out(plain.size() + crypto_aead_chacha20poly1305_ietf_ABYTES, 0);
    unsigned long long outLen = 0;
    if (crypto_aead_chacha20poly1305_ietf_encrypt(
            reinterpret_cast<unsigned char *>(out.data()), &outLen,
            reinterpret_cast<const unsigned char *>(plain.constData()),
            plain.size(), nullptr, 0, nullptr,
            reinterpret_cast<const unsigned char *>(nonce12.constData()),
            reinterpret_cast<const unsigned char *>(key.constData()))
        != 0)
        return {};
    out.resize(outLen);
    return out;
}

QByteArray AirPlayPairing::chachaDecrypt(const QByteArray &key,
                                         const QByteArray &nonce8,
                                         const QByteArray &cipher)
{
    QByteArray nonce12(4, 0);
    nonce12 += nonce8.left(8);
    QByteArray out(cipher.size(), 0);
    unsigned long long outLen = 0;
    if (crypto_aead_chacha20poly1305_ietf_decrypt(
            reinterpret_cast<unsigned char *>(out.data()), &outLen, nullptr,
            reinterpret_cast<const unsigned char *>(cipher.constData()),
            cipher.size(), nullptr, 0,
            reinterpret_cast<const unsigned char *>(nonce12.constData()),
            reinterpret_cast<const unsigned char *>(key.constData()))
        != 0)
        return {};
    out.resize(outLen);
    return out;
}

AirPlayPairing::PostResult AirPlayPairing::post(const QString &path,
                                               const QByteArray &body)
{
    PostResult r;
    QNetworkRequest req{QUrl(
        QStringLiteral("http://%1:%2%3").arg(m_host).arg(m_port).arg(path))};
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/octet-stream"));
    req.setRawHeader("User-Agent", "AirPlay/320.20");
    req.setRawHeader("Connection", "keep-alive");
    req.setRawHeader("X-Apple-HKP", "3");
    req.setTransferTimeout(12000);
    QNetworkReply *reply = m_net->post(req, body);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    timer.setInterval(15000);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start();
    loop.exec();
    timer.stop();
    const bool timedOut = !timer.isActive() && !reply->isFinished();
    if (timedOut)
        reply->abort();
    r.code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    r.body = reply->readAll();
    r.error = reply->errorString();
    r.ok = !timedOut && (reply->error() == QNetworkReply::NoError);
    reply->deleteLater();
    {
        // TLV summary for the debug log (no secrets: small scalar tags by
        // value, long tags by length).
        QString sum;
        const QMap<int, QByteArray> t = tlvRead(r.body);
        for (auto it = t.constBegin(); it != t.constEnd(); ++it) {
            if (it->size() <= 2)
                sum += QStringLiteral("%1=0x%2 ").arg(it.key()).arg(
                    QString::fromLatin1(it->toHex()));
            else
                sum += QStringLiteral("%1:%2B ").arg(it.key()).arg(it->size());
        }
        QFile f(QStringLiteral("/tmp/opencode/cast-debug.log"));
        if (f.open(QIODevice::Append | QIODevice::Text)) {
            QTextStream ts(&f);
            ts << QDateTime::currentDateTime().toString("hh:mm:ss.zzz") << ' '
               << path << " code=" << r.code << " tlv={" << sum.trimmed()
               << "}\n";
        }
    }
    return r;
}

bool AirPlayPairing::Srp::compute(const QByteArray &pin,
                                     const QByteArray &saltIn,
                                     const QByteArray &serverBIn, bool padGInK,
                                     bool padSInK)
{
    BN_CTX *ctx = BN_CTX_new();
    if (!ctx)
        return false;
    BIGNUM *N = nullptr, *g = BN_new(), *a = nullptr, *A = nullptr;
    BIGNUM *B = nullptr, *x = nullptr, *k = nullptr, *u = nullptr;
    BIGNUM *S = nullptr, *t1 = BN_new(), *t2 = BN_new(), *e = BN_new();
    bool ok = false;
    BN_hex2bn(&N, kSrpN_Hex);
    BN_set_word(g, kSrpG);
    if (!N || BN_num_bytes(N) != kPadLen)
        goto done;
    // a = 256-bit random; A = g^a mod N.
    {
        unsigned char rnd[32];
        RAND_bytes(rnd, sizeof(rnd));
        a = BN_bin2bn(rnd, sizeof(rnd), nullptr);
        A = BN_new();
        BN_mod_exp(A, g, a, N, ctx);
        this->aDbg = QByteArray(reinterpret_cast<char *>(rnd), 32);
        this->A = bnToPadded(A);
    }
    B = bnFromBin(serverBIn);
    if (!B || BN_is_zero(B) || BN_cmp(B, N) >= 0)
        goto done; // B % N must be non-zero and in range
    // x = H(salt | H("Pair-Setup:" | PIN)).
    {
        const QByteArray inner = sha512(QByteArray("Pair-Setup:") + pin);
        x = BN_bin2bn(
            reinterpret_cast<const unsigned char *>(sha512(saltIn + inner).constData()),
            64, nullptr);
    }
    // k = H(pad(N) | pad(g)) — pad(g) is the debated bit, see flags.
    BN_mod_exp(t1, g, x, N, ctx);                        // t1 = g^x
    {
        const QByteArray gBytes = padGInK ? bnToPadded(g) : bnToBin(g);
        k = BN_bin2bn(
            reinterpret_cast<const unsigned char *>(
                sha512(bnToPadded(N) + gBytes).constData()),
            64, nullptr);
    }
    BN_mod_mul(t2, k, t1, N, ctx);                       // t2 = k*g^x
    BN_mod_sub(t1, B, t2, N, ctx);                       // t1 = (B - k*g^x)
    // u = H(pad(A) | pad(B)); e = a + u*x; S = base^e mod N.
    u = BN_bin2bn(
        reinterpret_cast<const unsigned char *>(
            sha512(this->A + bnToPadded(B)).constData()),
        64, nullptr);
    BN_mul(t2, u, x, ctx);
    BN_add(e, a, t2);
    S = BN_new();
    BN_mod_exp(S, t1, e, N, ctx);
    // K = H(S) — padded or raw S is the other debated bit, see flags.
    // M = H(H(padN)^H(g_min) | H(I) | s | A_min | B_min | K):
    // srptools hashes every int in MINIMAL form — notably g as a single
    // 0x05 byte (NOT padded!) and minimal A/B. Full-length values coincide
    // either way; the g case differs systematically (1 B vs 384 B).
    {
        const QByteArray padN = bnToPadded(N);
        const QByteArray gmin = bnToBin(g);
        const QByteArray sBytes = padSInK ? bnToPadded(S) : bnToBin(S);
        K = sha512(sBytes);
        QByteArray hn = sha512(padN), hg = sha512(gmin);
        QByteArray xorb;
        xorb.resize(hn.size());
        for (int i = 0; i < hn.size(); ++i)
            xorb[i] = hn[i] ^ hg[i];
        // Minimal encoding of the XOR (strip leading zeros, keep ≥1 byte).
        int lead = 0;
        while (lead + 1 < xorb.size() && xorb.at(lead) == 0)
            ++lead;
        const QByteArray aMin = bnToBin(A), bMin = bnToBin(B);
        proofM = sha512(xorb.mid(lead) + sha512(QByteArray("Pair-Setup"))
                        + saltIn + aMin + bMin + K);
    }
    ok = true;
done:
    BN_free(N);
    BN_free(g);
    BN_free(a);
    BN_free(A);
    BN_free(B);
    BN_free(x);
    BN_free(k);
    BN_free(u);
    BN_free(S);
    BN_free(t1);
    BN_free(t2);
    BN_free(e);
    BN_CTX_free(ctx);
    return ok;
}

static bool tlvHasError(const QMap<int, QByteArray> &tlv, QString *msg = nullptr)
{
    auto it = tlv.find(7); // Error
    if (it == tlv.end())
        return false;
    if (msg)
        *msg = QStringLiteral("eszközhiba %1").arg(quint8(it->at(0)));
    return true;
}

bool AirPlayPairing::begin()
{
    // Fresh identity per pairing attempt (like pyatv's initialize()).
    unsigned char seed[32];
    RAND_bytes(seed, sizeof(seed));
    m_srp.edPriv = QByteArray(reinterpret_cast<char *>(seed), 32);
    unsigned char pub[32], priv64[64];
    crypto_sign_seed_keypair(pub, priv64, seed);
    m_srp.edPub = QByteArray(reinterpret_cast<char *>(pub), 32);
    m_srp.pairingId =
        QUuid::createUuid().toString(QUuid::WithoutBraces).toLower().toLatin1();

    PostResult r = post(QStringLiteral("/pair-pin-start"), QByteArray());
    if (!r.ok && r.code != 200) {
        Q_EMIT notice(tr("AirPlay PIN-kérés sikertelen (%1)").arg(r.error),
                      "err");
        return false;
    }
    QList<QPair<int, QByteArray>> m1;
    m1 += {0, QByteArray(1, 0)}; // Method = PairSetup
    m1 += {6, QByteArray(1, 1)}; // SeqNo = M1
    r = post(QStringLiteral("/pair-setup"), tlvWrite(m1));
    if (!r.ok || r.code != 200) {
        Q_EMIT notice(tr("AirPlay pair-setup sikertelen (%1)").arg(r.error),
                      "err");
        return false;
    }
    const QMap<int, QByteArray> tlv = tlvRead(r.body);
    QString errMsg;
    if (tlvHasError(tlv, &errMsg)) {
        // Error 3 (BackOff) after too many attempts: report the wait.
        QString wait;
        auto it = tlv.find(8);
        if (it != tlv.end() && !it->isEmpty())
            wait = tr(" — várj %1 másodpercet").arg(quint8(it->at(0)));
        Q_EMIT notice(tr("AirPlay elutasította (%1)%2").arg(errMsg, wait),
                      "err");
        return false;
    }
    if (!tlv.contains(2) || !tlv.contains(3)) {
        Q_EMIT notice(tr("AirPlay: váratlan pair-setup válasz"), "err");
        return false;
    }
    m_srp.salt = tlv[2];
    // B may arrive split across TLV chunks — tlvRead concatenates.
    if (tlv[3].size() != kPadLen) {
        Q_EMIT notice(tr("AirPlay: hibás szerverkulcs"), "err");
        return false;
    }
    m_srp.serverB = tlv[3];
    // Salt + B are public server values; logging them enables offline
    // cross-checking of our SRP math (see /tmp/opencode/srpcheck.py).
    castDebug(QStringLiteral("pair: salt=%1").arg(
        QString::fromLatin1(m_srp.salt.toHex())));
    castDebug(QStringLiteral("pair: B=%1").arg(
        QString::fromLatin1(m_srp.serverB.toHex())));
    Q_EMIT notice(tr("Add meg a tévén látható PIN-kódot"), "info");
    return true;
}

bool AirPlayPairing::finish(const QString &pin, const QString &clientName)
{
    const QString digits = [pin] {
        QString d;
        for (QChar c : pin) {
            if (c.isDigit())
                d += c;
        }
        return d;
    }();
    if (digits.isEmpty() || m_srp.salt.isEmpty()) {
        Q_EMIT notice(tr("Hiányzó PIN vagy lejárt munkamenet"), "err");
        return false;
    }
    // PIN length only (never the code itself): catches field-concat bugs.
    castDebug(QStringLiteral("pair: finish pinlen=%1").arg(digits.size()));
    // SRP with the M2 salt + public key stored by begin(). ONE shot per
    // session: the TV kills the session (and escalates backoff) after a bad
    // proof, so in-session retries are useless. Variant order follows
    // srptools-exact first (K = H of minimal S, padded g in k), then the
    // other padding combos across fresh user rounds (m_variantIdx).
    static const bool kVariants[][2] = {{true, false}, {true, true},
                                        {false, false}, {false, true}};
    const bool padG = kVariants[m_variantIdx % 4][0];
    const bool padS = kVariants[m_variantIdx % 4][1];
    bool m3ok = false;
    PostResult r;
    castDebug(QStringLiteral("pair: finish variant #%1 padG=%2 padS=%3")
                  .arg(m_variantIdx % 4 + 1).arg(padG).arg(padS));
    if (!m_srp.compute(digits.toLatin1(), m_srp.salt, m_srp.serverB, padG,
                       padS)) {
        Q_EMIT notice(tr("AirPlay SRP-hiba"), "err");
        return false;
    }
    // M3: {SeqNo:3, A, proof}. A + M are public values — safe to log
    // for cross-checking against a reference implementation.
    // 'a' is single-use ephemeral; logging it here (debug file only)
    // lets us verify the whole chain offline. Scrubbed before commit.
    castDebug(QStringLiteral("pair: M3 a=%1").arg(
        QString::fromLatin1(m_srp.aDbg.toHex())));
    castDebug(QStringLiteral("pair: M3 A=%1").arg(
        QString::fromLatin1(m_srp.A.toHex())));
    castDebug(QStringLiteral("pair: M3 M=%1").arg(
        QString::fromLatin1(m_srp.proofM.toHex())));
    QList<QPair<int, QByteArray>> m3;
    m3 += {6, QByteArray(1, 3)};
    m3 += {3, m_srp.A};
    m3 += {4, m_srp.proofM};
    r = post(QStringLiteral("/pair-setup"), tlvWrite(m3));
    if (!r.ok || (r.code != 200 && r.code != 204)) {
        Q_EMIT notice(tr("AirPlay M3 elutasítva (%1)").arg(r.error), "err");
        return false;
    }
    {
        const QMap<int, QByteArray> m3resp = tlvRead(r.body);
        QString m3err;
        if (!m3resp.contains(6)) {
            // Empty/garbage body (session died after a failed attempt):
            // must not be mistaken for success.
            Q_EMIT notice(tr("AirPlay M3 üres válasz"), "err");
            return false;
        }
        if (tlvHasError(m3resp, &m3err)) {
            castDebug(QStringLiteral("pair: M3 -> %1").arg(m3err));
            // Advance to the next padding convention for the NEXT fresh
            // round (this session is dead now).
            ++m_variantIdx;
            Q_EMIT notice(
                tr("AirPlay proof elutasítva — új PIN-nel próbáld újra"),
                "err");
            return false;
        }
        m3ok = true;
    }
    if (!m3ok) {
        Q_EMIT notice(tr("AirPlay proof elutasítva (PIN jó? próbáld újra)"),
                      "err");
        return false;
    }
    // M5: {SeqNo:5, enc(id, ltpk, sig[, name])}.
    m_srp.sessionKey = hkdf("Pair-Setup-Encrypt-Salt",
                            "Pair-Setup-Encrypt-Info", m_srp.K);
    const QByteArray iosKey = hkdf("Pair-Setup-Controller-Sign-Salt",
                                   "Pair-Setup-Controller-Sign-Info",
                                   m_srp.K);
    unsigned char sig[64];
    {
        unsigned char sk[64], pk[32];
        crypto_sign_seed_keypair(
            pk, sk,
            reinterpret_cast<const unsigned char *>(m_srp.edPriv.constData()));
        const QByteArray info = iosKey + m_srp.pairingId + m_srp.edPub;
        crypto_sign_detached(
            sig, nullptr,
            reinterpret_cast<const unsigned char *>(info.constData()),
            info.size(), sk);
    }
    QList<QPair<int, QByteArray>> inner;
    inner += {1, m_srp.pairingId};
    inner += {3, m_srp.edPub};
    inner += {0x0A, QByteArray(reinterpret_cast<char *>(sig), 64)};
    inner += {0x11, opackName(clientName.isEmpty() ? QStringLiteral("omaplayer")
                                                   : clientName)};
    const QByteArray enc =
        chachaEncrypt(m_srp.sessionKey, "PS-Msg05", tlvWrite(inner));
    if (enc.isEmpty()) {
        Q_EMIT notice(tr("AirPlay titkosítási hiba"), "err");
        return false;
    }
    QList<QPair<int, QByteArray>> m5;
    m5 += {6, QByteArray(1, 5)};
    m5 += {5, enc};
    r = post(QStringLiteral("/pair-setup"), tlvWrite(m5));
    if (!r.ok || r.code != 200) {
        Q_EMIT notice(tr("AirPlay M5 elutasítva (%1)").arg(r.error), "err");
        return false;
    }
    const QMap<int, QByteArray> tlv = tlvRead(r.body);
    QString errMsg;
    if (tlvHasError(tlv, &errMsg) || !tlv.contains(5)) {
        Q_EMIT notice(tr("AirPlay párosítás sikertelen (%1)").arg(errMsg),
                      "err");
        return false;
    }
    const QByteArray dec =
        chachaDecrypt(m_srp.sessionKey, "PS-Msg06", tlv[5]);
    if (dec.isEmpty()) {
        Q_EMIT notice(tr("AirPlay M6 visszafejtés sikertelen (rossz PIN?)"),
                      "err");
        return false;
    }
    const QMap<int, QByteArray> atv = tlvRead(dec);
    if (!atv.contains(1) || !atv.contains(3)) {
        Q_EMIT notice(tr("AirPlay: hiányos M6 válasz"), "err");
        return false;
    }
    m_creds.ltpk = atv[3];
    m_creds.ltsk = m_srp.edPriv;
    m_creds.atvId = atv[1];
    m_creds.clientId = m_srp.pairingId;
    QSettings s;
    s.setValue(QStringLiteral("AirPlay/pairing/") + deviceKey(),
               m_creds.serialize());
    Q_EMIT notice(tr("AirPlay párosítva"), "ok");
    return true;
}

AirPlayPairing::MediaResult
AirPlayPairing::postMedia(const QString &path, const QByteArray &body,
                          const QString &contentType)
{
    MediaResult r;
    QNetworkRequest req{QUrl(
        QStringLiteral("http://%1:%2%3").arg(m_host).arg(m_port).arg(path))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    req.setRawHeader("User-Agent", "AirPlay/320.20");
    req.setRawHeader("Connection", "keep-alive");
    req.setTransferTimeout(12000);
    QNetworkReply *reply = m_net->post(req, body);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    timer.setInterval(15000);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start();
    loop.exec();
    timer.stop();
    if (!timer.isActive() && !reply->isFinished())
        reply->abort();
    r.code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    r.error = reply->errorString();
    r.ok = (reply->error() == QNetworkReply::NoError) || r.code == 200
        || r.code == 204;
    reply->deleteLater();
    return r;
}

bool AirPlayPairing::verify()
{
    if (!m_creds.isValid()) {
        Q_EMIT notice(tr("Nincs mentett AirPlay párosítás"), "err");
        return false;
    }
    unsigned char xseed[32];
    RAND_bytes(xseed, sizeof(xseed));
    m_xPriv = QByteArray(reinterpret_cast<char *>(xseed), 32);
    unsigned char xpubRaw[32];
    crypto_scalarmult_base(xpubRaw, xseed);
    m_xPub = QByteArray(reinterpret_cast<char *>(xpubRaw), 32);

    QList<QPair<int, QByteArray>> m1;
    m1 += {6, QByteArray(1, 1)};
    m1 += {3, m_xPub};
    PostResult r = post(QStringLiteral("/pair-verify"), tlvWrite(m1));
    if (!r.ok || r.code != 200) {
        Q_EMIT notice(tr("AirPlay verify sikertelen (%1)").arg(r.error),
                      "err");
        return false;
    }
    QMap<int, QByteArray> tlv = tlvRead(r.body);
    if (!tlv.contains(3) || !tlv.contains(5)) {
        Q_EMIT notice(tr("AirPlay: váratlan verify-válasz"), "err");
        return false;
    }
    const QByteArray serverXPub = tlv[3];
    unsigned char shared[32];
    if (crypto_scalarmult(
            shared, reinterpret_cast<const unsigned char *>(m_xPriv.constData()),
            reinterpret_cast<const unsigned char *>(serverXPub.constData()))
        != 0) {
        Q_EMIT notice(tr("AirPlay ECDH-hiba"), "err");
        return false;
    }
    const QByteArray encKey =
        hkdf("Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info",
             QByteArray(reinterpret_cast<char *>(shared), 32));
    const QByteArray dec =
        chachaDecrypt(encKey, "PV-Msg02", tlv[5]);
    if (dec.isEmpty()) {
        Q_EMIT notice(tr("AirPlay verify visszafejtés sikertelen"), "err");
        return false;
    }
    const QMap<int, QByteArray> atv = tlvRead(dec);
    if (!atv.contains(1) || !atv.contains(0x0A)
        || atv[1] != m_creds.atvId) {
        Q_EMIT notice(tr("AirPlay: azonosító nem egyezik"), "err");
        return false;
    }
    // sig over (serverXPub | atvId | clientXPub) with ATV long-term key.
    const QByteArray info = serverXPub + m_creds.atvId + m_xPub;
    if (crypto_sign_verify_detached(
            reinterpret_cast<const unsigned char *>(atv[0x0A].constData()),
            reinterpret_cast<const unsigned char *>(info.constData()),
            info.size(),
            reinterpret_cast<const unsigned char *>(m_creds.ltpk.constData()))
        != 0) {
        Q_EMIT notice(tr("AirPlay: aláírás érvénytelen"), "err");
        return false;
    }
    // Our proof: sign (clientXPub | clientId | serverXPub) with our seed.
    unsigned char sig[64], sk[64], pk[32];
    crypto_sign_seed_keypair(
        pk, sk,
        reinterpret_cast<const unsigned char *>(m_creds.ltsk.constData()));
    const QByteArray info2 = m_xPub + m_creds.clientId + serverXPub;
    crypto_sign_detached(
        sig, nullptr,
        reinterpret_cast<const unsigned char *>(info2.constData()),
        info2.size(), sk);
    QList<QPair<int, QByteArray>> inner;
    inner += {1, m_creds.clientId};
    inner += {0x0A, QByteArray(reinterpret_cast<char *>(sig), 64)};
    const QByteArray enc =
        chachaEncrypt(encKey, "PV-Msg03", tlvWrite(inner));
    QList<QPair<int, QByteArray>> m3;
    m3 += {6, QByteArray(1, 3)};
    m3 += {5, enc};
    r = post(QStringLiteral("/pair-verify"), tlvWrite(m3));
    if (!r.ok || (r.code != 200 && r.code != 204)) {
        Q_EMIT notice(tr("AirPlay verify M3 elutasítva (%1)").arg(r.error),
                      "err");
        return false;
    }
    return true;
}
