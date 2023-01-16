import time
import struct
import random
import requests
from websocket import create_connection


def request_sum():
    a = random.randint(1, 1000)
    b = random.randint(1, 1000)
    result = requests.get(f'http://127.0.0.1:8000/sum?a={a}&b={b}').text
    c = int(result)
    assert a + b == c, 'Wrong sum'


def request_sum_ws(socket):
    a = random.randint(1, 1000)
    b = random.randint(1, 1000)
    socket.send_binary(struct.pack('<II', a, b))
    result = socket.recv()
    c = struct.unpack('<I', result)[0]
    assert a + b == c, 'Wrong sum'


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


if __name__ == '__main__':

    print('Will benchmark classic REST API')
    benchmark_request(request_sum)

    print('Will benchmark WebSockets')
    sum_socket = create_connection('ws://127.0.0.1:8000/sum-ws')
    benchmark_request(lambda: request_sum_ws(sum_socket))
    sum_socket.close()
