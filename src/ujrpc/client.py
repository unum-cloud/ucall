from io import BytesIO
import requests
import base64
import random

import numpy as np
from PIL import Image


class Client:
    """JSON-RPC Client that uses classic sync Python `requests` to pass JSON calls over HTTP"""

    def __init__(self, uri: str = '127.0.0.1', port: int = 8545) -> None:
        self.url = f'http://{uri}:{port}/'

    def __getattr__(self, name):

        def call(*args, id=None, **kwargs):
            params = kwargs
            if len(args) != 0:
                assert len(kwargs) == 0, "Can't mix positional and keyword parameters!"
                params = args

            if id is None:
                id = random.randint(1, 2**16)

            return self.__call__({
                'method': name,
                'params': params,
                'jsonrpc': '2.0',
                'id': id,
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
                req['params'][k].save(buf, req['params'][k].format,  compression='raw', compression_level=0)
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
