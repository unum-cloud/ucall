import socket
import errno
import os
import requests
import random
import socket
import json
import string
import base64
from typing import Optional, List
from ujrpc.client import Client


# Using such strings is much faster than JSON package
# rpc = json.dumps({
#     'method': 'sum',
#     'params': {'a': a, 'b': b},
#     'jsonrpc': '2.0',
#     'id': identity,
# })
REQUEST_PATTERN = '{"jsonrpc":"2.0","id":%i,"method":"sum","params":{"a":%i,"b":%i}}'
REQUEST_BIG_PATTERN = '{"jsonrpc":"2.0","id":%i,"method":"sum","params":{"a":%i,"b":%i,"text":"%s"}}'
HTTP_HEADERS = 'POST / HTTP/1.1\r\nHost: 127.0.0.1:8540\r\nUser-Agent: python-requests/2.27.1\r\nAccept-Encoding: gzip, deflate\r\nAccept: */*\r\nConnection: keep-alive\r\nContent-Length: %i\r\nContent-Type: application/json\r\n\r\n'

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


def recvall(sock, buffer_size=4096):
    data = b""
    while True:
        chunk = sock.recv(buffer_size)
        if not chunk:
            break
        data += chunk
    return data


def parse_response(response: bytes) -> object:
    if len(response) == 0:
        raise requests.Timeout()
    # return json.JSONDecoder().raw_decode(response.decode())[0]
    return json.loads(response.decode())


class ClientHTTP:
    """JSON-RPC Client that uses classic sync Python `requests` to pass JSON calls over HTTP"""

    def __init__(self, uri: str = '127.0.0.1', port: int = 8545, identity: int = PROCESS_ID) -> None:
        self.identity = identity
        self.response = None
        self.expected = 0
        self.url = f'http://{uri}:{port}/'
        self.payload = ''.join(random.choices(
            string.ascii_uppercase, k=80))

    def __call__(self, **kwargs) -> int:
        self.send(**kwargs)
        return self.recv()

    def send(self, a: Optional[int] = None, b: Optional[int] = None) -> int:

        a = random.randint(1, 1000) if a is None else a
        b = random.randint(1, 1000) if b is None else b
        json_str = REQUEST_BIG_PATTERN % (self.identity, a, b, self.payload)
        jsonrpc = json.loads(json_str)
        self.expected = a + b
        self.response = requests.post(self.url, json=jsonrpc)

    def recv(self):
        response = self.response.json()
        received = response['result']

        assert response['jsonrpc']
        assert response.get('id', None) == self.identity
        assert self.expected == received, 'Wrong sum'
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
        # self.sock.settimeout(0.01)
        response_bytes = recvall(self.sock).decode()
        self.sock.settimeout(None)
        response = json.loads(response_bytes)
        assert 'error' not in response, response['error']
        received = response['result']
        assert response['jsonrpc']
        assert response.get('id', None) == self.identity
        assert self.expected == received, 'Wrong sum'
        return received


class ClientTCPHTTP:
    """JSON-RPC Client that operates directly over TCP/IPv4 stack, with HTTP"""

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
        headers = HTTP_HEADERS % (len(jsonrpc))
        self.expected = a + b
        self.sock = make_tcp_socket(self.uri, self.port) if socket_is_closed(
            self.sock) else self.sock
        self.sock.send((headers + jsonrpc).encode())

    def recv(self) -> int:
        # self.sock.settimeout(0.01)
        response_bytes = self.sock.recv(4096).decode()
        self.sock.settimeout(None)
        response = json.loads(
            response_bytes[response_bytes.index("\r\n\r\n"):])
        assert 'error' not in response, response['error']
        received = response['result']
        assert response['jsonrpc']
        assert response.get('id', None) == self.identity
        assert self.expected == received, 'Wrong sum'
        return received


class ClientTCPHTTPBase64:
    """JSON-RPC Client that uses classic sync Python `requests` to pass JSON calls over HTTP"""

    def __init__(self, uri: str = '127.0.0.1', port: int = 8545, identity: int = PROCESS_ID) -> None:
        self.identity = identity
        self.expected = -1
        self.uri = uri
        self.port = port
        self.sock = None
        self.payload = base64.b64encode(random.randbytes(42)).decode()

    def __call__(self, **kwargs) -> int:
        self.send(**kwargs)
        return self.recv()

    def send(self):
        jsonrpc = '{"jsonrpc":"2.0","id":%i,"method":"echo","params":{"data":"%s"}}' % (
            self.identity, self.payload)
        headers = HTTP_HEADERS % (len(jsonrpc))
        self.expected = self.payload
        self.sock = make_tcp_socket(self.uri, self.port) if socket_is_closed(
            self.sock) else self.sock
        self.sock.send((headers + jsonrpc).encode())

    def recv(self) -> int:
        # self.sock.settimeout(0.01)
        response_bytes = self.sock.recv(4096).decode()
        self.sock.settimeout(None)
        response = json.loads(
            response_bytes[response_bytes.index("\r\n\r\n"):])
        assert 'error' not in response, response['error']
        received = response['result']
        assert response['jsonrpc']
        assert response.get('id', None) == self.identity
        assert self.expected == received, 'Wrong count'
        return received


class ClientHTTPBatches:
    """JSON-RPC Client that uses classic sync Python `requests` to pass JSON calls over HTTP"""

    def __init__(self, uri: str = '127.0.0.1', port: int = 8545, identity: int = PROCESS_ID) -> None:
        self.identity = identity
        self.response = None
        self.expected = None
        self.url = f'http://{uri}:{port}/'
        self.payload = ''.join(random.choices(
            string.ascii_uppercase, k=80))

    def send(self, a: List[int] = [random.randint(0, 2**32) for _ in range(random.randint(2, 50))],
             b: List[int] = [random.randint(0, 2**32) for _ in range(random.randint(2, 50))]) -> int:

        self.expected = [ai + bi for ai, bi in zip(a, b)]
        json_str = [
            REQUEST_BIG_PATTERN % (self.identity, ai, bi, self.payload)
            for ai, bi in zip(a, b)]
        jsonrpc = json.loads('[%s]' % ','.join(json_str))
        self.response = requests.post(self.url, json=jsonrpc)

    def recv(self):
        response = self.response.json()
        received = []
        for ri in response:
            received.append(ri['result'])
            assert ri['jsonrpc']

        assert self.expected == received, 'Wrong sum'
        return received


class ClientReddit:

    def __init__(self, uri: str = '127.0.0.1', port: int = 8545, identity: int = PROCESS_ID) -> None:
        self.identity = identity
        self.expected = -1
        self.client = Client(uri, port, use_http=True)
        self.bin_len = 512
        self.bin = base64.b64encode(random.randbytes(self.bin_len)).decode()
        self.text = ''.join(random.choices(
            string.ascii_uppercase, k=self.bin_len))

    def __call__(self) -> str:
        a = random.randint(1, 1000)
        b = random.randint(1, 1000)
        res = self.client.perform(a=a, b=b, bin=self.bin, text=self.text)
        assert res.json == f'{self.text}_{a*b}_{self.bin_len}'
        return res.json
