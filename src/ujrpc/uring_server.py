from ujrpc import uring as ujrpc
from ujrpc._server import _Server


class RichServer(_Server):
    def __init__(self, **kwargs):
        self.server = ujrpc.Server(**kwargs)
