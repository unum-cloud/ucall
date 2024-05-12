# UCall Examples and Benchmarks

## Echo and Summation

The `echo` example is a simple demonstration of a server that echoes back the request.
To make it a bit more challenging, the same server also contains the `sum` endpoint, which accepts two numbers and returns their aggregate.

- `echo_server_ucall.cpp` - UCall server implementation in C++
- `echo_server_ucall.py` - UCall server implementation in Python
- `echo_server_fastapi.py` - UCall server implementation in FastAPI
- `echo_client.py` - shared client implementation for benchmarking

To run those:

```sh

```

## Redis

Redis example is a basic in-memory key-value store implementation with just two endpoints - `set` and `get`.
The server implementations are available in C++ and Python for UCall.

- `redis_server_ucall.cpp` - UCall server implementation in C++
- `redis_server_ucall.py` - UCall server implementation in Python
- `redis_server_fastapi.py` - UCall server implementation in FastAPI
- `redis_client.py` - shared client implementation for benchmarking

## Multimedia

Multimedia example showcases the more edvanced UCall functionality, like built-in support for NumPy arrays and Pillow images.
It also uses batch endpoints for AI inference and image processing, the functionality not available in FastAPI.

- `multimedia_server_ucall.cpp` - UCall server implementation in C++
- `multimedia_server_ucall.py` - UCall server implementation in Python
- `multimedia_client.py` - shared client implementation for benchmarking


===

The simplest possible endpoint after `hello-world` and `echo`, is probably `sum`.
We would just accept two numbers and return their aggregate.
Packets are tiny, so it is great for benchmarking the request latency.

## Reproducing Benchmarks

### FastAPI

```sh
pip install uvicorn fastapi websocket-client requests tqdm fire
cd examples && uvicorn sum.fastapi_server:app --log-level critical &
cd ..
python examples/bench.py "fastapi_client.ClientREST" --progress
python examples/bench.py "fastapi_client.ClientWebSocket" --progress
kill %%
```

Want to dispatch more clients and aggregate statistics?

```sh
python examples/bench.py "fastapi_client.ClientREST" --threads 8
python examples/bench.py "fastapi_client.ClientWebSocket" --threads 8
```

### UCall

UCall can produce both a POSIX compliant old-school server, and a modern `io_uring`-based version for Linux kernel 5.19 and newer.
You would either run `ucall_example_sum_posix` or `ucall_example_sum_uring`.

```sh
sudo apt-get install cmake g++ build-essential
cmake -DCMAKE_BUILD_TYPE=Release -B build_release
cmake --build build_release --config Release
build_release/build/bin/ucall_example_sum_posix &
build_release/build/bin/ucall_example_sum_uring &
python examples/bench.py "jsonrpc_client.CaseTCP" --progress
python examples/bench.py "jsonrpc_client.CaseHTTP" --progress
python examples/bench.py "jsonrpc_client.CaseHTTPBatches" --progress
kill %%
```

Want to customize server settings?

```sh
build_release/build/bin/ucall_example_sum_uring --nic=127.0.0.1 --port=8545 --threads=16 --silent=false
```

Want to dispatch more clients and aggregate more accurate statistics?

```sh
python examples/bench.py "jsonrpc_client.CaseTCP" --threads 32 --seconds 100
python examples/bench.py "jsonrpc_client.CaseHTTP" --threads 32 --seconds 100
python examples/bench.py "jsonrpc_client.CaseHTTPBatches" --threads 32 --seconds 100
```

A lot has been said about the speed of Python code ~~or the lack of~~.
To get more accurate numbers for mean request latency, you can use wrk for html requests

```sh
wrk -t1 -c32 -d2s http://localhost:8545/ -s json.lua
wrk -t1 -c32 -d2s http://localhost:8545/ -s json16.lua
```

Or the GoLang version for jsonRPC requests:

```sh
go run ./examples/login/jsonrpc_client.go -h
go run ./examples/login/jsonrpc_client.go -b 100
```

Or push it even further dispatching dozens of processes with GNU `parallel` utility:

```sh
sudo apt install parallel
parallel go run ./examples/login/jsonrpc_client.go run ::: {1..32}
```

### gRPC Results

```sh
pip install grpcio grpcio-tools
python ./examples/login/grpc_server.py &
python examples/bench.py "grpc_client.ValidateClient" --progress
python examples/bench.py "grpc_client.ValidateClient" --threads 32
kill %%
```
