# graphcard-bench: Memgraph against the T4 model of the graph card on LDBC FinBench.
# `make all` installs, fetches the data, loads it and runs every bench; `make pack` packs the results.

# FinBench scale factor: 1 (0.7 GB) or 10 (2.7 GB).
SF       ?= 1
MG_VER   ?= 3.12.0
MG_PORT  ?= 7687
NPROC    := $(shell nproc)
# Memgraph clients of gpc_batch.py and merge threads of gpc.khop_batch.
CLIENTS  ?= $(NPROC)
THREADS  ?= 32
# Threads of cpubench and the largest bw run.
CPU_THREADS ?= $(NPROC)
# Priority of the benches: a shared machine keeps its other work going.
NICE     ?= nice -n 10
# Memgraph clients of parallel.py: 1, 8, 16, ... up to the cores, and the cores themselves.
SWEEP    ?= $(shell for p in 1 8 16 32 64 128 256 512; do [ $$p -lt $(NPROC) ] && printf '%s ' $$p; done; echo $(NPROC))

PYTHON   ?= $(shell command -v python3.12 || command -v python3)
VENV     := venv
PY       := $(CURDIR)/$(VENV)/bin/python
MG_ROOT  := $(CURDIR)/memgraph
MG_HOME  := $(CURDIR)/run
DATA     := $(CURDIR)/data
SFD      := $(DATA)/sf$(strip $(SF))
LOAD     := $(SFD)/load
PARAMS   := $(SFD)/params/sf$(strip $(SF))_read_params/complex_1_param.csv
OUT      := $(CURDIR)/results/$(shell hostname)-sf$(strip $(SF))
MG_INC   := $(MG_ROOT)/usr/include/memgraph
CXX      ?= g++
MODFLAGS := -std=c++20 -O3 -march=native -fPIC -shared -pthread -I$(MG_INC) -Iinclude -It4
export MG_ROOT MG_HOME MG_PORT

all: deps data module start load bench stop pack

# ==========================
# ===  Install           ===
# ==========================

deps: $(MG_ROOT)/usr/lib/memgraph/memgraph $(VENV)/.ok

# Memgraph from its .deb, unpacked here: no root and nothing outside this directory. The image
# takes it from memgraph/memgraph instead. download.memgraph.com may crawl: curl starts again from
# where it stopped, as for FinBench. The .deb of 3.12.0 lacks text_search_config.hpp of mgp.hpp,
# so include/ keeps it from the same tag of the source.
$(MG_ROOT)/usr/lib/memgraph/memgraph:
	. /etc/os-release && case "$$ID-$$VERSION_ID" in \
		ubuntu-22.04|ubuntu-24.04|debian-12) d="$$ID-$$VERSION_ID";; \
		*) echo "no Memgraph package for $$ID $$VERSION_ID: use Ubuntu 22.04, 24.04 or Debian 12"; exit 1;; esac && \
	mkdir -p $(MG_ROOT) && ok= && for i in $$(seq 200); do \
		curl -fL -C - --speed-time 30 --speed-limit 10000 -o $(MG_ROOT)/memgraph.deb.part \
			https://download.memgraph.com/memgraph/v$(MG_VER)/$$d/memgraph_$(MG_VER)-1_amd64.deb && { ok=1; break; }; \
		sleep 5; done && test -n "$$ok" && \
	dpkg-deb -x $(MG_ROOT)/memgraph.deb.part $(MG_ROOT) && rm $(MG_ROOT)/memgraph.deb.part
	LD_LIBRARY_PATH=$(MG_ROOT)/usr/lib/memgraph $(MG_ROOT)/usr/lib/memgraph/memgraph --version

