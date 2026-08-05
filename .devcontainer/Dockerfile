FROM mcr.microsoft.com/devcontainers/base:ubuntu-24.04

ARG MISE_VERSION=2026.7.18

RUN <<EOF
    set -e

    sudo apt-get update
    sudo apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        file \
        tar \
        xz-utils \
        findutils
    
    rm -rf /var/lib/apt/lists/*
EOF

RUN <<EOF
    set -e

    curl -fsSL https://mise.run | \
        MISE_VERSION="${MISE_VERSION}" \
        MISE_INSTALL_EXT=tar.gz \
        MISE_INSTALL_HELP=0 \
        MISE_INSTALL_PATH=/usr/local/bin/mise \
        sh
EOF
