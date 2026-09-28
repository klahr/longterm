#ifndef SSHAGENT_H
#define SSHAGENT_H

#include <QByteArray>

struct ssh_key_struct;
struct evp_pkey_st;

// Answers ssh-agent requests arriving on forwarded agent channels, offering
// only the key the connection logged in with
class SshAgent
{
public:
    explicit SshAgent(ssh_key_struct *key);
    ~SshAgent();

    bool isValid() const { return m_privateKey && !m_publicKeyBlob.isEmpty(); }
    // Consumes the complete requests at the start of input, returns the replies
    QByteArray process(QByteArray *input);

private:
    QByteArray reply(const QByteArray &request);
    QByteArray sign(const QByteArray &data, quint32 flags);

    evp_pkey_st *m_privateKey;
    int m_keyType;
    QByteArray m_publicKeyBlob;
};

#endif // SSHAGENT_H
