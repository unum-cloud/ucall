import ujrpc


def sum(a: int, b: int):
    return a+b


if __name__ == '__main__':
    server = ujrpc.Server(8545, 1)
    server.add_procedure(sum)
    server.run(100000, 100000)
