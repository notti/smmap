import mmap

f = open('data' ,'r+')
x = mmap.mmap(f.fileno(), 10, 'h')
print x[0]
x[1]=5
x[2]=-5