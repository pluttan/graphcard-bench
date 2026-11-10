# remap.awk: dense account numbers 0..N-1. FILENAME account.csv builds the map; edge files get
# fromId and toId replaced, edges with an unknown end are counted and dropped.
BEGIN { FS = OFS = "|"; n = 0 }
FNR == 1 { if (FILENAME ~ /account/) print "id|ext|isBlocked" > "account2.csv"; else { out = FILENAME; sub(/\.csv$/, "2.csv", out); print "fromId|toId|createTime|amount" > out }; next }
FILENAME ~ /account/ { m[$1] = n; print n, $1, $2 > "account2.csv"; n++; next }
{ if (!($1 in m) || !($2 in m)) { lost[FILENAME]++; next }; print m[$1], m[$2], $3, $4 > out }
END { for (f in lost) print f, "dropped", lost[f] > "/dev/stderr" }
