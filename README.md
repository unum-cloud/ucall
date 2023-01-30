<h1 align="center">Uninterrupted JSON RPC</h1>
<h3 align="center">
Simplest Remote Procedure Calls Library<br/>
1000x Faster than FastAPI<br/>
</h3>
<br/>

<p align="center">
  <a href="https://discord.gg/xuDmpbEDnQ"><img height="25" src="https://github.com/unum-cloud/ukv/raw/main/assets/icons/discord.svg" alt="Discord"></a>
	&nbsp;&nbsp;&nbsp;
  <a href="https://www.linkedin.com/company/unum-cloud/"><img height="25" src="https://github.com/unum-cloud/ukv/raw/main/assets/icons/linkedin.svg" alt="LinkedIn"></a>
  &nbsp;&nbsp;&nbsp;
  <a href="https://twitter.com/unum_cloud"><img height="25" src="https://github.com/unum-cloud/ukv/raw/main/assets/icons/twitter.svg" alt="Twitter"></a>
  &nbsp;&nbsp;&nbsp;
	<a href="https://unum.cloud/post"><img height="25" src="https://github.com/unum-cloud/ukv/raw/main/assets/icons/blog.svg" alt="Blog"></a>
	&nbsp;&nbsp;&nbsp;
	<a href="https://github.com/unum-cloud/ujrpc"><img height="25" src="https://github.com/unum-cloud/ukv/raw/main/assets/icons/github.svg" alt="GitHub"></a>
</p>

---

Most modern networking is built either on slow and ambiguous REST APIs or unnecessarily complex gRPC. FastAPI, for example, looks very easy to use:

```python
from fastapi import FastAPI

app = FastAPI()

@app.get('/sum')
def sum(a: int, b: int):
    return a + b
```

It takes over a millisecond to handle such a call on the same machine.
In that time, light could have traveled 300 km through optics to the neighboring city or country, in my case.

---

To make networking faster, one needs just 2 components:

1. efficient serialization format,
2. an I/O layer without interrupts (hence the name).

Today, libraries like `simdjson` can parse JSON documents faster than gRPC will unpack binary `ProtoBuf`.
Moreover, with `io_uring`, we can avoid system calls and interrupts on the hot path and still use the TCP/IP stack for maximum compatibility.
By now, you believe that one can be faster than gRPC, but would that sacrifice usability?

```python
from ujrpc import Server

serve = Server()

@serve
def sum(a: int, b: int):
    return a + b
```

We don't think so.
This tiny solution already works for C, C++, and Python.
It is even easier to use than FastAPI but is 1000x faster.
We are inviting others to contribute bindings to other languages as well.

## Benchmarks

| Setup                       | Linux, AMD Epyc | MacOS, Intel i9 |
| :-------------------------- | --------------: | --------------: |
| Fast API REST               |          995 μs |        2,854 μs |
| Fast API WebSocket          |          896 μs |        2,820 μs |
| Fast API WebSocket, reusing |          103 μs |          396 μs |
|                             |                 |                 |
| gRPC                        |                 |                 |
| gRCP, reusing               |                 |                 |
|                             |                 |                 |
| UJRPC over HTTP             |                 |                 |
| UJRPC over TCP              |           55 μs |                 |
| UJRPC over TCP, reusing     |                 |                 |
|                             |                 |                 |
| UCX                         |       18'141 μs |                 |
| UCX, reusing                |           90 μs |                 |

> μ stands for micro, μs subsequently means microseconds.
> First column shows timing for a server with Ubuntu 22.04, based on a 64-core AMD Epyc CPU.
> Second column shows timing of a Macbook Pro with Intel Core i9 CPU.

<details>
  <summary>Replicating Fast API Results</summary>

    ```sh
    pip install uvicorn[standard] fastapi
    pip install websocket-client requests
    uvicorn benchmark.fast_api_server:app --log-level critical &
    python ./benchmark/fast_api_client.py
    kill %% # Kill the most recent background job
    ```
</details>

<details>
  <summary>Replicating Py-UCX Results</summary>

    ```sh
    conda create -y -n ucx -c conda-forge -c rapidsai ucx-proc=*=cpu ucx ucx-py python=3.9
    conda activate ucx
    uvicorn benchmark.fast_api_server:app --log-level critical &
    python ./benchmark/fast_api_client.py
    kill %% # Kill the most recent background job
    ```
</details>

<details>
  <summary>Replicating gRPC Results</summary>

    ```sh
    pip install grpcio grpcio-tools
    python ./benchmark/grpc_server.py &
    python ./benchmark/grpc_client.py
    kill %% # Kill the most recent background job
    ```
</details>

<details>
  <summary>Replicating UJRPC Results</summary>

    ```sh
    cmake -DCMAKE_BUILD_TYPE=Release -B ./build_release && make -j8 --silent -C ./build_release
    ./build_release/ujrpc_server_bench &
    python ./benchmark/ujrpc_client.py
    kill %% # Kill the most recent background job
    ```
</details>

<details>
  <summary>Debugging</summary>
  
    ```sh
    cmake -DCMAKE_BUILD_TYPE=Debug -B ./build_debug && make -j8 --silent -C ./build_debug
    ```
</details>

## Why JSON RPC?

- Transport independant: UDP, TCP, bring what you want.
- Application layer is optional: use HTTP or not.

## Roadmap

- [x] Batch requests
- [x] JSON-RPC over raw TCP sockets
- [x] JSON-RPC over TCP with HTTP
- [ ] Concurrent sessions
- [ ] AF_XDP on Linux
- [ ] Amazon Ion support
- [ ] UDP support
