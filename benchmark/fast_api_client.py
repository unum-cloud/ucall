import struct
import random
import requests
from websocket import create_connection

from benchmark import benchmark_request

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





if __name__ == '__main__':

    print('Will benchmark classic REST API')
    benchmark_request(request_sum)

    print('Will benchmark WebSockets')
    sum_socket = create_connection('ws://127.0.0.1:8000/sum-ws')
    benchmark_request(lambda: request_sum_ws(sum_socket))
    sum_socket.close()
