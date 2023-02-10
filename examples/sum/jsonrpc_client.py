import socket
import errno
import os
import requests
import random
import socket
import json
import string
from typing import Optional, List


# Using such strings is much faster than JSON package
# rpc = json.dumps({
#     'method': 'sum',
#     'params': {'a': a, 'b': b},
#     'jsonrpc': '2.0',
#     'id': identity,
# })
REQUEST_PATTERN = '{"jsonrpc":"2.0","id":%i,"method":"sum","params":{"a":%i,"b":%i}}'
REQUEST_BIG_PATTERN = '{"jsonrpc":"2.0","id":%i,"method":"sum","params":{"a":%i,"b":%i,"text":"%s"}}'

# The ID of the current running process, used as a default
# identifier for requests originating from here.
PROCESS_ID = os.getpid()


def make_tcp_socket(ip: str, port: int):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((ip, port))
    return sock


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
            # Raise on unknown exception
            raise
    return False


def parse_response(response: bytes) -> object:
    if len(response) == 0:
        raise requests.Timeout()
    # return json.JSONDecoder().raw_decode(response.decode())[0]
    return json.loads(response.decode())


class ClientHTTP:
    """JSON-RPC Client that uses classic sync Python `requests` to pass JSON calls over HTTP"""

    def __init__(self, uri: str = '127.0.0.1', port: int = 8545, identity: int = PROCESS_ID) -> None:
        self.identity = identity
        self.url = f'http://{uri}:{port}/'
        self.payload = ''.join(random.choices(
            string.ascii_uppercase, k=80))

    def __call__(self, *, a: Optional[int] = None, b: Optional[int] = None) -> int:

        a = random.randint(1, 1000) if a is None else a
        b = random.randint(1, 1000) if b is None else b
        jsonrpc = REQUEST_BIG_PATTERN % (self.identity, a, b, self.payload)
        expected = a + b
        response = requests.post(self.url, json=jsonrpc).json()
        received = response['result']

        assert response['jsonrpc']
        assert response.get('id', None) == self.identity
        assert expected == received, 'Wrong sum'
        return received


class ClientTCP:
    """JSON-RPC Client that operates directly over TCP/IPv4 stack, without HTTP"""

    def __init__(self, uri: str = '127.0.0.1', port: int = 8545, identity: int = PROCESS_ID) -> None:
        self.identity = identity
        self.expected = -1
        self.uri = uri
        self.port = port
        self.sock = None
        self.payload = ''.join(random.choices(
            string.ascii_uppercase, k=80))

    def __call__(self, **kwargs) -> int:
        self.send(**kwargs)
        return self.recv()

    def send(self, *, a: Optional[int] = None, b: Optional[int] = None) -> int:

        a = random.randint(1, 1000) if a is None else a
        b = random.randint(1, 1000) if b is None else b
        jsonrpc = REQUEST_BIG_PATTERN % (self.identity, a, b, self.payload)
        self.expected = a + b
        self.sock = make_tcp_socket(self.uri, self.port) if socket_is_closed(
            self.sock) else self.sock
        self.sock.send(jsonrpc.encode())

    def recv(self) -> int:
        self.sock.settimeout(0.01)
        response_bytes = self.sock.recv(4096)
        self.sock.settimeout(None)

        response = parse_response(response_bytes)
        received = response['result']
        assert response['jsonrpc']
        assert response.get('id', None) == self.identity
        assert self.expected == received, 'Wrong sum'
        return received


class ClientHTTPBatches:
    """JSON-RPC Client that uses classic sync Python `requests` to pass JSON calls over HTTP"""

    def __init__(self, uri: str = '127.0.0.1', port: int = 8545, identity: int = PROCESS_ID) -> None:
        self.identity = identity
        self.url = f'http://{uri}:{port}/'
        self.payload = ''.join(random.choices(
            string.ascii_uppercase, k=80))

    def __call__(self, a: List[int], b: List[int]) -> int:
        expected = [ai + bi for ai, bi in zip(a, b)]
        jsonrpc = [
            REQUEST_BIG_PATTERN % (self.identity, ai, bi, self.payload)
            for ai, bi in zip(a, b)]
        jsonrpc = '[%s]' % ','.join(jsonrpc)
        response = requests.post(self.url, json=jsonrpc).json()
        received = [ri['result'] for ri in response]

        assert response['jsonrpc']
        assert expected == received, 'Wrong sum'
        return received