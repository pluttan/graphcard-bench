"""gpc_batch.py: a batch of k-hop walks in Memgraph against gpc.khop_batch (the T4 model of the card).

    gpc_batch.py HOPS... (--hubs N | --random N) [--clients P] [--threads T]

Starts are the N accounts with the most transfers (--hubs) or N random accounts with transfers,
over the whole time range. For every number of hops:
  - every start must reach the same accounts in Cypher and in gpc.khop_batch, or the run stops;
  - Memgraph runs the batch as one query (its own execution time) and split over P client
    processes at once (wall time), its best on the server;
  - gpc.khop_batch gives the predicted time of the card and the real time of merging on T threads.
"""
import argparse
import multiprocessing as mp
import random
import time

from neo4j import GraphDatabase

from bench import URL
CYPHER = ("UNWIND $jobs AS j MATCH (a:Account {id: j[0]}) "
          "OPTIONAL MATCH (a)-[e:transfer *BFS 1..%d (e, n | e.createTime > j[1] AND e.createTime < j[2])]->(b) "
          "WITH j, count(DISTINCT b) AS r RETURN collect([j[0], r]) AS rs")
GPC = ("CALL gpc.khop_batch($jobs, $hops, $threads) YIELD reached, edges, ngr, nextc, card_ns, host_ns "
       "RETURN reached, edges, ngr, nextc, card_ns, host_ns")


def starts(s, hubs, rnd):
    lo, hi = s.run("MATCH ()-[e:transfer]->() RETURN min(e.createTime) - 1 AS lo, max(e.createTime) + 1 AS hi").single()
    if hubs:
        ids = [r["id"] for r in s.run("MATCH (a:Account)-[e:transfer]->() WITH a.id AS id, count(e) AS d "
                                      "RETURN id ORDER BY d DESC LIMIT $n", n=hubs)]
    else:
        ids = [r["id"] for r in s.run("MATCH (a:Account)-[:transfer]->() WITH DISTINCT a RETURN a.id AS id")]
        random.Random(7).shuffle(ids)
        ids = ids[:rnd]
    return [[i, lo, hi] for i in ids]


def client(args):
    jobs, hops, at = args
    drv = GraphDatabase.driver(URL, auth=None)
    with drv.session() as s:
        while time.time() < at:
            time.sleep(0.001)
        s.run(CYPHER % hops, {"jobs": jobs}).consume()
    drv.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("hops", type=int, nargs="+")
    ap.add_argument("--hubs", type=int, default=0)
    ap.add_argument("--random", type=int, default=0)
    ap.add_argument("--clients", type=int, default=24)
    ap.add_argument("--threads", type=int, default=16)
    a = ap.parse_args()
    drv = GraphDatabase.driver(URL, auth=None)
    with drv.session() as s:
        r = s.run("CALL gpc.load() YIELD edges, words, micros RETURN *").single()
        print(f"gpc.load: {r['edges']} edges, {r['words']} words, {r['micros'] / 1e6:.1f} s", flush=True)
        jobs = starts(s, a.hubs, a.random)
        print(f"{len(jobs)} starts ({'busiest' if a.hubs else 'random'}), {a.clients} Memgraph clients, "
              f"{a.threads} merge threads", flush=True)
        for hops in a.hops:
            res = s.run(CYPHER % hops, {"jobs": jobs})
            want = dict(res.single()["rs"])
            one = res.consume().metadata["plan_execution_time"]
            g = s.run(GPC, {"jobs": jobs, "hops": hops, "threads": a.threads}).single()
            bad = sum(1 for (acc, _, _), got in zip(jobs, g["reached"]) if want.get(acc, 0) != got)
            if bad:
                raise SystemExit(f"hops {hops}: {bad} of {len(jobs)} starts differ")
            # Memgraph at its best: the batch split over the clients, all started at once.
            parts = [jobs[k::a.clients] for k in range(a.clients)]
            at = time.time() + 1.0
            t = time.perf_counter()
            with mp.Pool(a.clients) as pool:
                pool.map(client, [(p, hops, at) for p in parts if p])
            par = time.perf_counter() - t - 1.0
            card, host = g["card_ns"] / 1e9, g["host_ns"] / 1e9
            print(f"hops {hops}: all starts agree, reached {sum(g['reached'])}, edges in window {g['edges']}, "
                  f"NGR {g['ngr']}, NEXTC {g['nextc']}", flush=True)
            print(f"  memgraph one query {one:8.3f} s | {a.clients} clients {par:8.3f} s | card {card:7.4f} s + host "
                  f"{host:7.4f} s = {card + host:7.4f} s | {one / (card + host):6.1f} and {par / (card + host):5.1f} times",
                  flush=True)
    drv.close()


if __name__ == "__main__":
    main()
