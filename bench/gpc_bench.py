"""gpc_bench.py: Cypher BFS against gpc.khop (the T4 model in place of the card) on the same starts.

    gpc_bench.py HOPS... [--params FILE] [--hubs N]

First gpc.load() mirrors the transfer edges. Then for every start and number of hops both must
reach the same number of accounts, or the run stops. Then one batch query per form: Memgraph
reports the execution time of Cypher; gpc.khop gives the SPE commands and the predicted time of
the card (card) plus the real time of merging the answers on the host (host).
"""
import argparse

from neo4j import GraphDatabase

from batch import CYPHER, timed
from bench import URL, from_params, hubs

ONE = ("MATCH (a:Account {id: $id})-[e:transfer *BFS 1..%d (e, n | e.createTime > $t0 AND e.createTime < $t1)]->(b) "
       "RETURN count(DISTINCT b) AS reached")
GPC_ONE = "MATCH (a:Account {id: $id}) CALL gpc.khop(a, $hops, $t0, $t1) YIELD reached RETURN reached"
GPC = ("UNWIND $jobs AS j MATCH (a:Account {id: j[0]}) CALL gpc.khop(a, $hops, j[1], j[2]) "
       "YIELD reached, edges, ngr, nextc, card_ns, host_ns "
       "RETURN sum(reached) AS reached, sum(edges) AS edges, sum(ngr) AS ngr, sum(nextc) AS nextc, "
       "sum(card_ns) AS card_ns, sum(host_ns) AS host_ns")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("hops", type=int, nargs="+")
    ap.add_argument("--params")
    ap.add_argument("--hubs", type=int, default=0)
    a = ap.parse_args()
    drv = GraphDatabase.driver(URL, auth=None)
    with drv.session() as s:
        r = s.run("CALL gpc.load() YIELD edges, accounts, words, micros RETURN *").single()
        print(f"gpc.load: {r['edges']} edges, {r['accounts']} accounts, {r['words']} words of T4, "
              f"{r['micros'] / 1e6:.1f} s", flush=True)
        jobs = [list(j) for j in (from_params(s, a.params, 10**9) if a.params else hubs(s, a.hubs))]
        print(f"{len(jobs)} start accounts", flush=True)
        for hops in a.hops:
            bad = 0
            for acc, t0, t1 in jobs:
                p = {"id": acc, "hops": hops, "t0": t0, "t1": t1}
                c = s.run(ONE % hops, p).single()["reached"]
                g = s.run(GPC_ONE, p).single()["reached"]
                if c != g:
                    bad += 1
                    if bad <= 5:
                        print(f"  mismatch at {acc}: cypher {c}, gpc {g}", flush=True)
            if bad:
                raise SystemExit(f"hops {hops}: {bad} of {len(jobs)} starts differ")
            tc, rc, _ = timed(s, CYPHER % hops, {"jobs": jobs})
            r = s.run(GPC, {"jobs": jobs, "hops": hops}).single()
            if rc["reached"] != r["reached"]:
                raise SystemExit(f"hops {hops}: batch reached {rc['reached']} against {r['reached']}")
            card, host = r["card_ns"] / 1e6, r["host_ns"] / 1e6
            print(f"hops {hops}: all {len(jobs)} starts agree, reached {r['reached']}, edges in window {r['edges']}, "
                  f"NGR {r['ngr']}, NEXTC {r['nextc']}", flush=True)
            print(f"  memgraph exec {tc * 1e3:9.2f} ms | card {card:8.2f} ms + host {host:7.2f} ms = "
                  f"{card + host:8.2f} ms | {tc * 1e3 / (card + host):6.1f} times", flush=True)
    drv.close()


if __name__ == "__main__":
    main()
