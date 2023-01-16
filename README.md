# Uninterrupted JSON RPC

A light-weight kernel bypass library using io_uring, SIMDJSON, Amazon Ion, Nvidia UCX to achieve ultimate performance.



## Typical Results

| Setup              | Linux Server | Apple Macbook |
| :----------------- | ------------ | ------------- |
| Fast API REST      | 950 micros   |               |
| Fast API WebSocket | 103 micros   |               |


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