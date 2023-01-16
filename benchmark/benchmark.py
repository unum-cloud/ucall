import time

def benchmark_request(callable, count_cycles=10_000):
    start_time = time.time()
    failures = 0
    transmits = 0

    for _ in range(count_cycles):
        try:
            callable()
            transmits += 1
        except:
            failures += 1
    duration = time.time() - start_time
    failures_p = failures * 100.0 / (transmits + failures)
    latency = duration * 1_000_000 / (transmits)

    print(f'- Took {duration:.1f} seconds')
    print(f'- Performed {transmits:,} transmissions')
    print(f'- Recorded {failures_p:.3%} failures')
    print(f'- Mean latency is {latency:.1f} microseconds')