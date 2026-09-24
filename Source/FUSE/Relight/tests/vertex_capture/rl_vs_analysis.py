#!/usr/bin/env python3
"""FUSE Relight RL-1.6: Python twin of the vertexshader hash component's shader analysis.

An independent implementation of capture/vertex_capture/src/vs_hash.cpp (analyzeVertexShader): the
constant ranges dxvk-remix's dxso compiler records while it translates a D3D9 vertex shader
(DxsoShaderMetaInfo maxConstIndexF / I / B), replayed over the token stream. Used by the Wine test
(rl_vertex_capture.py) to compute the expected hash with Tools/FUSE/Relight/remix_hash_ref.py, and
by rl_vertex_capture_analysis_twin (random shaders: C++ vs Python).

  analyze(tokens, swvp) -> (valid, maxF, maxI, maxB)
  vertex_shader_hash(ref, bytecode, fconsts, iconsts, bwords, swvp) -> int | None
"""
import struct

HW_LAYOUT = (256, 16, 16)
SW_LAYOUT = (8192, 2048, 2048)

# D3DSHADER_INSTRUCTION_OPCODE_TYPE
OPS = dict(NOP=0, MOV=1, ADD=2, SUB=3, MAD=4, MUL=5, RCP=6, RSQ=7, DP3=8, DP4=9, MIN=10, MAX=11, SLT=12, SGE=13,
           EXP=14, LOG=15, LIT=16, DST=17, LRP=18, FRC=19, M4x4=20, M4x3=21, M3x4=22, M3x3=23, M3x2=24, CALL=25,
           CALLNZ=26, LOOP=27, RET=28, ENDLOOP=29, LABEL=30, DCL=31, POW=32, CRS=33, SGN=34, ABS=35, NRM=36,
           SINCOS=37, REP=38, ENDREP=39, IF=40, IFC=41, ELSE=42, ENDIF=43, BREAK=44, BREAKC=45, MOVA=46, DEFB=47,
           DEFI=48, TEXKILL=65, TEX=66, EXPP=78, LOGP=79, CND=80, DEF=81, CMP=88, DP2ADD=90, DSX=91, DSY=92,
           TEXLDD=93, SETP=94, TEXLDL=95, BREAKP=96, PHASE=0xFFFD, COMMENT=0xFFFE, END=0xFFFF)
O = OPS

NO_DST = {O["NOP"], O["CALL"], O["CALLNZ"], O["LOOP"], O["RET"], O["ENDLOOP"], O["LABEL"], O["REP"], O["ENDREP"],
          O["IF"], O["IFC"], O["ELSE"], O["ENDIF"], O["BREAK"], O["BREAKC"], O["BREAKP"], O["TEXKILL"], O["PHASE"]}

# Which sources the dxso compiler loads (emitVectorAlu, emitMatrixAlu, emitControlFlow*, ...).
LOADED = {}
for n in ("MOV", "MOVA", "RCP", "RSQ", "EXP", "LOG", "LIT", "FRC", "SGN", "ABS", "NRM", "SINCOS", "EXPP", "LOGP",
          "DSX", "DSY", "REP", "IF", "TEX", "TEXLDL"):
    LOADED[O[n]] = {0}
for n in ("ADD", "SUB", "MUL", "DP3", "DP4", "MIN", "MAX", "SLT", "SGE", "DST", "POW", "CRS", "SETP", "IFC", "BREAKC",
          "M4x4", "M4x3", "M3x4", "M3x3", "M3x2"):
    LOADED[O[n]] = {0, 1}
for n in ("MAD", "LRP", "CMP", "CND", "DP2ADD"):
    LOADED[O[n]] = {0, 1, 2}
LOADED[O["LOOP"]] = {1}
LOADED[O["TEXLDD"]] = {0, 2, 3}

MATRIX_ROWS = {O["M4x4"]: 4, O["M3x4"]: 4, O["M4x3"]: 3, O["M3x3"]: 3, O["M3x2"]: 2}

