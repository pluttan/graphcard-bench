#!/bin/bash
# prep.sh RAW OUT: LDBC FinBench raw tables (RAW = sfN/raw) into the files load.py reads.
# account.csv (id|isBlocked), transfer.csv and withdraw.csv (fromId|toId|createTime|amount) are the
# parts glued under one header; remap.awk then numbers the accounts from 0 into *2.csv, because
# toInteger of Memgraph 3.12 loses the 19-digit ids above 2^53.
set -euo pipefail
raw=$(realpath "$1")
out=$2
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
cd "$out"
glue() {  # glue TABLE FIELDS HEADER
    { echo "$3"; for f in "$raw/$1"/part-*.csv; do tail -n +2 "$f" | cut -d'|' -f"$2"; done; } > "$1.csv"
}
glue account 1,4 "id|isBlocked"
glue transfer 1,2,4,6 "fromId|toId|createTime|amount"
glue withdraw 1,2,6,8 "fromId|toId|createTime|amount"
awk -f "$here/remap.awk" account.csv transfer.csv withdraw.csv
wc -l account2.csv transfer2.csv withdraw2.csv
