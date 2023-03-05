<h1 align="center">Uninterrupted JSON RPC</h1>
<h3 align="center">
Remote Procedure Calls<br/>
Up to 100x Faster than FastAPI<br/>
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

Most modern networking is built either on slow and ambiguous REST APIs or unnecessarily complex gRPC.
FastAPI, for example, looks very approachable.
We aim to be equally or even simpler to use.

<table>
<tr>
<th width="50%">FastAPI</th><th width="50%">UJRPC</th>
</tr>
<tr>
<td>

```sh
pip install fastapi uvicorn
```

</td>
<td>

```sh
pip install ujrpc
```

</td>
</tr>
<tr>
<td>

```python
from fastapi import FastAPI
import uvicorn

server = FastAPI()

@server.get('/sum')
def sum(a: int, b: int):
    return a + b

uvicorn.run(...)    
```

</td>
<td>

```python
from ujrpc.posix import Server # or `ujrpc.uring`

server = Server()

@server
def sum(a: int, b: int):
    return a + b

server.run()    
```

</td>
</tr>
</table>

It takes over a millisecond to handle a trivial FastAPI call on a recent 8-core CPU.
In that time, light could have traveled 300 km through optics to the neighboring city or country, in my case.
How does UJRPC compare to FastAPI and gRPC?

| Setup                   |   🔁   | Server | Latency w 1 client | Throughput w 32 clients |
| :---------------------- | :---: | :----: | -----------------: | ----------------------: |
| Fast API over REST      |   ❌   |   🐍    |           1'203 μs |               3'184 rps |
| Fast API over WebSocket |   ✅   |   🐍    |              86 μs |            11'356 rps ¹ |
|                         |       |        |                    |                         |
| gRPC                    |   ✅   |   🐍    |             164 μs |               9'849 rps |
| gRPC                    |   ✅   |   C    |                  - |                       - |
|                         |       |        |                    |                         |
| UJRPC with POSIX        |   ❌   |   C    |              62 μs |              79'000 rps |
| UJRPC with io_uring     |   ✅   |   🐍    |              23 μs |              43'000 rps |
| UJRPC with io_uring     |   ✅   |   C    |              22 μs |             231'000 rps |

> All benchmarks were conducted on AWS on general purpose instances with **Ubuntu 22.10 AMI**, as it is the first major AMI to come with **Linux Kernel 5.19**, featuring much wider `io_uring` support for networking operations. These specific numbers were obtained on `c7g.metal` beefy instances with Graviton 3 chips.
> The 🔁 column marks, if the TCP/IP connection is being reused during subsequent requests.
> The "latency" column report the amount of time between sending a request and receiving a response. μ stands for micro, μs subsequently means microseconds.
> The "throughput" column reports the number of Requests Per Second when querying the same server application from multiple client processes running on the same machine.
> ¹ FastAPI couldn't process concurrent requests with WebSockets.

## How?!

How can a tiny pet-project with just a couple thousand lines of codes dwarf two of the most established networking libraries?
**UJRPC stands on the shoulders of Titans**.
Two Titans, to be exact:

- `io_uring` for interrupt-less ~~hence the name~~ IO, to avoid system calls, reduce the latency on the hot path and still use the TCP/IP stack.
  - `io_uring_prep_read_fixed` to read into registered buffers on 5.1+.
  - `io_uring_prep_accept_direct` for reusable descriptors on 5.19+.
  - `io_uring_register_files_sparse` on 5.19+.
  - `IORING_SETUP_COOP_TASKRUN` optional feature on 5.19+.
  - `IORING_SETUP_SINGLE_ISSUER` optional feature on 6.0+.
- SIMD-accelerated parsers and serializers with explicit memory controls, to increase the bandwidth parsing large messages, and avoid expensive memory allocations.
  - [`simdjson`][simdjson] to parse JSON docs faster than gRPC can unpack a binary `ProtoBuf`.
  - [`Turbo-Base64`][base64] to parse binary strings packed in JSON in a base-64 form.
  - [`picohttpparser`][picohttpparser] to parse HTTP headers.

You have already seen the latency of the round trip..., the throughput in requests per second..., wanna see the bandwidth?
Try yourself!

```python
@server
def echo(data: bytes):
    return data
```

### Free Tier Throughput

We will leave bandwidth measurements to enthusiasts, but will share some more numbers.
The general logic is that you can't squeeze high performance from Free-Tier machines.
Currently AWS provides following options: `t2.micro` and `t4g.small`, on older Intel and newer Graviton 2 chips.
This library is so fast, that it doesn't need more than 1 core, so you can run a super fast server even on tiny free-tier machines!
Here is the bandwidth they can sustain.

| Setup                   |   🔁   | Server | Clients | `t2.micro` | `t4g.small` |
| :---------------------- | :---: | :----: | :-----: | ---------: | ----------: |
| Fast API over REST      |   ❌   |   🐍    |    1    |    328 rps |     424 rps |
| Fast API over WebSocket |   ✅   |   🐍    |    1    |  1'504 rps |   3'051 rps |
| gRPC                    |   ✅   |   🐍    |    1    |  1'169 rps |   1'974 rps |
|                         |       |        |         |            |             |
| UJRPC with POSIX        |   ❌   |   C    |    1    |  1'082 rps |   2'438 rps |
| UJRPC with io_uring     |   ✅   |   C    |    1    |          - |   5'864 rps |
| UJRPC with POSIX        |   ❌   |   C    |   32    |  3'399 rps |  39'877 rps |
| UJRPC with io_uring     |   ✅   |   C    |   32    |          - |  88'455 rps |

[simdjson]: https://github.com/simdjson/simdjson
[base64]: https://github.com/powturbo/Turbo-Base64
[picohttpparser]: https://github.com/h2o/picohttpparser

> This tiny solution already works for C, C++, and Python.
> We are inviting others to contribute bindings to other languages as well.


## Installation

```sh
pip install uform
```

A CMake user?

```cmake
include(FetchContent)
FetchContent_Declare(
    ujrpc
    GIT_REPOSITORY https://github.com/unum-cloud/ujrpc
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(ujrpc)
include_directories(${ujrpc_SOURCE_DIR}/include)
```

## Roadmap

- [x] Batch Requests
- [x] JSON-RPC over raw TCP sockets
- [x] JSON-RPC over TCP with HTTP
- [x] Concurrent sessions
- [ ] HTTP**S** Support
- [ ] Batch-capable Endpoints
- [ ] WebSockets
- [ ] Complementing JSON with Amazon Ion
- [ ] Custom UDP-based JSON-RPC like protocol
- [ ] AF_XDP on Linux

## Why JSON-RPC?

- Transport independent: UDP, TCP, bring what you want.
- Application layer is optional: use HTTP or not.
- Unlike REST APIs, there is just one way to pass arguments.

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
