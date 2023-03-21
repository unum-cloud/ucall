#!/usr/bin/env python3
import os
import struct
import random
import string
import base64
from typing import Optional

# Dependencies
import requests
from websocket import create_connection

# The ID of the current running proccess, used as a default
# identifier for requests originating from here.
PROCESS_ID = os.getpid()


class ClientREST:

    def __init__(self, uri: str = '127.0.0.1', port: int = 8000, identity: int = PROCESS_ID) -> None:
        self.identity = identity
        self.url = f'http://{uri}:{port}/'

    def __call__(self, *, a: Optional[int] = None, b: Optional[int] = None) -> int:
        a = random.randint(1, 1000) if a is None else a
        b = random.randint(1, 1000) if b is None else b
        result = requests.get(f'{self.url}sum?a={a}&b={b}').text
        c = int(result)
        assert a + b == c, 'Wrong sum'
        return c


class ClientRESTReddit:

    def __init__(self, uri: str = '127.0.0.1', port: int = 8000, identity: int = PROCESS_ID) -> None:
        self.identity = identity
        self.url = f'http://{uri}:{port}/'
        self.bin_len = 1500
        self.avatar = base64.b64encode(random.randbytes(self.bin_len)).decode()
        self.bio = ''.join(random.choices(
            string.ascii_uppercase, k=self.bin_len))
        self.name = 'John'

    def __call__(self) -> int:
        age = random.randint(1, 1000)
        result = requests.get(
            f'{self.url}create_user?age={age}&bio={self.bio}&name={self.name}&text={self.bio}').text
        return result


class ClientWebSocket:

    def __init__(self, uri: str = '127.0.0.1', port: int = 8000, identity: int = PROCESS_ID) -> None:
        self.identity = identity
        self.sock = create_connection(f'ws://{uri}:{port}/sum-ws')

    def __call__(self, *, a: Optional[int] = None, b: Optional[int] = None) -> int:
        a = random.randint(1, 1000) if a is None else a
        b = random.randint(1, 1000) if b is None else b
        self.sock.send_binary(struct.pack('<II', a, b))
        result = self.sock.recv()
        c = struct.unpack('<I', result)[0]
        assert a + b == c, 'Wrong sum'
        return c
