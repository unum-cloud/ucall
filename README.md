# Uninterrupted JSON RPC

A light-weight kernel bypass library using io_uring, SIMDJSON, Amazon Ion, Nvidia UCX to achieve ultimate performance.

```sh
cmake -DCMAKE_BUILD_TYPE=Debug -B ./build_debug && make -j8 --silent -C ./build_debug
```

## Typical Results

| Setup                      | Linux Server | Apple Macbook |
| :------------------------- | ------------ | ------------- |
| Fast API REST              | 995 micros   |               |
| Fast API WebSocket         | 896 micros   |               |
| Fast API Reusing WebSocket | 103 micros   |               |
| UJRPC                      | 55 micros    |               |


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