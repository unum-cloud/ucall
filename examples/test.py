import random
import json
import requests

import pytest

from sum.jsonrpc_client import ClientTCP as SumClientTCP
from sum.jsonrpc_client import ClientHTTP as SumClientHTTP
from sum.jsonrpc_client import ClientHTTPBatches as SumClientHTTPBatches


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
    for connections in range(1, 100):
        shuffled_n_identities(SumClientTCP, count_clients=connections)


def test_shuffled_http():
    for connections in range(1, 100):
        shuffled_n_identities(SumClientHTTP, count_clients=connections)


def test_shuffled_http_batches():
    for connections in range(1, 100):
        shuffled_n_identities(SumClientHTTPBatches, count_clients=connections)


def test_uniform_batches():
    client = SumClientHTTPBatches()
    for batch_size in range(0, 1000):
        numbers = [random.randint(1, 1000) for _ in range(batch_size)]
        client(numbers, numbers)


def test_uniform_batches():
    client = SumClientHTTPBatches()
    for batch_size in range(0, 1000):
        numbers = [random.randint(1, 1000) for _ in range(batch_size)]
        client(numbers, numbers)


class ClientGeneric:
    """JSON-RPC Client that uses classic sync Python `requests` to pass JSON calls over HTTP"""

    def __init__(self, uri: str = '127.0.0.1', port: int = 8545) -> None:
        self.url = f'http://{uri}:{port}/'

    def __call__(self, jsonrpc: object) -> object:
        return requests.post(self.url, json=jsonrpc).json()


def test_normal():
    client = ClientGeneric()
    response = client({
        'method': 'sum',
        'params': {'a': 2, 'b': 2},
        'jsonrpc': '2.0',
        'id': 100,
    })
    assert response == 4


def test_notification():
    client = ClientGeneric()
    response = client({
        'method': 'sum',
        'params': {'a': 2, 'b': 2},
        'jsonrpc': '2.0',
    })
    assert response is None


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


if __name__ == '__main__':
    pytest.main()

    test_shuffled_tcp()
    test_normal()
    test_notification()
    test_method_missing()
    test_param_missing()
    test_param_type()
    test_non_uniform_batch()
