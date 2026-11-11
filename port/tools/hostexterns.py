#!/usr/bin/env python3
"""Host transform: block-scope `extern` redeclarations that disagree in type.

mwccarm takes each block-scope `extern` declaration on its own terms, so the
decomp's big TUs (Player.cpp declares a global as `u16` in one method and as
`char[]` in the next) compile for the DS. GCC treats every block-scope extern
of a name as the same entity across the translation unit and rejects the
second type: "conflicting declaration". The source is byte-matched and must
not change, so the port transforms its HOST COPY instead, as tools/hostgen.py
does for MMIO:

  each block-scope declaration whose type disagrees with an earlier one gets a
  private C++ name bound to the SAME linker symbol through an asm label --
      extern char data_0209f49c[];
  becomes
      extern char data_0209f49c__h2[] __asm__("data_0209f49c");
  and the name is rewritten from there to the end of the enclosing block.

Only data and extern "C" functions conflict this way (C++ functions overload
instead), and both link by their plain name, so the asm label is exact.

  hostexterns.py --check FILE...      print the files that need the transform
  hostexterns.py IN OUT               write the transformed copy
"""
import collections
import re
import sys

DECL = re.compile(r'^(\s*)extern\s+(?!"C")(.*?);\s*(//.*|/\*.*\*/\s*)?$')
IDENT = re.compile(r'[A-Za-z_]\w*')
KEYWORDS = {'const', 'volatile', 'unsigned', 'signed', 'struct', 'union', 'enum',
            'int', 'char', 'short', 'long', 'void', 'float', 'double', 'bool'}


def declared_name(body):
    """The declarator name in `TYPE NAME[...]`, `TYPE NAME(...)`, `TYPE (*NAME)(...)`."""
    b = re.sub(r'__attribute__\s*\(\(.*?\)\)', '', body)
    b = b.split('=')[0]
    # function pointer / pointer-to-array: (*NAME) or (&NAME)
    m = re.search(r'\(\s*[*&]\s*(?:const\s+)?([A-Za-z_]\w*)\s*\)', b)
    if m:
        return m.group(1), m.start(1), m.end(1)
    # the last identifier before the first '(' or '[' (or the end)
    cut = len(b)
    for ch in '([':
        i = b.find(ch)
        if i != -1:
            cut = min(cut, i)
    ids = [(mm.group(0), mm.start(), mm.end()) for mm in IDENT.finditer(b[:cut])]
    ids = [x for x in ids if x[0] not in KEYWORDS]
    return ids[-1] if ids else None


def normalize(body, name):
    return re.sub(r'\s+', ' ', re.sub(r'\b%s\b' % re.escape(name), '@', body)).strip()


def strip_code(line):
    """The line with strings, chars and comments blanked (for brace counting).
    Comments are handled before quotes, so the apostrophe in
    `/* this TU's */` is prose, not a char literal. An unterminated `/*`
    blanks the rest of the line (see unterminated_comment)."""
    out, i, n = [], 0, len(line)
    while i < n:
        c = line[i]
        if line.startswith('//', i):
            break
        if line.startswith('/*', i):
            j = line.find('*/', i + 2)
            if j == -1:
                break
            out.append(' ')
            i = j + 2
            continue
        if c in '"\'':
            q = c; out.append(' '); i += 1
            while i < n and line[i] != q:
                i += 2 if line[i] == '\\' else 1
            i += 1
            continue
        out.append(c); i += 1
    return ''.join(out)


def unterminated_comment(line):
    """True when the line opens a /* comment it does not close."""
    i, n = 0, len(line)
    while i < n:
        if line.startswith('//', i):
            return False
        if line.startswith('/*', i):
            j = line.find('*/', i + 2)
            if j == -1:
                return True
            i = j + 2
            continue
        if line[i] in '"\'':
            q = line[i]; i += 1
            while i < n and line[i] != q:
                i += 2 if line[i] == '\\' else 1
            i += 1
            continue
        i += 1
    return False


