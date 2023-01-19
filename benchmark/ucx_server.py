#!/usr/bin/env python3
import asyncio

import ucp
import numpy as np

triplet = np.empty(3, dtype='u4')
port = 13337
port_reuse = 13338
listener = None
listener_reuse = None


async def respond_reusers(endpoint: ucp.Endpoint):
    while True:
        await endpoint.recv(triplet[:2])
        triplet[2] = triplet[0] + triplet[1]
        await endpoint.send(triplet[2:])


async def respond(endpoint: ucp.Endpoint):
    await endpoint.recv(triplet[:2])
    triplet[2] = triplet[0] + triplet[1]
    await endpoint.send(triplet[2:])


async def main():
    global listener, listener_reuse
    listener = ucp.create_listener(respond, port)
    listener_reuse = ucp.create_listener(respond_reusers, port_reuse)
    while not listener.closed() and not listener_reuse.closed():
        await asyncio.sleep(0.001)


if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        listener.close()
        listener_reuse.close()
