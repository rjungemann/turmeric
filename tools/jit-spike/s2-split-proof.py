#!/usr/bin/env python3
"""S2 proof-of-architecture: split an emitted TU at the preamble marker into
(a) a host-resident runtime impl (de-static'd, compiled to a .so by gcc) and
(b) a declarations-only header spliced ahead of the program half for c2mir.

Mechanical text surgery, throwaway by design -- the production version of
this is an emitter mode.  usage: split.py in.c outdir/name
"""
import re, sys, os

MARK = '/* ==== tur: end of fixed runtime preamble ==== */'

# The 11 host-TLS names (findings 15): the program half reaches them through
# tur_tls_* accessors (the emitted #else branch), so the impl .so must expose
# accessors pointing at ITS OWN (real, __thread) variables -- shadowing
# libturi's copies via the preload so both halves share one storage.
TLS = {
    '__stm_current_tx': ('void **', 'tur_tls_stm_current_tx_ptr'),
    'tur_handler_chain': ('void **', 'tur_tls_handler_chain_ptr'),
    'tur_panicking': ('int *', 'tur_tls_panicking_ptr'),
    'tur_cur_shift_reset': ('void **', 'tur_tls_cur_shift_reset_ptr'),
    'tur_current_fiber': ('void **', 'tur_tls_current_fiber_ptr'),
    'tur_fiber_cancelled_flag': ('bool *', 'tur_tls_fiber_cancelled_flag_ptr'),
    'tur_current_thread_state': ('void **', 'tur_tls_current_thread_state_ptr'),
    'tur_cancel_jmpbuf': ('jmp_buf *', 'tur_tls_cancel_jmpbuf_ptr'),
    'tur_cancel_jmpbuf_valid': ('int *', 'tur_tls_cancel_jmpbuf_valid_ptr'),
    'tur_current_scheduler_mt': ('void **', 'tur_tls_current_scheduler_mt_ptr'),
    'tur__rtv_': ('int64_t *', 'tur_tls_rtv_ptr'),
}


def de_static(text):
    """Remove one file-scope `static ` -- at line start OR after a leading
    __attribute__((...)) (the emitter writes both orders)."""
    # `static inline` loses BOTH: plain C99 `inline` at external linkage
    # emits no standalone definition, so the .so would lack the symbol
    # (found via tur_frame_init: unresolved import).
    return re.sub(r'^((?:__attribute__\(\([^)]*\)\)\s+)?)static\s+(inline\s+)?',
                  r'\1', text, count=1)


def code_part(line):
    """Line with a same-line trailing /* comment */ removed -- `int x = 0;
    /* note */` must still read as terminated by ';'."""
    m = re.search(r'/\*.*\*/\s*$', line)
    return line[:m.start()].rstrip() if m else line.rstrip()


def body_span(lines, i):
    """(start, end) inclusive of the {...} block opening at/after line i."""
    d, started = 0, False
    j = i
    while j < len(lines):
        d += lines[j].count('{') - lines[j].count('}')
        if '{' in lines[j]:
            started = True
        if started and d <= 0:
            return j
        j += 1
    return None


