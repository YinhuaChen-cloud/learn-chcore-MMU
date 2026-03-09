# gdb_dump_bst.py
import gdb, struct

NODE_SIZE = 24  # 4(value) + 4(pad) + 8(left) + 8(right)

def read_node(addr):
    inf = gdb.selected_inferior()
    data = inf.read_memory(addr, NODE_SIZE).tobytes()
    value, _pad, left, right = struct.unpack("<iiQQ", data)
    return value, left, right

class DumpBST(gdb.Command):
    """dump_bst <root_addr> [max_depth]"""
    def __init__(self):
        super(DumpBST, self).__init__("dump_bst", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        argv = gdb.string_to_argv(arg)
        if len(argv) < 1:
            print("usage: dump_bst <root_addr> [max_depth]")
            return
        root = int(gdb.parse_and_eval(argv[0]))
        max_depth = int(argv[1]) if len(argv) > 1 else 64

        seen = set()

        def dfs(addr, depth):
            if addr == 0:
                return
            if depth > max_depth:
                print("  " * depth + f"{addr:#x}  <depth-limit>")
                return
            if addr in seen:
                print("  " * depth + f"{addr:#x}  <cycle>")
                return
            seen.add(addr)

            try:
                v, l, r = read_node(addr)
                print("  " * depth + f"{addr:#x}: value={v}, L={l:#x}, R={r:#x}")
                dfs(l, depth + 1)
                dfs(r, depth + 1)
            except gdb.MemoryError:
                print("  " * depth + f"{addr:#x}  <invalid memory>")

        dfs(root, 0)

DumpBST()