BUILTIN_TYPES = {'u8', 'u16', 'u32', 'u64', 's8', 's16', 's32', 's64', 'Fix12i', 'size_t',
                 'Vector3', 'Vector3_16', 'Matrix4x3', 'SharedFilePtr', 'dActor_c', 'fBase_c',
                 'Player', 'BMD_File', 'KCL_File'}


def header_names(include_dirs):
    """Identifiers declared in the repo headers (rough: any word in them)."""
    import os
    names = set()
    for d in include_dirs:
        for root, _, files in os.walk(d):
            for f in files:
                if f.endswith('.h'):
                    names.update(IDENT.findall(open(os.path.join(root, f), errors='replace').read()))
    return names


HEADER_NAMES = None


FUNC_DEF = re.compile(r'^(?:\s*extern\s*"C"\s*)?(?!\s*(?:extern|return|typedef|if|while|for|switch|else|do)\b)'
                      r'\s*[A-Za-z_][\w\s\*&<>,]*?(?<![:\w])([A-Za-z_]\w*)\s*\([^;]*$')
# file-scope prototypes without `extern`, and `extern "C"` one-liners
PROTO = re.compile(r'^(\s*)(?!(?:return|typedef|using|goto|delete|new|throw)\b)'
                   r'((?:extern\s*"C"\s*)?[A-Za-z_][\w\s\*&<>,]*?[\s\*&]'
                   r'(?<![:\w])[A-Za-z_]\w*\s*\([^;{}]*\)\s*(?:const\s*)?);\s*(//.*|/\*.*\*/\s*)?$')


DATA_DEF = re.compile(r'^(?!\s*(?:extern|return|typedef|using|struct|class|union|enum|namespace|static_assert|template|goto|case|default|break|continue)\b)'
                      r'\s*(?:static\s+|const\s+|volatile\s+)*[A-Za-z_][\w:<>]*(?:\s+[A-Za-z_][\w:<>]*)*[\s\*&]+'
                      r'([A-Za-z_]\w*)\s*(?:\[[^\]]*\])*\s*(?:=[^;]*|\([^;]*\))?(?:;|\s*=\s*\{[^;]*$|\s*=\s*$)')


def file_decl(line):
    """(indent, body, name, start, end) for a file-scope declaration line:
    `extern T x;`, `extern "C" T f(...);` or a bare prototype `T f(...);`."""
    m = DECL.match(line)
    if m:
        nm = declared_name(m.group(2))
        if not nm:
            return None
        k = len('extern ')          # spans are relative to the returned body
        return (m.group(1), 'extern ' + m.group(2), (nm[0], nm[1] + k, nm[2] + k))
    m = PROTO.match(line)
    if m and FUNC_DEF.match(line.replace(";", " {")):
        body = m.group(2)
        nm = declared_name(re.sub(r'^extern\s*"C"\s*', '', body))
        if nm:
            off = len(body) - len(re.sub(r'^extern\s*"C"\s*', '', body))
            return (m.group(1), body, (nm[0], nm[1] + off, nm[2] + off))
    return None


PARAM_RE = re.compile(r'^(?:(?:const|volatile|struct|union|enum|unsigned|signed)\s+)*'
                      r'[A-Za-z_][\w:]*(?:<[^()]*>)?(?:\s+(?:const|volatile))?'
                      r'(?:\s*[\*&]+\s*(?:const\s*)?|\s+)[A-Za-z_]?\w*\s*(?:\[\w*\])*$')
CONTROL = {'if', 'while', 'for', 'switch', 'return', 'sizeof', 'else', 'do', 'case',
           'goto', 'new', 'delete', 'throw', 'catch', 'operator', 'virtual'}


def looks_like_params(p):
    """True for a parameter-type list (`void *a, int`), false for call
    arguments (`a, b`, `this->x + 1`): a local `Vector3 v(a, b);` is not a
    prototype."""
    p = p.strip()
    if p in ('', 'void'):
        return True
    for part in p.split(','):
        part = part.strip()
        if part == '...':
            continue
        if part in KEYWORDS:
            continue
        if not PARAM_RE.match(part) or re.match(r'^this\b', part):
            return False
        if re.match(r'^[A-Za-z_]\w*$', part):     # a lone identifier: call arg
            return False
    return True


