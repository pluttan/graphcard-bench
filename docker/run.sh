#!/bin/bash
# Start of the graphcard-bench image: make all with the arguments of docker run, make gpu when the
# container sees a GPU, then the results archive and the log into /out.
# Their names carry the start time: a second run into the same /out keeps the first one, and a
# failed pack copies nothing over a good archive.
# The C++ parts are built here, not in the image: -march=native follows the host that runs them.
set -uo pipefail
cd /gcb
log=
tee_pid=
stamp=$(date -u +%Y%m%d-%H%M%S)
if [ -d /out ]; then
    log=/out/run-$(hostname)-$stamp.log
    exec > >(tee "$log") 2>&1
    tee_pid=$!
fi

make all "$@"
rc=$?
if [ $rc -eq 0 ] && nvidia-smi -L >/dev/null 2>&1; then
    make gpu "$@" || echo "make gpu failed: the CPU results stay"
fi
make -s stop
packed=0
make pack "$@" && packed=1 || rc=1

if [ -d /out ]; then
    out=
    if [ $packed -eq 1 ]; then
        for f in results-*.tgz; do
            cp "$f" "/out/${f%.tgz}-$stamp.tgz" && out="$out ${f%.tgz}-$stamp.tgz"
        done
    fi
    chown "$(stat -c %u:%g /out)" "$log" ${out:+$(for f in $out; do echo "/out/$f"; done)} 2>/dev/null
    echo "in /out:$out $(basename "$log")"
fi
# tee writes the last lines only after its input closes; the container ends with this shell.
if [ -n "$tee_pid" ]; then
    exec >&- 2>&-
    wait "$tee_pid"
fi
exit $rc
