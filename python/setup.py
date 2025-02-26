#!/usr/bin/env python
# -*- coding: utf-8 -*-
import sys, os, platform, sysconfig
import shutil
import subprocess
import glob

from setuptools import find_packages
from setuptools.dist import Distribution

# need to use distutils.core for correct placement of cython dll
if '--inplace' in sys.argv:
    from distutils.core import setup
    from distutils.extension import Extension
else:
    from setuptools import setup
    from setuptools.extension import Extension

class BinaryDistribution(Distribution):
    def has_ext_modules(self):
        return platform.system() in ('Darwin', 'Linux')

CURRENT_DIR = os.path.dirname(__file__)

def get_lib_path():
    """Get library path, name and version"""
     # We can not import `libinfo.py` in setup.py directly since __init__.py
    # Will be invoked which introduces dependencies
    libinfo_py = os.path.join(CURRENT_DIR, './decord/_ffi/libinfo.py')
    libinfo = {'__file__': libinfo_py}
    exec(compile(open(libinfo_py, "rb").read(), libinfo_py, 'exec'), libinfo, libinfo)
    version = libinfo['__version__']

    lib_path = libinfo['find_lib_path']()
    libs = [lib_path[0]]

    return libs, version

LIBS, VERSION = get_lib_path()

include_libs = False
wheel_include_libs = False
if "bdist_wheel" in sys.argv or os.getenv('CONDA_BUILD'):
    wheel_include_libs = True
else:
    include_libs = True

setup_kwargs = {}

# For bdist_wheel only
if wheel_include_libs:
    with open("MANIFEST.in", "w") as fo:
        for path in LIBS:
            shutil.copy(path, os.path.join(CURRENT_DIR, 'decord'))
            _, libname = os.path.split(path)
            fo.write("include decord/%s\n" % libname)
    setup_kwargs = {
        "include_package_data": True
    }

# For source tree setup
# Conda build also includes the binary library
if include_libs:
    rpath = [os.path.relpath(path, CURRENT_DIR) for path in LIBS]
    setup_kwargs = {
        "include_package_data": True,
        "data_files": [('decord', ['libdecord.dylib'])]
    }

setup(
    name='eva-decord',
    version="0.6.0",
    description='EVA\'s Decord Video Loader',
    zip_safe=False,
    maintainer='Decord committers',
    maintainer_email='georgia.tech.db@gmail.com',
    packages=find_packages(),
    python_requires='>=3.9.0',
    install_requires=[
        'numpy>=1.26.4',
    ],
    url='https://github.com/georgia-tech-db/eva-decord',
    distclass=BinaryDistribution,
    classifiers=[
        'Development Status :: 3 - Alpha',
        'Programming Language :: Python :: 3',
        'License :: OSI Approved :: Apache Software License',
    ],
    license='APACHE',
    **setup_kwargs
)

if wheel_include_libs:
    # Wheel cleanup
    os.remove("MANIFEST.in")
    for path in LIBS:
        _, libname = os.path.split(path)
        os.remove("decord/%s" % libname)