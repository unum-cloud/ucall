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
    mean_latency_secs: float = 0
    total_secs: float = 0

    @property
    def failure_percent(self):
        return self.transmits_failed * 100.0 / \
            (self.transmits_success + self.transmits_failed)

    def __repr__(self) -> str:
        bandwidth = self.requests / \
            self.total_secs if self.total_secs > 0 else 0.0
        result = f'''
        - Took {self.total_secs:.1f} CPU seconds
        - Performed {self.transmits_success:,} successful transmits
        - Recorded {self.failure_percent:.3%} failed transmits
        - Mean latency is {self.mean_latency_secs * 1e6:.1f} microseconds
        - Mean bandwidth is {bandwidth:.1f} requests/s
        '''
        return I(result)


def benchmark(callable, transmits_count: int, debug: bool = False) -> Stats:
    stats = Stats()
    tasks_range = range(transmits_count)
    if debug:
        tasks_range = tqdm(tasks_range)

    for _ in range(transmits_count):
        t1 = time.monotonic_ns()
        successes = callable()
        stats.requests += successes
        t2 = time.monotonic_ns()
        stats.transmits_success += successes != 0
        stats.transmits_failed += successes == 0
        stats.total_secs += (t2 - t1) / 1.0e9

    stats.mean_latency_secs = stats.total_secs / stats.transmits_success
    return stats


async def benchmark_async(callable, *args, transmits_count: int = 10_000):
    return
    transmits_failed = 0
    transmits_success = 0

    start_time = time.time()
    for _ in range(transmits_count):
        try:
            await callable(*args)
            transmits_success += 1
        except Exception as e:
            transmits_failed += 1
    duration = time.time() - start_time
    _finalize_benchmark(transmits_success, transmits_failed, duration)


def benchmark_parallel(callable, process_count: int = 1, transmits_count: int = 100_000, debug: bool = False):

    if process_count == 1:
        return benchmark(callable=callable, transmits_count=transmits_count, debug=debug)

    transmits_success = Value('i', 0)
    transmits_failed = Value('i', 0)
    requests = Value('i', 0)
    mean_latency_secs = Value('f', 0)

    def run():
        stats = benchmark(callable, transmits_count, debug)
        transmits_success.value += stats.transmits_success
        transmits_failed.value += stats.transmits_failed
        requests.value += stats.requests
        mean_latency_secs.value += stats.mean_latency_secs

    global_start_time = time.monotonic_ns()

    procs = []
    for _ in range(process_count):
        procs.append(Process(target=run))

    [x.start() for x in procs]
    [x.join() for x in procs]

    global_end_time = time.monotonic_ns()
    total_secs = (global_end_time - global_start_time) / 1.0e9

    return Stats(
        transmits_success=transmits_success.value,
        transmits_failed=transmits_failed.value,
        requests=requests.value,
        mean_latency_secs=mean_latency_secs.value / process_count,
        total_secs=total_secs,
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
