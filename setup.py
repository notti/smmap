from distutils.core import setup, Extension
setup(name="mmap", version="1.0",
              ext_modules=[Extension("mmap", ["mmap.c"])])