def block_proto(line):
    """(indent, body, (name, start, end)) for a block-scope prototype without
    `extern` (`void f(void *p);` inside a function), else None."""
    m = PROTO.match(line)
    if not m or m.group(2).lstrip().startswith(('extern', 'virtual', 'static', 'inline', 'friend')):
        return None
    body = m.group(2)
    inner = body[body.find('(') + 1:body.rfind(')')]
    if not looks_like_params(inner):
        return None
    nm = declared_name(body)
    if not nm or nm[0] in CONTROL or nm[0] in KEYWORDS:
        return None
    return (m.group(1), body, nm)


def join_decls(lines):
    """Join declarations split over several lines (`extern void f(int a,` /
    `    int b);`) so each is one line for the matchers above."""
    out, acc = [], None
    for line in lines:
        if acc is not None:
            acc = acc.rstrip('\n') + ' ' + line.strip() + '\n'
            sc = strip_code(acc)
            if sc.count('(') <= sc.count(')') and ';' in sc:
                out.append(acc); acc = None
            elif len(acc) > 2000:
                out.append(acc); acc = None
            continue
        sc = strip_code(line)
        if re.match(r'^\s*extern\s+(?!"C"\s*\{)', sc) and sc.count('(') > sc.count(')') and ';' not in sc:
            acc = line
            continue
        out.append(line)
    if acc is not None:
        out.append(acc)
    return out


def loosen(body, name_end):
    """`int f(char *a, int b)` -> `int f(...)`: mwccarm converts between
    unrelated pointer types at a call, g++ will not, and every renamed
    declaration is a C-linkage alias of one symbol. AAPCS passes integer and
    pointer arguments identically to a variadic callee, so the call is the
    same -- except for floating-point arguments (VFP registers under the
    hard-float ABI), whose declarations are left exact."""
    rest = body[name_end:]
    m = re.match(r'\s*\(', rest)
    if not m:
        return body                     # data, or a function pointer
    depth, i = 0, m.end() - 1
    for j in range(i, len(rest)):
        if rest[j] == '(':
            depth += 1
        elif rest[j] == ')':
            depth -= 1
            if depth == 0:
                params = rest[i + 1:j]
                # floats travel in VFP registers; references and class
                # objects cannot pass through `...` at all
                if re.search(r'\b(float|double)\b', params) or '(' in params or '&' in params:
                    return body
                return body[:name_end] + rest[:i] + '(...)' + rest[j + 1:]
    return body


def _scan(lines):
    """Brace-aware walk yielding (index, line, fn_depth, in_comment)."""
    kinds = []
    in_comment = False
    last_code = ''
    for idx, line in enumerate(lines):
        if in_comment:
            if '*/' in line:
                in_comment = False
            yield idx, line, None
            continue
        sc = strip_code(line)
        prev_last = last_code
        if sc.strip():
            last_code = sc.strip()
        if unterminated_comment(line):
            in_comment = True
        # fn_depth: function-body depth, or -1 directly inside a struct/class
        # body (member declarations are never transformed)
        fd = real = sum(1 for k in kinds if k == 'B')
        if kinds and kinds[-1] == 'S':
            fd = -1
        _scan.real_depth = real
        _scan.c_linkage = 'C' in kinds or bool(re.match(r'^\s*extern\s*"C"\s', line))
        # innermost of namespace / extern "C": decides how a name is mangled
        lk = [k for k in kinds if k in 'CN']
        _scan.ns_depth = kinds.count('N')
        _scan.in_ns_cxx = bool(lk) and lk[-1] == 'N' and not re.match(r'^\s*extern\s*"C"\s', line)
        yield idx, line, fd
        for i, ch in enumerate(sc):
            if ch == '{':
                head = sc[:i].rstrip()
                # strip_code blanks the "C" of extern "C" {, leaving `extern  {`
                if re.search(r'extern\s*$', head):
                    kinds.append('C')
                elif re.search(r'\bnamespace\b[\w\s:]*$', head) or \
                        (not head and re.search(r'\bnamespace\b[\w\s:]*$', prev_last)):
                    kinds.append('N')       # file scope, but C++ names get mangled
                elif re.search(r'\b(struct|class|union|enum)\b[^;()]*$', head) or \
                        (not head and re.search(r'\b(struct|class|union|enum)\b[^;()]*$', prev_last)):
                    kinds.append('S')
                else:
                    kinds.append('B')
            elif ch == '}':
                if kinds:
                    kinds.pop()


