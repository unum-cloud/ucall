import ujrpc.uring as ujrpc
import random

server = ujrpc.Server(port=8545)


@server.route
def sum(a: int, b: int):
    return a+b


@server.route
def count_of(data: bytes):
    return data


@server.route
def transform(age: float, name: str, value: int, identity: bytes):
    if age < 15:
        return False

    if age >= 15 and age < 19:
        return (False, f'{name} must be older than 19')

    new_identity = identity.decode() + f'_{name}'

    return {'name': name,
            'pins': [random.random()*age for _ in range(round(age))],
            'val': {'len': value,
                    'identity': new_identity.encode(),
                    'data': [random.randbytes(value) for _ in range(value)]}
            }


server.run()
