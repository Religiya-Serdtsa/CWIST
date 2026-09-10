#!/usr/bin/env python3
"""Hand-encode a FileDescriptorSet for tests/test_proto_gen_sample.proto.

The cwist proto descriptor-set test must not depend on protoc being installed,
so this emits the same descriptor protoc would produce (field order preserved,
including the synthetic oneof behind the proto3 `optional` field) using raw
wire-format writes — no protobuf library required.

Usage: make_sample_descriptor.py OUT_FILE
"""
import sys

# descriptor.proto field numbers used below:
# FileDescriptorSet.file=1; FileDescriptorProto: name=1 package=2
# message_type=4 enum_type=5 service=6 syntax=12
# DescriptorProto: name=1 field=2 nested_type=3 oneof_decl=8 options=7
# FieldDescriptorProto: name=1 number=3 label=4 type=5 type_name=6
# oneof_index=9 proto3_optional=17
# OneofDescriptorProto.name=1; EnumDescriptorProto: name=1 value=2
# EnumValueDescriptorProto: name=1 number=2; MessageOptions.map_entry=7
# ServiceDescriptorProto: name=1 method=2; MethodDescriptorProto.name=1
LABEL_OPTIONAL, LABEL_REPEATED = 1, 3
T_DOUBLE, T_INT64, T_UINT64, T_INT32, T_FIXED64, T_FIXED32, T_BOOL, T_STRING = 1, 3, 4, 5, 6, 7, 8, 9
T_MESSAGE, T_UINT32, T_ENUM, T_SFIXED32, T_SFIXED64, T_SINT32, T_SINT64 = 11, 13, 14, 15, 16, 17, 18
T_FLOAT = 2


def varint(v: int) -> bytes:
    v &= (1 << 64) - 1
    out = bytearray()
    while v >= 0x80:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    out.append(v)
    return bytes(out)


def tag(num: int, wire: int) -> bytes:
    return varint((num << 3) | wire)


def vint(num: int, value: int) -> bytes:
    return tag(num, 0) + varint(value)


def ldelim(num: int, payload: bytes) -> bytes:
    return tag(num, 2) + varint(len(payload)) + payload


def sfield(num: int, text: str) -> bytes:
    return ldelim(num, text.encode("utf-8"))


def field(name: str, number: int, label: int, ptype: int,
          type_name: str = "", oneof_index: int | None = None,
          proto3_optional: bool = False) -> bytes:
    out = sfield(1, name) + vint(3, number) + vint(4, label) + vint(5, ptype)
    if type_name:
        out += sfield(6, type_name)
    if oneof_index is not None:
        out += vint(9, oneof_index)
    if proto3_optional:
        out += vint(17, 1)
    return out


def enum(name: str, values: list[tuple[str, int]]) -> bytes:
    out = sfield(1, name)
    for vname, vnum in values:
        out += ldelim(2, sfield(1, vname) + vint(2, vnum))
    return out


def message(name: str, fields: list[bytes], nested: list[bytes] = None,
            oneofs: list[str] = None, map_entry: bool = False) -> bytes:
    out = sfield(1, name)
    for f in fields:
        out += ldelim(2, f)
    for n in nested or []:
        out += ldelim(3, n)
    for o in oneofs or []:
        out += ldelim(8, sfield(1, o))
    if map_entry:
        out += ldelim(7, vint(7, 1))  # options { map_entry: true }
    return out


def service(name: str, methods: list[str]) -> bytes:
    out = sfield(1, name)
    for m in methods:
        out += ldelim(2, sfield(1, m))
    return out


def build() -> bytes:
    pkg = "proto_gen_sample"
    fq = lambda n: f".{pkg}.{n}"

    color = enum("Color", [("COLOR_UNSPECIFIED", 0), ("COLOR_RED", 1), ("COLOR_GREEN", 2)])

    inner = message("Inner", [
        field("note", 1, LABEL_OPTIONAL, T_STRING),
        field("delta", 2, LABEL_OPTIONAL, T_SINT64),
    ])

    outer = message("Outer", [
        field("name", 1, LABEL_OPTIONAL, T_STRING),
        field("id", 2, LABEL_OPTIONAL, T_INT64),
        field("active", 3, LABEL_OPTIONAL, T_BOOL),
        field("ids", 4, LABEL_REPEATED, T_INT64),
        field("tags", 5, LABEL_REPEATED, T_STRING),
        field("inner", 6, LABEL_OPTIONAL, T_MESSAGE, fq("Inner")),
        field("items", 7, LABEL_REPEATED, T_MESSAGE, fq("Inner")),
        field("color", 8, LABEL_OPTIONAL, T_ENUM, fq("Color")),
        field("shades", 9, LABEL_REPEATED, T_ENUM, fq("Color")),
    ])

    counts_entry = message("CountsEntry", [
        field("key", 1, LABEL_OPTIONAL, T_STRING),
        field("value", 2, LABEL_OPTIONAL, T_INT64),
    ], map_entry=True)
    labels_entry = message("LabelsEntry", [
        field("key", 1, LABEL_OPTIONAL, T_UINT32),
        field("value", 2, LABEL_OPTIONAL, T_STRING),
    ], map_entry=True)

    gadget = message("Gadget", [
        field("f32", 1, LABEL_OPTIONAL, T_FIXED32),
        field("f64", 2, LABEL_OPTIONAL, T_FIXED64),
        field("sf32", 3, LABEL_OPTIONAL, T_SFIXED32),
        field("sf64", 4, LABEL_OPTIONAL, T_SFIXED64),
        field("ratio", 5, LABEL_OPTIONAL, T_DOUBLE),
        field("score", 6, LABEL_OPTIONAL, T_FLOAT),
        field("ids64", 7, LABEL_REPEATED, T_FIXED64),
        field("weights", 8, LABEL_REPEATED, T_DOUBLE),
        field("text", 9, LABEL_OPTIONAL, T_STRING, oneof_index=0),
        field("code", 10, LABEL_OPTIONAL, T_SINT32, oneof_index=0),
        field("inner", 11, LABEL_OPTIONAL, T_MESSAGE, fq("Inner"), oneof_index=0),
        field("counts", 12, LABEL_REPEATED, T_MESSAGE, fq("Gadget.CountsEntry")),
        field("labels", 13, LABEL_REPEATED, T_MESSAGE, fq("Gadget.LabelsEntry")),
        # proto3 `optional` arrives as a synthetic oneof member
        field("maybe", 14, LABEL_OPTIONAL, T_INT32, oneof_index=1, proto3_optional=True),
    ], nested=[counts_entry, labels_entry], oneofs=["payload", "_maybe"])

    fdesc = (sfield(1, "test_proto_gen_sample.proto") + sfield(2, pkg) +
             ldelim(4, inner) + ldelim(4, outer) + ldelim(4, gadget) +
             ldelim(5, color) +
             ldelim(6, service("Greeter", ["SayHello"])) +
             sfield(12, "proto3"))
    return ldelim(1, fdesc)  # FileDescriptorSet.file


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: make_sample_descriptor.py OUT_FILE")
    with open(sys.argv[1], "wb") as fh:
        fh.write(build())
    print(f"wrote {sys.argv[1]}")
