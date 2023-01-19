#!/usr/bin/env python3
import random
import asyncio

import ucp
import numpy as np

from benchmark import benchmark_request

triplet = np.empty(3, dtype='u4')
host = ucp.get_address()
port = 13337
port_reuse = 13338

def sync(to_await):
    """Waits until async operation is completed, as if it was synchronous"""
    async_response = []

    async def run_and_capture_result():
        r = await to_await
        async_response.append(r)

    loop = asyncio.get_event_loop()
    loop.run_until_complete(run_and_capture_result())
    return async_response[0]

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


def bench_reusing():
    endpoint = sync(ucp.create_endpoint(host, port_reuse))
    benchmark_request(lambda: sync(request_sum_reuse(endpoint)))
    sync(endpoint.close())


def bench_creating():
    benchmark_request(lambda: sync(request_sum()))


if __name__ == '__main__':
    print("Creating")
    bench_creating()
    print("Reuseing")
    bench_reusing()