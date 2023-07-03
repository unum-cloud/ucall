import numpy as np
from PIL import Image
from ucall.server import Server

server = Server(uring_if_possible=True,
                port=8545,
                ssl_pk='./examples/login/certs/main.key',
                ssl_certs=['./examples/login/certs/srv.crt',
                           './examples/login/certs/cas.pem']
                )


@server
def validate_session(user_id: int, session_id: int) -> bool:
    return (user_id ^ session_id) % 23 == 0


@server
def create_user(age: int, name: str, avatar: bytes, bio: str) -> str:
    return f'Created {name} aged {age} with bio {bio} and avatar_size {len(avatar)}'


@server
def rotate_avatar(image: Image.Image) -> Image.Image:
    rotated = image.rotate(45)
    return rotated


@server
def validate_all_sessions(user_ids: np.ndarray, session_ids: np.ndarray) -> bool:
    return np.mod(np.logical_xor(user_ids, session_ids), 23)


if __name__ == '__main__':
    server.run()
