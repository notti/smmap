import mmap

f = open('data' ,'r+')
x = mmap.mmap(f.fileno(), 10, 'h')
print x[0]
print x[0:3]
x[1]=5
x[2]=-5
x[5:7] = (2,3)
x[5:7] = (2,3,4)
