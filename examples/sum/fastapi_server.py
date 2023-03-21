#!/usr/bin/env python3
import struct
import base64

from fastapi import FastAPI, WebSocket

app = FastAPI()


@app.get('/sum')
async def sum(a: int, b: int):
    return a + b


@app.get('/perform')
async def perform(a: int, b: int, bin: str, text: str):
    return f'{text}_{a*b}_{len(base64.b64decode(bin))}'


@app.websocket('/sum-ws')
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    while True:
        data = await websocket.receive_bytes()
        a, b = struct.unpack('<II', data)
        c = a + b
        await websocket.send_bytes(struct.pack('<I', c))


@app.get('/')
async def root():
    return 'Call the `sum` method for REST API or `sum-ws` for WebSockets'
