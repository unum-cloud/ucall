import socket
import errno
import time
from multiprocessing import Process, Value

from tqdm import tqdm


def _do_benchmark(callable, count_cycles: int, debug: bool = False):
    transmits = 0
    failures = 0
    if debug:
        for _ in tqdm(range(count_cycles)):
            callable()
            transmits += 1
    else:
        for _ in range(count_cycles):
            try:
                callable()
                transmits += 1
            except:
                failures += 1
    return (transmits, failures)


def _finalize_benchmark(transmits, failures, duration, threads=1):
    failures_p = failures * 100.0 / (transmits + failures)
    latency = ((duration * 1_000_000 / (transmits / threads))
               if transmits else float('inf'))
    speed = transmits / duration

    print(f'- Took {duration:.1f} seconds')
    print(f'- Performed {transmits:,} transmissions')
    print(f'- Recorded {failures_p:.3%} failures')
    print(f'- Mean latency is {latency:.1f} microseconds')
    print(f'- Mean speed is {speed:.1f} requests/s')


def benchmark_request_single(callable, count_cycles: int = 100_000, debug: bool = False):
    start_time = time.time()
    transmits, failures = _do_benchmark(callable, count_cycles, debug)
    duration = time.time() - start_time
    _finalize_benchmark(transmits, failures, duration)


def benchmark_request(callable, process_cnt: int = 1, count_cycles: int = 100_000, debug: bool = False):
    transmits = Value('i', 0)
    failures = Value('i', 0)

    def run():
        t, f = _do_benchmark(callable, count_cycles//process_cnt, debug)
        transmits.value += t
        failures.value += f

    procs = []
    for _ in range(process_cnt):
        procs.append(Process(target=run))

    start_time = time.time()
    [x.start() for x in procs]
    [x.join() for x in procs]
    duration = time.time() - start_time

    _finalize_benchmark(transmits.value, failures.value, duration, process_cnt)


async def benchmark_request_async(callable, *args, count_cycles: int = 10_000):
    failures = 0
    transmits = 0

    start_time = time.time()
    for _ in range(count_cycles):
        try:
            await callable(*args)
            transmits += 1
        except Exception as e:
            failures += 1
    duration = time.time() - start_time

    _finalize_benchmark(transmits, failures, duration)


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
