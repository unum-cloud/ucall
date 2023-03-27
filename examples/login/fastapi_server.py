#!/usr/bin/env python3
import struct
import base64

from fastapi import FastAPI, WebSocket

app = FastAPI()


@app.get('/validate_session')
async def validate_session(user_id: int, session_id: int):
    return (user_id ^ session_id) % 23 == 0


@app.get('/create_user')
async def create_user(age: int, name: str, avatar: str, bio: str):
    return f'Created {name} aged {age} with bio {bio} and avatar_size {len(base64.b64decode(avatar))}'


@app.websocket('/validate_session_ws')
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    while True:
        data = await websocket.receive_bytes()
        user_id, session_id = struct.unpack('<II', data)
        result = (user_id ^ session_id) % 23 == 0
        await websocket.send_bytes(struct.pack('<I', result))


@app.get('/')
async def root():
    return 'Call the `sum` method for REST API or `validate_session_ws` for WebSockets'
