import numpy as np
from PIL import Image
from ujrpc.rich_posix import Server

server = Server(port=8545)


@server
def mul(a: np.ndarray, b: np.ndarray):
    return a * b


@server
def validate_session(user_id: int, session_id: int):
    return (user_id ^ session_id) % 23 == 0


@server
def create_user(age: int, name: str, avatar: bytes, bio: str):
    return f'Created {name} aged {age} with bio {bio} and avatar_size {len(avatar)}'


@server
def rotate(image: Image.Image):
    rotated = image.rotate(45)
    return rotated


if __name__ == '__main__':
    server.run()