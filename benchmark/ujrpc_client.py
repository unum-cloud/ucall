import requests
import random
import socket
import json
import io

from benchmark import benchmark_request



def request_sum_http():
    a = random.randint(1, 1000)
    b = random.randint(1, 1000)
    rpc = {
        "method": "sum",
        "params": {"a": a, "b":b},
        "jsonrpc": "2.0",
        "id": 0,
    }
    response = requests.get('http://127.0.0.1:8545/', json=rpc).json()
    assert response["jsonrpc"]
    assert response["id"] == 0
    c = int(response["result"])
    assert a + b == c, 'Wrong sum'


def request_sum_http_tcp(client):
    a = random.randint(1, 1000)
    b = random.randint(1, 1000)
    rpc = json.dumps({
        "method": "sum",
        "params": {"a": a, "b":b},
        "jsonrpc": "2.0",
        "id": 0,
    })
    lines = [
        'POST / HTTP/1.1',
        'Content-Type: application/json-rpc',
        f'Content-Length: {len(rpc)}',
    ]
    request = '\r\n'.join(lines) + '\r\n\r\n' + rpc
    client.send(request.encode())
    response = json.loads(client.recv(4096))
    assert response["jsonrpc"]
    assert response["id"] == 0
    c = int(response["result"])
    assert a + b == c, 'Wrong sum'

def request_sum_tcp_reusing(client):

    a = random.randint(1, 1000)
    b = random.randint(1, 1000)
    rpc = json.dumps({
        "method": "sum",
        "params": {"a": a, "b":b},
        "jsonrpc": "2.0",
        "id": 0,
    })
    client.send(rpc.encode())
    response_bytes = bytes()
    while not len(response_bytes):
        response_bytes = client.recv(4096)
    response = json.loads(response_bytes.decode())
    assert response["jsonrpc"]
    # assert response["id"] == 0 # TODO: Depends on patching
    c = int(response["result"])
    assert a + b == c, 'Wrong sum'

def make_socket():
    client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    client.connect(('127.0.0.1', 8545))
    return client

def request_sum_tcp():
    client = make_socket()
    request_sum_tcp_reusing(client)
    client.close()



if __name__ == "__main__":

    # Testing TCP connection
    benchmark_request(request_sum_tcp)

    # Testing reusable TCP connection
    # client = make_socket()
    # benchmark_request(lambda: request_sum_tcp_reusing(client))
    # client.close()

