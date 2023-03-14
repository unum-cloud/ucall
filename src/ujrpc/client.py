import requests
import numpy as np
from io import BytesIO
import base64


class HTTPClient:
    """JSON-RPC Client that uses classic sync Python `requests` to pass JSON calls over HTTP"""
    response = None

    def __init__(self, uri: str = '127.0.0.1', port: int = 8545) -> None:
        self.url = f'http://{uri}:{port}/'

    def is_numpy(self, buf):
        return buf[:6] == b'\x93NUMPY'

    def __call__(self, jsonrpc: object) -> object:
        for k, v in jsonrpc['params'].items():
            if isinstance(v, np.ndarray):
                buf = BytesIO()
                np.save(buf, jsonrpc['params'][k])
                buf.seek(0)
                jsonrpc['params'][k] = base64.b64encode(
                    buf.getvalue()).decode()

        res = requests.post(self.url, json=jsonrpc).json()

        if 'result' in res:
            try:
                bin = base64.b64decode(res['result'])
                if self.is_numpy(bin):
                    buf = BytesIO(bin)
                    res['result'] = np.load(buf, allow_pickle=True)
            except:
                pass

        return res
