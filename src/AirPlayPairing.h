#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QMap>

class QNetworkAccessManager;

// AirPlay 2 (HAP-style) pairing: PIN shown on the TV → SRP-6a pair-setup →
// ed25519 pair-verify. Mirrors pyatv's flow (hap.py + hap_srp.py), which is
// proven against real TVs:
//
//   POST /pair-pin-start            → TV shows PIN
//   POST /pair-setup {M1}           → {salt, B}
//   POST /pair-setup {M3: A, proof} → (server proof, unchecked like pyatv)
//   POST /pair-setup {M5: enc(id, ltpk, sig, name)} → {enc(atv id, sig, ltpk)}
//   POST /pair-verify {M1: x-pub}   → {x-pub, enc(id, sig)}
//   POST /pair-verify {M3: enc(id, sig)} → 200, channel authenticated
//
// Crypto: SRP-6a/RFC5054 3072-bit (OpenSSL BIGNUM, SHA-512), HKDF-SHA512
// (HMAC), ed25519/X25519/ChaCha20-Poly1305 (libsodium). Credentials persist
// in QSettings as "ltpk:ltsk:atvId:clientId" hex (pyatv format).
class AirPlayPairing : public QObject
{
    Q_OBJECT

public:
    struct Credentials {
        QByteArray ltpk;     // TV long-term (ed25519 pub, 32 B)
        QByteArray ltsk;     // our long-term (ed25519 seed, 32 B)
        QByteArray atvId;    // TV identifier
        QByteArray clientId; // our pairing id (uuid ascii)
        bool isValid() const { return !ltpk.isEmpty() && !ltsk.isEmpty(); }
        QString serialize() const;
        static Credentials parse(const QString &s);
    };

    explicit AirPlayPairing(QObject *parent = nullptr);

    void setDevice(const QString &host, quint16 port, const QString &deviceId);
    QString deviceKey() const; // settings key: deviceId or host

    // Step 1 (TV shows PIN). Remembers salt + B. False on transport error.
    bool begin();
    // Step 2 with the on-screen PIN. Persists credentials on success.
    bool finish(const QString &pin, const QString &clientName);
    // Authenticate the channel with saved credentials (needed pre-/play).
    bool verify();
    bool hasPairing() const { return m_creds.isValid(); }
    void clearPairing();
    // Plain POST on the verified connection (QNAM pool shared with verify).
    struct MediaResult {
        bool ok = false;
        int code = 0;
        QString error;
    };
    MediaResult postMedia(const QString &path, const QByteArray &body,
                          const QString &contentType);

signals:
    void notice(const QString &text, const QString &kind);

private:
    // TLV8 (HomeKit): tag(1) len(1) value, values >255 B split across tags.
    // Order-sensitive: wire order follows the list (pyatv sends SeqNo
    // first); a QMap would re-sort and some TVs reject that.
    static QMap<int, QByteArray> tlvRead(const QByteArray &data);
    static QByteArray tlvWrite(const QList<QPair<int, QByteArray>> &data);
    // opack minimal: {"name": str} → E1 + str + str (strings ≤ 32 B inline).
    static QByteArray opackName(const QString &name);

    static QByteArray sha512(const QByteArray &data);
    static QByteArray hkdf(const QByteArray &salt, const QByteArray &info,
                           const QByteArray &ikm);
    // ChaCha20-Poly1305 (IETF), 12 B nonce = 4 zero bytes + 8 B value.
    static QByteArray chachaEncrypt(const QByteArray &key,
                                    const QByteArray &nonce8,
                                    const QByteArray &plain);
    static QByteArray chachaDecrypt(const QByteArray &key,
                                    const QByteArray &nonce8,
                                    const QByteArray &cipher);

    // Blocking binary POST with AirPlay pairing headers. Returns {ok, body}.
    struct PostResult {
        bool ok = false;
        int code = 0;
        QByteArray body;
        QString error;
    };
    PostResult post(const QString &path, const QByteArray &body);

    // SRP-6a client state (set up in begin/finish).
    struct Srp {
        QByteArray aDbg;   // private ephemeral, debug only (see M3 logging)
        QByteArray A;      // public, padded to N (384 B)
        QByteArray salt;   // server salt (16 B)
        QByteArray serverB; // server public (384 B, from M2)
        QByteArray K;      // session key H(S) (64 B)
        QByteArray proofM; // client proof M (64 B)
        QByteArray edPriv; // ed25519 seed (32 B)
        QByteArray edPub;  // ed25519 public (32 B)
        QByteArray pairingId; // uuid ascii (36 B)
        QByteArray sessionKey; // HAP encrypt key (32 B)
        bool compute(const QByteArray &pin, const QByteArray &saltIn,
                     const QByteArray &serverBIn,
                     bool padGInK = true, bool padSInK = true);
    };

    QNetworkAccessManager *m_net = nullptr;
    QString m_host;
    quint16 m_port = 7000;
    QString m_deviceId;
    Credentials m_creds;
    Srp m_srp;
    QByteArray m_xPriv, m_xPub; // verify-time X25519 pair
    int m_variantIdx = 0; // padding-convention attempts across rounds
};
