"""bench.py: k-hop walks over transfers in Memgraph, in Cypher and in the khop module, on the same accounts.

    bench.py SAMPLES HOPS... [--threads T] [--params FILE | --hubs]

Start accounts: by default SAMPLES random accounts with transfers and the middle half of the time range;
--params FILE takes the first SAMPLES rows of a FinBench parameter file (ext id|startTime|endTime|...),
each with its own window; --hubs takes the SAMPLES accounts with the most outgoing transfers and the
whole time range.

For every sampled account both forms must reach the same number of accounts, or the run stops.
Prints per form and hops: median and 90th percentile of the host clock, accounts reached, edges looked at,
and for the module its own clock and edges per second. With --threads T it also runs T clients at once
and prints queries per second.
"""
import argparse
import os
import random
import statistics
import sys
import threading
import time

from neo4j import GraphDatabase

# MG_URL, or MG_PORT on 127.0.0.1, as mg.sh starts Memgraph.
URL = os.environ.get("MG_URL", "bolt://127.0.0.1:" + os.environ.get("MG_PORT", "7687"))


def cypher(hops):
    return ("MATCH (a:Account {id: $id})-[e:transfer *BFS 1.." + str(hops) +
            " (e, n | e.createTime > $t0 AND e.createTime < $t1)]->(b) RETURN count(DISTINCT b) AS reached")


MODULE = ("MATCH (a:Account {id: $id}) CALL khop.out(a, $hops, $t0, $t1) YIELD reached, edges, micros "
          "RETURN reached, edges, micros")


def window(session):
    r = session.run("MATCH ()-[e:transfer]->() RETURN min(e.createTime) AS lo, max(e.createTime) AS hi").single()
    lo, hi = r["lo"], r["hi"]
    return lo + (hi - lo) // 4, hi - (hi - lo) // 4  # the middle half of the time range


def sample(session, n, seed=7):
    ids = [r["id"] for r in session.run(
        "MATCH (a:Account)-[:transfer]->() WITH DISTINCT a RETURN a.id AS id")]
    random.Random(seed).shuffle(ids)
    return ids[:n]


def from_params(session, path, n):
    ext2id = {r["ext"]: r["id"] for r in session.run("MATCH (a:Account) RETURN a.ext AS ext, a.id AS id")}
    rows = []
    with open(path) as f:
        next(f)
        for line in f:
            ext, lo, hi = line.split("|")[:3]
            rows.append((ext2id[ext], int(lo), int(hi)))
            if len(rows) == n:
                break
    return rows


def hubs(session, n):
    lo, hi = session.run("MATCH ()-[e:transfer]->() RETURN min(e.createTime) - 1 AS lo, max(e.createTime) + 1 AS hi").single()
    ids = [r["id"] for r in session.run(
        "MATCH (a:Account)-[e:transfer]->() WITH a.id AS id, count(e) AS d RETURN id ORDER BY d DESC LIMIT $n", n=n)]
    return [(i, lo, hi) for i in ids]


def pct(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(len(xs) * p))]


def run_one(session, query, params):
    t = time.perf_counter()
    rec = session.run(query, params).single()
    return time.perf_counter() - t, rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("samples", type=int)
    ap.add_argument("hops", type=int, nargs="+")
    ap.add_argument("--threads", type=int, default=0)
    ap.add_argument("--params")
    ap.add_argument("--hubs", action="store_true")
    a = ap.parse_args()
    drv = GraphDatabase.driver(URL, auth=None)
    with drv.session() as s:
        if a.params:
            jobs = from_params(s, a.params, a.samples)
        elif a.hubs:
            jobs = hubs(s, a.samples)
        else:
            t0, t1 = window(s)
            jobs = [(i, t0, t1) for i in sample(s, a.samples)]
        print(f"accounts {len(jobs)}, first window {jobs[0][1]}..{jobs[0][2]}", flush=True)
        for hops in a.hops:
            cy, mo, inside, reached, edges = [], [], [], [], []
            for acc, t0, t1 in jobs:
                p = {"id": acc, "hops": hops, "t0": t0, "t1": t1}
                dt_c, rc = run_one(s, cypher(hops), p)
                dt_m, rm = run_one(s, MODULE, p)
                if rc["reached"] != rm["reached"]:
                    sys.exit(f"mismatch at account {acc}, hops {hops}: cypher {rc['reached']}, module {rm['reached']}")
                cy.append(dt_c), mo.append(dt_m), inside.append(rm["micros"] / 1e6)
                reached.append(rm["reached"]), edges.append(rm["edges"])
            tot_e, tot_in = sum(edges), sum(inside)
            print(f"hops {hops}: reached median {statistics.median(reached)}, max {max(reached)}; "
                  f"edges median {statistics.median(edges)}, max {max(edges)}", flush=True)
            print(f"  cypher  host median {statistics.median(cy)*1e3:8.3f} ms  p90 {pct(cy,.9)*1e3:8.3f} ms", flush=True)
            print(f"  module  host median {statistics.median(mo)*1e3:8.3f} ms  p90 {pct(mo,.9)*1e3:8.3f} ms  "
                  f"inside median {statistics.median(inside)*1e3:8.3f} ms  "
                  f"{tot_e / tot_in / 1e6 if tot_in else 0:7.2f} M edges/s inside", flush=True)
            if a.threads:
                for name, q in (("cypher", cypher(hops)), ("module", MODULE)):
                    done = [0]
                    lock = threading.Lock()

                    def worker(k):
                        with drv.session() as ws:
                            for acc, t0, t1 in jobs[k::a.threads]:
                                ws.run(q, {"id": acc, "hops": hops, "t0": t0, "t1": t1}).consume()
                                with lock:
                                    done[0] += 1

                    ts = [threading.Thread(target=worker, args=(k,)) for k in range(a.threads)]
                    t = time.perf_counter()
                    [x.start() for x in ts]
                    [x.join() for x in ts]
                    dt = time.perf_counter() - t
                    print(f"  {name:<7} {a.threads} clients: {done[0] / dt:9.1f} queries/s", flush=True)
    drv.close()


if __name__ == "__main__":
    main()