def transform(lines, hdr=frozenset()):
    """Make the TU's declarations of each global agree, as GCC requires.

    * Every block-scope `extern` gets a private C++ name bound to the same
      linker symbol (asm label), renamed to the end of its block.
    * A file-scope `extern` that disagrees with another declaration or a
      definition of the name in this TU is renamed the same way, from there
      until the name's next file-scope declaration or definition.
    * A name declared only at block scope (and in no header) gets one
      file-scope declaration -- its first block-scope type -- ahead of the
      top-level construct that first declares it, for uses mwccarm resolved
      through an earlier block's declaration."""
    lines = join_decls(lines)
    # pass 1: what the TU defines / declares at file scope
    defined, file_decl_body = set(), collections.defaultdict(set)
    all_bodies = collections.defaultdict(set)
    for idx, line, fd in _scan(lines):
        if fd is not None and fd > 0:
            m = DECL.match(line)
            nm = declared_name(m.group(2)) if m else None
            if nm:
                all_bodies[nm[0]].add(normalize(m.group(2), nm[0]))
        if fd != 0:
            continue
        fdcl = file_decl(line)
        if fdcl:
            ind, body, nm = fdcl
            # linkage is part of the identity: g++ rejects a C and a C++
            # declaration of one variable even though both name one symbol
            link = 'C:' if (_scan.c_linkage or body.lstrip().startswith('extern "C"')) else '+:'
            key = link + normalize(re.sub(r'^extern\s*("C"\s*)?', '', body), nm[0])
            file_decl_body[nm[0]].add(key)
            all_bodies[nm[0]].add(key)
            continue
        fm = FUNC_DEF.match(line)
        if fm and '::' not in line.split('(')[0]:
            defined.add(fm.group(1))
            continue
        dm = DATA_DEF.match(line)
        if dm:
            defined.add(dm.group(1))

    renames = []            # block: (fn_depth_at_decl, old, new)
    file_renames = {}       # file scope: old -> new
    rename_ns = {}          # old -> namespace depth the rename was made at
    counter = collections.Counter()
    inserted = set()
    pending_global = {}
    file_words = set()
    cur_header = ''
    inserts = {}
    top_start = 0
    out = []
    changed = 0
    prev_code = ''
    for idx, line, fd in _scan(lines):
        if fd is None:
            out.append(line)
            continue
        st = line.strip()
        if fd == 0:
            sc0 = strip_code(line)
            # identifiers DECLARED at file scope so far (a function's own
            # `struct Handler`, or a class's nested typedef, is not among them)
            dm0 = re.match(r'^\s*(?:typedef|struct|class|union|enum|using|extern|namespace)\b', sc0)
            if dm0 or DATA_DEF.match(line):
                file_words.update(IDENT.findall(sc0))
            if '(' in sc0 and not sc0.strip().startswith(('#', 'extern', 'typedef')):
                cur_header = sc0
        if fd == 0 and st and not st.startswith(('#', '//', '/*', '*', '}')) and \
           (prev_code.endswith((';', '}', '{')) or prev_code == '' or prev_code.startswith(('#', '//'))):
            top_start = len(out)
        if st:
            prev_code = st
        m = DECL.match(line)
        nm = declared_name(m.group(2)) if m else None
        kw = 'extern '
        if m:
            ind, body = m.group(1), m.group(2)
        elif fd > 0:
            bp = block_proto(line)
            if bp:
                ind, body, nm = bp
                kw = ''
        is_func = bool(nm) and bool(re.match(r'\s*\(', body[nm[2]:])) if nm else False
        if nm and fd > 0 and is_func and not _scan.c_linkage:
            nm = None               # C++ linkage: overloads, never a conflict
        if nm and fd > 0:
            name, s_, e_ = nm
            counter[name] += 1
            new = '%s__h%d' % (name, counter[name])
            if is_func:
                body = loosen(body, e_)
            type_ids = set(IDENT.findall(body[:s_] + ' ' + body[e_:])) - KEYWORDS
            # inside a member function, a type may be the class's own nested
            # one (daCoin_c::Handler): only names declared at file scope count
            pool = file_words if '::' in cur_header else (file_words | hdr)
            visible = all(t in pool or t in BUILTIN_TYPES for t in type_ids)
            if (visible and name not in inserted and name not in defined
                    and name not in file_decl_body):
                # One file-scope declaration for uses outside the declaring
                # blocks, under its own private name too (a header may declare
                # the name with yet another type).
                g = '%s__g' % name
                inserts.setdefault(top_start, []).append(
                    'extern %s __asm__("%s");\n' % ((body[:s_] + g + body[e_:]).strip(), name))
                inserted.add(name)
                pending_global[name] = g
            body = body[:s_] + new + body[e_:]
            out.append('%s%s%s __asm__("%s");\n' % (ind, kw, body.rstrip(), name))
            renames.append((fd, name, new))
            changed += 1
            continue
        fdcl = file_decl(line) if fd == 0 else None
        if fdcl:
            ind, body, (name, s_, e_) = fdcl
            f_is_func = bool(re.match(r'\s*\(', body[e_:]))
            c_link = _scan.c_linkage or body.lstrip().startswith('extern "C"')
            if f_is_func and not c_link:
                fdcl = None
            elif _scan.ns_depth:
                # a namespace member is reached qualified (Ns::f), so it keeps
                # its name whatever its linkage
                fdcl = None
        if fdcl:
            if name in defined or len(file_decl_body[name]) > 1 or name in inserted:
                counter[name] += 1
                new = '%s__f%d' % (name, counter[name])
                if f_is_func:
                    body = loosen(body, e_)
                body = body[:s_] + new + body[e_:]
                out.append('%s%s __asm__("%s");\n' % (ind, body.rstrip(), name))
                file_renames[name] = new
                rename_ns[name] = _scan.ns_depth   # a namespace's renames end with it
                changed += 1
                continue
            file_renames.pop(name, None)
        elif fd == 0:
            fm = FUNC_DEF.match(line)
            if fm and '::' not in line.split('(')[0]:
                file_renames.pop(fm.group(1), None)
            else:
                dm = DATA_DEF.match(line)
                if dm:
                    file_renames.pop(dm.group(1), None)
        for _, old, new in renames:
            line = re.sub(r'(?<![\w.>:])%s\b' % re.escape(old), new, line)
        # drop renames made inside a namespace that has since closed
        for n_ in [n_ for n_, d_ in rename_ns.items() if d_ > _scan.ns_depth]:
            file_renames.pop(n_, None)
            rename_ns.pop(n_, None)
        ns_decl = _scan.ns_depth > 0 and fd == 0 and file_decl(line) is not None
        if fd >= 0 and not ns_decl:   # members and namespace members keep their names
            for old, new in file_renames.items():
                line = re.sub(r'(?<![\w.>:])%s\b' % re.escape(old), new, line)
        out.append(line)
        # block renames end with their block; once back at file scope, the
        # names declared only in blocks resolve to their file-scope copy
        sc = strip_code(line)
        if pending_global and _scan.real_depth + sc.count('{') - sc.count('}') <= 0 and '}' in sc:
            for n_, g_ in pending_global.items():
                file_renames.setdefault(n_, g_)
            pending_global.clear()
        if '}' in sc and renames:
            depth_after = _scan.real_depth + sc.count('{') - sc.count('}')
            ended = [r for r in renames if r[0] > depth_after]
            renames = [r for r in renames if r[0] <= depth_after]
            # mwccarm keeps a block's extern visible after the block: later
            # uses in the same function find the file-scope copy
            for _, old_, _n in ended:
                if old_ in pending_global and not any(r[1] == old_ for r in renames):
                    file_renames[old_] = pending_global[old_]
    final = []
    for i, l in enumerate(out):
        final.extend(inserts.get(i, []))
        final.append(l)
    transform.conflicts = sum(1 for v in all_bodies.values() if len(v) > 1)
    return final, changed


