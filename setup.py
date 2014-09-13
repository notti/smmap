from distutils.core import setup, Extension
setup(name="smmap", version="0.2", description="struct mmap",
      author="Gernot Vormayr",
      author_email="gvormayr@gmail.com",
      url="https://github.com/notti/smmap",
      ext_modules=[Extension("smmap", ["mmap.c"])])
