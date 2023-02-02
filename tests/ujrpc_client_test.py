import os
import requests
import random
import socket
import json
import argparse
import errno


current_process_id = os.getpid()
pattern = '{"jsonrpc":"2.0","method":"sum","params":{"a":%i,"b":%i},"id":%i}'


def safe_call(callable):
    try:
        return callable()
    except AssertionError:
        return 0
    except Exception as e:
        return 0


def socket_is_closed(sock: socket.socket) -> bool:
    """
    Returns True if the remote side did close the connection
    """
    if sock is None:
        return True
    try:
        buf = sock.recv(1, socket.MSG_PEEK | socket.MSG_DONTWAIT)
        if buf == b'':
            return True
    except BlockingIOError as exc:
        if exc.errno != errno.EAGAIN:
            raise
    return False


def random_request(identity) -> tuple[str, int]:
    a = random.randint(1, 1000)
    b = random.randint(1, 1000)
    
    rpc = pattern % (a, b, identity)
    return rpc, a + b


def parse_response(response: bytes) -> object:
    if len(response) == 0:
        raise requests.Timeout()
    return json.loads(response.decode())


def make_socket():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect(('127.0.0.1', 1122))
    return sock


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

    def send(self, requests_batch, expected):
        if socket_is_closed(self.sock):
            self.sock = make_socket()

        jsonrpc = json.dumps(requests_batch)
        self.sock.send(jsonrpc.encode())
        self.expected = expected

    def recv(self, length):
        self.sock.settimeout(0.1)
        response_bytes = self.sock.recv(4096)
        self.sock.settimeout(None)

        response = parse_response(response_bytes)
        received_first = int(response[0]['result'])
        received_last = int(response[-1]['result'])
        assert isinstance(response, list)
        assert len(response) == length
        assert response[0]['jsonrpc']
        assert self.expected == received_first, 'Wrong sum'
        assert self.expected == received_last, 'Wrong sum'
        return length


def test_concurrent_connections(client_type=ClientSumTCP, count_connections=3, count_cycles=1000):

    clients = [client_type(identity=identity)
               for identity in range(count_connections)]

    for _ in range(count_cycles):
        random.shuffle(clients)
        for client in clients:
            client.send
        random.shuffle(clients)
        for client in clients:
            client.recv


def test_without_concurrent_connections(client_type=ClientSumTCP, count_cycles=1000):

    client = client_type(identity=0)
    for _ in range(count_cycles):
        client.send
        client.recv


def test_with_batches_wcc(client_type=ClientSumBatchesTCP, batch_size=1000, count_cycles=1000):

    client = client_type(identity=0)
    requests_batch = []

    a = random.randint(1, 1000)
    b = random.randint(1, 1000)

    for _ in range(batch_size):
        requests_batch.append({'jsonrpc': '2.0', 'method': 'sum', 'params': {'a': a, 'b': b},
                               'id': current_process_id})

    for _ in range(count_cycles):
        client.send(requests_batch=requests_batch, expected=a+b)
        client.recv(length=batch_size)


def test_with_batches_cc(client_type=ClientSumBatchesTCP, count_connections=3, batch_size=1000, count_cycles=1000):

    clients = [client_type(identity=identity)
               for identity in range(count_connections)]
    requests_batch = []

    a = random.randint(1, 1000)
    b = random.randint(1, 1000)

    for _ in range(batch_size):
        requests_batch.append({'jsonrpc': '2.0', 'method': 'sum', 'params': {'a': a, 'b': b},
                               'id': current_process_id})

    for __ in range(count_connections):
        for _ in range(count_cycles):
            random.shuffle(clients)
            for client in clients:
                client.send(
                    requests_batch=requests_batch, expected=a+b)
            random.shuffle(clients)
            for client in clients:
                client.recv(length=batch_size)


if __name__ == '__main__':

    parser = argparse.ArgumentParser()
    parser.add_argument(
        '-d', '--debug',
        help='handle exception and log progress',
        action='store_true')
    args = parser.parse_args()

    batch_size = 100
    count_cycles = 1000

    test_without_concurrent_connections(ClientSumTCP, count_cycles)
    print(f'- finished without concurrency connections test')

    for concurrency in range(2, 10):
        test_concurrent_connections(ClientSumTCP, concurrency, count_cycles)
        print(f'- finished concurrency tests with {concurrency} connections')

    test_with_batches_wcc(ClientSumBatchesTCP, batch_size, count_cycles)
    print(f'- finished batches test')

    for concurrency in range(2, 10):
        test_with_batches_cc(ClientSumBatchesTCP,
                             concurrency, batch_size, count_cycles)
        print(
            f'- finished batches with concurrency connections {concurrency} test')