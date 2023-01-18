#!/usr/bin/env python3
import struct
import random

# Dependencies
import requests
from websocket import create_connection

from benchmark import benchmark_request


def request_sum():
    a = random.randint(1, 1000)
    b = random.randint(1, 1000)
    result = requests.get(f'http://127.0.0.1:8000/sum?a={a}&b={b}').text
    c = int(result)
    assert a + b == c, 'Wrong sum'


def request_sum_ws_reusing(socket):
    a = random.randint(1, 1000)
    b = random.randint(1, 1000)
    socket.send_binary(struct.pack('<II', a, b))
    result = socket.recv()
    c = struct.unpack('<I', result)[0]
    assert a + b == c, 'Wrong sum'

def make_socket():
    return create_connection('ws://127.0.0.1:8000/sum-ws')

def request_sum_ws():
    socket = make_socket()
    request_sum_ws_reusing(socket)
    socket.close()

if __name__ == '__main__':

    print('Will benchmark classic REST API')
    benchmark_request(request_sum)

    print('Will benchmark WebSockets')
    benchmark_request(request_sum_ws)

    print('Will benchmark reused WebSockets')
    socket = make_socket()
    benchmark_request(lambda: request_sum_ws_reusing(socket))
    socket.close()
