#!/usr/bin/env python3
"""fuzz.py - differential tests for Alpine mode's translator (linux/runtime/lx_dbt*.c).

    fuzz.py [--seed N] [--cases N] [--only KIND,...] OUT.S

Writes an ARM assembly program of random test cases. Each case loads
random values into r0-r12, the flags and the VFP registers, runs one
instruction (or a small sequence: a conditional skip, a call and return, a
jump table...), then stores r0-r12, the flags, d0-d15, the FPSCR and a
64-byte data buffer. The program writes all of it to stdout. Run it natively
(qemu-arm -cpu arm1176, an ARMv6KZ with VFPv2: the reference) and through
psihost (qemu-arm -cpu ti925t: an ARMv4T, as the 5mx) and compare: see
run-fuzz.sh. Values that depend on where the program is loaded are made
relative to the program's own base first.
"""
import random, sys

KINDS = ['dp', 'dppc', 'mul', 'ls', 'extra', 'ldm', 'media', 'misc', 'excl', 'vfp', 'vfpls', 'flow']

R = random.Random()

def reg(no=()):
    while True:
        r = R.randint(0, 12)
        if r not in no:
            return r

def cond():
    return R.choice(['', '', '', 'eq', 'ne', 'cs', 'cc', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le'])

def imm8r():
    v = R.randint(0, 255)
    rot = R.randint(0, 15) * 2
    return ((v >> rot) | (v << (32 - rot))) & 0xFFFFFFFF if rot else v

def op2(allow_regshift=True):
    k = R.randint(0, 3 if allow_regshift else 2)
    if k == 0:
        return '#%d' % imm8r()
    if k == 1:
        return 'r%d' % reg()
    if k == 2:
        sh = R.choice(['lsl', 'lsr', 'asr', 'ror'])
        n = R.randint(1, 31)
        if R.random() < 0.1:
            return 'r%d, rrx' % reg()
        return 'r%d, %s #%d' % (reg(), sh, n)
    return 'r%d, %s r%d' % (reg(), R.choice(['lsl', 'lsr', 'asr', 'ror']), reg())

DP = ['and', 'eor', 'sub', 'rsb', 'add', 'adc', 'sbc', 'rsc', 'orr', 'mov', 'bic', 'mvn']
TEST = ['tst', 'teq', 'cmp', 'cmn']

def g_dp():
    if R.random() < 0.25:
        return ['%s%s r%d, %s' % (R.choice(TEST), cond(), reg(), op2())]
    o = R.choice(DP)
    s = R.choice(['', 's'])
    if o in ('mov', 'mvn'):
        return ['%s%s%s r%d, %s' % (o, cond(), s, reg(), op2())]
    return ['%s%s%s r%d, r%d, %s' % (o, cond(), s, reg(), reg(), op2())]

def g_dppc():
    # pc as an operand: the result made relative to the base (r12 is not used)
    rd = reg((12,))
    c = cond()
    k = R.randint(0, 3)
    if k == 0:
        ins = 'add%s r%d, pc, #%d' % (c, rd, R.randint(0, 255) * 4)
    elif k == 1:
        ins = 'sub%s r%d, pc, #%d' % (c, rd, R.randint(0, 255) * 4)
    elif k == 2:
        ins = 'add%s r%d, pc, r%d' % (c, rd, reg((12, rd)))
    else:
        ins = 'add%s r%d, r%d, pc' % (c, rd, reg((12, rd)))
    return addr(rd, '__base') + [ins] + addr(12, '__base') + ['sub r%d, r%d, r12' % (rd, rd), 'mov r12, #0']

def g_mul():
    k = R.randint(0, 5)
    d, n, m, a = reg(), reg(), reg(), reg()
    s = R.choice(['', 's'])
    if k == 0: return ['mul%s%s r%d, r%d, r%d' % (cond(), s, d, n, m)]
    if k == 1: return ['mla%s%s r%d, r%d, r%d, r%d' % (cond(), s, d, n, m, a)]
    lo = reg(); hi = reg((lo,))
    o = R.choice(['umull', 'smull', 'umlal', 'smlal', 'umaal'])
    if o == 'umaal': s = ''
    return ['%s%s%s r%d, r%d, r%d, r%d' % (o, cond(), s, lo, hi, n, m)]

# memory: r11 points into the data buffer (64 bytes) at +16..+40
# the addresses of __data, __base and __out are worked out once, at the start,
# and kept on the stack (8 words: [data, base, out, ...]); DEPTH is how much the
# case has pushed since
DEPTH = [0]
SLOT = {'__data': 0, '__base': 4, '__out': 8}
def addr(r, label):
    """r = the address of label"""
    return ['ldr r%d, [sp, #%d]' % (r, SLOT[label] + DEPTH[0])]

def membase(rb=11, align=None):
    off = R.randint(16, 40)
    if align: off &= ~(align - 1)
    return addr(rb, '__data') + ['add r%d, r%d, #%d' % (rb, rb, off)]

def g_ls():
    rd = reg((11,))
    c = cond()
    b = R.choice(['', 'b'])
    l = R.choice(['ldr', 'str'])
    k = R.randint(0, 4)
    al = 4 if not b else None
    pre = membase(11, al)
    if k == 0: ins = '%s%s%s r%d, [r11, #%d]' % (l, c, b, rd, R.randint(-12, 12) & (~3 if al else ~0))
    elif k == 1: ins = '%s%s%s r%d, [r11, #%d]!' % (l, c, b, rd, R.randint(-12, 12) & (~3 if al else ~0))
    elif k == 2: ins = '%s%s%s r%d, [r11], #%d' % (l, c, b, rd, R.randint(-12, 12) & (~3 if al else ~0))
    elif k == 3:
        rm = reg((11, rd))
        return pre + ['mov r%d, #%d' % (rm, R.randint(0, 3) * (4 if al else 1)), '%s%s%s r%d, [r11, r%d]' % (l, c, b, rd, rm)] + addr(12, '__data') + ['sub r11, r11, r12', 'mov r12, #0']
    else:
        rm = reg((11, rd))
        return pre + ['mov r%d, #%d' % (rm, R.randint(0, 3)), '%s%s%s r%d, [r11, -r%d, lsl #2]' % (l, c, b, rd, rm)] + addr(12, '__data') + ['sub r11, r11, r12', 'mov r12, #0']
    return pre + [ins] + addr(12, '__data') + ['sub r11, r11, r12', 'mov r12, #0']

def g_extra():
    c = cond()
    k = R.randint(0, 5)
    if k < 4:
        rd = reg((11,))
        o = R.choice(['ldrh', 'strh', 'ldrsh', 'ldrsb'])
        al = 2 if o != 'ldrsb' else None
        pre = membase(11, al)
        off = R.randint(-8, 8) & (~1 if al else ~0)
        f = R.choice(['[r11, #%d]' % off, '[r11, #%d]!' % off, '[r11], #%d' % off])
        return pre + ['%s%s r%d, %s' % (o, c, rd, f)] + addr(12, '__data') + ['sub r11, r11, r12', 'mov r12, #0']
    # ldrd / strd: an even register pair, base r11 (not in the pair)
    rd = R.choice([0, 2, 4, 6, 8])
    o = R.choice(['ldrd', 'strd'])
    pre = membase(11, 4)
    off = R.randint(-2, 2) * 4
    f = R.choice(['[r11, #%d]' % off, '[r11, #%d]!' % off, '[r11], #%d' % off, '[r11]'])
    return pre + ['%s%s r%d, r%d, %s' % (o, c, rd, rd + 1, f)] + addr(12, '__data') + ['sub r11, r11, r12', 'mov r12, #0']

def g_ldm():
    c = cond()
    o = R.choice(['ldmia', 'ldmib', 'ldmda', 'ldmdb', 'stmia', 'stmib', 'stmda', 'stmdb'])
    regs = sorted(R.sample([r for r in range(11)], R.randint(1, 5)))
    w = R.choice(['', '!'])
    pre = membase(11, 4)
    return pre + ['%s%s r11%s, {%s}' % (o, c, w, ', '.join('r%d' % r for r in regs))] + addr(12, '__data') + ['sub r11, r11, r12', 'mov r12, #0']

MEDIA3 = ['sadd16', 'sasx', 'ssax', 'ssub16', 'sadd8', 'ssub8', 'qadd16', 'qasx', 'qsax', 'qsub16', 'qadd8', 'qsub8',
          'shadd16', 'shasx', 'shsax', 'shsub16', 'shadd8', 'shsub8', 'uadd16', 'uasx', 'usax', 'usub16', 'uadd8', 'usub8',
          'uqadd16', 'uqasx', 'uqsax', 'uqsub16', 'uqadd8', 'uqsub8', 'uhadd16', 'uhasx', 'uhsax', 'uhsub16', 'uhadd8', 'uhsub8']

def g_media():
    c = cond()
    k = R.randint(0, 9)
    d, n, m = reg(), reg(), reg()
    rot = R.choice(['', '', ', ror #8', ', ror #16', ', ror #24'])
    if k == 0: return ['%s%s r%d, r%d%s' % (R.choice(['uxtb', 'uxth', 'sxtb', 'sxth', 'uxtb16', 'sxtb16']), c, d, m, rot)]
    if k == 1: return ['%s%s r%d, r%d, r%d%s' % (R.choice(['uxtab', 'uxtah', 'sxtab', 'sxtah', 'uxtab16', 'sxtab16']), c, d, n, m, rot)]
    if k == 2: return ['%s%s r%d, r%d' % (R.choice(['rev', 'rev16', 'revsh']), c, d, m)]
    if k == 3: return ['%s r%d, r%d, r%d' % (R.choice(MEDIA3), d, n, m), 'sel%s r%d, r%d, r%d' % (c, reg(), reg(), reg())]
    if k == 4:
        if R.random() < 0.5: return ['pkhbt%s r%d, r%d, r%d%s' % (c, d, n, m, R.choice(['', ', lsl #%d' % R.randint(1, 31)]))]
        return ['pkhtb%s r%d, r%d, r%d, asr #%d' % (c, d, n, m, R.randint(1, 32))]
    if k == 5: return ['%s%s r%d, #%d, r%d%s' % (R.choice(['ssat', 'usat']), c, d, R.randint(1, 31), m, R.choice(['', ', lsl #%d' % R.randint(1, 31), ', asr #%d' % R.randint(1, 32)]))]
    if k == 6: return ['%s%s r%d, #%d, r%d' % (R.choice(['ssat16', 'usat16']), c, d, R.randint(1, 15), m)]
    if k == 7: return ['%s%s r%d, r%d, r%d' % (R.choice(['smuad', 'smuadx', 'smusd', 'smusdx', 'smmul', 'smmulr', 'usad8']), c, d, n, m)]
    if k == 8:
        a = reg()
        return ['%s%s r%d, r%d, r%d, r%d' % (R.choice(['smlad', 'smladx', 'smlsd', 'smlsdx', 'smmla', 'smmlar', 'smmls', 'smmlsr', 'usada8']), c, d, n, m, a)]
    lo = reg(); hi = reg((lo,))
    return ['%s%s r%d, r%d, r%d, r%d' % (R.choice(['smlald', 'smlaldx', 'smlsld', 'smlsldx']), c, lo, hi, n, m)]

def g_misc():
    c = cond()
    d, n, m, a = reg(), reg(), reg(), reg()
    k = R.randint(0, 4)
    if k == 0: return ['clz%s r%d, r%d' % (c, d, m)]
    if k == 1: return ['%s%s r%d, r%d, r%d' % (R.choice(['qadd', 'qsub', 'qdadd', 'qdsub']), c, d, m, n)]
    if k == 2:
        # (the translator does not set Q when smla's addition overflows: DECISIONS.md 39; cleared here)
        return ['%s%s r%d, r%d, r%d, r%d' % (R.choice(['smlabb', 'smlabt', 'smlatb', 'smlatt', 'smlawb', 'smlawt']), c, d, n, m, a),
                'mrs r12, cpsr', 'bic r12, r12, #0x08000000', 'msr cpsr_f, r12']
    if k == 3: return ['%s%s r%d, r%d, r%d' % (R.choice(['smulbb', 'smulbt', 'smultb', 'smultt', 'smulwb', 'smulwt']), c, d, n, m)]
    lo = reg(); hi = reg((lo,))
    return ['%s%s r%d, r%d, r%d, r%d' % (R.choice(['smlalbb', 'smlalbt', 'smlaltb', 'smlaltt']), c, lo, hi, n, m)]

def g_excl():
    rd = reg((11,)); st = reg((11, rd))
    k = R.randint(0, 2)
    pre = membase(11, 8)
    if k == 0: body = ['ldrex r%d, [r11]' % rd, 'add r%d, r%d, #1' % (rd, rd), 'strex r%d, r%d, [r11]' % (st, rd)]
    elif k == 1: body = ['ldrexb r%d, [r11]' % rd, 'strexb r%d, r%d, [r11]' % (st, rd)]
    else: body = ['ldrexh r%d, [r11]' % rd, 'strexh r%d, r%d, [r11]' % (st, rd)]
    return pre + body + addr(12, '__data') + ['sub r11, r11, r12', 'mov r12, #0']

def sreg(): return 's%d' % R.randint(0, 31)
def dreg(): return 'd%d' % R.randint(0, 15)

def g_vfp():
    c = cond()
    k = R.randint(0, 9)
    t = R.choice(['f32', 'f64'])
    rg = sreg if t == 'f32' else dreg
    if k <= 2: return ['%s%s.%s %s, %s, %s' % (R.choice(['vadd', 'vsub', 'vmul', 'vdiv', 'vnmul', 'vmla', 'vmls', 'vnmla', 'vnmls']), c, t, rg(), rg(), rg())]
    if k == 3: return ['%s%s.%s %s, %s' % (R.choice(['vabs', 'vneg', 'vsqrt', 'vmov']), c, t, rg(), rg())]
    if k == 4:
        return ['%s.%s %s, %s' % (R.choice(['vcmp', 'vcmpe']), t, rg(), R.choice([rg(), '#0'])), 'vmrs%s APSR_nzcv, fpscr' % c]
    if k == 5:
        if t == 'f32': return ['vcvt%s.f64.f32 %s, %s' % (c, dreg(), sreg())]
        return ['vcvt%s.f32.f64 %s, %s' % (c, sreg(), dreg())]
    if k == 6: return ['%s%s.%s.%s %s, %s' % (R.choice(['vcvt', 'vcvtr']), c, R.choice(['s32', 'u32']), t, sreg(), rg())]
    if k == 7: return ['vcvt%s.%s.%s %s, %s' % (c, t, R.choice(['s32', 'u32']), rg(), sreg())]
    if k == 8:
        r1 = reg(); r2 = reg((r1,))
        return R.choice([['vmov%s r%d, %s' % (c, r1, sreg())], ['vmov%s %s, r%d' % (c, sreg(), r1)],
                         ['vmov%s r%d, r%d, %s' % (c, r1, r2, dreg())], ['vmov%s %s, r%d, r%d' % (c, dreg(), r1, r2)]])
    rm = R.choice([0, 1, 2, 3])
    return ['vmrs r%d, fpscr' % reg(), 'bic r12, r12, r12', 'orr r12, r12, #%d' % (rm << 22), 'vmsr fpscr, r12',
            '%s%s.%s %s, %s, %s' % (R.choice(['vadd', 'vmul', 'vdiv']), c, t, rg(), rg(), rg()), 'mov r12, #0', 'vmsr fpscr, r12']

def g_vfpls():
    c = cond()
    pre = membase(11, 4)
    k = R.randint(0, 3)
    if k == 0: body = ['%s%s %s, [r11, #%d]' % (R.choice(['vldr', 'vstr']), c, R.choice([sreg(), dreg()]), R.randint(-3, 3) * 4)]
    elif k == 1:
        n = R.randint(1, 3); s0 = R.randint(0, 12)
        body = ['%s%s r11%s, {%s}' % (R.choice(['vldmia', 'vstmia']), c, R.choice(['', '!']), ', '.join('d%d' % (s0 + i) for i in range(n)))]
    elif k == 2:
        n = R.randint(1, 3); s0 = R.randint(0, 25)
        body = ['%s%s r11!, {%s}' % (R.choice(['vldmdb', 'vstmdb']), c, ', '.join('s%d' % (s0 + i) for i in range(n)))]
    else:
        n = R.randint(1, 3); s0 = R.randint(0, 12)
        body = ['%s%s r11!, {%s}' % (R.choice(['vldmdb', 'vstmdb']), c, ', '.join('d%d' % (s0 + i) for i in range(n)))]
    return pre + body + addr(12, '__data') + ['sub r11, r11, r12', 'mov r12, #0']

FLOW_N = [0]
def g_flow():
    FLOW_N[0] += 1
    L = 'L%d' % FLOW_N[0]
    k = R.randint(0, 5)
    c = R.choice(['eq', 'ne', 'cs', 'cc', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le'])
    d = reg((12,))
    if k == 0: return ['b%s %s_skip' % (c, L), 'add r%d, r%d, #1' % (d, d), '%s_skip:' % L, 'add r%d, r%d, #2' % (d, d)]
    if k == 1: return ['bl%s %s_fn' % (c, L), 'b %s_end' % L, '%s_fn:' % L, 'add r%d, r%d, #5' % (d, d), 'bx lr', '%s_end:' % L]
    if k == 2:
        # a jump table, as gcc makes one
        i = reg((12, d))
        return ['and r%d, r%d, #3' % (i, i), 'cmp r%d, #2' % i, 'addls pc, pc, r%d, lsl #2' % i, 'b %s_d' % L,
                'b %s_0' % L, 'b %s_1' % L, 'b %s_2' % L,
                '%s_0:' % L, 'add r%d, r%d, #10' % (d, d), 'b %s_end' % L, '%s_1:' % L, 'add r%d, r%d, #20' % (d, d), 'b %s_end' % L,
                '%s_2:' % L, 'add r%d, r%d, #30' % (d, d), 'b %s_end' % L, '%s_d:' % L, 'add r%d, r%d, #40' % (d, d), '%s_end:' % L]
    if k == 3:
        i = reg((12, d))
        return ['and r%d, r%d, #3' % (i, i), 'ldr r12, =%s_t - %s_p - 8' % (L, L), '%s_p:' % L, 'add r12, pc, r12', 'ldr pc, [r12, r%d, lsl #2]' % i,
                '%s_t:' % L, '.word %s_0 - __base' % L, '.word %s_1 - __base' % L, '.word %s_0 - __base' % L, '.word %s_1 - __base' % L,
                '%s_0:' % L, 'add r%d, r%d, #7' % (d, d), 'b %s_end' % L, '%s_1:' % L, 'add r%d, r%d, #9' % (d, d), '%s_end:' % L, 'mov r12, #0'] if False else \
               ['b%s %s_skip' % (c, L), 'mov r%d, #77' % d, '%s_skip:' % L]
    if k == 4: return ['push {r%d, lr}' % d, 'bl %s_fn' % L, 'pop {r%d, lr}' % d, 'b %s_end' % L, '%s_fn:' % L, 'push {r4, lr}', 'add r%d, r%d, #3' % (d, d), 'pop {r4, pc}', '%s_end:' % L]
    return ['mov%s r%d, #%d' % (c, d, R.randint(0, 255)), 'mrs r%d, cpsr' % reg((12, d)), 'and r%d, r%d, #0xf0000000' % (d, d)]

GEN = {'dp': g_dp, 'dppc': g_dppc, 'mul': g_mul, 'ls': g_ls, 'extra': g_extra, 'ldm': g_ldm, 'media': g_media,
       'misc': g_misc, 'excl': g_excl, 'vfp': g_vfp, 'vfpls': g_vfpls, 'flow': g_flow}

def fval():
    k = R.randint(0, 9)
    if k == 0: return R.choice([0, 0x80000000, 0x7F800000, 0xFF800000, 0x7FC00000, 0x7F800001, 0x00000001, 0x3F800000])
    if k == 1: return R.randint(0, 0x007FFFFF)          # subnormal
    return R.getrandbits(32) if k < 4 else (R.randint(0x3C000000, 0x4C000000) | (R.randint(0, 1) << 31))

def main():
    a = sys.argv[1:]
    seed, cases, only, out = 1, 400, None, None
    while a:
        x = a.pop(0)
        if x == '--seed': seed = int(a.pop(0))
        elif x == '--cases': cases = int(a.pop(0))
        elif x == '--only': only = a.pop(0).split(',')
        else: out = x
    R.seed(seed)
    kinds = only or KINDS
    L = []
    w = L.append
    w('\t.arch armv6kz\n\t.fpu vfp\n\t.syntax unified\n\t.arm\n\t.text\n\t.global _start\n__base:\n_start:')
    w('\tsub sp, sp, #32')
    w('\tadr r0, __base')
    w('\tstr r0, [sp, #4]')
    w('\tldr r1, 9f')
    w('\tadd r1, r0, r1')
    w('\tstr r1, [sp, #0]')
    w('\tldr r1, 9f + 4')
    w('\tadd r1, r0, r1')
    w('\tstr r1, [sp, #8]')
    w('\tmov r10, r1')
    w('\tb 8f')
    w('9:\t.word __data - __base')
    w('\t.word __out - __base')
    w('8:')
    for n in range(cases):
        kind = R.choice(kinds)
        DEPTH[0] = 4
        body = GEN[kind]()
        DEPTH[0] = 0
        w('@ case %d: %s' % (n, kind))
        w('\tstr r10, [sp, #-4]!')
        DEPTH[0] = 4
        # inputs: the data buffer, the registers, the flags, the FPSCR, the VFP registers
        for x in addr(12, '__data'): w(x if x.endswith(':') else '\t' + x)
        for i in range(16):
            w('\tldr r11, =0x%08x' % R.getrandbits(32))
            w('\tstr r11, [r12, #%d]' % (i * 4))
        for i in range(32):
            w('\tldr r11, =0x%08x' % fval())
            w('\tvmov s%d, r11' % i)
        w('\tldr r11, =0x%08x' % (R.randint(0, 3) << 22))
        w('\tvmsr fpscr, r11')
        for r in range(13):
            w('\tldr r%d, =0x%08x' % (r, R.getrandbits(32) if R.random() < 0.8 else R.choice([0, 1, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0x7FFF, 0x8000, 0xFF, 0x80])))
        w('\tmsr cpsr_f, #0x%x' % (R.randint(0, 15) << 28))
        for ins in body:
            w(ins if ins.endswith(':') else '\t' + ins)
        # outputs (r10 is the output pointer: kept on the stack)
        w('\tstr r0, [sp, #-4]!')
        DEPTH[0] = 8
        w('\tldr r0, [sp, #4]')
        w('\tstmia r0!, {r1-r9}')
        w('\tstr r11, [r0], #4')
        w('\tstr r12, [r0], #4')
        w('\tmrs r1, cpsr')
        w('\tand r1, r1, #0xf8000000')
        w('\tstr r1, [r0], #4')
        w('\tvmrs r1, fpscr')
        w('\tstr r1, [r0], #4')
        w('\tvstmia r0!, {d0-d15}')
        w('\tldr r1, [sp], #4')
        DEPTH[0] = 4
        w('\tstr r1, [r0], #4')           # r0
        w('\tstr r10, [r0], #4')
        for x in addr(1, '__data'): w(x if x.endswith(':') else '\t' + x)
        w('\tldmia r1!, {r2-r9}')
        w('\tstmia r0!, {r2-r9}')
        w('\tldmia r1!, {r2-r9}')
        w('\tstmia r0!, {r2-r9}')
        w('\tadd sp, sp, #4')
        DEPTH[0] = 0
        w('\tmov r10, r0')
        w('\tb 1f\n\t.ltorg\n1:')
    # write the output and exit
    # open(argv[1], O_WRONLY|O_CREAT|O_TRUNC, 0644), and write it all
    w('\tldr r0, [sp, #32 + 8]')
    w('\tldr r1, =0x241')
    w('\tldr r2, =0644')
    w('\tmov r7, #5')
    w('\tsvc #0')
    w('\tmov r4, r0')
    w('\tldr r5, [sp, #8]')
    w('7:\tsubs r2, r10, r5')
    w('\tbeq 6f')
    w('\tmov r0, r4')
    w('\tmov r1, r5')
    w('\tmov r7, #4')
    w('\tsvc #0')
    w('\tcmp r0, #0')
    w('\tble 6f')
    w('\tadd r5, r5, r0')
    w('\tb 7b')
    w('6:')
    w('\tmov r0, #0')
    w('\tmov r7, #248')
    w('\tsvc #0')
    w('\t.ltorg')
    w('\t.data')
    w('\t.align 3')
    w('__data:\n\t.space 64')
    w('\t.bss')
    w('\t.align 3')
    w('__out:\n\t.space %d' % (cases * 260))
    src = '\n'.join(L) + '\n'
    # (adr __out: in .bss, out of adr's reach: load its address pc-relatively instead)

    open(out, 'w').write(src)

if __name__ == '__main__':
    main()
