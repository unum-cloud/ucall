import socket
import errno
import time
from multiprocessing import Process, Value
from dataclasses import dataclass
from inspect import cleandoc as I

from tqdm import tqdm


@dataclass
class Stats:
    transmits_success: int = 0
    transmits_failed: int = 0
    requests: int = 0
    latency_secs: float = 0
    total_secs: float = 0

    def __repr__(self) -> str:
        failures_p = self.transmits_failed * 100.0 / \
            (self.transmits_success + self.transmits_failed)
        bandwidth = self.requests / self.total_secs
        result = f'''
        - Took {self.total_secs:.1f} seconds
        - Performed {self.transmits_success:,} transmits_success
        - Recorded {failures_p:.3%} transmits_failed
        - Mean latency is {self.latency_secs:.1f} microseconds
        - Mean speed is {bandwidth:.1f} requests/s
        '''
        return I(result)


def benchmark(callable, count_cycles: int, debug: bool = False) -> Stats:
    stats = Stats()
    tasks_range = range(count_cycles)
    if debug:
        tasks_range = tqdm(tasks_range)

    for _ in range(count_cycles):
        t1 = time.monotonic_ns()
        successes = callable()
        stats.requests += successes
        stats.transmits_success += successes != 0
        stats.transmits_failed += successes == 0
        t2 = time.monotonic_ns()
        stats.total_secs += (t2 - t1) / 1e9

    stats.latency_secs = stats.total_secs / stats.transmits_success
    return stats


async def benchmark_async(callable, *args, count_cycles: int = 10_000):
    return
    transmits_failed = 0
    transmits_success = 0

    start_time = time.time()
    for _ in range(count_cycles):
        try:
            await callable(*args)
            transmits_success += 1
        except Exception as e:
            transmits_failed += 1
    duration = time.time() - start_time
    _finalize_benchmark(transmits_success, transmits_failed, duration)


def benchmark_parallel(callable, process_count: int = 1, count_cycles: int = 100_000, debug: bool = False):

    if process_count == 1:
        return benchmark(callable=callable, count_cycles=count_cycles, debug=debug)

    transmits_success = Value('i', 0)
    transmits_failed = Value('i', 0)
    requests = Value('i', 0)
    total_secs = Value('f', 0)
    mean_latency_secs = Value('f', 0)

    def run():
        stats = benchmark(callable, count_cycles, debug)
        transmits_success.value += stats.transmits_success
        transmits_failed.value += stats.transmits_failed
        requests.value += stats.requests
        total_secs.value += stats.total_secs
        mean_latency_secs.value += stats.latency_secs

    procs = []
    for _ in range(process_count):
        procs.append(Process(target=run))

    [x.start() for x in procs]
    [x.join() for x in procs]

    return Stats(
        transmits_success=transmits_success.value,
        transmits_failed=transmits_failed.value,
        requests=requests.value,
        latency_secs=mean_latency_secs.value / process_count,
        total_secs=total_secs.value,
    )


def socket_is_closed(sock: socket.socket) -> bool:
    """
    Returns True if the remote side did close the connection

    """
    try:
        buf = sock.recv(1, socket.MSG_PEEK | socket.MSG_DONTWAIT)
        if buf == b'':
            return True
    except BlockingIOError as exc:
        if exc.errno != errno.EAGAIN:
            # Raise on unknown exception
            raise
    return False
