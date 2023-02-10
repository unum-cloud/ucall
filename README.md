<h1 align="center">Uninterrupted JSON RPC</h1>
<h3 align="center">
Understandable Remote Procedure Calls<br/>
100x Faster than FastAPI<br/>
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
It is even easier to use than FastAPI but is 100x faster.
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

All benchmarks were conducted on AWS on general purpose instances with Ubuntu 22.10 AMI, as it is the first major AMI to come with Linux Kernel 5.19, featuring much wider io_uring support for networking operations.

| Setup                   |   🔁   | 1 client on m6i.metal | 32 clients on m6i.metal |
| :---------------------- | :---: | --------------------: | ----------------------: |
| Fast API over REST      |   ❌   |    1'002 rps @ 998 μs |    3'553 rps @ 8'988 μs |
| Fast API over WebSocket |   ✅   |    12'312 rps @ 81 μs |                         |
| gRPC                    |   ✅   |                       |                         |
|                         |       |                       |                         |
| UJRPC over TCP, reset   |   ❌   |                 90 μs |                         |
| UJRPC over TCP, reuse   |   ✅   |                 25 μs |                         |

> In every cell we report the average number of Requests Per Second, as well as the average request latency as measured on the client side.
> μ stands for micro, μs subsequently means microseconds.

Lets start a cluster of small clients and attack some free-tier AWS services, measuring the number of operations they can handle.

| Setup                   |   🔁   | t2.micro | t4g.small |
| :---------------------- | :---: | -------: | --------: |
| Fast API over REST      |   ❌   |          |           |
| Fast API over WebSocket |   ✅   |          |           |
| gRPC                    |   ✅   |          |           |
|                         |       |          |           |
| UJRPC over TCP, reset   |   ❌   |          |           |
| UJRPC over TCP, reuse   |   ✅   |          |           |


### Reproducing Results

#### FastAPI

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

#### UJRPC

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

Want to dispatch more clients and aggregate statistics?

```sh
python examples/bench.py "sum.jsonrpc_client.ClientTCP" --threads 32
python examples/bench.py "sum.jsonrpc_client.ClientHTTP" --threads 32
python examples/bench.py "sum.jsonrpc_client.ClientHTTPBatches" --threads 32
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

#### gRPC Results

```sh
pip install grpcio grpcio-tools
python ./sum/grpc_server.py &
python examples/bench.py "sum.grpc_client.gRPCClient" --progress
python examples/bench.py "sum.grpc_client.gRPCClient" --threads 32
kill %%
```

## Why JSON-RPC?

- Transport independent: UDP, TCP, bring what you want.
- Application layer is optional: use HTTP or not.
- Unlike REST APIs, there is just one way to pass arguments.

## Roadmap

- [x] Batch requests
- [x] JSON-RPC over raw TCP sockets
- [x] JSON-RPC over TCP with HTTP
- [x] Concurrent sessions
- [ ] HTTP**S** Support
- [ ] Complementing JSON with Amazon Ion
- [ ] Custom UDP-based JSON-RPC like protocol
- [ ] AF_XDP on Linux
