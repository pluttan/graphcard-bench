# graphcard-bench in a container: Ubuntu 24.04 with Memgraph, the toolchain and the venv inside.
# The C++ parts are built at start with -march=native, so one image runs on any x86-64 host.
#   docker build -t graphcard-bench https://github.com/pluttan/graphcard-bench.git
#   docker run --rm --hostname "$(hostname)" -v gcb-data:/gcb/data -v "$PWD":/out graphcard-bench
FROM ubuntu:24.04

ARG MG_VER=3.12.0
ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8

# The toolchain, python with venv and the tools that machine.txt reads.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl g++ make python3 python3-venv procps numactl hostname \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /gcb

# Memgraph unpacked as make deps does it, plus the libraries its package depends on. Only the
# Makefile and requirements.txt come in first: a change of the benches keeps this layer.
COPY Makefile requirements.txt ./
RUN curl -fL --retry 5 -o /tmp/mg.deb \
        https://download.memgraph.com/memgraph/v${MG_VER}/ubuntu-24.04/memgraph_${MG_VER}-1_amd64.deb \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        $(dpkg-deb -f /tmp/mg.deb Depends | sed 's/([^)]*)//g; s/|[^,]*//g; s/,/ /g') \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p memgraph && dpkg-deb -x /tmp/mg.deb memgraph && rm /tmp/mg.deb \
    && make deps MG_VER=${MG_VER}

COPY . .

# FinBench goes to a volume: the next run does not fetch it again.
VOLUME /gcb/data
ENTRYPOINT ["/gcb/docker/run.sh"]
