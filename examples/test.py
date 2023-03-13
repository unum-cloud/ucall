import random
import json
import requests
import numpy as np
import pytest
import base64
from sum.jsonrpc_client import ClientTCP as SumClientTCP
from sum.jsonrpc_client import ClientHTTP as SumClientHTTP
from sum.jsonrpc_client import ClientHTTPBatches as SumClientHTTPBatches
from ujrpc.client import HTTPClient


class ClientGeneric:
    """JSON-RPC Client that uses classic sync Python `requests` to pass JSON calls over HTTP"""

    def __init__(self, uri: str = '127.0.0.1', port: int = 8545) -> None:
        self.url = f'http://{uri}:{port}/'

    def __call__(self, jsonrpc: object) -> object:
        return requests.post(self.url, json=jsonrpc).json()


def shuffled_n_identities(class_, count_clients: int = 3, count_cycles: int = 1000):

    clients = [
        class_(identity=identity)
        for identity in range(count_clients)
    ]

    for _ in range(count_cycles):
        random.shuffle(clients)
        for client in clients:
            client.send()
        random.shuffle(clients)
        for client in clients:
            client.recv()


def test_shuffled_tcp():
    for connections in range(1, 10):
        shuffled_n_identities(SumClientTCP, count_clients=connections)


def test_shuffled_http():
    for connections in range(1, 10):
        shuffled_n_identities(SumClientHTTP, count_clients=connections)


def test_shuffled_http_batches():
    for connections in range(1, 10):
        print(connections)
        shuffled_n_identities(SumClientHTTPBatches, count_clients=connections)


def test_uniform_batches():
    client = SumClientHTTPBatches()
    for batch_size in range(1, 1000):
        numbers = [random.randint(1, 1000) for _ in range(batch_size)]
        client.send(numbers, numbers)
        client.recv()


# def test_transform():
#     client = ClientGeneric()
#     identity = 'This is an identity'
#     response = client({
#         'method': 'transform',
#         'params': {'age': 20, 'name': 'Eager', 'value': 3, 'identity': base64.b64encode(identity.encode()).decode()},
#         'jsonrpc': '2.0',
#         'id': 100,
#     })
#     new_id = base64.b64decode(response['val']['identity']).decode()
#     assert new_id == identity + f'_Eager'


def test_normal():
    client = ClientGeneric()
    response = client({
        'method': 'sum',
        'params': {'a': 2, 'b': 2},
        'jsonrpc': '2.0',
        'id': 100,
    })
    assert response['result'] == 4


def test_notification():
    client = ClientGeneric()
    response = client({
        'method': 'sum',
        'params': {'a': 2, 'b': 2},
        'jsonrpc': '2.0',
    })
    assert len(response) == 0


def test_method_missing():
    client = ClientGeneric()
    response = client({
        'method': 'sumsum',
        'params': {'a': 2, 'b': 2},
        'jsonrpc': '2.0',
        'id': 0,
    })
    assert response['error']['code'] == -32601


def test_param_missing():
    client = ClientGeneric()
    response = client({
        'method': 'sum',
        'params': {'a': 2},
        'jsonrpc': '2.0',
        'id': 0,
    })
    assert response['error']['code'] == -32602


def test_param_type():
    client = ClientGeneric()
    response = client({
        'method': 'sum',
        'params': {'a': 2.0, 'b': 3.5},
        'jsonrpc': '2.0',
        'id': 0,
    })
    assert response['error']['code'] == -32602


def test_non_uniform_batch():
    a = 2
    b = 2
    r_normal = {'method': 'sum', 'params': {
        'a': a, 'b': b}, 'jsonrpc': '2.0', 'id': 0}
    r_notification = {'method': 'sum', 'params': {
        'a': a, 'b': b}, 'jsonrpc': '2.0'}

    client = ClientGeneric()
    response = client([
        r_normal,
        r_notification,
    ])


def test_numpy():
    a = np.random.randint(0, 101, size=(1, 3, 10))
    b = np.random.randint(0, 101, size=(1, 3, 10))
    res = a * b
    client = HTTPClient()
    response = client({
        'method': 'mul',
        'params': {'a': a, 'b':  b},
        'jsonrpc': '2.0',
        'id': 100,
    })
    assert isinstance(response['result'], np.ndarray)
    assert np.array_equal(response['result'], res)


if __name__ == '__main__':
    pytest.main()

    test_normal()
    test_shuffled_tcp()
    test_numpy()
    test_uniform_batches()
    test_shuffled_http_batches()
    test_shuffled_http()
    test_notification()
    test_method_missing()
    test_param_missing()
    test_param_type()
    test_non_uniform_batch()
