from io import BytesIO
import requests
import base64
import random

import numpy as np
from PIL import Image


class Client:
    """JSON-RPC Client that uses classic sync Python `requests` to pass JSON calls over HTTP"""
    response = None

    def __init__(self, uri: str = '127.0.0.1', port: int = 8545) -> None:
        self.url = f'http://{uri}:{port}/'

    def __getattr__(self, name, *args, **kwargs):

        def call(id=None, **kwargs):
            if id is None:
                id = random.randint(1, 2**16)

            return self.__call__({
                'method': name,
                'params': kwargs,
                'jsonrpc': '2.0',
                'id': id,
            })

        return call

    def pack(self, jsonrpc):
        for k, v in jsonrpc['params'].items():
            buf = BytesIO()
            if isinstance(v, np.ndarray):
                np.save(buf, v)
                buf.seek(0)
                jsonrpc['params'][k] = base64.b64encode(
                    buf.getvalue()).decode()

            if isinstance(v, Image.Image):
                if not v.format:
                    v.format = 'tiff'
                v.save(buf, v.format,  compression='raw', compression_level=0)
                buf.seek(0)
                jsonrpc['params'][k] = base64.b64encode(
                    buf.getvalue()).decode()

        return jsonrpc

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

    def __call__(self, jsonrpc: object) -> object:
        jsonrpc = self.pack(jsonrpc)
        res = requests.post(self.url, json=jsonrpc).json()

        if 'result' in res:
            try:
                bin = base64.b64decode(res['result'])
                res['result'] = self.unpack(bin)
            except:
                pass

        return res