SCALAR_INIT = re.compile(r'^(\s*)(?:const\s+)?((?:unsigned\s+|signed\s+)?(?:int|long|short|char|bool|'
                         r'[su](?:8|16|32|64)|Fix12i|size_t|[A-Za-z_]\w*\s*\*+|void\s*\*+))\s*(?<=[\s*])'
                         r'([A-Za-z_]\w*)\s*=\s*([^;{}]+);\s*(//.*)?$')


def top_level_comma(expr):
    d = 0
    for ch in strip_code(expr):
        if ch in '([':
            d += 1
        elif ch in ')]':
            d -= 1
        elif ch == ',' and d == 0:
            return True
    return False


def typedef_names(include_dir):
    """Names the headers make typedefs (and not struct tags): mwccarm accepts
    `struct Vector3 v;` for a typedef'd Vector3, g++ does not."""
    import os
    tds, tags = set(), set()
    for root, _, files in os.walk(include_dir):
        for f in files:
            if not f.endswith('.h'):
                continue
            t = open(os.path.join(root, f), errors='replace').read()
            tds.update(re.findall(r'\btypedef\b[^;{}]*?\b([A-Za-z_]\w*)\s*;', t))
            tds.update(re.findall(r'\}\s*([A-Za-z_]\w*)\s*;', t) if 'typedef' in t else [])
            tags.update(re.findall(r'\b(?:struct|class|union)\s+([A-Za-z_]\w*)\s*[{:]', t))
    return tds - tags


