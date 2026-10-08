#!/usr/bin/env python3
"""离线读 global-metadata.dat (il2cpp v29)，按关键词查类/字段/方法。

为什么要有这个: 判断"远处的流送中心/ LOD 到底读的是谁"，靠猜名字不靠谱。
元数据里能拿到完整的类型表、字段表、方法表 —— 全是只读的静态信息，
不动游戏、不动内存，跑一遍就有结论。

表布局(v29 header 是 32 组 offset/size，逐个对上):
  [11] fields          12 字节/项
  [5]  methods         32 字节/项
  [10] parameters      12 字节/项
  [19] typeDefinitions 92 字节/项
用法:
  python3 il2cpp_meta_probe.py <global-metadata.dat> [关键词...]
"""
import struct
import sys
from collections import defaultdict

TABLE_FIELDS = 11
TABLE_METHODS = 5
TABLE_TYPEDEFS = 19
FIELD_ENTRY = 12
METHOD_ENTRY = 32
TYPE_ENTRY = 92


class Metadata:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        sanity, version = struct.unpack_from("<II", self.data, 0)
        if sanity != 0xFAB11BAF:
            raise SystemExit(f"不是 il2cpp 元数据 (sanity={sanity:#x})")
        self.version = version
        pairs = struct.unpack_from("<64I", self.data, 8)
        self.tables = [(pairs[i], pairs[i + 1]) for i in range(0, 64, 2)]
        self.string_off, self.string_size = self.tables[2]
        self.field_off, self.field_size = self.tables[TABLE_FIELDS]
        self.method_off, self.method_size = self.tables[TABLE_METHODS]
        self.type_off, self.type_size = self.tables[TABLE_TYPEDEFS]
        self.type_count = self.type_size // TYPE_ENTRY
        self.field_count = self.field_size // FIELD_ENTRY
        self.method_count = self.method_size // METHOD_ENTRY

    def string(self, offset):
        if offset < 0 or offset >= self.string_size:
            return f"<bad:{offset}>"
        start = self.string_off + offset
        end = self.data.find(b"\x00", start)
        return self.data[start:end].decode("utf-8", "replace")

    def type_defs(self):
        for i in range(self.type_count):
            base = self.type_off + i * TYPE_ENTRY
            d = struct.unpack_from("<17i", self.data, base)
            counts = struct.unpack_from("<4H", self.data, base + 68)
            yield {
                "index": i,
                "name": self.string(d[0]),
                "namespace": self.string(d[1]),
                "parent": d[4],
                "field_start": d[8],
                "field_count": counts[2],
                "method_start": d[9],
                "method_count": counts[0],
            }

    def field(self, index):
        base = self.field_off + index * FIELD_ENTRY
        name_i, type_i, token = struct.unpack_from("<iIi", self.data, base)
        return self.string(name_i), type_i

    def method(self, index):
        base = self.method_off + index * METHOD_ENTRY
        name_i, declaring, return_t, param_start, generic, token, flags, iflags, slot, pcount = \
            struct.unpack_from("<6i4H", self.data, base)
        return {
            "name": self.string(name_i),
            "declaring": declaring,
            "param_count": pcount,
            "flags": flags,
        }


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    meta = Metadata(sys.argv[1])
    print(f"il2cpp v{meta.version}: {meta.type_count} 类型 / {meta.field_count} 字段 / {meta.method_count} 方法")

    types = list(meta.type_defs())
    by_index = {t["index"]: t for t in types}
    keywords = [k.lower() for k in sys.argv[2:]] or [
        "streamingcenter", "envcenter", "playercenter", "lod", "streaming"
    ]

    def full(t):
        return f"{t['namespace']}.{t['name']}" if t["namespace"] else t["name"]

    # 1) 哪些字段直接踩中关键词 -> 打印声明它的类
    print("\n=== 字段命中 ===")
    seen = set()
    for t in types:
        for fi in range(t["field_start"], t["field_start"] + t["field_count"]):
            if fi < 0 or fi >= meta.field_count:
                continue
            name, type_i = meta.field(fi)
            low = name.lower()
            if any(k in low for k in keywords):
                key = (full(t), name)
                if key in seen:
                    continue
                seen.add(key)
                print(f"  {full(t):58s} . {name}")

    # 2) 哪些方法名踩中关键词 -> 打印声明它的类 + 参数个数
    print("\n=== 方法命中 ===")
    seen = set()
    for t in types:
        for mi in range(t["method_start"], t["method_start"] + t["method_count"]):
            if mi < 0 or mi >= meta.method_count:
                continue
            m = meta.method(mi)
            low = m["name"].lower()
            if any(k in low for k in keywords):
                key = (full(t), m["name"], m["param_count"])
                if key in seen:
                    continue
                seen.add(key)
                print(f"  {full(t):58s} :: {m['name']}({m['param_count']})")

    # 3) 类名命中关键词 -> 把这些类的成员整体列出(看清这个系统有什么)
    print("\n=== 相关类的成员 ===")
    for t in types:
        if not any(k in t["name"].lower() for k in keywords):
            continue
        print(f"\n-- {full(t)}  (字段 {t['field_count']} / 方法 {t['method_count']})")
        for fi in range(t["field_start"], t["field_start"] + t["field_count"]):
            if 0 <= fi < meta.field_count:
                name, _ = meta.field(fi)
                print(f"     field  {name}")
        for mi in range(t["method_start"], t["method_start"] + t["method_count"]):
            if 0 <= mi < meta.method_count:
                m = meta.method(mi)
                print(f"     method {m['name']}({m['param_count']})")


if __name__ == "__main__":
    main()
