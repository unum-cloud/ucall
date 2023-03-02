import os
import sys
import re
from os.path import dirname
import multiprocessing
import subprocess
from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext


class CMakeExtension(Extension):
    def __init__(self, name, source_dir=''):
        Extension.__init__(self, name, sources=[])
        self.source_dir = os.path.abspath(source_dir)


class CMakeBuild(build_ext):
    def build_extension(self, ext):
        self.parallel = multiprocessing.cpu_count() // 2
        extension_dir = os.path.abspath(dirname(
            self.get_ext_fullpath(ext.name)))

        # required for auto-detection & inclusion of auxiliary 'native' libs
        if not extension_dir.endswith(os.path.sep):
            extension_dir += os.path.sep

        cmake_args = [
            f'-DCMAKE_LIBRARY_OUTPUT_DIRECTORY={extension_dir}',
            f'-DCMAKE_ARCHIVE_OUTPUT_DIRECTORY={extension_dir}',
            f'-DPYTHON_EXECUTABLE={sys.executable}',
        ]

        # Adding CMake arguments set as environment variable
        # (needed e.g. to build for ARM OSx on conda-forge)
        if 'CMAKE_ARGS' in os.environ:
            cmake_args += [
                item for item in os.environ['CMAKE_ARGS'].split(' ') if item]

        if sys.platform.startswith('darwin'):
            # Cross-compile support for macOS - respect ARCHFLAGS if set
            archs = re.findall(r'-arch (\S+)', os.environ.get('ARCHFLAGS', ''))
            if archs:
                cmake_args += [
                    '-DCMAKE_OSX_ARCHITECTURES={}'.format(';'.join(archs))]

        # Set CMAKE_BUILD_PARALLEL_LEVEL to control the parallel build level
        # across all generators.
        build_args = []
        if 'CMAKE_BUILD_PARALLEL_LEVEL' not in os.environ:
            # self.parallel is a Python 3 only way to set parallel jobs by hand
            # using -j in the build_ext call, not supported by pip or PyPA-build.
            if hasattr(self, 'parallel') and self.parallel:
                build_args += [f'-j{self.parallel}']

        subprocess.check_call(['cmake', ext.source_dir] + cmake_args)
        subprocess.check_call(
            ['cmake', '--build', '.', '--target', "py_" + ext.name] + build_args)

    def run(self):
        build_ext.run(self)


setup(
    name='ujrpc',
    version='0.0.3',
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
    ext_modules=[CMakeExtension('ujrpc')],
    cmdclass={
        'build_ext': CMakeBuild,
    },
    zip_safe=False,
)