def spelling_fixups(lines, tdnames):
    """Spellings mwccarm takes and g++ rejects, each meaning-preserving:
    `extern "C" extern T x;`, `extern struct X;` and `struct T` for a
    typedef'd T."""
    out = []
    tdre = re.compile(r'\bstruct\s+(%s)\b(?!\s*[{:])' % '|'.join(map(re.escape, sorted(tdnames)))) if tdnames else None
    for line in lines:
        # mwccarm's call-range pragma is scoped by its #pragma push/pop; g++
        # ignores push/pop but knows ARM `long_calls`, and would put every
        # later declaration of the TU under it (changing the function types)
        if re.match(r'^\s*#\s*pragma\s+(long_calls|no_long_calls|long_calls_off)\b', line):
            line = '// (host) ' + line.lstrip()
        line = re.sub(r'\bextern\s*"C"\s+extern\b', 'extern "C"', line)
        # an `inline` out-of-class member definition that other TUs call out of
        # line (mwccarm emitted it anyway; g++ only emits inline functions a
        # TU uses): drop `inline` so the host emits it too
        line = re.sub(r'^inline\s+(?=[A-Za-z_]\w*::~?[A-Za-z_]\w*\s*\()', '', line)
        # a C TU's own booleans: C++ already has false/true, with these values
        if re.match(r'^\s*enum\s*\{\s*false\s*,\s*true\s*\}\s*;', line):
            line = '// (host) ' + line.lstrip()
        # C's `T x[];` at file scope is a declaration of an array defined
        # elsewhere; C++ wants it spelled extern
        if re.match(r'^(?!\s*(?:extern|typedef|static|return)\b)[A-Za-z_][\w\s\*]*\b[A-Za-z_]\w*\s*\[\s*\]\s*;', line):
            line = 'extern ' + line
        # a storage class inside a cast: `((extern int (*)(...))f)`
        line = re.sub(r'\(\s*extern\s+(?=[A-Za-z_][\w\s\*]*\(\s*\*\s*\))', '(', line)
        # one-line local twins: `typedef struct T {...} T;` and the anonymous
        # `typedef struct {...} T;` -- in C++ `struct T {...};` is the same type
        # and lets the TU's `struct T x;` spelling stand
        line = re.sub(r'\btypedef\s+struct\s+([A-Za-z_]\w*)\s*(\{[^{}]*\})\s*\1\s*;', r'struct \1 \2;', line)
        line = re.sub(r'\btypedef\s+struct\s*(\{[^{}]*\})\s*([A-Za-z_]\w*)\s*;', r'struct \2 \1;', line)
        line = re.sub(r'^(\s*)extern\s+((?:struct|class|union)\s+[A-Za-z_]\w*\s*;)', r'\1\2', line)
        if tdre:
            line = tdre.sub(r'\1', line)
        out.append(line)
    # mwccarm's `#pragma cplusplus off` compiles what follows as C, so its
    # functions get C linkage; on the host that is an extern "C" region
    opened = False
    for k, line in enumerate(out):
        if re.match(r'^\s*#\s*pragma\s+cplusplus\s+off\b', line) and not opened:
            out[k] = 'extern "C" {  // (host) #pragma cplusplus off\n'
            opened = True
        elif re.match(r'^\s*#\s*pragma\s+cplusplus\s+(on|reset)\b', line):
            out[k] = ('}  // (host) #pragma cplusplus on\n' if opened else '// (host) ' + line.lstrip())
            opened = False
    if opened:
        out.append('}  // (host) end of #pragma cplusplus off\n')
    # the same twins spread over several lines
    i = 0
    while i < len(out):
        m = re.match(r'^(\s*)typedef\s+struct\s*([A-Za-z_]\w*)?\s*\{\s*(//.*)?$', out[i])
        if m:
            depth, j = 0, i
            while j < len(out):
                sc = strip_code(out[j])
                depth += sc.count('{') - sc.count('}')
                if depth <= 0 and j > i or (depth <= 0 and '}' in sc):
                    break
                j += 1
            if j < len(out):
                cm = re.match(r'^(\s*)\}\s*([A-Za-z_]\w*)\s*;\s*(//.*)?$', out[j])
                if cm and (m.group(2) is None or m.group(2) == cm.group(2)):
                    out[i] = '%sstruct %s {\n' % (m.group(1), cm.group(2))
                    out[j] = '%s};\n' % cm.group(1)
            i = j + 1 if j > i else i + 1
            continue
        i += 1
    return out


