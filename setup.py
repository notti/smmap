from distutils.core import setup, Extension
setup(name="smmap", version="1.0",
              ext_modules=[Extension("smmap", ["mmap.c"])])
