// bw: memory read bandwidth with T threads, each summing its own buffer (first touch in the thread).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
int main(int argc, char** argv) {
    int T = atoi(argv[1]);
    // 256 MB a thread, but not over 16 GB in all: the buffers must not fit any cache.
    size_t n = std::min<size_t>(256ull << 20, (16ull << 30) / T) / 8;
    std::vector<std::vector<unsigned long>> buf(T);
    std::vector<std::thread> pool;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<unsigned long> sink{0};
    for (int t = 0; t < T; ++t)
        pool.emplace_back([&, t] {
            buf[t].assign(n, t + 1);
            ++ready;
            while (!go) {}
            unsigned long s = 0;
            for (int r = 0; r < 4; ++r)
                for (size_t i = 0; i < n; ++i) s += buf[t][i];
            sink += s;
        });
    while (ready < T) {}
    auto t0 = std::chrono::steady_clock::now();
    go = true;
    for (auto& th : pool) th.join();
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("threads %3d  read %7.1f GB/s\n", T, 4.0 * T * n * 8 / s / 1e9);
}
