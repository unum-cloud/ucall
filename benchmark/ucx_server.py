#!/usr/bin/env python3
import asyncio

import ucp
import numpy as np

triplet = np.empty(3, dtype='u4')
port = 13337
listener = None


async def respond(endpoint: ucp.Endpoint):
    await endpoint.recv(triplet[:2])
    triplet[2] = triplet[0] + triplet[1]
    await endpoint.send(triplet[2:])


async def main():
    global listener
    listener = ucp.create_listener(respond, port)
    while not listener.closed():
        await asyncio.sleep(0.001)


if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        listener.close()
