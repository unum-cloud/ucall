import json
import errno
import socket
import random
import base64
from io import BytesIO

import numpy as np
from PIL import Image


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


def make_tcp_socket(ip: str, port: int):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((ip, port))
    return sock


def recvall(sock, buffer_size=4096):
    data = b''
    while True:
        chunk = sock.recv(buffer_size)
        if not chunk:
            break
        data += chunk
    return data


class Client:
    """JSON-RPC Client that uses classic sync Python `requests` to pass JSON calls over HTTP"""
    uri: str
    port: int
    sock: socket
    use_http: bool
    http_template: str

    def __init__(self, uri: str = '127.0.0.1', port: int = 8545, use_http: bool = True) -> None:
        self.uri = uri
        self.port = port
        self.use_http = use_http
        self.sock = None
        self.http_template = f'POST / HTTP/1.1\r\nHost: {uri}:{port}\r\nUser-Agent: py-ujrpc\r\nAccept: */*\r\nConnection: keep-alive\r\nContent-Length: %i\r\nContent-Type: application/json\r\n\r\n'

    def __getattr__(self, name):
        def call(*args, **kwargs):
            params = kwargs
            if len(args) != 0:
                assert len(
                    kwargs) == 0, 'Can\'t mix positional and keyword parameters!'
                params = args

            return self.__call__({
                'method': name,
                'params': params,
                'jsonrpc': '2.0',
            })

        return call

    def pack(self, req):
        keys = None
        if isinstance(req['params'], dict):
            keys = req['params'].keys()
        else:
            keys = range(0, len(req['params']))

        for k in keys:
            buf = BytesIO()
            if isinstance(req['params'][k], np.ndarray):
                np.save(buf, req['params'][k])
                buf.seek(0)
                req['params'][k] = base64.b64encode(
                    buf.getvalue()).decode()

            if isinstance(req['params'][k], Image.Image):
                if not req['params'][k].format:
                    req['params'][k].format = 'tiff'
                req['params'][k].save(
                    buf, req['params'][k].format,  compression='raw', compression_level=0)
                buf.seek(0)
                req['params'][k] = base64.b64encode(
                    buf.getvalue()).decode()

        return req

    def unpack(self, bin):
        buf = BytesIO(bin)

        if bin[:6] == b'\x93NUMPY':
            return np.load(buf, allow_pickle=True)

        try:
            img = Image.open(buf)
            img.verify()
            buf.seek(0)
            return Image.open(buf)  # Must reopen after verify
        except:
            pass  # Not an Image file

        return bin

    def _send(self, json_data: dict):
        request = json.dumps(json_data)
        if self.use_http:
            request = self.http_template % (len(request)) + request

        self.sock = make_tcp_socket(self.uri, self.port) if socket_is_closed(
            self.sock) else self.sock
        self.sock.send(request.encode())

    def _recv(self):
        response_bytes = recvall(self.sock)
        response = None
        if self.use_http:
            response = json.loads(
                response_bytes[response_bytes.index(b'\r\n\r\n'):])
        else:
            response = json.loads(response_bytes)
        return response

    def __call__(self, jsonrpc: object) -> object:
        jsonrpc['id'] = random.randint(1, 2**16)
        jsonrpc = self.pack(jsonrpc)
        self._send(jsonrpc)
        res = self._recv()

        if 'result' in res:
            try:
                bin = base64.b64decode(res['result'])
                res['result'] = self.unpack(bin)
            except:
                pass

        return res
