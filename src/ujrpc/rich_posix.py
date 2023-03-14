from ujrpc import posix as ujrpc
from ujrpc._server import _Server


class Server(_Server):
    def __init__(self, **kwargs):
        self.server = ujrpc.Server(**kwargs)