# matched against strip_code() output, where extern "C" reads `extern    `
ASM_FUNC = re.compile(r'^\s*(?:extern\s*(?:"C")?\s*)?(?:static\s+|inline\s+)*asm\b[^;{]*\(')
ASM_STMT = re.compile(r'^(\s*)asm\s*\{[^{}]*\}\s*;?\s*(//.*)?$')


def strip_asm(lines):
    """Drop hand-written ARM `asm` function definitions (and mwccarm's
    `#pragma thumb`): a host compiler cannot take them. What each did is
    provided natively for the host (port/vita/game_host.cpp); the TU's C
    functions stay. The removed span is replaced by a one-line marker."""
    out, i, n = [], 0, len(lines)
    while i < n:
        line = lines[i]
        if re.match(r'^\s*#\s*pragma\s+(thumb|arm)\b', line):
            out.append('// (host) ' + line.lstrip()); i += 1
            continue
        # an inline `asm { ... }` statement inside a C function (CP15 reads,
        # the udf of __builtin_trap): no host meaning; the functions that
        # matter are replaced as a whole in port/vita/game_host.cpp
        sm = ASM_STMT.match(line)
        if sm:
            out.append('%s/* (host) asm statement dropped */\n' % sm.group(1)); i += 1
            continue
        # the same, spread over lines: `asm {` ... `}`
        sm = re.match(r'^(\s*)asm\s*\{\s*(//.*)?$', line)
        if sm:
            j = i + 1
            while j < n and not re.match(r'^\s*\}\s*;?\s*(//.*)?$', lines[j]):
                j += 1
            out.append('%s/* (host) asm statement dropped */\n' % sm.group(1))
            out.extend(['\n'] * (j - i))
            i = j + 1
            continue
        if ASM_FUNC.match(strip_code(line)):
            depth, j, seen = 0, i, False
            while j < n:
                sc = strip_code(lines[j])
                depth += sc.count('{') - sc.count('}')
                seen = seen or '{' in sc
                if seen and depth <= 0:
                    break
                if not seen and ';' in sc:      # a prototype, not a definition
                    break
                j += 1
            if seen:
                name = re.search(r'([A-Za-z_]\w*)\s*\(', strip_code(line).split('asm', 1)[1])
                out.append('// (host) asm %s: provided natively\n' % (name.group(1) if name else '?'))
                out.extend(['\n'] * (j - i))   # keep line numbers stable
                i = j + 1
                continue
        out.append(line); i += 1
    return out


