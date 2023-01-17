/**
 * @brief A benchmark for the construction speed of the UNSW index
 * and the resulting accuracy (recall) of the Approximate Nearest Neighbors
 * Search queries.
 */
#include <benchmark/benchmark.h>

#include "ujrpc/ujrpc.h"

namespace bm = benchmark;

static void bench_sum(bm::State& state) {

    std::size_t requests = 0;
    std::size_t failures = 0;

    for (auto _ : state) {
        ++requests;
    }

    if (!requests)
        return;

    state.counters["ops/s"] = bm::Counter(requests, bm::Counter::kIsRate);
    state.counters["stable,%"] = failures * 100.0 / requests;
}

int main(int argc, char** argv) {
    bm::RegisterBenchmark("sum", bench_sum);
    bm::Initialize(&argc, argv);
    bm::RunSpecifiedBenchmarks();
    return 0;
}