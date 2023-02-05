from concurrent import futures

import grpc
import grpc_schema_pb2_grpc as pb2_grpc
import grpc_schema_pb2 as pb2


class gRPCService(pb2_grpc.gRPCServicer):

    def __init__(self, *args, **kwargs):
        pass

    def GetServerResponse(self, request, context):
        return pb2.SumResponse(c=(request.a + request.b))


def serve():
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=10))
    pb2_grpc.add_gRPCServicer_to_server(gRPCService(), server)
    server.add_insecure_port('[::]:50051')
    server.start()
    server.wait_for_termination()


if __name__ == '__main__':
    serve()
