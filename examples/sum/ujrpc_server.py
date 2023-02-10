import ujrpc

def sum(a:int, b:int):
    return a+b

srv = ujrpc.Server(8545, 1)
res = srv.add_procedure(sum)
srv.run(100000, 100000)