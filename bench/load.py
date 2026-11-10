"""load.py DIR: load accounts, transfers and withdrawals of LDBC FinBench into Memgraph.

DIR holds account2.csv (id|ext|isBlocked) and transfer2.csv, withdraw2.csv (fromId|toId|createTime|amount),
made from sf*/raw by remap.awk. Accounts are numbered 0..N-1 there: toInteger in Memgraph 3.12 parses
a string through a double, so the 19-digit FinBench ids above 2^53 collapse into each other. The original
id stays as the string property ext. The import runs in the analytical mode, the bench then
runs in the transactional mode, as a live database does.
"""
import os
import sys
import time

from neo4j import GraphDatabase

d = sys.argv[1]
STEPS = [
    "STORAGE MODE IN_MEMORY_ANALYTICAL",
    "MATCH (n) DETACH DELETE n",
    "CREATE INDEX ON :Account(id)",
    f'LOAD CSV FROM "{d}/account2.csv" WITH HEADER DELIMITER "|" AS row '
    'CREATE (:Account {id: toInteger(row.id), ext: row.ext, isBlocked: row.isBlocked = "true"})',
]
for edge in ("transfer", "withdraw"):
    STEPS.append(
        f'LOAD CSV FROM "{d}/{edge}2.csv" WITH HEADER DELIMITER "|" AS row '
        'MATCH (a:Account {id: toInteger(row.fromId)}), (b:Account {id: toInteger(row.toId)}) '
        f'CREATE (a)-[:{edge} {{createTime: toInteger(row.createTime), amount: toFloat(row.amount)}}]->(b)')
STEPS += ["STORAGE MODE IN_MEMORY_TRANSACTIONAL"]

drv = GraphDatabase.driver(os.environ.get("MG_URL", "bolt://127.0.0.1:" + os.environ.get("MG_PORT", "7687")), auth=None)
with drv.session() as s:
    for q in STEPS:
        t = time.perf_counter()
        s.run(q).consume()
        print(f"{time.perf_counter() - t:7.1f} s  {q[:90]}", flush=True)
    for q in ("MATCH (n:Account) RETURN count(n) AS c", "MATCH ()-[e:transfer]->() RETURN count(e) AS c",
              "MATCH ()-[e:withdraw]->() RETURN count(e) AS c"):
        print(q, "->", s.run(q).single()["c"], flush=True)
    print([r.values() for r in s.run("SHOW STORAGE INFO")][:6])
drv.close()
