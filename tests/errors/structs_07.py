from lpython import i32, dataclass

@dataclass
class S:
    x: i32
    y: i32

def main0():
    s: S = S(y=2)

main0()