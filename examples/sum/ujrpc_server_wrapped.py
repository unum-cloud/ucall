import numpy as np
from ujrpc.posix_server import PyServer

server = PyServer(port=8545)


@server
def mul(a: np.ndarray, b: np.ndarray):
    return a * b


@server
def sum(a: int, b: int):
    return a+b


if __name__ == '__main__':
    server.run()
