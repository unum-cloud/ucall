#!/usr/bin/env python3
import random

import grpc
import grpc_pb2_grpc as pb2_grpc
import grpc_pb2 as pb2

from benchmark import benchmark_request


class gRPCClient(object):
    """
    Client for gRPC functionality
    """

    def __init__(self):
        self.host = 'localhost'
        self.server_port = 50051

        # instantiate a channel
        self.channel = grpc.insecure_channel(
            '{}:{}'.format(self.host, self.server_port))

        # bind the client and the server
        self.stub = pb2_grpc.gRPCStub(self.channel)

    def get_url(self, a, b):
        """
        Client function to call the rpc for GetServerResponse
        """
        result = pb2.Sum(a=a, b=b)
        return self.stub.GetServerResponse(result)


def request_sum(client=None):
    if not client:
        client = gRPCClient()
    a = random.randint(1, 1000)
    b = random.randint(1, 1000)
    result = client.get_url(a=a, b=b)
    c = result.c
    assert a + b == c, 'Wrong sum'


if __name__ == '__main__':

    print('Will benchmark gRPC reusing client')
    client = gRPCClient()
    benchmark_request(lambda: request_sum(client))

    print('Will benchmark gRPC recreating client')
    benchmark_request(request_sum)
