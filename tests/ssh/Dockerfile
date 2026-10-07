FROM debian:13-slim
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential qtbase5-dev qtdeclarative5-dev qt5-qmake cmake perl pkg-config libssl-dev zlib1g-dev \
    openssh-server mosh tmux locales netcat-openbsd python3 procps \
 && sed -i 's/# en_US.UTF-8/en_US.UTF-8/' /etc/locale.gen && locale-gen \
 && useradd -m -s /bin/bash tester && echo 'tester:secret' | chpasswd \
 && mkdir -p /run/sshd \
 && printf 'PasswordAuthentication yes\nAcceptEnv LANG LC_* LONGTERM_*\nAllowTcpForwarding yes\nGatewayPorts no\n' > /etc/ssh/sshd_config.d/test.conf \
 && rm -rf /var/lib/apt/lists/*
