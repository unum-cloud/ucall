import random

from ujrpc.posix import Server

server = Server(port=8545)


@server
def sum(a: int, b: int):
    return a+b


@server
def echo(data: bytes):
    return data


@server
def transform(age: float, name: str, value: int, identity: bytes):

    if age < 15:
        return False

    if age >= 15 and age < 19:
        return (False, f'{name} must be older than 19')

    new_identity = identity.decode() + f'_{name}'

    return {
        'name': name,
        'pins': [random.random()*age for _ in range(round(age))],
        'val': {
            'len': value,
            'identity': new_identity.encode(),
            'data': [random.randbytes(value) for _ in range(value)],
        }
    }


if __name__ == '__main__':
    server.run()
