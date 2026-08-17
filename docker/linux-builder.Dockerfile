# Linux x86_64 builder for edgy-xrpld / edgy-xahaud.
# Same idea as xahaud/release-builder.sh (Docker in, stripped binary out)
# but GCC 15: Edgy and current rippled are C++23. xahaud's Holy Build Box
# is GCC 11 / C++20 and cannot compile this tree.
FROM gcc:15

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates \
        ccache \
        cmake \
        curl \
        git \
        ninja-build \
        pkg-config \
        python3 \
        python3-pip \
        python3-venv \
        unzip \
        wget \
    && rm -rf /var/lib/apt/lists/* \
    && pip3 install --break-system-packages 'conan>=2.17,<3'

ENV CC=gcc \
    CXX=g++ \
    CMAKE_EXE_LINKER_FLAGS="-static-libstdc++"
