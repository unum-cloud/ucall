#!/usr/bin/env python3
import random
from typing import Optional

import grpc
from . import grpc_schema_pb2 as pb2
from . import grpc_schema_pb2_grpc as pb2_grpc


class gRPCClient:
    """
    Client for gRPC functionality
    """

    def __init__(self, uri: str = '127.0.0.1', port: int = 50051) -> None:
        self.host = uri
        self.server_port = port

        # instantiate a channel
        self.channel = grpc.insecure_channel(f'{self.host}:{self.server_port}')

        # bind the client and the server
        self.stub = pb2_grpc.gRPCStub(self.channel)

    def get_url(self, a, b):
        """
        Client function to call the rpc for GetServerResponse
        """
        result = pb2.Sum(a=a, b=b)
        return self.stub.GetServerResponse(result)

    def __call__(self, *, a: Optional[int] = None, b: Optional[int] = None) -> int:
        a = random.randint(1, 1000) if a is None else a
        b = random.randint(1, 1000) if b is None else b
        result = self.get_url(a=a, b=b)
        c = result.c
        assert a + b == c, 'Wrong sum'
        return c
