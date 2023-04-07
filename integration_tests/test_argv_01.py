from lpython import i32
from sys import argv

def test():
    exe: str = argv[0]
    i: i32
    for i in range(len(exe)-1, 0, -1):
        if exe[i] == '/':
            break
    assert exe[i+1:len(exe)] == 'test_argv_01'

test()
