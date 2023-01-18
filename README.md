# Uninterrupted JSON RPC

A light-weight kernel bypass library using io_uring, SIMDJSON, Amazon Ion, Nvidia UCX to achieve ultimate performance.

- [x] Batch requests
- [x] JSON-RPC over raw TCP sockets
- [ ] JSON-RPC over TCP with HTTP
- [ ] Concurrent sessions
- [ ] AF_XDP on Linux
- [ ] Amazon Ion support
- [ ] UDP support

```sh
cmake -DCMAKE_BUILD_TYPE=Debug -B ./build_debug && make -j8 --silent -C ./build_debug
```

## Typical Latencies

| Setup                       | Linux, Epyc | MacOS, i9 |
| :-------------------------- | ----------: | --------: |
| Fast API REST               |      995 μs |  2,854 μs |
| Fast API WebSocket          |      896 μs |  2,820 μs |
| Fast API WebSocket, reusing |      103 μs |    396 μs |
| UJRPC over HTTP             |             |           |
| UJRPC over TCP              |       55 μs |           |
| UJRPC over TCP, reusing     |             |           |

> μ stands for micro, μs subsequently means microseconds.
> First column shows timing for a server with Ubuntu 22.04, based on a 64-core AMD Epyc CPU.
> Second column shows timing of a Macbook Pro with Intel Core i9 CPU.

### Replicating Fast API Results

```sh
pip install uvicorn[standard] fastapi
pip install websocket-client requests
uvicorn benchmark.fast_api_server:app --log-level critical &
python ./benchmark/fast_api_client.py
kill %% # Kill most recent background job
```

### Replicating Py-UCX Results

```sh
conda create -y -n ucx -c conda-forge -c rapidsai ucx-proc=*=cpu ucx ucx-py python=3.9
conda activate ucx
uvicorn benchmark.fast_api_server:app --log-level critical &
python ./benchmark/fast_api_client.py
kill %% # Kill most recent background job
```

### Replicating UJRPC Results

```sh
cmake -DCMAKE_BUILD_TYPE=Release -B ./build_release && make -j8 --silent -C ./build_release
./build_release/ujrpc_server_bench &
python ./benchmark/ujrpc_client.py
kill %% # Kill most recent background job
```

## Why JSON RPC?

- Transport independant: UDP, TCP, bring what you want.
- Application layer is optional: use HTTP or not.