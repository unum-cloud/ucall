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
We don't think so.

```python
from ujrpc import Server

serve = Server()

@serve
def sum(a: int, b: int):
    return a + b
```

This tiny solution already works for C, C++, and Python.
It is even easier to use than FastAPI but is 1000x faster.
Moreover, it supports tensor-like types common in Machine Learning and useful for batch processing:

```python
import numpy as np
from ujrpc import Server

serve = Server()

@serve
def sum_arrays(a: np.array, b: np.array):
    return a + b
```

We are inviting others to contribute bindings to other languages as well.

## Benchmarks

| Setup                       | Linux, AMD Epyc | MacOS, Intel i9 |
| :-------------------------- | --------------: | --------------: |
| Fast API REST               |          995 μs |        2,854 μs |
| Fast API WebSocket          |          896 μs |        2,820 μs |
| Fast API WebSocket, reusing |          103 μs |          396 μs |
|                             |                 |                 |
| gRPC                        |          373 μs |         1061 μs |
| gRCP, reusing               |          270 μs |          459 μs |
|                             |                 |                 |
| UCX                         |       18'141 μs |                 |
| UCX, reusing                |           90 μs |                 |
|                             |                 |                 |
| UJRPC over TCP              |           90 μs |                 |
| UJRPC over TCP, reusing     |           25 μs |                 |

> μ stands for micro, μs subsequently means microseconds.
> First column shows timing for a server with Ubuntu 22.04, based on a 64-core AMD Epyc CPU.
> Second column shows timing of a Macbook Pro with Intel Core i9 CPU.

### Reproducing Results

#### FastAPI

```sh
pip install uvicorn fastapi websocket-client requests
cd examples && uvicorn sum.fastapi_server:app --log-level critical & ; cd ..
python examples/bench.py "sum.fastapi_client.ClientREST" --progress
python examples/bench.py "sum.fastapi_client.ClientWebSocket" --progress
kill %%
```

Want to dispatch more clients and aggregate statistics?

```sh
python examples/bench.py "sum.fastapi_client.ClientREST" --threads 8
python examples/bench.py "sum.fastapi_client.ClientWebSocket" --threads 8
```

#### UJRPC

UJRPC can produce both a POSIX compliant old-school server, and a modern `io_uring`-based version for Linux kernel 5.19 and newer.
You would either run `ujrpc_server_posix_bench` or `ujrpc_server_uring_bench`.

```sh
cmake -DCMAKE_BUILD_TYPE=Release -B ./build_release  && make -C ./build_release && ./build_release/ujrpc_server_posix_bench &
python examples/bench.py "sum.jsonrpc_client.ClientTCP" --progress
python examples/bench.py "sum.jsonrpc_client.ClientHTTP" --progress
python examples/bench.py "sum.jsonrpc_client.ClientHTTPBatches" --progress
kill %%
```

Want to dispatch more clients and aggregate statistics?

```sh
python examples/bench.py "sum.jsonrpc_client.ClientTCP" --threads 8
python examples/bench.py "sum.jsonrpc_client.ClientHTTP" --threads 8
python examples/bench.py "sum.jsonrpc_client.ClientHTTPBatches" --threads 8
```

A lot has been said about the speed of Python code ~~or the lack of~~.
To get more accurate numbers for mean request latency, you can use the GoLang version:

```sh
go run ./examples/sum/ujrpc_client.go
```

Or push it even further dispatching dozens of processes with GNU `parallel` utility:

```
sudo apt install parallel
parallel go run ./examples/sum/ujrpc_client.go run ::: {1..8}
```

#### Py-UCX Results

```sh
conda create -y -n ucx -c conda-forge -c rapidsai ucx-proc=*=cpu ucx ucx-py python=3.9
conda activate ucx
? &
python ./sum/ucx_client.py
kill %%
```

#### gRPC Results

```sh
pip install grpcio grpcio-tools
python ./sum/grpc_server.py &
python ./sum/grpc_client.py
kill %%
```

## Why JSON RPC?

- Transport independant: UDP, TCP, bring what you want.
- Application layer is optional: use HTTP or not.

## Roadmap

- [x] Batch requests
- [x] JSON-RPC over raw TCP sockets
- [x] JSON-RPC over TCP with HTTP
- [x] Concurrent sessions
- [ ] HTTPs Support
- [ ] Complementing JSON with Amazon Ion
- [ ] Custom UDP-based JSON-RPC like protocol
- [ ] AF_XDP on Linux