REG_CONST, REG_CONSTINT, REG_CONST2, REG_CONST3, REG_CONST4, REG_CONSTBOOL = 2, 7, 11, 12, 13, 14
FLOAT_BASE = {REG_CONST: 0, REG_CONST2: 2048, REG_CONST3: 4096, REG_CONST4: 6144}


def reg_type(tok):
    return ((tok >> 28) & 7) | ((tok >> 8) & 0x18)


def analyze(tokens, swvp=False):
    """Returns (valid, maxF, maxI, maxB)."""
    fcount, icount, bcount = SW_LAYOUT if swvp else HW_LAYOUT
    if not tokens or (tokens[0] >> 16) != 0xFFFE:
        return (False, 0, 0, 0)
    major = (tokens[0] >> 8) & 0xFF
    defined = {"f": set(), "i": set(), "b": set()}
    mx = {"f": 0, "i": 0, "b": 0}

    def load(t, num, relative):
        if t in FLOAT_BASE:
            n = num + FLOAT_BASE[t]
            if relative:
                mx["f"] = fcount
            elif n not in defined["f"]:
                mx["f"] = min(max(mx["f"], n + 1), fcount)
        elif t == REG_CONSTINT:
            if num not in defined["i"]:
                mx["i"] = min(max(mx["i"], num + 1), icount)
        elif t == REG_CONSTBOOL:
            if num not in defined["b"]:
                mx["b"] = min(max(mx["b"], num + 1), bcount)

    i = 1
    n = len(tokens)
    while i < n:
        tok = tokens[i]
        op = tok & 0xFFFF
        if op == O["END"]:
            return (True, mx["f"], mx["i"], mx["b"])
        if op == O["COMMENT"]:
            i += 1 + ((tok >> 16) & 0x7FFF)
            continue
        first = i + 1
        if major >= 2:
            last = first + ((tok >> 24) & 0xF)
        elif op == O["DEF"]:
            last = first + 5
        else:
            last = first
            while last < n and tokens[last] & 0x80000000:
                last += 1
        if last > n:
            return (False, 0, 0, 0)
        i = last
        params = tokens[first:last]
        if op in (O["DEF"], O["DEFI"], O["DEFB"]):
            if params:
                t = reg_type(params[0])
                num = params[0] & 0x7FF
                if t in FLOAT_BASE:
                    defined["f"].add(num + FLOAT_BASE[t])
                elif t == REG_CONSTINT:
                    defined["i"].add(num)
                elif t == REG_CONSTBOOL:
                    defined["b"].add(num)
            continue
        if op == O["DCL"]:
            continue
        p = 0
        if op not in NO_DST and p < len(params):
            d = params[p]
            p += 1
            if d & (1 << 13) and major >= 3:
                p += 1
            if tok & (1 << 28):
                p += 1
        loaded = LOADED.get(op, set())
        s = 0
        while p < len(params):
            src = params[p]
            p += 1
            rel = bool(src & (1 << 13))
            if rel and major >= 2:
                p += 1
            if s in loaded:
                rows = MATRIX_ROWS.get(op, 1) if s == 1 else 1
                for r in range(rows):
                    load(reg_type(src), (src & 0x7FF) + r, rel)
            s += 1
    return (False, 0, 0, 0)


def vertex_shader_hash(ref, bytecode, fconsts, iconsts, bwords, swvp=False):
    """remix_hash_ref.vertex_shader with this analysis. fconsts: bytes (16 per register), iconsts:
    bytes (16 per register), bwords: bytes (u32 bit words). None when the shader is not a valid VS."""
    tokens = list(struct.unpack("<%dI" % (len(bytecode) // 4), bytecode[:len(bytecode) // 4 * 4]))
    valid, mf, mi, mb = analyze(tokens, swvp)
    if not valid:
        return None
    return ref.vertex_shader(bytecode, fconsts, mf, iconsts, mi, bwords, mb)
