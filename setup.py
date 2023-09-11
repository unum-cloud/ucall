import os
import sys
import re
import platform
import shutil
from os.path import dirname
import multiprocessing
import subprocess
from setuptools import setup, Extension, find_packages
from setuptools.command.build_ext import build_ext

__version__ = open("VERSION", "r").read().strip()
__lib_name__ = "ucall"


this_directory = os.path.abspath(dirname(__file__))
with open(os.path.join(this_directory, "README.md")) as f:
    long_description = f.read()


class CMakeExtension(Extension):
    def __init__(self, name, source_dir=""):
        Extension.__init__(self, name, sources=[])
        self.source_dir = os.path.abspath(source_dir)


class CMakeBuild(build_ext):
    def build_extension(self, ext):
        self.parallel = multiprocessing.cpu_count() // 2
        extension_dir = os.path.abspath(dirname(self.get_ext_fullpath("ucall")))

        # required for auto-detection & inclusion of auxiliary 'native' libs
        if not extension_dir.endswith(os.path.sep):
            extension_dir += os.path.sep

        cmake_args = [
            f"-DCMAKE_LIBRARY_OUTPUT_DIRECTORY={extension_dir}",
            f"-DCMAKE_ARCHIVE_OUTPUT_DIRECTORY={extension_dir}",
            f"-DPYTHON_EXECUTABLE={sys.executable}",
        ]

        # Adding CMake arguments set as environment variable
        # (needed e.g. to build for ARM OSx on conda-forge)
        if "CMAKE_ARGS" in os.environ:
            cmake_args += [item for item in os.environ["CMAKE_ARGS"].split(" ") if item]

        if sys.platform.startswith("darwin"):
            # Cross-compile support for macOS - respect ARCHFLAGS if set
            archs = re.findall(r"-arch (\S+)", os.environ.get("ARCHFLAGS", ""))
            if archs:
                cmake_args += ["-DCMAKE_OSX_ARCHITECTURES={}".format(";".join(archs))]

        # Set CMAKE_BUILD_PARALLEL_LEVEL to control the parallel build level
        # across all generators.
        build_args = []
        if sys.platform.startswith("win32"):
            build_args += ["--config", "Release"]

        if "CMAKE_BUILD_PARALLEL_LEVEL" not in os.environ:
            # self.parallel is a Python 3 only way to set parallel jobs by hand
            # using -j in the build_ext call, not supported by pip or PyPA-build.
            if hasattr(self, "parallel") and self.parallel:
                build_args += [f"-j{self.parallel}"]

        build_name = ext.name.replace(".", "_") + "_python"
        subprocess.check_call(["cmake", ext.source_dir] + cmake_args)
        subprocess.check_call(
            ["cmake", "--build", ".", "--target", build_name] + build_args
        )

        # Add these lines to copy the .so file to the expected directory
        if sys.platform.startswith("darwin"):
            suffix = "darwin.so"
        elif sys.platform.startswith("linux"):
            suffix = "linux-gnu.so"
        else:
            raise RuntimeError(f"Unsupported platform: {sys.platform}")

        backend = ext.name.split(".")[-1]
        submodules_folder = os.path.join(extension_dir, "ucall")
        os.mkdir(submodules_folder)
        expected_output = os.path.join(
            submodules_folder,
            f"{backend}.cpython-{sys.version_info.major}{sys.version_info.minor}-{suffix}",
        )
        actual_output = os.path.join(extension_dir, build_name + ".so")
        shutil.copyfile(actual_output, expected_output)


ext_modules = [CMakeExtension("ucall.posix")]

if platform.system() == "Linux":
    ext_modules.append(CMakeExtension("ucall.epoll"))
    ext_modules.append(CMakeExtension("ucall.uring"))

setup(
    name=__lib_name__,
    version=__version__,
    author="Ash Vardanian",
    author_email="info@unum.cloud",
    url="https://github.com/unum-cloud/ucall",
    description="Up to 100x Faster FastAPI. JSON-RPC with io_uring, SIMD-acceleration, and pure CPython bindings",
    long_description=long_description,
    long_description_content_type="text/markdown",
    license="Apache-2.0",
    classifiers=[
        "Development Status :: 5 - Production/Stable",
        "Natural Language :: English",
        "Intended Audience :: Developers",
        "Intended Audience :: Information Technology",
        "License :: OSI Approved :: Apache Software License",
        "Programming Language :: Python :: 3 :: Only",
        "Programming Language :: Python :: Implementation :: CPython",
        "Programming Language :: C",
        "Operating System :: Unix",
        "Operating System :: POSIX",
        "Operating System :: POSIX :: Linux",
        "Operating System :: MacOS",
        "Programming Language :: C",
        "Programming Language :: C++",
        "Programming Language :: Python :: Implementation :: CPython",
        "Programming Language :: Python :: 3 :: Only",
        "Programming Language :: Python :: 3.9",
        "Programming Language :: Python :: 3.10",
        "Topic :: Communications :: File Sharing",
        "Topic :: Internet :: WWW/HTTP",
        "Topic :: System :: Networking",
    ],
    packages=find_packages(where="./python"),  # Your Python packages are under ./python
    package_dir={"": "./python"},  # Tell setuptools that packages are under ./python
    ext_modules=ext_modules,
    cmdclass={"build_ext": CMakeBuild},
    entry_points={"console_scripts": ["ucall=ucall.cli:cli"]},
    install_requires=["numpy", "pillow"],
    zip_safe=False,
    python_requires=">=3.9",
)