# The game's own C runtime (src/cstd4.cpp, MSL) defines a few names the host
# C library also defines. The host's win: the game's copy is renamed out of
# the way (its callers bind to the host's, with the same contract).
HOST_LIBC = {'abort', 'exit'}


def rename_libc_defs(lines):
    out = []
    for idx, line, fd in _scan(lines):
        if fd == 0:
            m = re.match(r'^(\s*(?:extern\s*"C"\s*)?[A-Za-z_][\w\s\*]*?\b)(%s)(\s*\([^;]*)$'
                         % '|'.join(sorted(HOST_LIBC)), line)
            if m and ';' not in strip_code(line):
                line = '%s%s__game%s' % (m.group(1), m.group(2), m.group(3))
        out.append(line)
    return out


def _private_names():
    import os
    f = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'vita', 'game_private_names.txt')
    try:
        return {l.strip() for l in open(f) if l.strip() and not l.startswith('#')}
    except OSError:
        return set()


PRIVATE = _private_names()


def private_labels(lines):
    """asm labels naming a game-private runtime name (see
    port/vita/game_private_names.txt) bind to its sm64ds_game_ spelling."""
    if not PRIVATE:
        return lines
    rx = re.compile(r'__asm__\("(%s)"\)' % '|'.join(map(re.escape, sorted(PRIVATE))))
    return [rx.sub(lambda m: '__asm__("sm64ds_game_%s")' % m.group(1), l) for l in lines]


def split_goto_inits(lines):
    """In a TU that uses goto, every block-scope `T x = e;` (non-const scalar
    T) becomes `T x; x = e;`. g++ rejects a goto that jumps past an
    initialization where mwccarm takes it; for a scalar the split is the same
    program, wherever it is applied."""
    if not any(re.search(r'\bgoto\b', strip_code(l)) for l in lines):
        return lines
    res = list(lines)
    for idx, line, fd in _scan(lines):
        if fd is None or fd <= 0:
            continue
        if re.match(r'^\s*const\b', line):
            continue
        m = SCALAR_INIT.match(line)
        if m and not top_level_comma(m.group(4)):
            res[idx] = '%s%s %s; %s = %s;\n' % (m.group(1), m.group(2), m.group(3),
                                               m.group(3), m.group(4).strip())
    return res


def main():
    import os
    repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    hdr = header_names([os.path.join(repo, 'include')])
    if len(sys.argv) >= 2 and sys.argv[1] == '--check':
        # a TU needs the transform when it declares some name with two types
        for f in sys.argv[2:]:
            transform(open(os.path.join(repo, f) if not os.path.isabs(f) else f,
                           errors='replace').readlines(), hdr)
            if transform.conflicts:
                print(f)
        return
    src, dst = sys.argv[1], sys.argv[2]
    # `--or ORIG`: the hostgen copy may not exist (hostgen writes nothing for
    # a TU that is all asm); then transform the original instead
    if len(sys.argv) >= 5 and sys.argv[3] == '--or' and not os.path.exists(src):
        src = sys.argv[4]
    lines = strip_asm(open(src, errors='replace').readlines())
    lines = spelling_fixups(lines, typedef_names(os.path.join(repo, 'include')))
    out, n = transform(lines, hdr)
    out = split_goto_inits(out)
    renamed = rename_libc_defs(out)
    libc_hdr = renamed != out
    out = renamed
    out = private_labels(out)
    # mwccarm intrinsics these TUs use, spelled for g++
    prelude = ['// host copy: port/tools/hostexterns.py\n',
               '#include <string.h>\n',
               '#define __builtin__clear(p, n) memset((void *)(p), 0, (n))\n']
    if libc_hdr:     # its own exit/abort renamed: calls go to the host's
        prelude.append('#include <stdlib.h>\n')
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    open(dst, 'w').writelines(prelude + out)
    print('hostexterns: %s: %d declaration(s) renamed' % (src, n), file=sys.stderr)


if __name__ == '__main__':
    main()
