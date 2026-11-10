# graphcard-bench in a container: Ubuntu 24.04 with Memgraph, the toolchain and the venv inside.
# The C++ parts are built at start with -march=native, so one image runs on any x86-64 host.
#   docker build -t graphcard-bench https://github.com/pluttan/graphcard-bench.git#main
#   docker run --rm --init --hostname "$(hostname)" -v gcb-data:/gcb/data -v "$PWD":/out graphcard-bench
# The branch goes in the address: the legacy builder without buildx looks for master otherwise.

# Memgraph comes from its own image on Docker Hub, built on the same Ubuntu 24.04 from the same
# .deb: download.memgraph.com may crawl at a few bytes a second, Docker Hub does not.
ARG MG_VER=3.12.0
FROM memgraph/memgraph:${MG_VER} AS mg

FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8

# The toolchain, python with venv, the tools that machine.txt reads and the libraries that the
# Memgraph package depends on.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl g++ make python3 python3-venv procps numactl hostname \
        openssl libatomic1 libpython3.12t64 libseccomp2 libssl3t64 libuuid1 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /gcb

# Memgraph where make deps unpacks it, then the venv. Only the Makefile and requirements.txt come in
# first: a change of the benches keeps this layer.
COPY --from=mg /usr/lib/memgraph memgraph/usr/lib/memgraph
COPY --from=mg /usr/include/memgraph memgraph/usr/include/memgraph
COPY Makefile requirements.txt ./
RUN make deps && LD_LIBRARY_PATH=memgraph/usr/lib/memgraph memgraph/usr/lib/memgraph/memgraph --version

COPY . .

# FinBench goes to a volume: the next run does not fetch it again.
VOLUME /gcb/data
ENTRYPOINT ["/gcb/docker/run.sh"]
