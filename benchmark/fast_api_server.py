# To run this:
# uvicorn benchmark.fast_api_server:app --reload
import struct

from fastapi import FastAPI, WebSocket

app = FastAPI()


@app.get('/sum')
async def sum(a: int, b: int):
    return a + b


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
    return 'Call the `sum` method'
