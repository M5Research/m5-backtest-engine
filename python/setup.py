"""Build the m5_engine extension with CMake."""

import os
import subprocess
import sys

from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext

class CMakeExtension(Extension):
    def __init__(self, name, sourcedir=""):
        Extension.__init__(self, name, sources=[])
        self.sourcedir = os.path.abspath(sourcedir)

class CMakeBuild(build_ext):
    def build_extension(self, ext):
        extdir = os.path.abspath(os.path.dirname(self.get_ext_fullpath(ext.name)))
        if not extdir.endswith(os.path.sep):
            extdir += os.path.sep

        import pybind11

        cmake_args = [
            f"-DCMAKE_LIBRARY_OUTPUT_DIRECTORY={extdir}",
            f"-DPYTHON_EXECUTABLE={sys.executable}",
            f"-Dpybind11_DIR={pybind11.get_cmake_dir()}",
            "-DBUILD_PYBIND11=ON",
            "-DCMAKE_BUILD_TYPE=Release",
            "-G",
            "Ninja",
        ]

        build_args = []
        if sys.platform.startswith("win"):
            cmake_args += [f"-DCMAKE_LIBRARY_OUTPUT_DIRECTORY_{'RELEASE'}={extdir}"]
        else:
            build_args += ["-j", str(os.cpu_count())]

        build_temp = os.path.join(self.build_temp, ext.name)
        if not os.path.exists(build_temp):
            os.makedirs(build_temp)

        subprocess.check_call(["cmake", ext.sourcedir] + cmake_args, cwd=build_temp)
        subprocess.check_call(["cmake", "--build", "."] + build_args, cwd=build_temp)

setup(
    name="m5_engine",
    version="0.1.0",
    description="Python bindings for the C++ backtesting engine",
    ext_modules=[CMakeExtension("m5_engine._core", sourcedir="../core")],
    cmdclass={"build_ext": CMakeBuild},
    packages=["m5_engine"],
    zip_safe=False,
)
