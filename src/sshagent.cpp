#include "sshagent.h"

#include <libssh/libssh.h>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <cstring>

// Message numbers from draft-miller-ssh-agent
static const char AgentFailure = 5;
static const char AgentRequestIdentities = 11;
static const char AgentIdentitiesAnswer = 12;
static const char AgentSignRequest = 13;
static const char AgentSignResponse = 14;
static const quint32 AgentRsaSha256 = 2;
static const quint32 AgentRsaSha512 = 4;

// Requests are small, anything larger is a broken stream
static const quint32 MaxRequestLength = 256 * 1024;

namespace {

void appendUint32(QByteArray &out, quint32 value)
{
    out.append(char(value >> 24));
    out.append(char(value >> 16));
    out.append(char(value >> 8));
    out.append(char(value));
}

void appendString(QByteArray &out, const QByteArray &value)
{
    appendUint32(out, quint32(value.size()));
    out.append(value);
}

quint32 uint32At(const QByteArray &in, int pos)
{
    const uchar *p = reinterpret_cast<const uchar *>(in.constData()) + pos;
    return quint32(p[0]) << 24 | quint32(p[1]) << 16 | quint32(p[2]) << 8 | quint32(p[3]);
}

bool readUint32(const QByteArray &in, int *pos, quint32 *value)
{
    if (in.size() - *pos < 4)
        return false;
    *value = uint32At(in, *pos);
    *pos += 4;
    return true;
}

bool readString(const QByteArray &in, int *pos, QByteArray *value)
{
    quint32 length;
    if (!readUint32(in, pos, &length) || quint32(in.size() - *pos) < length)
        return false;
    *value = in.mid(*pos, int(length));
    *pos += int(length);
    return true;
}

QByteArray mpint(const BIGNUM *number)
{
    QByteArray bytes(BN_num_bytes(number), '\0');
    BN_bn2bin(number, reinterpret_cast<unsigned char *>(bytes.data()));
    // A set top bit would read as negative
    if (!bytes.isEmpty() && (uchar(bytes.at(0)) & 0x80))
        bytes.prepend('\0');
    return bytes;
}

}

SshAgent::SshAgent(ssh_key_struct *key)
    : m_privateKey(nullptr)
    , m_keyType(ssh_key_type(key))
{
    char *publicBase64 = nullptr;
    if (ssh_pki_export_pubkey_base64(key, &publicBase64) == SSH_OK)
        m_publicKeyBlob = QByteArray::fromBase64(publicBase64);
    ssh_string_free_char(publicBase64);

    // libssh has no public signing call, so hand the key to OpenSSL as PEM
    char *pem = nullptr;
    if (ssh_pki_export_privkey_base64_format(key, nullptr, nullptr, nullptr, &pem, SSH_FILE_FORMAT_PEM) != SSH_OK)
        return;
    BIO *bio = BIO_new_mem_buf(pem, -1);
    if (bio) {
        m_privateKey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
    }
    std::fill(pem, pem + std::strlen(pem), '\0');
    ssh_string_free_char(pem);
}

SshAgent::~SshAgent()
{
    EVP_PKEY_free(m_privateKey);
}

QByteArray SshAgent::process(QByteArray *input)
{
    QByteArray replies;
    while (input->size() >= 4) {
        const quint32 length = uint32At(*input, 0);
        if (length > MaxRequestLength) {
            input->clear();
            break;
        }
        if (quint32(input->size() - 4) < length)
            break;
        const QByteArray body = reply(input->mid(4, int(length)));
        input->remove(0, 4 + int(length));
        appendString(replies, body);
    }
    return replies;
}

QByteArray SshAgent::reply(const QByteArray &request)
{
    const QByteArray failure(1, AgentFailure);
    if (request.isEmpty() || !isValid())
        return failure;

    if (request.at(0) == AgentRequestIdentities) {
        QByteArray answer(1, AgentIdentitiesAnswer);
        appendUint32(answer, 1);
        appendString(answer, m_publicKeyBlob);
        appendString(answer, QByteArrayLiteral("longterm"));
        return answer;
    }

    if (request.at(0) == AgentSignRequest) {
        int pos = 1;
        QByteArray keyBlob;
        QByteArray data;
        quint32 flags = 0;
        if (!readString(request, &pos, &keyBlob) || !readString(request, &pos, &data)
                || !readUint32(request, &pos, &flags) || keyBlob != m_publicKeyBlob) {
            return failure;
        }
        const QByteArray signature = sign(data, flags);
        if (signature.isEmpty())
            return failure;
        QByteArray answer(1, AgentSignResponse);
        appendString(answer, signature);
        return answer;
    }

    return failure;
}

QByteArray SshAgent::sign(const QByteArray &data, quint32 flags)
{
    const EVP_MD *digest = nullptr;
    QByteArray algorithm;
    bool ecdsa = false;
    switch (m_keyType) {
    case SSH_KEYTYPE_ED25519:
        algorithm = "ssh-ed25519";
        break;
    case SSH_KEYTYPE_RSA:
        if (flags & AgentRsaSha512) {
            digest = EVP_sha512();
            algorithm = "rsa-sha2-512";
        } else if (flags & AgentRsaSha256) {
            digest = EVP_sha256();
            algorithm = "rsa-sha2-256";
        } else {
            digest = EVP_sha1();
            algorithm = "ssh-rsa";
        }
        break;
    case SSH_KEYTYPE_ECDSA_P256:
        digest = EVP_sha256();
        algorithm = "ecdsa-sha2-nistp256";
        ecdsa = true;
        break;
    case SSH_KEYTYPE_ECDSA_P384:
        digest = EVP_sha384();
        algorithm = "ecdsa-sha2-nistp384";
        ecdsa = true;
        break;
    case SSH_KEYTYPE_ECDSA_P521:
        digest = EVP_sha512();
        algorithm = "ecdsa-sha2-nistp521";
        ecdsa = true;
        break;
    default:
        return QByteArray();
    }

    QByteArray raw;
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    size_t length = 0;
    const unsigned char *input = reinterpret_cast<const unsigned char *>(data.constData());
    if (context && EVP_DigestSignInit(context, nullptr, digest, nullptr, m_privateKey) == 1
            && EVP_DigestSign(context, nullptr, &length, input, size_t(data.size())) == 1) {
        raw.resize(int(length));
        if (EVP_DigestSign(context, reinterpret_cast<unsigned char *>(raw.data()), &length,
                           input, size_t(data.size())) == 1) {
            raw.resize(int(length));
        } else {
            raw.clear();
        }
    }
    EVP_MD_CTX_free(context);
    if (raw.isEmpty())
        return QByteArray();

    // OpenSSL gives ECDSA signatures as DER, SSH wants the two numbers as mpints
    if (ecdsa) {
        const unsigned char *der = reinterpret_cast<const unsigned char *>(raw.constData());
        ECDSA_SIG *signature = d2i_ECDSA_SIG(nullptr, &der, raw.size());
        if (!signature)
            return QByteArray();
        const BIGNUM *r = nullptr;
        const BIGNUM *s = nullptr;
        ECDSA_SIG_get0(signature, &r, &s);
        raw.clear();
        appendString(raw, mpint(r));
        appendString(raw, mpint(s));
        ECDSA_SIG_free(signature);
    }

    QByteArray signature;
    appendString(signature, algorithm);
    appendString(signature, raw);
    return signature;
}
