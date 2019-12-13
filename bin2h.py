import array
import os
import sys

data=array.array('B')
size=0

if len(sys.argv)!=3:
	raise ValueError("Usage: bin2h.py source.bin array_name")
	
try:
	size=os.path.getsize(sys.argv[1])
except:
	raise ValueError('File not found')

file=open(sys.argv[1],'rb')
data.fromfile(file,size)
file.close()

print ('static const uint8_t %s[] PROGMEM ={' % sys.argv[2])

str=''

for i in range(0,size):

	str+='0x%2.2x,' % data[i]

	if (i&15)==15:
		print (str)
		str=''
	
if str!='':
	print (str)
	
print ('};')
