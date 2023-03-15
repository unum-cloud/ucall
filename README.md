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

<table width="100%">
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
| gRPC ²                  |   ✅   |   🐍    |             164 μs |               9'849 rps |
|                         |       |        |                    |                         |
| UJRPC with POSIX        |   ❌   |   C    |              62 μs |              79'000 rps |
| UJRPC with io_uring     |   ✅   |   🐍    |              23 μs |              43'000 rps |
| UJRPC with io_uring     |   ✅   |   C    |              22 μs |             231'000 rps |

<details>
  <summary>Table legend</summary>

All benchmarks were conducted on AWS on general purpose instances with **Ubuntu 22.10 AMI**.
It is the first major AMI to come with **Linux Kernel 5.19**, featuring much wider `io_uring` support for networking operations.
These specific numbers were obtained on `c7g.metal` beefy instances with Graviton 3 chips.

- The 🔁 column marks, if the TCP/IP connection is being reused during subsequent requests.
- The "server" column defines the programming language, in which the server was implemented.
- The "latency" column report the amount of time between sending a request and receiving a response. μ stands for micro, μs subsequently means microseconds.
- The "throughput" column reports the number of Requests Per Second when querying the same server application from multiple client processes running on the same machine.

> ¹ FastAPI couldn't process concurrent requests with WebSockets.

> ² We tried generating C++ backends with gRPC, but its numbers, suspiciously, weren't better. There is also an async gRPC option, that wasn't tried.

</details>

## How is that possible?!

How can a tiny pet-project with just a couple thousand lines of code compete with two of the most established networking libraries?
**UJRPC stands on the shoulders of Giants**:

- `io_uring` for interrupt-less IO.
  - `io_uring_prep_read_fixed` on 5.1+.
  - `io_uring_prep_accept_direct` on 5.19+.
  - `io_uring_register_files_sparse` on 5.19+.
  - `IORING_SETUP_COOP_TASKRUN` optional on 5.19+.
  - `IORING_SETUP_SINGLE_ISSUER` optional on 6.0+.

- SIMD-accelerated parsers with manual memory control.
  - [`simdjson`][simdjson] to parse JSON faster than gRPC can unpack `ProtoBuf`.
  - [`Turbo-Base64`][base64] to decode binary values from a `Base64` form.
  - [`picohttpparser`][picohttpparser] to navigate HTTP headers.

You have already seen the latency of the round trip..., the throughput in requests per second..., want to see the bandwidth?
Try yourself!

```python
@server
def echo(data: bytes):
    return data
```

## More Functionality than FastAPI

FastAPI supports native type, while UJRPC supports `numpy.ndarray`, `PIL.Image` and other custom types.
This comes handy when you build real applications or want to deploy Multi-Modal AI, like we do with [UForm](https://github.com/unum-cloud/uform).

```python
from ujrpc.rich_posix import Server
import ufrom

server = Server()
model = uform.get_model('unum-cloud/uform-vl-multilingual')

@server
def vectorize(description: str, photo: PIL.Image.Image) -> numpy.ndarray:
    image = model.preprocess_image(photo)
    tokens = model.preprocess_text(description)
    joint_embedding = model.encode_multimodal(image=image, text=tokens)

    return joint_embedding.cpu().detach().numpy()
```

We also have our own optional `Client` class that helps with those custom types.

```python
from ujrpc.client import Client

client = Client()
# Explicit JSON-RPC call:
response = client({
    'method': 'vectorize',
    'params': {
        'description': description,
        'image': image,
    },
    'jsonrpc': '2.0',
    'id': 100,
})
# Or the same with syntactic sugar:
response = client.vectorize(description=description, image=image) 
```

## Free Tier Throughput

We will leave bandwidth measurements to enthusiasts, but will share some more numbers.
The general logic is that you can't squeeze high performance from Free-Tier machines.
Currently AWS provides following options: `t2.micro` and `t4g.small`, on older Intel and newer Graviton 2 chips.
This library is so fast, that it doesn't need more than 1 core, so you can run a fast server even on a tiny Free-Tier server!

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

In this case, every server was bombarded by requests from 1 or a fleet of 32 other instances in the same availability zone.
If you want to reproduce those benchmarks, check out the [`sum` examples on GitHub][sum-examples].

## Quick Start

For Python:

```sh
pip install ujrpc
```

For CMake projects:

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

The C usage example is mouthful compared to Python.
We wanted to make it as lightweight as possible and to allow optional arguments without dynamic allocations and named lookups.
So unlike the Python layer, we expect the user to manually extract the arguments from the call context with `ujrpc_param_named_i64()`, and its siblings.

```c
#include <cstdio.h>
#include <ujrpc/ujrpc.h>

static void sum(ujrpc_call_t call, ujrpc_callback_tag_t) {
    int64_t a{}, b{};
    char printed_sum[256]{};
    bool got_a = ujrpc_param_named_i64(call, "a", 0, &a);
    bool got_b = ujrpc_param_named_i64(call, "b", 0, &b);
    if (!got_a || !got_b)
        return ujrpc_call_reply_error_invalid_params(call);

    int len = snprintf(printed_sum, 256, "%ll", a + b);
    ujrpc_call_reply_content(call, printed_sum, len);
}

int main(int argc, char** argv) {

    ujrpc_server_t server{};
    ujrpc_config_t config{};

    ujrpc_init(&config, &server);
    ujrpc_add_procedure(server, "sum", &sum, NULL);
    ujrpc_take_calls(server, 0);
    ujrpc_free(server);
    return 0;
}
```

## Roadmap

- [x] Batch Requests
- [x] JSON-RPC over raw TCP sockets
- [x] JSON-RPC over TCP with HTTP
- [x] Concurrent sessions
- [x] NumPy `array` and Pillow serialization
- [ ] HTTP**S** support
- [ ] Batch-capable endpoints for ML
- [ ] Zero-ETL relay calls
- [ ] Integrating with [UKV][ukv]
- [ ] WebSockets for web interfaces
- [ ] AF_XDP and UDP-based analogs on Linux

> Want to affect the roadmap and request a feature? Join the discussions on Discord.

## Why JSON-RPC?

- Transport independent: UDP, TCP, bring what you want.
- Application layer is optional: use HTTP or not.
- Unlike REST APIs, there is just one way to pass arguments.

[simdjson]: https://github.com/simdjson/simdjson
[base64]: https://github.com/powturbo/Turbo-Base64
[picohttpparser]: https://github.com/h2o/picohttpparser
[sum-examples]: https://github.com/unum-cloud/ujrpc/tree/dev/examples/sum
[ukv]: https://github.com/unum-cloud/ukv
