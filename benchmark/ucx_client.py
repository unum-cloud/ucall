#!/usr/bin/env python3
import random
import asyncio

import ucp
import numpy as np

from benchmark import benchmark_request_async

triplet = np.empty(3, dtype='u4')
host = ucp.get_address()
port = 13337
port_reuse = 13338

async def request_sum_reuse(endpoint: ucp.Endpoint):
    triplet[0] = random.randint(1, 1000)
    triplet[1] = random.randint(1, 1000)    
    await endpoint.send(triplet[:2])
    await endpoint.recv(triplet[2:])
    assert triplet[0] + triplet[1] == triplet[2], 'Wrong sum'


async def request_sum():
    endpoint = await ucp.create_endpoint(host, port)
    triplet[0] = random.randint(1, 1000)
    triplet[1] = random.randint(1, 1000)    
    await endpoint.send(triplet[:2])
    await endpoint.recv(triplet[2:])
    assert triplet[0] + triplet[1] == triplet[2], 'Wrong sum'
    await endpoint.close()


async def bench_reusing():
    endpoint = await ucp.create_endpoint(host, port_reuse)
    await benchmark_request_async(request_sum_reuse, endpoint)
    await endpoint.close()


async def bench_creating():
    await benchmark_request_async(request_sum)


if __name__ == '__main__':
    loop = asyncio.new_event_loop()
    print("Creating")
    loop.run_until_complete(bench_creating())
    print("Reuseing")
    loop.run_until_complete(bench_reusing())
    loop.close()