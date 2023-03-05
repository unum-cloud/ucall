# Summation Examples and Benchmarks

The simplest possible endpoint after `hello-world` and `echo`, is probably `sum`.
We would just accept two numbers and return their aggregate.
Packets are tiny, so it is great for benchmarking the request latency.

## Reproducing Benchmarks

### FastAPI

```sh
pip install uvicorn fastapi websocket-client requests tqdm fire
cd examples && uvicorn sum.fastapi_server:app --log-level critical &
cd ..
python examples/bench.py "sum.fastapi_client.ClientREST" --progress
python examples/bench.py "sum.fastapi_client.ClientWebSocket" --progress
kill %%
```

Want to dispatch more clients and aggregate statistics?

```sh
python examples/bench.py "sum.fastapi_client.ClientREST" --threads 8
python examples/bench.py "sum.fastapi_client.ClientWebSocket" --threads 8
```

### UJRPC

UJRPC can produce both a POSIX compliant old-school server, and a modern `io_uring`-based version for Linux kernel 5.19 and newer.
You would either run `ujrpc_example_sum_posix` or `ujrpc_example_sum_uring`.

```sh
sudo apt-get install cmake g++ build-essential
cmake -DCMAKE_BUILD_TYPE=Release -B ./build_release  && make -C ./build_release
./build_release/build/bin/ujrpc_example_sum_posix &
./build_release/build/bin/ujrpc_example_sum_uring &
python examples/bench.py "sum.jsonrpc_client.ClientTCP" --progress
python examples/bench.py "sum.jsonrpc_client.ClientHTTP" --progress
python examples/bench.py "sum.jsonrpc_client.ClientHTTPBatches" --progress
kill %%
```

Want to customize server settings?

```sh
./build_release/build/bin/ujrpc_example_sum_uring --nic=127.0.0.1 --port=8545 --threads=16 --silent=false
```

Want to dispatch more clients and aggregate more accurate statistics?

```sh
python examples/bench.py "sum.jsonrpc_client.ClientTCP" --threads 32 --seconds 100
python examples/bench.py "sum.jsonrpc_client.ClientHTTP" --threads 32 --seconds 100
python examples/bench.py "sum.jsonrpc_client.ClientHTTPBatches" --threads 32 --seconds 100
```

A lot has been said about the speed of Python code ~~or the lack of~~.
To get more accurate numbers for mean request latency, you can use the GoLang version:

```sh
go run ./examples/sum/ujrpc_client.go
```

Or push it even further dispatching dozens of processes with GNU `parallel` utility:

```sh
sudo apt install parallel
parallel go run ./examples/sum/ujrpc_client.go run ::: {1..32}
```

### gRPC Results

```sh
pip install grpcio grpcio-tools
python ./examples/sum/grpc_server.py &
python examples/bench.py "sum.grpc_client.gRPCClient" --progress
python examples/bench.py "sum.grpc_client.gRPCClient" --threads 32
kill %%
```
