"""batch.py: many k-hop walks in one query, timed by Memgraph itself, in Cypher and in the khop module.

    batch.py HOPS... [--params FILE] [--hubs N] [--random N]

One query takes all start accounts through UNWIND, so the driver, Bolt, parsing and planning are paid
once and plan_execution_time is the walk itself. Prints per form: accounts reached, edges looked at
(from the module), execution time, and edges per second.
"""
import argparse

from neo4j import GraphDatabase

from bench import URL, from_params, hubs, sample, window

CYPHER = ("UNWIND $jobs AS j MATCH (a:Account {id: j[0]})-[e:transfer *BFS 1..%d "
          "(e, n | e.createTime > j[1] AND e.createTime < j[2])]->(b) "
          "WITH j, count(DISTINCT b) AS r RETURN sum(r) AS reached")
MODULE = ("UNWIND $jobs AS j MATCH (a:Account {id: j[0]}) CALL khop.out(a, $hops, j[1], j[2]) "
          "YIELD reached, edges RETURN sum(reached) AS reached, sum(edges) AS edges")


def timed(session, q, p, reps=3):
    best = None
    for _ in range(reps):
        r = session.run(q, p)
        rec = r.single()
        m = r.consume().metadata
        t = m["plan_execution_time"]
        if best is None or t < best[0]:
            best = (t, rec, m["parsing_time"] + m["planning_time"])
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("hops", type=int, nargs="+")
    ap.add_argument("--params")
    ap.add_argument("--hubs", type=int, default=0)
    ap.add_argument("--random", type=int, default=0)
    a = ap.parse_args()
    drv = GraphDatabase.driver(URL, auth=None)
    with drv.session() as s:
        if a.params:
            jobs = from_params(s, a.params, 10**9)
        elif a.hubs:
            jobs = hubs(s, a.hubs)
        else:
            t0, t1 = window(s)
            jobs = [(i, t0, t1) for i in sample(s, a.random)]
        jobs = [list(j) for j in jobs]
        print(f"{len(jobs)} start accounts in one query", flush=True)
        for hops in a.hops:
            tc, rc, pc = timed(s, CYPHER % hops, {"jobs": jobs})
            tm, rm, pm = timed(s, MODULE, {"jobs": jobs, "hops": hops})
            if rc["reached"] != rm["reached"]:
                raise SystemExit(f"mismatch at hops {hops}: cypher {rc['reached']}, module {rm['reached']}")
            e = rm["edges"]
            print(f"hops {hops}: reached {rm['reached']}, edges {e}", flush=True)
            print(f"  cypher  exec {tc*1e3:9.2f} ms  {e/tc/1e6:7.2f} M edges/s  parse+plan {pc*1e3:.2f} ms", flush=True)
            print(f"  module  exec {tm*1e3:9.2f} ms  {e/tm/1e6:7.2f} M edges/s  parse+plan {pm*1e3:.2f} ms", flush=True)
    drv.close()


if __name__ == "__main__":
    main()
