import os
import requests
import random
import socket
import json
import argparse

from benchmark import benchmark_parallel, socket_is_closed

current_process_id = os.getpid()
pattern = '{"jsonrpc":"2.0","method":"sum","params":{"a":%i,"b":%i},"id":%i}'


def random_request(identity) -> tuple[str, int]:
    a = random.randint(1, 1000)
    b = random.randint(1, 1000)
    # Using such strings is much faster than JSON package
    # rpc = json.dumps({
    #     'method': 'sum',
    #     'params': {'a': a, 'b': b},
    #     'jsonrpc': '2.0',
    #     'id': identity,
    # })
    rpc = pattern % (a, b, identity)
    return rpc.encode(), a + b


def parse_response(response: bytes) -> object:
    if len(response) == 0:
        raise requests.Timeout()
    # return json.JSONDecoder().raw_decode(response.decode())[0]
    return json.loads(response.decode())


def make_socket():
    client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    client.connect(('127.0.0.1', 8545))
    return client


def request_sum_pythonic():
    jsonrpc, expected = random_request(current_process_id)
    response = requests.get('http://127.0.0.1:8545/', json=jsonrpc).json()
    received = int(response['result'])

    assert response['jsonrpc']
    assert response['id'] == current_process_id
    assert expected == received, 'Wrong sum'
    return 1


def request_sum_http(client=None):

    if socket_is_closed(client):
        client = make_socket()

    jsonrpc, expected = random_request(current_process_id)
    lines = [
        'POST / HTTP/1.1',
        'Content-Type: application/json-rpc',
        f'Content-Length: {len(jsonrpc)}',
    ]
    request = '\r\n'.join(lines) + '\r\n\r\n' + jsonrpc
    client.send(request.encode())
    client.settimeout(0.01)
    response_bytes = client.recv(4096)
    client.settimeout(None)
    response = parse_response(response_bytes)
    received = int(response['result'])

    assert response['jsonrpc']
    assert response['id'] == current_process_id
    assert expected == received, 'Wrong sum'
    return 1


def request_sum_tcp_send(client, identity=current_process_id) -> int:

    jsonrpc, expected = random_request(identity)
    client.send(jsonrpc)
    return expected


def request_sum_tcp_recv(client, expected, identity=current_process_id):

    client.settimeout(0.01)
    response_bytes = client.recv(4096)
    client.settimeout(None)
    response = parse_response(response_bytes)
    received = int(response['result'])

    assert response['jsonrpc']
    assert response['id'] == identity
    assert expected == received, 'Wrong sum'
    return 1


def request_sum_tcp(client=None):

    if socket_is_closed(client):
        client = make_socket()

    expected = request_sum_tcp_send(client)
    return request_sum_tcp_recv(client, expected)


def request_sum_tcp_batch(client=None):

    if socket_is_closed(client):
        client = make_socket()

    a = random.randint(1, 1000)
    b = random.randint(1, 1000)
    expected = a + b
    requests_batch = [
        {'method': 'sum', 'params': {'a': a, 'b': b},
            'jsonrpc': '2.0', 'id': current_process_id, },
        {'method': 'sum', 'params': {'a': a, 'b': b}, 'jsonrpc': '2.0'},
        {'method': 'sumsum', 'params': {'a': a, 'b': b},
            'jsonrpc': '2.0', 'id': current_process_id, },
        {'method': 'sum', 'params': {}, 'jsonrpc': '2.0', 'id': current_process_id, },
        {'method': 'sum', 'params': {'a': a, 'b': b},
            'jsonrpc': '1.0', 'id': current_process_id, },
        {'method': 'sum', 'params': {'aa': a, 'bb': b},
            'jsonrpc': 2.0, 'id': current_process_id, },
        {'id': current_process_id, },
        {'method': 'sum', 'params': {'a': a, 'b': b},
            'jsonrpc': '2.0', 'id': current_process_id, },
    ]
    rpc = json.dumps(requests_batch)
    client.send(rpc.encode())
    response_bytes = bytes()
    client.settimeout(0.01)
    response_bytes = client.recv(4096)
    client.settimeout(None)
    response = parse_response(response_bytes)
    received_first = int(response[0]['result'])
    received_last = int(response[-1]['result'])

    assert isinstance(response, list)
    assert len(response) == len(requests_batch) - 1
    assert response[0]['jsonrpc']
    assert expected == received_first, 'Wrong sum'
    assert expected == received_last, 'Wrong sum'
    return len(requests_batch)


def test_concurrent_connections(count_connections=3, count_cycles=1000):
    contexts = [[make_socket(), identity, -1]
                for identity in range(count_connections)]
    for cycle in range(count_cycles):
        random.shuffle(contexts)
        for context in contexts:
            if socket_is_closed(context[0]):
                context[0] = make_socket()
            context[2] = request_sum_tcp_send(context[0], context[1])
        random.shuffle(contexts)
        for context in contexts:
            try:
                request_sum_tcp_recv(context[0], context[2], context[1])
            except Exception:
                pass
            except (requests.Timeout, socket.timeout):
                pass


if __name__ == '__main__':

    parser = argparse.ArgumentParser()
    parser.add_argument(
        '-d', '--debug',
        help='handle exception and log progress',
        action='store_true')
    args = parser.parse_args()

    processes_range = [1, 2, 4, 8, 16]
    transmits_count = 1_000 if args.debug else 100_000

    for concurrency in range(2, 100):
        test_concurrent_connections(concurrency, 1000)
        print(f'- finished concurrency tests with {concurrency} connections')

    # Testing TCP connection
    for process_count in processes_range:
        print('TCP on %i processes' % process_count)
        stats = benchmark_parallel(
            request_sum_tcp,
            process_count=process_count,
            transmits_count=transmits_count,
            debug=args.debug,
        )
        print(stats)

    # Testing reusable TCP connection
    for process_count in processes_range:
        print('TCP Reusing on %i processes' % process_count)
        client = make_socket()
        stats = benchmark_parallel(
            lambda: request_sum_tcp(client),
            process_count=process_count,
            transmits_count=transmits_count,
            debug=args.debug,
        )
        client.close()
        print(stats)

    # Testing reusable TCP connection with batched requests
    for process_count in processes_range:
        print('TCP Reusing Batch on %i processes' % process_count)
        client = make_socket()
        stats = benchmark_parallel(
            lambda: request_sum_tcp_batch(client),
            process_count=process_count,
            transmits_count=transmits_count,
            debug=args.debug,
        )
        client.close()
        print(stats)
