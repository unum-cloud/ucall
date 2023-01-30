#!/usr/bin/env python3
import struct
import random

# Dependencies
import requests
from websocket import create_connection

import benchmark


def request_sum():
    a = random.randint(1, 1000)
    b = random.randint(1, 1000)
    result = requests.get(f'http://127.0.0.1:8000/sum?a={a}&b={b}').text
    c = int(result)
    assert a + b == c, 'Wrong sum'
    return 1


def request_sum_ws_reusing(socket):
    a = random.randint(1, 1000)
    b = random.randint(1, 1000)
    socket.send_binary(struct.pack('<II', a, b))
    result = socket.recv()
    c = struct.unpack('<I', result)[0]
    assert a + b == c, 'Wrong sum'
    return 1


def make_socket():
    return create_connection('ws://127.0.0.1:8000/sum-ws')


def request_sum_ws():
    socket = make_socket()
    request_sum_ws_reusing(socket)
    socket.close()


if __name__ == '__main__':

    for p in [1, 4]:
        print('Will benchmark classic REST API')
        benchmark.benchmark_parallel(request_sum, process_count=p)

        print('Will benchmark WebSockets')
        benchmark.benchmark_parallel(request_sum_ws, process_count=p)

        print('Will benchmark reused WebSockets')
        socket = make_socket()
        benchmark.benchmark_parallel(
            lambda: request_sum_ws_reusing(socket), process_count=p)
        socket.close()
