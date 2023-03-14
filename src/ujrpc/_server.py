import numpy as np
from typing import Callable, get_type_hints
import inspect
from functools import wraps
from io import BytesIO


class _Server:
    server = None

    def __call__(self, func: Callable):
        return self.route(func)

    def run(self, max_cycles: int = -1, max_seconds: float = -1):
        return self.server.run(max_cycles, max_seconds)

    def route(self, func: Callable):
        hints = get_type_hints(func)

        @wraps(func)
        def wrapper(*args, **kwargs):
            new_args = []
            new_kwargs = {}

            for arg, hint in zip(args, hints.values()):
                if isinstance(arg, bytes) and hint == np.ndarray:
                    new_args.append(np.load(BytesIO(arg), allow_pickle=True))
                else:
                    new_args.append(arg)

            for kw, arg in kwargs.items():
                if isinstance(arg, bytes) and hints[kw] == np.ndarray:
                    new_kwargs[kw] = np.load(BytesIO(arg), allow_pickle=True)
                else:
                    new_kwargs[kw] = arg

            res = func(*new_args, **new_kwargs)
            if isinstance(res, np.ndarray):
                buf = BytesIO()
                np.save(buf, res)
                return buf.getvalue()
            return res

        wrapper.__signature__ = inspect.signature(func)
        self.server.route(wrapper)
        return wrapper