def split(src):
    pre_txt, prog = src.split(MARK, 1)
    lines = pre_txt.split('\n')
    impl, decls = [], []
    i = 0
    accessors = []
    while i < len(lines):
        l = lines[i]
        stripped = l.lstrip()
        at_col0 = l and not l[0].isspace()
        # file-scope function definition?  signature at col 0, has '(',
        # doesn't end ';', block opens within 3 lines
        is_fn = False
        if at_col0 and '(' in l and not stripped.startswith(('#', '/*', '//', '*')) \
           and not l.rstrip().endswith(';'):
            head = ' '.join(lines[i:i + 3])
            paren = head.find('(')
            brace = head.find('{')
            if brace != -1 and (head.find(';') == -1 or head.find(';') > brace) \
               and ('=' not in head[:brace]):
                is_fn = True
        if is_fn:
            j = body_span(lines, i)
            sig_end = i
            while '{' not in lines[sig_end]:
                sig_end += 1
            sig = '\n'.join(lines[i:sig_end + 1])
            sig = sig[:sig.index('{')].rstrip()
            desig = de_static(sig)
            body = '\n'.join(lines[i:j + 1])
            impl.append(de_static(body))
            decls.append(desig + ';')
            i = j + 1
            continue
        # file-scope object with initializer or plain decl, col 0, static
        m = re.match(r'^(?:__attribute__\(\([^)]*\)\)\s+)?static\s+'
                     r'(TUR_THREAD_LOCAL\s+|_Thread_local\s+)?(.*)$', l) \
            if at_col0 else None
        if m and not stripped.startswith('static void') and ('=' in l or l.rstrip().endswith(';')):
            rest = m.group(2)
            # variable name = last ident before '=' or before '[' or ';'
            nm = re.search(r'([A-Za-z_][A-Za-z0-9_]*)\s*(\[[^\]]*\])?\s*(=|;)', rest)
            if nm and '(' not in rest.split('=')[0]:
                name = nm.group(1)
                # multi-line initializer?
                j = i
                while not code_part(lines[j]).endswith(';'):
                    j += 1
                full = '\n'.join(lines[i:j + 1])
                tls = bool(m.group(1))
                impl.append(de_static(full))
                if tls:
                    # ONE storage for thread-locals: BOTH halves route through
                    # the HOST's tur_tls accessors (libturi).  The first proof
                    # run gave the .so its own __thread vars + accessor
                    # overrides, but dlsym(RTLD_DEFAULT) prefers the
                    # EXECUTABLE's exports over a preload, so the program half
                    # bound libturi's slots while .so code used its own --
                    # divergent state, stm-stress lost counts.  Mirror the
                    # emitted #else branch in the impl instead.
                    if name in TLS:
                        rt, acc = TLS[name]
                        dtype = full.split(name)[0]
                        dtype = re.sub(r'^.*?(TUR_THREAD_LOCAL|_Thread_local)\s+',
                                       '', dtype).strip()
                        both = [f'extern {rt} {acc}(void);']
                        if rt == 'void **':
                            both.append(f'#define {name} (*({dtype} *){acc}())')
                        else:
                            both.append(f'#define {name} (*{acc}())')
                        impl.extend(both)
                        # decls half already carries the emitted #if/#else
                        # block for these names (kept verbatim), so nothing
                        # extra is needed there -- but the emitted block's GNU
                        # branch would re-DEFINE the var under gcc; the decls
                        # half is only ever compiled by c2mir (#else), safe.
                else:
                    head_no_init = full.split('=')[0].rstrip()
                    head_no_init = de_static(head_no_init)
                    decls.append(f'extern {head_no_init};')
                i = j + 1
                continue
        # col-0 `static` PROTOTYPE (no body): the definition now lives in the
        # .so with external linkage, so a static declaration would be a
        # "declared but never defined" error in the program half.  De-static
        # in both halves.
        if at_col0 and re.match(r'^(?:__attribute__\(\([^)]*\)\)\s+)?static\s', l) and '(' in l:
            j = i
            while '{' not in lines[j] and not code_part(lines[j]).endswith(';'):
                j += 1
            if code_part(lines[j]).endswith(';') and '{' not in ' '.join(lines[i:j + 1]):
                full = '\n'.join(lines[i:j + 1])
                desig = de_static(full)
                impl.append(desig)
                decls.append(desig)
                i = j + 1
                continue
        # everything else (includes, macros, types, non-static decls,
        # comments): both halves keep it verbatim
        impl.append(l)
        decls.append(l)
        i += 1
    impl_txt = '\n'.join(impl) + '\n' + '\n'.join(accessors) + '\n'
    decls_txt = '\n'.join(decls) + '\n/* ==== S2: host-resident runtime; declarations only ==== */\n'
    return impl_txt, decls_txt + prog


def main():
    src = open(sys.argv[1]).read()
    if MARK not in src:
        sys.exit('no preamble marker')
    impl, prog = split(src)
    base = sys.argv[2]
    open(base + '.rt.c', 'w').write(impl)
    open(base + '.prog.c', 'w').write(prog)
    print(f'rt: {impl.count(chr(10))} lines   prog: {prog.count(chr(10))} lines')


if __name__ == '__main__':
    main()
