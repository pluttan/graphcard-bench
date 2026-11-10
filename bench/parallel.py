"""parallel.py HOPS REPS P...: P client processes at once, each runs the batch walk from the 100 busiest
accounts REPS times. Prints edges per second of the whole server; a process has its own driver,
so the Python GIL does not limit the clients.
"""
import multiprocessing as mp
import sys
import time

from neo4j import GraphDatabase

from batch import CYPHER
from bench import URL, hubs


def client(args):
    jobs, hops, reps, start = args
    drv = GraphDatabase.driver(URL, auth=None)
    with drv.session() as s:
        while time.time() < start:
            time.sleep(0.001)
        for _ in range(reps):
            s.run(CYPHER % hops, {"jobs": jobs}).consume()
    drv.close()


def main():
    hops, reps = int(sys.argv[1]), int(sys.argv[2])
    drv = GraphDatabase.driver(URL, auth=None)
    with drv.session() as s:
        jobs = [list(j) for j in hubs(s, 100)]
        r = s.run("UNWIND $jobs AS j MATCH (a:Account {id: j[0]}) CALL khop.out(a, $hops, j[1], j[2]) "
                  "YIELD edges RETURN sum(edges) AS e", {"jobs": jobs, "hops": hops}).single()
        edges = r["e"]
    drv.close()
    for p in map(int, sys.argv[3:]):
        start = time.time() + 1.0
        t = time.perf_counter()
        with mp.Pool(p) as pool:
            pool.map(client, [(jobs, hops, reps, start)] * p)
        dt = time.perf_counter() - t - 1.0
        print(f"{p:3d} clients: {p * reps * edges / dt / 1e6:7.2f} M edges/s  ({p * reps} queries in {dt:.1f} s)", flush=True)


if __name__ == "__main__":
    main()