# A venv even where python3-venv is missing: without pip, then get-pip.
$(VENV)/.ok: requirements.txt
	$(PYTHON) -m venv $(VENV) 2>/dev/null || { rm -rf $(VENV) && $(PYTHON) -m venv --without-pip $(VENV) && \
		curl -fsSL https://bootstrap.pypa.io/get-pip.py | $(PY); }
	$(PY) -m pip install -q -r requirements.txt
	touch $@

# ==========================
# ===  Data              ===
# ==========================

data: $(LOAD)/transfer2.csv $(PARAMS)

# The server of LDBC stalls now and then, even before the first byte: curl gives up after 30 s
# without data and starts again from where it stopped, every 5 s. The archive comes to .part and
# takes its name only when whole, so a broken run does not leave half a file for make to trust.
$(SFD)/sf$(strip $(SF)).tar.gz:
	mkdir -p $(SFD) && ok= && for i in $$(seq 200); do \
		curl -fL -C - --speed-time 30 --speed-limit 10000 -o $@.part \
			https://datasets.ldbcouncil.org/finbench/sf$(strip $(SF)).tar.gz && { ok=1; break; }; \
		sleep 5; done && test -n "$$ok" && mv $@.part $@

$(LOAD)/transfer2.csv: $(SFD)/sf$(strip $(SF)).tar.gz
	tar -C $(SFD) -xzf $< && bench/prep.sh $(SFD)/sf$(strip $(SF))/raw $(LOAD)

$(PARAMS):
	mkdir -p $(SFD)/params && curl -fL --retry 5 -o $(SFD)/params.zip \
		https://datasets.ldbcouncil.org/finbench/sf$(strip $(SF))_read_params.zip && \
	$(PY) -c "import zipfile; zipfile.ZipFile('$(SFD)/params.zip').extractall('$(SFD)/params')"

# ==========================
# ===  Memgraph           ===
# ==========================

module: $(MG_HOME)/modules/gpc.so $(MG_HOME)/modules/khop.so

MG_DEPS  := $(MG_ROOT)/usr/lib/memgraph/memgraph include/text_search_config.hpp

$(MG_HOME)/modules/gpc.so: bench/gpc_khop.cpp t4/*.cpp t4/*.hpp $(MG_DEPS)
	mkdir -p $(dir $@) && $(CXX) $(MODFLAGS) -o $@ bench/gpc_khop.cpp t4/t4store.cpp

$(MG_HOME)/modules/khop.so: bench/khop.cpp $(MG_DEPS)
	mkdir -p $(dir $@) && $(CXX) $(MODFLAGS) -o $@ bench/khop.cpp

start: deps
	$(NICE) bench/mg.sh start

stop:
	bench/mg.sh stop

load: start data
	cd bench && $(NICE) $(PY) -u load.py $(LOAD)

# ==========================
# ===  Benches           ===
# ==========================

# Everything into OUT: the machine, the CPU alone, Memgraph per query, batch and server, the card model.
# Memgraph keeps no snapshot, so the bench loads the data first.
bench: module load build/cpubench build/bw
	mkdir -p $(OUT)
	$(MAKE) -s machine > $(OUT)/machine.txt
	for t in 1 8 $$(( $(CPU_THREADS) / 2 )) $(CPU_THREADS); do $(NICE) ./build/bw $$t; done | tee $(OUT)/bw.txt
	for n in 1000000 16000000; do $(NICE) ./build/cpubench $$n $(CPU_THREADS); done | tee $(OUT)/cpubench.txt
	cd bench && $(NICE) $(PY) -u batch.py 1 2 3 --params $(PARAMS) | tee $(OUT)/batch-tcr1.txt
	cd bench && $(NICE) $(PY) -u batch.py 1 2 3 4 --hubs 100 | tee $(OUT)/batch-hubs.txt
	cd bench && $(NICE) $(PY) -u parallel.py 3 4 $(SWEEP) | tee $(OUT)/parallel-hubs3.txt
	cd bench && $(NICE) $(PY) -u gpc_bench.py 1 2 3 4 --params $(PARAMS) | tee $(OUT)/gpc-tcr1.txt
	for o in "--hubs 100" "--hubs 1000" "--random 10000"; do (cd bench && $(NICE) \
		$(PY) -u gpc_batch.py 1 2 3 4 $$o --clients $(CLIENTS) --threads $(THREADS)); done | tee $(OUT)/gpc-batch.txt
	@echo "results in $(OUT)"

machine:
	@echo "== host"; hostname; date -u
	@echo "== os"; . /etc/os-release && echo "$$PRETTY_NAME"; uname -r
	@echo "== cpu"; lscpu | grep -E "Model name|^CPU\(s\)|Socket|Thread|NUMA node\(s\)|L3|max MHz"
	@echo "== memory"; free -g | head -2
	@echo "== numa"; command -v numactl >/dev/null && numactl -H | head -4 || echo "no numactl"
	@echo "== gpu"; command -v nvidia-smi >/dev/null && nvidia-smi -L || echo "no nvidia-smi"
	@echo "== memgraph"; LD_LIBRARY_PATH=$(MG_ROOT)/usr/lib/memgraph $(MG_ROOT)/usr/lib/memgraph/memgraph --version 2>/dev/null | tail -1

build/cpubench: bench/cpubench.cpp
	mkdir -p build && $(CXX) -std=c++17 -O3 -march=native -pthread -o $@ $<

build/bw: bench/bw.cpp
	mkdir -p build && $(CXX) -O3 -march=native -pthread -o $@ $<

# The GPU alone, as on the L40: sort, batched search and AND of sorted arrays, with cupy. cupy-cuda12x
# takes the CUDA 12 runtime and NVRTC. On a driver of CUDA 12.x both follow the driver: kernels from a
# newer NVRTC do not load on an older driver. A driver of CUDA 13 and newer runs the last 12.9.
# cuRAND fills the keys; its kernels come built for every card, so any 12.x of it runs on any 12.x.
# The card with the most free memory runs it, since the others may serve other work.
DRIVER_CUDA := $(shell nvidia-smi 2>/dev/null | grep -o 'CUDA Version: [0-9.]*' | grep -o '[0-9.]*$$')
CUDA_PIN    := $(shell v=$(DRIVER_CUDA); m=$${v%%.*}; [ -n "$$m" ] && { [ $$m -eq 12 ] && echo $$v; [ $$m -gt 12 ] && echo 12.9; }; true)
GPU_FREE    := $(shell nvidia-smi --query-gpu=index,memory.free --format=csv,noheader,nounits 2>/dev/null | sort -t, -k2 -n -r | head -1 | cut -d, -f1)

gpu: $(VENV)/.ok
	@test -n "$(DRIVER_CUDA)" || { echo "no nvidia-smi: no GPU to bench"; exit 1; }
	@test -n "$(CUDA_PIN)" || { echo "the driver gives CUDA $(DRIVER_CUDA): cupy-cuda12x needs 12 or newer"; exit 1; }
	$(PY) -m pip install -q cupy-cuda12x "nvidia-cuda-runtime-cu12==$(CUDA_PIN).*" "nvidia-cuda-nvrtc-cu12==$(CUDA_PIN).*" \
		nvidia-curand-cu12
	mkdir -p $(OUT) && CUDA_VISIBLE_DEVICES=$(GPU_FREE) $(PY) -u bench/gpubench.py 1000000 16000000 128000000 | tee $(OUT)/gpu.txt

pack:
	tar -czf results-$(shell hostname)-sf$(strip $(SF)).tgz -C results $(notdir $(OUT))
	@echo "send results-$(shell hostname)-sf$(strip $(SF)).tgz"

clean: stop
	rm -rf build run results-*.tgz

distclean: clean
	rm -rf $(VENV) $(MG_ROOT) $(DATA)

.PHONY: all deps data module start stop load bench machine gpu pack clean distclean
