from ujrpc.client import Client
from PIL import Image
import argparse
from pydoc import locate


def get_kwargs(buffer):
    kwargs = {}
    if buffer is not None:
        for arg in buffer:
            sp = None
            if '=' in arg:
                sp = arg.split('=')
            else:
                raise KeyError('Missing key in kwarg argument')
            kwargs[sp[0]] = sp[1]
    return kwargs


def cast(value: str, type_str):
    if type_str is None:
        if value.isdigit():
            return int(value)
        if value.replace('.', '', 1).isdigit():
            return float(value)
        if value in ['True', 'False']:
            return bool(value)
        return value

    std = type_str.lower()
    if std == 'image':
        return Image.open(value)
    if std == 'binary':
        return open(value, 'rb').read()

    return locate(std)(value)


def fix_types(args, kwargs):
    for i in range(len(args)):
        if ':' in args[i]:
            val, tp = args[i].split(':')
            args[i] = cast(val, tp)
        else:
            args[i] = cast(args[i], None)
    for key in kwargs.keys():
        if ':' in kwargs[key]:
            val, tp = kwargs[key].split(':')
            kwargs[key] = cast(val, tp)
        else:
            kwargs[key] = cast(kwargs[key], None)


def add_specials(kwargs, special, tp):
    if special is None:
        return
    for x in special:
        if not '=' in x:
            raise KeyError(f'Missing key in {tp} argument')
        k, v = x.split('=')
        kwargs[k] = v + ':' + tp


def cli():
    parsed = get_parser().parse_args()
    kwargs = get_kwargs(parsed.kwargs)
    args = parsed.positional if parsed.positional else []

    add_specials(kwargs, parsed.file, 'binary')
    add_specials(kwargs, parsed.image, 'image')

    print(args, kwargs)
    fix_types(args, kwargs)
    print(args, kwargs)
    client = Client(uri=parsed.uri, port=parsed.port, use_http=True)
    res = getattr(client, parsed.method)(*args, **kwargs)
    print(res.json)


def get_parser():
    parser = argparse.ArgumentParser(description='UJRPC Client CLI')
    parser.add_argument('method', type=str, help='method name')
    parser.add_argument('--uri', type=str,
                        help='server uri', default='localhost')
    parser.add_argument('--port', type=int, help='server port', default=8545)
    parser.add_argument('kwargs', nargs='*', help='method arguments')
    parser.add_argument('-f', '--file', nargs='*',
                        help='method positional arguments')
    parser.add_argument('-i', '--image', nargs='*',
                        help='method positional arguments')

    parser.add_argument('-p', '--positional', nargs='*',
                        help='method positional arguments')
    return parser


if __name__ == '__main__':
    cli()
