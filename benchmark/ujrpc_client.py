import os
import requests
import random
import socket
import json
import time

import fire

from benchmark import benchmark_parallel, socket_is_closed, safe_call

current_process_id = os.getpid()
pattern = '{"jsonrpc":"2.0","method":"sum","params":{"a":%i,"b":%i},"id":%i}'
ip = '127.0.0.1' # For default interface
# ip = '192.168.5.9' # For InfiniBand


def random_request(identity) -> tuple[str, int]:
    a = random.randint(1, 1000)
    b = random.randint(1, 1000)
    # Using such strings is much faster than JSON package
    # rpc = json.dumps({
    #     'method': 'sum',
    #     'params': {'a': a, 'b': b},
    #     'jsonrpc': '2.0',
    #     'id': identity,
    # })
    rpc = pattern % (a, b, identity)
    return rpc, a + b


def parse_response(response: bytes) -> object:
    if len(response) == 0:
        raise requests.Timeout()
    # return json.JSONDecoder().raw_decode(response.decode())[0]
    return json.loads(response.decode())


def make_socket():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((ip, 8545))
    return sock


def request_sum_pythonic():
    jsonrpc, expected = random_request(current_process_id)
    response = requests.get('http://127.0.0.1:8545/', json=jsonrpc).json()
    received = int(response['result'])

    assert response['jsonrpc']
    assert response['id'] == current_process_id
    assert expected == received, 'Wrong sum'
    return 1


class ClientSumHTTP:
    def __init__(self, sock: socket.socket = None, identity: int = current_process_id) -> None:
        self.sock = sock
        self.identity = identity
        self.expected = -1

    def __call__(self) -> int:
        self.send()
        return self.recv()

    def send(self):
        if socket_is_closed(self.sock):
            self.sock = make_socket()

        jsonrpc, expected = random_request(self.identity)
        lines = [
            'POST / HTTP/1.1',
            'Content-Type: application/json-rpc',
            f'Content-Length: {len(jsonrpc)}',
        ]
        request = '\r\n'.join(lines) + '\r\n\r\n' + jsonrpc
        self.sock.send(request.encode())
        self.expected = expected

    def recv(self):
        self.sock.settimeout(0.01)
        response_bytes = self.sock.recv(4096)
        self.sock.settimeout(None)

        response = parse_response(response_bytes)
        received = int(response['result'])
        assert response['jsonrpc']
        assert response['id'] == self.identity
        assert self.expected == received, 'Wrong sum'
        return 1


class ClientSumTCP:
    def __init__(self, sock: socket.socket = None, identity: int = current_process_id) -> None:
        self.sock = sock
        self.identity = identity
        self.expected = -1

    def __call__(self) -> int:
        self.send()
        return self.recv()

    def send(self):
        if socket_is_closed(self.sock):
            self.sock = make_socket()

        jsonrpc, expected = random_request(self.identity)
        self.sock.send(jsonrpc.encode())
        self.expected = expected

    def recv(self):
        self.sock.settimeout(0.01)
        response_bytes = self.sock.recv(4096)
        self.sock.settimeout(None)

        response = parse_response(response_bytes)
        received = int(response['result'])
        assert response['jsonrpc']
        assert response['id'] == self.identity
        assert self.expected == received, 'Wrong sum'
        return 1


class ClientSumBatchesTCP:

    def __init__(self, sock: socket.socket = None, identity: int = current_process_id) -> None:
        self.sock = sock
        self.identity = identity
        self.expected = -1

    def __call__(self) -> int:
        self.send()
        return self.recv()

    def send(self):
        if socket_is_closed(self.sock):
            self.sock = make_socket()

        a = random.randint(1, 1000)
        b = random.randint(1, 1000)
        requests_batch = [
            {'method': 'sum', 'params': {'a': a, 'b': b},
                'jsonrpc': '2.0', 'id': current_process_id, },
            {'method': 'sum', 'params': {'a': a, 'b': b}, 'jsonrpc': '2.0'},
            {'method': 'sumsum', 'params': {'a': a, 'b': b},
                'jsonrpc': '2.0', 'id': current_process_id, },
            {'method': 'sum', 'params': {}, 'jsonrpc': '2.0',
                'id': current_process_id, },
            {'method': 'sum', 'params': {'a': a, 'b': b},
                'jsonrpc': '1.0', 'id': current_process_id, },
            {'method': 'sum', 'params': {'aa': a, 'bb': b},
                'jsonrpc': 2.0, 'id': current_process_id, },
            {'id': current_process_id, },
            {'method': 'sum', 'params': {'a': a, 'b': b},
                'jsonrpc': '2.0', 'id': current_process_id, },
        ]
        jsonrpc = json.dumps(requests_batch)
        self.sock.send(jsonrpc.encode())
        self.expected = a + b

    def recv(self):
        self.sock.settimeout(0.01)
        response_bytes = self.sock.recv(4096)
        self.sock.settimeout(None)

        response = parse_response(response_bytes)
        received_first = int(response[0]['result'])
        received_last = int(response[-1]['result'])
        assert isinstance(response, list)
        assert len(response) == 7
        assert response[0]['jsonrpc']
        assert self.expected == received_first, 'Wrong sum'
        assert self.expected == received_last, 'Wrong sum'
        return 7


def test_concurrency(client_type=ClientSumTCP, count_connections=3, count_cycles=1000):

    clients = [client_type(identity=identity)
               for identity in range(count_connections)]
    for _ in range(count_cycles):
        random.shuffle(clients)
        for client in clients:
            safe_call(client.send)
        random.shuffle(clients)
        for client in clients:
            safe_call(client.recv)


def test(threads: int = 100):
    for concurrency in range(2, threads):
        test_concurrency(ClientSumTCP, count_connections=concurrency, count_cycles=1000)
        print(f'- finished concurrency tests with {concurrency} connections')


def benchmark(debug: bool = False):
    processes_range = [1, 2, 4, 8, 16]
    transmits_count = 1_000 if debug else 100_000

    # Testing TCP connection
    for process_count in processes_range:
        print('TCP on %i processes' % process_count)
        stats = benchmark_parallel(
            lambda: ClientSumTCP()(),
            process_count=process_count,
            transmits_count=transmits_count,
            debug=debug,
        )
        print(stats)

    # Testing reusable TCP connection
    for process_count in processes_range:
        print('TCP Reusing on %i processes' % process_count)
        stats = benchmark_parallel(
            ClientSumTCP(),
            process_count=process_count,
            transmits_count=transmits_count,
            debug=debug,
        )
        print(stats)

    # Testing reusable TCP connection with batched requests
    for process_count in processes_range:
        print('TCP Reusing Batch on %i processes' % process_count)
        stats = benchmark_parallel(
            ClientSumBatchesTCP(),
            process_count=process_count,
            transmits_count=transmits_count,
            debug=debug,
        )
        print(stats)


def run(seconds: int = 100, batch: bool = False):
    client = ClientSumTCP() if not batch else ClientSumBatchesTCP()
    start_time = time.time()
    while time.time() - start_time < seconds:
        safe_call(client)


if __name__ == '__main__':
    fire.Fire({
        'test': test,
        'benchmark': benchmark,
        'run': run,
    })
