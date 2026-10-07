#!/bin/sh
# Builds and runs the SSH test inside the image of the Dockerfile next to this,
# as root, with the source at /src and a build folder at /build:
#
#   docker build -t longterm-testenv tests/ssh
#   docker run --rm -v $PWD:/src:ro -v /tmp/longterm-build:/build -w /build longterm-testenv sh /src/tests/ssh/run.sh [test...]
#
# It starts sshd, makes the keys and a certificate authority the tests log in with,
# and installs longterm-status for the agent status tests.
set -e
[ -f libssh-build/lib/libssh.so ] || {
    cmake -S /src/3rdparty/libssh -B libssh-build -DCMAKE_BUILD_TYPE=Release -DWITH_SERVER=ON -DWITH_GSSAPI=OFF \
        -DWITH_PCAP=OFF -DWITH_EXAMPLES=OFF -DUNIT_TESTING=OFF -DCLIENT_TESTING=OFF >/dev/null
    cmake --build libssh-build --parallel >/dev/null
}
qmake /src/tests/ssh/ssh.pro LIBSSH_BUILD=$PWD/libssh-build >/dev/null
make -j"$(nproc)" >/dev/null

ssh-keygen -A >/dev/null
mkdir -p /tmp/secrets /home/tester/.ssh
[ -f /tmp/secrets/key ] || ssh-keygen -q -t ed25519 -N '' -f /tmp/secrets/key
# key2 is not in authorized_keys, only its certificate from the CA lets it in
[ -f /tmp/secrets/key2 ] || ssh-keygen -q -t ed25519 -N '' -f /tmp/secrets/key2
[ -f /etc/ssh/ca ] || ssh-keygen -q -t ed25519 -N '' -f /etc/ssh/ca
ssh-keygen -q -s /etc/ssh/ca -I test -n tester /tmp/secrets/key2.pub 2>/dev/null
grep -q TrustedUserCAKeys /etc/ssh/sshd_config.d/test.conf || echo 'TrustedUserCAKeys /etc/ssh/ca.pub' >> /etc/ssh/sshd_config.d/test.conf
cp /tmp/secrets/key.pub /home/tester/.ssh/authorized_keys
chown -R tester /home/tester/.ssh
cp /src/extras/agent-plugin/bin/longterm-status /usr/local/bin/longterm-status
chmod 755 /usr/local/bin/longterm-status
/usr/sbin/sshd

export QT_QPA_PLATFORM=offscreen XDG_RUNTIME_DIR=/tmp/rt LONGTERM_TEST_SECRETS=/tmp/secrets
mkdir -p /tmp/rt
chmod 700 /tmp/rt
./sshtest "$@"
