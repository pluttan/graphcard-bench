#!/bin/bash
# mg.sh start|stop|status: Memgraph from its .deb unpacked into MG_ROOT, no root, no system install.
#   MG_ROOT  unpacked package (default ./memgraph)    MG_HOME  data, logs, modules (default ./run)
#   MG_PORT  Bolt port on 127.0.0.1 (default 7687)
MG_ROOT=$(realpath -m "${MG_ROOT:-memgraph}")
MG_HOME=$(realpath -m "${MG_HOME:-run}")
MG_PORT=${MG_PORT:-7687}
MG=$MG_ROOT/usr/lib/memgraph
case "$1" in
start)
    if [ -f "$MG_HOME/memgraph.pid" ] && kill -0 "$(cat "$MG_HOME/memgraph.pid")" 2>/dev/null; then
        echo "memgraph already up, pid $(cat "$MG_HOME/memgraph.pid")"
        exit 0
    fi
    mkdir -p "$MG_HOME/db" "$MG_HOME/modules" "$MG_HOME/log"
    LD_LIBRARY_PATH=$MG nohup "$MG/memgraph" \
        --bolt-address=127.0.0.1 --bolt-port="$MG_PORT" \
        --data-directory="$MG_HOME/db" --log-file="$MG_HOME/log/memgraph.log" --log-level=WARNING \
        --query-modules-directory="$MG_HOME/modules,$MG/query_modules" \
        --telemetry-enabled=false --storage-properties-on-edges=true \
        --storage-snapshot-on-exit=false --storage-wal-enabled=false --storage-snapshot-interval-sec=0 \
        --query-execution-timeout-sec=3600 \
        > "$MG_HOME/log/stdout.log" 2>&1 &
    echo $! > "$MG_HOME/memgraph.pid"
    until (exec 3<>"/dev/tcp/127.0.0.1/$MG_PORT") 2>/dev/null; do
        kill -0 "$(cat "$MG_HOME/memgraph.pid")" 2>/dev/null || { echo "memgraph died:"; tail -5 "$MG_HOME/log/stdout.log"; exit 1; }
        sleep 0.5
    done
    echo "memgraph up on 127.0.0.1:$MG_PORT, pid $(cat "$MG_HOME/memgraph.pid")"
    ;;
stop)
    if [ -f "$MG_HOME/memgraph.pid" ]; then
        p=$(cat "$MG_HOME/memgraph.pid")
        kill "$p" 2>/dev/null
        for _ in $(seq 1 120); do kill -0 "$p" 2>/dev/null || break; sleep 0.5; done
        rm -f "$MG_HOME/memgraph.pid"
        echo stopped
    fi
    ;;
status)
    [ -f "$MG_HOME/memgraph.pid" ] && kill -0 "$(cat "$MG_HOME/memgraph.pid")" 2>/dev/null \
        && echo "up, pid $(cat "$MG_HOME/memgraph.pid")" || echo down
    ;;
*)
    sed -n 2,4p "$0"
    ;;
esac
