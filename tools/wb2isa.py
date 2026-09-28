#!/usr/bin/env python3
"""Convert an ISA encoding workbench export (JSON) into an ISA definition.

  tools/wb2isa.py design.json NAME [--base cpu4] [-o NAME/isa.py]

The workbench decides formats and which format each mnemonic lives in; the
definition also needs operands, opcodes and semantics. They come from here:

  formats      the workbench templates, verbatim;
  opcodes      numbered from 0 within each format, in the workbench's order;
  operands     the base ISA's operands for the same mnemonic (names, kinds,
               scales), bound to the new template's fields. Registers bind in
               order. Each immediate binds to the narrowest field that holds
               its base width, or the widest one left, so a template such as
               `iiii iiiiiiii jjjjjjjj` gives an 8-bit compare constant and a
               12-bit offset whichever letter comes first. If the format has
               fewer register fields than the base instruction, the
               instruction becomes two-address: the second source is dropped
               and its uses read the destination;
  state        STATE of the base with the workbench's register count;
  semantics    the base line for the same mnemonic (the notation is
               width-generic: sext() follows the new field width); `zeroN`
               for a new N gets `R[N] = 0`; anything else is left without
               semantics and listed, which the tools report as a gap.

Pseudo-ops, invariants, primitives and the notation preamble are the base's.
The output is a starting point to edit by hand, not a file to regenerate: once
written, <name>/isa.py is the definition.
"""
import argparse, json, os, re, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
from isatool import model


def bind_imms(base_imms, letters):
    """{operand name: letter}, width-matched; letters is {letter: width}."""
    free = dict(letters)
    out = {}
    for o in sorted(base_imms, key=lambda o: o.bits):
        if not free:
            return None
        fits = [l for l, w in free.items() if w >= o.bits]
        l = min(fits, key=lambda l: free[l]) if fits else max(free, key=lambda l: free[l])
        out[o.name] = l
        del free[l]
    return out


def convert(design, name, base):
    b = model.load(base)
    fmts = {f['id']: f for f in design['formats']}
    ngpr = design.get('regs', b.ngpr)
    formats = {}
    for f in design['formats']:
        formats[f['name']] = f['tpl']
    parsed = {n: model.Format(n, t) for n, t in formats.items()}
    rows, sems, gaps, notes = [], {}, [], []
    next_op = {n: 0 for n in formats}
    order = list(design['assign'].items())
    for mn, fid in order:
        fname = fmts[fid]['name']; f = parsed[fname]
        op = next_op[fname]; next_op[fname] += 1
        if op >= (1 << len(f.opos)):
            sys.exit(f"{fname} has {len(f.opos)} opcode bits, too few for its {sum(1 for _, x in order if x == fid)} instructions")
        bi = b.by_name.get(mn)
        if bi is None:
            m = re.fullmatch(r'zero([0-9a-f])', mn)
            rows.append((mn, fname, op, ()))
            if m and int(m.group(1), 16) < ngpr:
                sems[mn] = f"R[{int(m.group(1), 16)}] = 0"
            else:
                gaps.append(mn)
            continue
        regs = [o for o in bi.ops if o.reg]
        imms = [o for o in bi.ops if not o.reg]
        sem = b.semantics.get(mn)
        ops = []
        if len(regs) > len(f.reg_letters):
            if len(regs) - len(f.reg_letters) == 1 and len(regs) >= 2:
                drop = regs[1]
                notes.append(f"{mn}: two-address in {fname}, {drop.name} reads {regs[0].name}")
                if sem:
                    sem = re.sub(r'\b' + drop.name + r'\b', regs[0].name, sem)
                regs = [r for r in regs if r is not drop]
            else:
                notes.append(f"{mn}: needs {len(regs)} registers, {fname} has {len(f.reg_letters)}; left out")
                continue
        binding = bind_imms(imms, {l: len(f.pos[l]) for l in f.imm_letters})
        if binding is None:
            notes.append(f"{mn}: needs {len(imms)} immediates, {fname} has {len(f.imm_letters)}; left out")
            continue
        auto = dict(zip([o.name for o in imms], f.imm_letters))
        rename = {}
        for o in imms:        # an operand named for its width (imm7) follows the new width
            m = re.fullmatch(r'([a-z]+)(\d+)', o.name)
            w = len(f.pos[binding[o.name]])
            if m and int(m.group(2)) != w:
                rename[o.name] = m.group(1) + str(w)
        if rename:
            binding = {rename.get(k, k): v for k, v in binding.items()}
            auto = {rename.get(k, k): v for k, v in auto.items()}
            for old, new in rename.items():
                sem = re.sub(r'\b' + old + r'\b', new, sem) if sem else sem
        for o in bi.ops:
            if o.reg and o not in regs:
                continue
            if o.reg:
                ops.append(o.name)
            else:
                nm = rename.get(o.name, o.name)
                ops.append((nm, o.kind) + ((o.scale,) if o.scale != 1 else ()))
                w = len(f.pos[binding[nm]])
                if w < o.bits:
                    notes.append(f"{mn}: {o.name} narrows from {o.bits} to {w} bits")
        row = (mn, fname, op, tuple(ops))
        if binding != auto:
            row += ({k: v for k, v in binding.items()},)
        rows.append(row)
        if sem:
            sems[mn] = sem
        else:
            gaps.append(mn)
    forder = list(formats)
    rows.sort(key=lambda r: (forder.index(r[1]), r[2]))
    names = {r[0] for r in rows}
    pseudos = {n: v for n, v in b.pseudos.items() if v[0] in names}
    return b, ngpr, formats, rows, sems, gaps, notes, pseudos


