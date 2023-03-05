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
from ujrpc.posix import Server
# from ujrpc.uring import Server on 5.19+

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

<details>
  <summary>How those numbers were obtained?</summary>

All benchmarks were conducted on AWS on general purpose instances with **Ubuntu 22.10 AMI**.
It is the first major AMI to come with **Linux Kernel 5.19**, featuring much wider `io_uring` support for networking operations.
These specific numbers were obtained on `c7g.metal` beefy instances with Graviton 3 chips.

- The 🔁 column marks, if the TCP/IP connection is being reused during subsequent requests.
- The "latency" column report the amount of time between sending a request and receiving a response. μ stands for micro, μs subsequently means microseconds.
- The "throughput" column reports the number of Requests Per Second when querying the same server application from multiple client processes running on the same machine.

> ¹ FastAPI couldn't process concurrent requests with WebSockets.

</details>

## How is that possible?!

How can a tiny pet-project with just a couple thousand lines of code dwarf two of the most established networking libraries?
**UJRPC stands on the shoulders of Titans**:

- `io_uring` for interrupt-less ~~hence the name~~ IO, *to reduce the latency by avoiding system calls*.
  - `io_uring_prep_read_fixed` on 5.1+.
  - `io_uring_prep_accept_direct` on 5.19+.
  - `io_uring_register_files_sparse` on 5.19+.
  - `IORING_SETUP_COOP_TASKRUN` optional on 5.19+.
  - `IORING_SETUP_SINGLE_ISSUER` optional on 6.0+.
- SIMD-accelerated parsers with explicit memory controls, *mainly to increase the bandwidth on large messages*.
  - [`simdjson`][simdjson] to parse JSON faster than gRPC can unpack `ProtoBuf`.
  - [`Turbo-Base64`][base64] to decode binary values from a `Base64` form.
  - [`picohttpparser`][picohttpparser] to navigate HTTP headers.

You have already seen 

- the latency of the round trip..., 
- the throughput in requests per second..., 
- wanna see the bandwidth?

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

This tiny solution already works for C, C++, and Python.
We are inviting others to contribute bindings to other languages as well.
If you want to reproduce those benchmarks, check out the [`sum` examples on GitHub][sum-examples].

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

[simdjson]: https://github.com/simdjson/simdjson
[base64]: https://github.com/powturbo/Turbo-Base64
[picohttpparser]: https://github.com/h2o/picohttpparser
[sum-examples]: https://github.com/unum-cloud/ujrpc/tree/dev/examples/sum
