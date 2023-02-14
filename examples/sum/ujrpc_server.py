import ujrpc

server = ujrpc.Server(8545, 1)

@server.add_procedure
def sum(a: int, b: int):
    return a+b

server.run()
