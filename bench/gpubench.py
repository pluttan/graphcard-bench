"""gpubench: the DISC commands of the GPC bench on the GPU, with the same keys mix(i).

    gpubench.py N...

The structure is a sorted array of keys and values in GPU memory. Searches go as one batch of
queries; the set commands use a batched search of one operand in the other. Sorting and
compaction run on CUB inside cupy, so Python only launches kernels.
Every line: command, N, nanoseconds per key, millions of keys per second, and for the long
commands the mean board power and millions of keys per joule.
"""
import subprocess
import sys
import threading
import time

import cupy as cp
import numpy as np

MULT = np.uint64(0x9E3779B97F4A7C15)
HALF = np.uint64(1 << 63)


def mix(first, n):
    return cp.arange(first, first + n, dtype=cp.uint64) * MULT


def report(cmd, n, keys, seconds, watts=None):
    line = f"{cmd:<9} {n:>10} {seconds * 1e9 / keys:9.3f} ns {keys / seconds / 1e6:10.2f} Mkeys/s"
    if watts:
        line += f" {watts:6.1f} W {keys / seconds / watts / 1e6:8.2f} Mkeys/J"
    print(line, flush=True)


def best_time(f, reps=5):
    f()
    cp.cuda.Device().synchronize()
    best = 1e9
    for _ in range(reps):
        start, stop = cp.cuda.Event(), cp.cuda.Event()
        start.record()
        f()
        stop.record()
        stop.synchronize()
        best = min(best, cp.cuda.get_elapsed_time(start, stop) / 1e3)
    return best


class Power:
    """Board power from nvidia-smi every 50 ms while a command repeats."""

    def __init__(self):
        self.samples, self.on = [], True
        self.thread = threading.Thread(target=self.run, daemon=True)

    def run(self):
        while self.on:
            out = subprocess.run(["nvidia-smi", "--query-gpu=power.draw", "--format=csv,noheader,nounits"],
                                 capture_output=True, text=True).stdout
            try:
                self.samples.append(float(out.strip().splitlines()[0]))
            except (ValueError, IndexError):
                pass
            time.sleep(0.05)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.on = False
        self.thread.join()

    def mean(self):
        tail = self.samples[len(self.samples) // 4:]  # the board takes a moment to reach its load
        return sum(tail) / len(tail) if tail else None


def sustained(f, keys_per_call, seconds=3.0):
    """Repeat f for a few seconds; give the rate and the mean board power over that time."""
    f()
    cp.cuda.Device().synchronize()
    calls = 0
    with Power() as p:
        t0 = time.perf_counter()
        while time.perf_counter() - t0 < seconds:
            f()
            calls += 1
            if calls % 4 == 0:
                cp.cuda.Device().synchronize()
        cp.cuda.Device().synchronize()
        dt = time.perf_counter() - t0
    return dt / calls, p.mean(), keys_per_call


def sorted_pairs(first, n):
    k = mix(first, n)
    v = cp.arange(first, first + n, dtype=cp.uint64)
    order = cp.argsort(k)
    return k[order], v[order]


def member(x, sorted_y):
    """For every key of x: is it in sorted_y?"""
    pos = cp.searchsorted(sorted_y, x)
    pos = cp.minimum(pos, sorted_y.size - 1)
    return sorted_y[pos] == x


def bench(n):
    host = np.arange(n, dtype=np.uint64) * MULT
    dev = cp.empty(n, dtype=cp.uint64)
    report("H2D", n, n, best_time(lambda: dev.set(host)))
    report("D2H", n, n, best_time(lambda: dev.get()))
    del host, dev

    report("BUILD", n, n, best_time(lambda: sorted_pairs(0, n), reps=3))
    ks, vs = sorted_pairs(0, n)

    q = min(n, 16_000_000)
    rs = cp.random.RandomState(12345)
    hit = ks[rs.randint(0, n, size=q)]
    miss = mix(n, q)[rs.permutation(q)]

    def srch(qs):
        pos = cp.minimum(cp.searchsorted(ks, qs), n - 1)
        found = ks[pos] == qs
        return cp.where(found, vs[pos], 0)

    report("SRCH", n, q, best_time(lambda: srch(hit)))
    report("SRCHmiss", n, q, best_time(lambda: srch(miss)))
    report("NEXT", n, q, best_time(lambda: ks[cp.minimum(cp.searchsorted(ks, hit, side="right"), n - 1)]))
    per, watts, keys = sustained(lambda: srch(hit), q)
    report("SRCH", n, keys, per, watts)

    # One query at a time: the host waits for each answer, as the CPE waits for lnh64.
    one = hit[:1].copy()
    t0 = time.perf_counter()
    for i in range(2000):
        srch(one).get()
    report("SRCH1", n, 2000, time.perf_counter() - t0)

    # Set commands: A = mix(0..n-1), B = mix(n/2..3n/2-1), per input key of both operands.
    a, _ = sorted_pairs(0, n)
    b, _ = sorted_pairs(n // 2, n)
    report("AND", n, 2 * n, best_time(lambda: a[member(a, b)]))
    report("NOT", n, 2 * n, best_time(lambda: a[~member(a, b)]))
    report("OR", n, 2 * n, best_time(lambda: cp.sort(cp.concatenate((a, b[~member(b, a)])))))
    per, watts, keys = sustained(lambda: a[member(a, b)], 2 * n)
    report("AND", n, keys, per, watts)
    got = int((a > HALF).sum())
    report("GR", n, got, best_time(lambda: a[int(cp.searchsorted(a, cp.array([HALF]), side="right")[0]):].copy()))


def main():
    print(cp.cuda.runtime.getDeviceProperties(0)["name"].decode(), flush=True)
    for arg in sys.argv[1:]:
        bench(int(arg))
        cp.get_default_memory_pool().free_all_blocks()


if __name__ == "__main__":
    main()
