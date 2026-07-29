#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ONNX 模型结构审计工具（中文输出，仅依赖 Python 标准库）。

不安装 onnx/onnxruntime，直接按 protobuf 线格式解析 ModelProto，
提取 ir_version / opset / 输入输出 / 节点算子 / 初始化张量，
并给出面向 TPU-MLIR(BM1684) 转换的初步就绪评估。

用法：python3 tools/inspect_onnx.py weights/best.onnx
"""
import sys
import os
from collections import Counter

# ---------- protobuf 线格式最小解析器 ----------
def read_varint(buf, pos):
    shift = 0
    result = 0
    while pos < len(buf):
        b = buf[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if not (b & 0x80):
            return result, pos
        shift += 7
    raise ValueError("varint 越界")

def iter_fields(buf):
    """yield (field_num, wire_type, value)。value：varint->int；2->bytes；1/5->bytes。"""
    pos = 0
    n = len(buf)
    while pos < n:
        tag, pos = read_varint(buf, pos)
        field_num, wire_type = tag >> 3, tag & 0x7
        if wire_type == 0:  # varint
            val, pos = read_varint(buf, pos)
            yield field_num, wire_type, val
        elif wire_type == 2:  # length-delimited
            length, pos = read_varint(buf, pos)
            val = buf[pos:pos + length]
            pos += length
            yield field_num, wire_type, val
        elif wire_type == 1:  # fixed64
            val = buf[pos:pos + 8]; pos += 8
            yield field_num, wire_type, val
        elif wire_type == 5:  # fixed32
            val = buf[pos:pos + 4]; pos += 4
            yield field_num, wire_type, val
        else:
            raise ValueError(f"未知 wire_type={wire_type} field={field_num}")

# ---------- ONNX 数据类型 ----------
ELEM_TYPE = {1: "FLOAT", 2: "UINT8", 3: "INT8", 6: "INT32", 7: "INT64",
             9: "STRING", 11: "DOUBLE", 12: "UINT32", 13: "UINT64", 16: "BFLOAT"}

def parse_dimension(dim_buf):
    """TensorShapeProto.Dimension: dim_value=1(varint), dim_param=2(string)。"""
    val = None
    for fn, wt, v in iter_fields(dim_buf):
        if fn == 1:
            val = v
        elif fn == 2:
            val = v.decode("utf-8", "replace") if isinstance(v, bytes) else v
    return val if val is not None else "?"

def parse_tensor_shape(shape_buf):
    """TensorShapeProto: dim=1(repeated Dimension)。"""
    dims = []
    for fn, wt, v in iter_fields(shape_buf):
        if fn == 1:
            dims.append(parse_dimension(v))
    return dims

def parse_value_info(vi_buf):
    """ValueInfoProto: name=1, type=2(TypeProto)。"""
    name = ""
    elem_type = None
    shape = []
    for fn, wt, v in iter_fields(vi_buf):
        if fn == 1:
            name = v.decode("utf-8", "replace")
        elif fn == 2:  # TypeProto
            for tfn, twt, tv in iter_fields(v):
                if tfn == 1:  # tensor_type
                    for etfn, etwt, etv in iter_fields(tv):
                        if etfn == 1:  # elem_type
                            elem_type = etv
                        elif etfn == 2:  # shape
                            shape = parse_tensor_shape(etv)
    return name, elem_type, shape

def parse_tensor_proto(tp_buf):
    """TensorProto: dims=1(repeated int64, 可能 packed), data_type=2, name=8, raw_data=9。"""
    name = ""
    data_type = None
    dims = []
    for fn, wt, v in iter_fields(tp_buf):
        if fn == 1:  # dims（repeated int64）
            if wt == 0:       # 非打包单个 dim
                dims.append(v)
            elif wt == 2:     # 打包
                pos = 0
                while pos < len(v):
                    d, pos = read_varint(v, pos)
                    dims.append(d)
        elif fn == 2:  # data_type
            data_type = v
        elif fn == 8:  # name
            name = v.decode("utf-8", "replace")
    return name, data_type, dims

def parse_node(node_buf):
    """NodeProto: op_type=4, name=3, input=1, output=2。"""
    op_type = ""
    name = ""
    n_in = 0
    n_out = 0
    for fn, wt, v in iter_fields(node_buf):
        if fn == 3:
            name = v.decode("utf-8", "replace")
        elif fn == 4:
            op_type = v.decode("utf-8", "replace")
        elif fn == 1:
            n_in += 1
        elif fn == 2:
            n_out += 1
    return op_type, name, n_in, n_out

def parse_graph(graph_buf):
    g = {"name": "", "inputs": [], "outputs": [], "nodes": [], "initializers": []}
    for fn, wt, v in iter_fields(graph_buf):
        if fn == 2:
            g["name"] = v.decode("utf-8", "replace")
        elif fn == 1:  # node
            g["nodes"].append(parse_node(v))
        elif fn == 5:  # initializer
            g["initializers"].append(parse_tensor_proto(v))
        elif fn == 11:  # input
            g["inputs"].append(parse_value_info(v))
        elif fn == 12:  # output
            g["outputs"].append(parse_value_info(v))
    return g

def parse_model(buf):
    m = {"ir_version": None, "producer": "", "opset": [], "graph": None}
    for fn, wt, v in iter_fields(buf):
        if fn == 1:  # ir_version
            m["ir_version"] = v
        elif fn == 2:  # producer_name
            m["producer"] = v.decode("utf-8", "replace")
        elif fn == 7:  # graph
            m["graph"] = parse_graph(v)
        elif fn == 8:  # opset_import (OperatorSetIdProto: domain=1, version=2)
            domain = ""
            version = None
            for ofn, owt, ov in iter_fields(v):
                if ofn == 1:
                    domain = ov.decode("utf-8", "replace")
                elif ofn == 2:
                    version = ov
            m["opset"].append((domain, version))
    return m

# TPU-MLIR (BM1684) 常见支持算子（YOLOv7 类检测模型典型集合）
COMMON_SUPPORTED = {
    "Conv", "BatchNormalization", "Relu", "LeakyRelu", "SiLU", "Sigmoid", "Mul", "Add",
    "Constant", "ConstantOfShape",
    "Concat", "Reshape", "Transpose", "Slice", "Split", "MaxPool", "GlobalAveragePool",
    "AveragePool", "Resize", "Gemm", "MatMul", "Softmax", "ReduceMean", "ReduceMax",
    "Pad", "Gather", "Shape", "Unsqueeze", "Squeeze", "Cast", "Pow", "Sqrt",
    "Sub", "Div", "Exp", "Flatten", "Tile", "Range", "Where", "Equal",
}

def fmt_shape(shape):
    return "[" + ",".join(str(d) for d in shape) + "]"

def main():
    if len(sys.argv) < 2:
        print("用法: python3 tools/inspect_onnx.py <model.onnx>")
        return 1
    path = sys.argv[1]
    if not os.path.exists(path):
        print(f"错误 | ONNX审计 | 模型文件不存在：{path}")
        return 1
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        buf = f.read()
    m = parse_model(buf)
    g = m["graph"] or {"inputs": [], "outputs": [], "nodes": [], "initializers": []}

    print("=" * 60)
    print("ONNX 模型结构审计报告")
    print("=" * 60)
    print(f"文件路径        : {path}")
    print(f"文件大小(字节)  : {size:,}")
    print(f"ir_version      : {m['ir_version']}")
    print(f"producer        : {m['producer']}")
    opset_str = ", ".join(f"{d or 'ai.onnx'}={v}" for d, v in m["opset"]) or "(无)"
    print(f"opset_import    : {opset_str}")
    print(f"graph 名称      : {g['name']}")
    print(f"节点数          : {len(g['nodes'])}")
    print(f"初始化张量数    : {len(g['initializers'])}")

    print("\n--- 输入 ---")
    for name, et, shape in g["inputs"]:
        etn = ELEM_TYPE.get(et, str(et))
        print(f"  {name} : {etn} {fmt_shape(shape)}")
    print("--- 输出 ---")
    for name, et, shape in g["outputs"]:
        etn = ELEM_TYPE.get(et, str(et))
        print(f"  {name} : {etn} {fmt_shape(shape)}")

    ops = Counter(n[0] for n in g["nodes"])
    print("\n--- 算子统计 ---")
    for op, cnt in sorted(ops.items(), key=lambda x: (-x[1], x[0])):
        flag = "" if op in COMMON_SUPPORTED else "  <需确认TPU-MLIR支持>"
        print(f"  {op:24s} x{cnt}{flag}")

    # 动态维度检查
    dyn = []
    for name, et, shape in g["inputs"] + g["outputs"]:
        if any(isinstance(d, str) for d in shape):
            dyn.append((name, shape))
    print("\n--- 动态维度检查 ---")
    if dyn:
        for name, shape in dyn:
            print(f"  {name} 含动态维度 {fmt_shape(shape)}（转换前建议固化为静态）")
    else:
        print("  未发现动态维度（batch/空间维度均为静态或带 dim_value）")

    # 初始化张量概览
    print("\n--- 初始化张量（前 5 个）---")
    for name, dt, dims in g["initializers"][:5]:
        print(f"  {name} : {ELEM_TYPE.get(dt, dt)} {fmt_shape(dims)}")
    if len(g["initializers"]) > 5:
        print(f"  ... 共 {len(g['initializers'])} 个")

    # 转换就绪评估
    print("\n--- 面向 TPU-MLIR(BM1684) 转换的初步评估 ---")
    unknown = [op for op in ops if op not in COMMON_SUPPORTED]
    if not unknown:
        print("  算子集合均为 YOLOv7 类常见算子，预期 TPU-MLIR 可处理（最终以 x86 转换结果为准）。")
    else:
        print(f"  存在需确认算子：{', '.join(unknown)}（转换时若报不支持需补充算子或改写）")
    if dyn:
        print("  含动态维度，转换前建议固化为静态 batch/尺寸。")
    if m["ir_version"] and m["ir_version"] > 9:
        print(f"  ir_version={m['ir_version']} 偏高，建议确认 TPU-MLIR 支持的 ONNX 版本范围。")
    print("  注：本评估为静态结构判断，不等于转换一定成功；最终须在 x86 TPU-MLIR 实测。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
