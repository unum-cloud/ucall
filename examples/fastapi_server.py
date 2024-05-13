import io
import base64
import random
from typing import List

import numpy as np
from PIL import Image
from fastapi import FastAPI, HTTPException

app = FastAPI()


@app.post("/echo")
async def echo(data: bytes) -> bytes:
    return data


@app.get("/validate_session")
async def validate_session(user_id: int, session_id: int) -> bool:
    return (user_id ^ session_id) % 23 == 0


@app.post("/create_user")
async def create_user(age: int, name: str, avatar: str, bio: str):
    avatar_bytes = base64.b64decode(avatar)
    return (
        f"Created {name} aged {age} with bio {bio} and avatar_size {len(avatar_bytes)}"
    )


@app.post("/validate_user_identity")
async def validate_user_identity(
    user_id: int,
    name: str,
    age: float,
    access_token: str,
):
    if age < 18:
        raise HTTPException(status_code=400, detail=f"{name} must be older than 18")
    access_token_bytes = base64.b64decode(access_token)
    if not access_token_bytes.decode().startswith(name):
        raise HTTPException(status_code=400, detail=f"Invalid access token for {name}")

    suggested_session_ids = [random.random() * age * user_id for _ in range(round(age))]
    return {
        "session_ids": suggested_session_ids,
        "user": {
            "name": name,
            "age": age,
            "user_id": user_id,
            "access_token": access_token_bytes,
            "repeated_sesson_ids": suggested_session_ids,
        },
    }


redis = {}


@app.put("/set")
async def set(key: str, value: str) -> bool:
    redis[key] = value
    return True


@app.get("/get")
async def get(key: str) -> str:
    return redis.get(key, None)


@app.post("/resize")
async def resize(image: str, width: int, height: int) -> str:
    image_bytes = base64.b64decode(image)
    pil_image = Image.open(io.BytesIO(image_bytes))
    resized_image = pil_image.resize((width, height))
    buf = io.BytesIO()
    resized_image.save(buf, format="PNG")
    image_base64 = base64.b64encode(buf.getvalue()).decode()
    return image_base64


@app.post("/resize_batch")
async def resize_batch(images: List[str], width: int, height: int) -> List[str]:
    resized_images = []
    for image_data in images:
        image_bytes = base64.b64decode(image_data)
        pil_image = Image.open(io.BytesIO(image_bytes))
        resized_image = pil_image.resize((width, height))
        buf = io.BytesIO()
        resized_image.save(buf, format="PNG")
        resized_images.append(base64.b64encode(buf.getvalue()).decode())
    return resized_images


@app.post("/dot_product")
async def dot_product(a: str, b: str) -> float:
    a_array = np.frombuffer(base64.b64decode(a), dtype=np.float32)
    b_array = np.frombuffer(base64.b64decode(b), dtype=np.float32)
    return float(np.dot(a_array, b_array))


@app.post("/dot_product_batch")
async def dot_product_batch(a: List[str], b: List[str]) -> List[float]:
    results = []
    for a_data, b_data in zip(a, b):
        a_array = np.frombuffer(base64.b64decode(a_data), dtype=np.float32)
        b_array = np.frombuffer(base64.b64decode(b_data), dtype=np.float32)
        results.append(float(np.dot(a_array, b_array)))
    return results
