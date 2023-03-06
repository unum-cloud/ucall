from concurrent import futures

import grpc
import sum_pb2_grpc as pb2_grpc
import sum_pb2 as pb2


class SumServiceService(pb2_grpc.SumServiceServicer):

    def __init__(self, *args, **kwargs):
        pass

    def Sum(self, request, context):
        return pb2.SumResponse(result=(request.a + request.b))


def serve():
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=10))
    pb2_grpc.add_SumServiceServicer_to_server(SumServiceService(), server)
    server.add_insecure_port('[::]:50051')
    server.start()
    server.wait_for_termination()


if __name__ == '__main__':
    serve()
