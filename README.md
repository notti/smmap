smmap
=====

This python module is a mix of the python struct and mmap modules. Function signature is like the original
mmap module with a bunch of stuff stripped out (windows, resize, msync, ...) to get faster started, and
which I don't need.


`mmap(fileno, length, format[, access[, offset]])`

fileno, length, offset are the same as in the mmap module. access only supports `ACCESS_READ` and `ACCES_WRITE`.
format needs to be a single character string:

 * `b` signed char
 * `B` unsigned char
 * `h` short
 * `H` unsigned short
 * `i` int
 * `I` unsigned int
 * `l` long
 * `L` unsigned long
 * `f` float
 * `d` double

