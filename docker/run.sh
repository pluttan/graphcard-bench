#!/bin/bash
# Start of the graphcard-bench image: make all with the arguments of docker run, make gpu when the
# container sees a GPU, then the results archive and the log into /out.
# The C++ parts are built here, not in the image: -march=native follows the host that runs them.
set -uo pipefail
cd /gcb
log=
tee_pid=
if [ -d /out ]; then
    log=/out/run-$(hostname).log
    exec > >(tee "$log") 2>&1
    tee_pid=$!
fi

make all "$@"
rc=$?
if [ $rc -eq 0 ] && nvidia-smi -L >/dev/null 2>&1; then
    make gpu "$@" || echo "make gpu failed: the CPU results stay"
fi
make -s stop
make pack "$@" || rc=1

if [ -d /out ]; then
    cp results-*.tgz /out/ 2>/dev/null
    chown "$(stat -c %u:%g /out)" /out/results-*.tgz "$log" 2>/dev/null
    echo "in /out: $(cd /out && ls results-*.tgz "$(basename "$log")" 2>/dev/null | tr '\n' ' ')"
fi
# tee writes the last lines only after its input closes; the container ends with this shell.
if [ -n "$tee_pid" ]; then
    exec >&- 2>&-
    wait "$tee_pid"
fi
exit $rc
