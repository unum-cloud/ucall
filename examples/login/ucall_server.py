import random

from ucall.posix import Server
from ucall.server import Protocol

server = Server(
    port=8545,
    protocol=Protocol.JSONRPC_HTTP,
    # ssl_pk='./examples/login/certs/main.key',
    # ssl_certs=[
    #   './examples/login/certs/srv.crt',
    #   './examples/login/certs/cas.pem']
)


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


if __name__ == "__main__":
    server.run()
