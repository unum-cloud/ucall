import os
import sys
from setuptools import setup, find_packages, Extension


compile_args = [
    '-O3', '-g',
    '-Wno-unknown-pragmas', '-Wno-unused-variable',
]

if sys.platform == 'darwin':
    compile_args.append('-mmacosx-version-min=10.13')

setup(

    name='ujrpc',
    version='0.1.0',
    packages=find_packages(),
    license='Apache-2.0',

    classifiers=[
        'Development Status :: 4 - Beta',

        'Natural Language :: English',
        'Intended Audience :: Developers',
        'Intended Audience :: Information Technology',
        'License :: OSI Approved :: Apache Software License',

        'Programming Language :: Python :: 3 :: Only',
        'Programming Language :: Python :: Implementation :: CPython',
        'Programming Language :: C',

        'Operating System :: MacOS',
        'Operating System :: Unix',
        'Operating System :: Microsoft :: Windows',

        'Topic :: System :: Clustering',
        'Topic :: Database :: Database Engines/Servers',
        'Topic :: Scientific/Engineering :: Artificial Intelligence',
    ],

    # https://llllllllll.github.io/c-extension-tutorial/building-and-importing.html
    include_dirs=['.'],
    ext_modules=[
        Extension(
            'ujrpc',
            sources=['src/python.c'],
            extra_compile_args=compile_args,
        ),
    ],
)