def render(design, name, base, src):
    b, ngpr, formats, rows, sems, gaps, notes, pseudos = convert(design, name, base)
    special = dict(b.special)
    pre = re.sub(r'R\[0\.\.\d+\]', f'R[0..{ngpr - 1}]', b.preamble)
    L = [f'"""{name.upper()} instruction set definition (pure data, read by isatool/model.py).',
         '',
         f'Converted by tools/wb2isa.py from the encoding workbench design',
         f'{src} ("{design.get("name", "")}"), with operands,',
         f'semantics, pseudo-ops, invariants and primitives taken from {base}/isa.py for the',
         'same mnemonics. From here on this file is the definition: edit it, then run',
         '`make isa` (isatool/gen.py) to regenerate the tables, both executors and',
         f'docs/isa/{name}-encoding.md.',
         '']
    if gaps:
        L += ['Instructions without semantics (assemble and decode; executing one stops the',
              'machine with a message): ' + ', '.join(gaps) + '.', '']
    if notes:
        L += ['Conversion notes:'] + [f'  {n}' for n in notes] + ['']
    L += ['"""', '', f"NAME = {name!r}", '',
          'STATE = {',
          f"    'gpr': {ngpr},                   # r0..r{ngpr - 1}",
          f"    'gpr_bits': {b.gpr_bits},",
          f"    'special': {special!r},",
          '}', '', 'FORMATS = {']
    for n, t in formats.items():
        L.append(f"    {n!r}: {t!r},")
    L += ['}', '', "# (name, format, opcode = value of the format's o bits, operands in assembly order[, fields])",
          'INSTRUCTIONS = [']
    cur = None
    for r in rows:
        if r[1] != cur:
            cur = r[1]; L.append(f"    # {cur}  {formats[cur]}")
        nbits = len(model.Format(cur, formats[cur]).opos)
        extra = f", {r[4]!r}" if len(r) > 4 else ""
        L.append(f"    ({(repr(r[0]) + ','):11s} {r[1]!r}, 0b{r[2]:0{nbits}b}, {r[3]!r}{extra}),")
    L += [']', '', 'PSEUDOS = {']
    for n, (real, perm) in pseudos.items():
        L.append(f"    {n!r}: ({real!r}, {perm!r}),")
    L += ['}', '', f"INVARIANTS = {b.invariants!r}", '',
          f"PRIMITIVES = {b.primitives!r}",
          f"PRIMITIVE_PREFIX = {b.prim_prefix!r}",
          f"PRIMITIVE_C = {b.prim_c!r}",
          f"PRIMITIVE_PY = {b.prim_py!r}", '',
          f'SEMANTICS_PREAMBLE = """{pre}"""', '', 'SEMANTICS = {']
    for r in rows:
        if r[0] in sems:
            L.append(f"    {r[0]!r}: {sems[r[0]]!r},")
    L += ['}', '']
    return "\n".join(L), gaps, notes


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('design'); ap.add_argument('name')
    ap.add_argument('--base', default='cpu4')
    ap.add_argument('-o')
    a = ap.parse_args()
    design = json.load(open(a.design))
    text, gaps, notes = render(design, a.name, a.base, os.path.basename(a.design))
    out = a.o or os.path.join(ROOT, a.name, 'isa.py')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, 'w').write(text)
    print(f"wrote {os.path.relpath(out, ROOT)}: {text.count(chr(10))} lines; "
          f"{len(gaps)} without semantics{': ' + ', '.join(gaps) if gaps else ''}")
    for n in notes:
        print("  " + n)


if __name__ == '__main__':
    main()
