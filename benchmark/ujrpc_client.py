import requests
import random
import socket
import json
import argparse

from benchmark import benchmark_parallel, socket_is_closed


def request_sum_http():
    a = random.randint(1, 1000)
    b = random.randint(1, 1000)
    rpc = {
        'method': 'sum',
        'params': {'a': a, 'b': b},
        'jsonrpc': '2.0',
        'id': 0,
    }
    response = requests.get('http://127.0.0.1:8545/', json=rpc).json()
    assert response['jsonrpc']
    # assert response['id'] == 0
    c = int(response['result'])
    assert a + b == c, 'Wrong sum'
    return 1


def request_sum_http_tcp(client):
    a = random.randint(1, 1000)
    b = random.randint(1, 1000)
    rpc = json.dumps({
        'method': 'sum',
        'params': {'a': a, 'b': b},
        'jsonrpc': '2.0',
        'id': 0,
    })
    lines = [
        'POST / HTTP/1.1',
        'Content-Type: application/json-rpc',
        f'Content-Length: {len(rpc)}',
    ]
    request = '\r\n'.join(lines) + '\r\n\r\n' + rpc
    client.send(request.encode())
    response = json.loads(client.recv(4096))
    assert response['jsonrpc']
    # assert response['id'] == 0
    c = int(response['result'])
    assert a + b == c, 'Wrong sum'
    return 1


def make_socket():
    client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    client.connect(('127.0.0.1', 8545))
    return client


def request_sum_tcp_reusing(client):

    if socket_is_closed(client):
        client = make_socket()

    a = random.randint(1, 1000)
    b = random.randint(1, 1000)
    rpc = json.dumps({
        'method': 'sum',
        'params': {'a': a, 'b': b},
        'jsonrpc': '2.0',
        'id': 0,
    })
    client.send(rpc.encode())
    response_bytes = bytes()
    while not len(response_bytes):
        response_bytes = client.recv(4096)
    response = json.loads(response_bytes.decode())
    assert response['jsonrpc']
    # assert response['id'] == 0 # TODO: Depends on patching
    c = int(response['result'])
    assert a + b == c, 'Wrong sum'
    return 1


def request_sum_tcp_reusing_batch(client):

    if socket_is_closed(client):
        client = make_socket()

    a = random.randint(1, 1000)
    b = random.randint(1, 1000)
    requests_batch = [
        {'method': 'sum', 'params': {'a': a, 'b': b}, 'jsonrpc': '2.0', 'id': 0, },
        {'method': 'sum', 'params': {'a': a, 'b': b}, 'jsonrpc': '2.0'},
        {'method': 'sumsum', 'params': {'a': a, 'b': b}, 'jsonrpc': '2.0', 'id': 0, },
        {'method': 'sum', 'params': {}, 'jsonrpc': '2.0', 'id': 0, },
        {'method': 'sum', 'params': {'a': a, 'b': b}, 'jsonrpc': '1.0', 'id': 0, },
        {'method': 'sum', 'params': {'aa': a, 'bb': b}, 'jsonrpc': 2.0, 'id': 0, },
        {'id': 0, },
        {'method': 'sum', 'params': {'a': a, 'b': b}, 'jsonrpc': '2.0', 'id': 0, },
    ]
    rpc = json.dumps(requests_batch)
    client.send(rpc.encode())
    response_bytes = bytes()
    while not len(response_bytes):
        response_bytes = client.recv(8192)
    response = json.loads(response_bytes.decode())
    assert isinstance(response, list) and len(
        response) == len(requests_batch) - 1  # The second request is a notification
    assert response[0]['jsonrpc']
    c = int(response[0]['result'])
    c_last = int(response[-1]['result'])
    assert a + b == c, 'Wrong sum'
    assert a + b == c_last, 'Wrong sum'
    return len(requests_batch)


def request_sum_tcp():
    client = make_socket()
    request_sum_tcp_reusing(client)
    client.close()
    return 1


if __name__ == '__main__':

    parser = argparse.ArgumentParser()
    parser.add_argument(
        '-d', '--debug',
        help='handle exception and log progress',
        action='store_true')
    args = parser.parse_args()

    for p in [1, 4]:

    # Testing TCP connection
    for p in [1, 4, 8, 16]:
        print('TCP on %i processes' % p)
        stats = benchmark_parallel(
            request_sum_tcp,
            process_count=p,
            debug=args.debug,
        )
        print(stats)

    # Testing reusable TCP connection
    for p in [1, 4, 8, 16]:
        print('TCP Reusing on %i processes' % p)
        client = make_socket()
        stats = benchmark_parallel(
            lambda: request_sum_tcp_reusing(client),
            process_count=p,
            debug=args.debug,
        )
        client.close()
        print(stats)

    # Testing reusable TCP connection with batched requests
    for p in [1]:
        print('TCP Reusing Batch on %i processes' % p)
        client = make_socket()
        stats = benchmark_parallel(
            lambda: request_sum_tcp_reusing_batch(client),
            process_count=p,
            debug=args.debug,
        )
        client.close()
        print(stats)
