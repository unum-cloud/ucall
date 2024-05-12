import random
from typing import List

import PIL.Image as pil

from ucall.posix import Server

redis = dict()

server = Server(port=8545)


@server
def validate_session(user_id: int, session_id: int):
    return (user_id ^ session_id) % 23 == 0


@server
def echo(data: bytes):
    return data


@server
def create_user(age: int, name: str, avatar: bytes, bio: str):
    return f"Created {name} aged {age} with bio {bio} and avatar_size {len(avatar)}"


@server
def transform(age: float, name: str, value: int, identity: bytes):

    if age < 15:
        return False

    if age >= 15 and age < 19:
        return (False, f"{name} must be older than 19")

    new_identity = identity.decode() + f"_{name}"

    return {
        "name": name,
        "pins": [random.random() * age for _ in range(round(age))],
        "val": {
            "len": value,
            "identity": new_identity.encode(),
            "data": [random.randbytes(value) for _ in range(value)],
        },
    }


@server
def set(key: str, value: str) -> bool:
    """Statefull Redis-like API for building collections"""
    redis[key] = value
    return True


@server
def get(key: str) -> str:
    """Statefull Redis-like API for retrieving collections"""
    return redis.get(key, None)


@server
def resize(image: pil.Image, width: int, height: int):
    return image.resize((width, height))


@server(batch=True, name="resize")
def resize(images: List[pil.Image], width: int, height: int):
    images = [image for image in images.resize((width, height))]
    return images


if __name__ == "__main__":
    server.run()
