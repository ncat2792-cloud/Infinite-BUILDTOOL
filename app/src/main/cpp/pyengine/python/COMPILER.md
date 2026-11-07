# Private Python 2.7 compiler

CPython 2.7.13 is distributed under LICENSE. Source: https://github.com/python/cpython/tree/v2.7.13

CompilerConfig and CompilerEncodings provide a filesystem-free compiler runtime. CompilerEntry emits reviewed target opcodes. CompilerGameObjects copies constants through opaque native constructor callbacks. No private PyObject is passed to the game. Complex constants are not currently supported.
