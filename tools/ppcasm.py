"""Minimal two-pass PowerPC assembler front end for the cup patch.

Instructions are encoded one at a time with llvm-mc; branches, symbols and data
directives are handled here so that branch targets can be absolute addresses
(game code) or labels (patch code).

Syntax (one statement per line, '#' starts a comment):
  label:                 define a label at the current address
  .org ADDR              continue assembling at ADDR (must not go backwards)
  .align N               pad with zeros to a multiple of N
  .space N               N zero bytes
  .byte/.short/.long V   data; V may be a number or label
  .ascii "text"          raw bytes (no terminator); \\0 and \\n escapes allowed
  .set NAME, VALUE       define a constant
  b/bl/beq/... TARGET    TARGET = label or absolute address; 'cr7, TARGET' allowed
  @ha(x) @h(x) @l(x)     high-adjusted / high / low 16 bits of a symbol or number
"""
import re
import subprocess

COND = {'beq': (12, 2), 'bne': (4, 2), 'blt': (12, 0), 'bge': (4, 0),
        'bgt': (12, 1), 'ble': (4, 1), 'bdnz': (16, 0)}


def _encode(line):
    r = subprocess.run(['llvm-mc', '--triple=powerpc-unknown-eabi', '--show-encoding'],
                       input=line + '\n', capture_output=True, text=True)
    m = re.search(r'encoding: \[([^\]]*)\]', r.stdout)
    if not m or r.returncode != 0:
        raise ValueError(f'cannot assemble "{line}": {r.stderr.strip()}')
    return bytes(int(x, 16) for x in m.group(1).split(','))


class Assembler:
    def __init__(self, base, symbols=None):
        self.base = base
        self.symbols = dict(symbols or {})

    def _value(self, text):
        text = text.strip()
        m = re.fullmatch(r'([A-Za-z_.]\w*)\s*([+-])\s*(.+)', text)
        if m and m.group(1) in self.symbols:
            v = self.symbols[m.group(1)]
            o = self._value(m.group(3))
            return v + o if m.group(2) == '+' else v - o
        if text in self.symbols:
            return self.symbols[text]
        return int(text, 0)

    def _subst(self, line):
        def rep(m):
            v = self._value(m.group(2)) & 0xffffffff
            if m.group(1) == 'ha':
                r = ((v + 0x8000) >> 16) & 0xffff
            elif m.group(1) == 'h':
                r = (v >> 16) & 0xffff
            else:
                r = v & 0xffff
            # signed for addi-style use is handled by llvm-mc accepting 0..65535 only for
            # logical ops; for addi/lwz offsets convert to signed
            return str(r)
        line = re.sub(r'@(ha|h|l)\(([^)]*)\)', rep, line)
        line = re.sub(r'\bcr(\d)\b', r'\1', line)
        def const(m):
            return str(self._value(m.group(0)))
        line = re.sub(r'\b[A-Z_][A-Z0-9_]*(?:\s*[+-]\s*(?:0x[0-9a-fA-F]+|\d+))?', const, line)
        # displacement form "@l(x)(rN)" produced "123(rN)": make 16-bit values signed
        def signed(m):
            v = int(m.group(1))
            return str(v - 0x10000 if v >= 0x8000 else v) + m.group(2)
        line = re.sub(r'\b(\d+)(\(\d+\))', signed, line)
        return line

    def _statements(self, text):
        for raw in text.splitlines():
            line = raw.split('#')[0].strip()
            if not line:
                continue
            while True:
                m = re.match(r'^([A-Za-z_.]\w*):\s*(.*)$', line)
                if not m:
                    break
                yield ('label', m.group(1))
                line = m.group(2).strip()
            if line:
                yield ('stmt', line)

    def _size(self, line, pc):
        op = line.split()[0]
        arg = line[len(op):].strip()
        if op == '.org':
            return self._value(arg) - pc
        if op == '.align':
            n = self._value(arg)
            return (-pc) % n
        if op == '.space':
            return self._value(arg)
        if op == '.byte':
            return len(arg.split(','))
        if op == '.short':
            return 2 * len(arg.split(','))
        if op == '.long':
            return 4 * len(arg.split(','))
        if op == '.ascii':
            return len(self._string(arg))
        if op == '.set':
            return 0
        return 4

    @staticmethod
    def _string(arg):
        s = arg.strip()
        assert s.startswith('"') and s.endswith('"'), arg
        return s[1:-1].encode('latin1').decode('unicode_escape').encode('latin1')

    def assemble(self, text):
        stmts = list(self._statements(text))
        # pass 1: labels and constants
        pc = self.base
        for kind, v in stmts:
            if kind == 'label':
                self.symbols[v] = pc
                continue
            if v.startswith('.set'):
                name, val = v[4:].split(',', 1)
                self.symbols[name.strip()] = self._value(val)
                continue
            pc += self._size(v, pc)
        # pass 2: encode
        out = bytearray()
        pc = self.base
        for kind, v in stmts:
            if kind == 'label' or v.startswith('.set'):
                continue
            op = v.split()[0]
            arg = v[len(op):].strip()
            if op in ('.org', '.align', '.space'):
                n = self._size(v, pc)
                assert n >= 0, f'.org goes backwards at {pc:#x}'
                out += bytes(n)
            elif op in ('.byte', '.short', '.long'):
                w = {'.byte': 1, '.short': 2, '.long': 4}[op]
                for x in arg.split(','):
                    out += (self._value(x) & ((1 << (8 * w)) - 1)).to_bytes(w, 'big')
            elif op == '.ascii':
                out += self._string(arg)
            else:
                out += self._instruction(v, pc)
            pc = self.base + len(out)
        return bytes(out)

    def _instruction(self, line, pc):
        m = re.match(r'^(b|bl|beq|bne|blt|bge|bgt|ble|bdnz)\s+(?:cr(\d),\s*)?(\S+)$', line)
        if m:
            op, crf, target = m.group(1), int(m.group(2) or 0), self._value(m.group(3))
            off = target - pc
            if op in ('b', 'bl'):
                assert -0x2000000 <= off < 0x2000000, f'branch out of range at {pc:#x}'
                return (0x48000000 | (off & 0x3fffffc) | (op == 'bl')).to_bytes(4, 'big')
            bo, bi = COND[op]
            if op != 'bdnz':
                bi += 4 * crf
            assert -0x8000 <= off < 0x8000, f'conditional branch out of range at {pc:#x}'
            return (0x40000000 | bo << 21 | bi << 16 | (off & 0xfffc)).to_bytes(4, 'big')
        return _encode(self._subst(line))
