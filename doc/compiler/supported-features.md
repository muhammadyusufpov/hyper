# Compiler supported features

This list reflects what **`hyper compile`** can lower today (JIT and `--emit-exe`). For syntax samples see `doc/examples/`.

## Language constructs

- Variables: `let`, `let mut`, typed bindings (`name: Type = …`, `Array[T]`, `Dict[K, V]`)
- Functions: `fn` / `def`, parameters including `ref` (mutable binding; struct instances share field storage)
- Control flow: `if` / `elif` / `else`, `while`, `for` / `for-in`, `break` / `continue`, ternary `a if cond else b`
- Error flow: `raise`, `raises` on functions, `handle attempt else fallback` (no `try` / `except`)
- Operators: arithmetic (`+`, `-`, `*`, `/`, `//`, `%`, `**`), comparisons, `and` / `or`, compound assignment
- Literals: integers, floats, strings, f-strings, lists, dicts, `None`, booleans
- Structs: fields with `pub` / `mut`, methods, `__init__`, field get/set; traits (method name + arity)
- Modules: `import m`, `import m as alias`, `from m import name`
- Decorators: `@parallel`, `@vectorize` on `for` (sequential loops today; same per-index results)

## Builtins and standard library (compile path)

| Feature | Notes |
|---------|--------|
| `print(...)` | Variadic |
| `open(path, mode?)` | Buffered file handle |
| `with open(...) as f:` | Auto-close; file methods |
| File methods | `read`, `readline`, `readlines`, `write`, `seek`, `tell`, `size`, `flush`, `close`, … |
| `with open_mmap(path) as m:` | `read_chunk(offset, size)` |
| `input(prompt?)` | Stdin line read |
| `clock()` | Seconds since UNIX epoch (`f64`) |
| Collection methods | list/array `len()`, `append(x)`; dict `len()`, `keys()`; string `len()` |
| Dict get/set | Hash map (`IndexMap` in JIT, open addressing in AOT); insertion order kept |
| String methods | Full Python-compatible set on compile path: `upper`/`lower`/`capitalize`/`title`/`swapcase`, `strip`/`lstrip`/`rstrip`, `startswith`/`endswith`, `split`/`rsplit`, `replace`, `join`, `find`/`rfind`/`index`/`rindex`, `count`, `isdigit`/`isalpha`/`isalnum`/`isspace`/`islower`/`isupper`/`istitle`/`isascii`, `center`/`ljust`/`rjust`/`zfill`, `removeprefix`/`removesuffix`, `partition`/`rpartition` |
| `import json` | `loads`, `dumps`, `load`, `dump` |

Integer `/`, `%`, `//` guard division by zero at runtime.

## Codegen modes

- JIT via Cranelift (`hyper compile`)
- Object emission (`--emit-obj`)
- Executable linking with C runtime (`--emit-exe`)

## CI-verified programs

| Program | What it checks |
|---------|----------------|
| `ci/smoke.hyp` | Core language; run / JIT / `--emit-exe` output parity |
| `ci/divzero.hyp` | `RuntimeError` exit code 70 |
| `ci/io_compile.hyp` | File I/O on compile path |
| `ci/json_compile.hyp` | JSON module on compile path |
| `ci/mmap_compile.hyp` | Memory-mapped files on compile path |
| `ci/input_compile.hyp` | `input()` on compile path |
| `ci/clock_compile.hyp` | `clock()` on compile path |
| `ci/collections_compile.hyp` | list/array/dict `len`, `append`, `keys` on compile path |
| `ci/dict_compile.hyp` | 256-key dict get/set, overwrite, insertion-order print/`keys()` (JIT and `--emit-exe`) |
| `ci/strings_compile.hyp` | string methods on compile path (JIT and `--emit-exe`) |
| `ci/break_continue.hyp` | `break` / `continue` in `while`, `for` and `for-in`; run / JIT / `--emit-exe` output parity |
| `ci/raise_handle.hyp` | `raise` / `raises` / `handle` on run and compile |
| `ci/traits_compile.hyp` | Trait conformance on compile path |
| `ci/pub_mut.hyp` | `pub` / `mut` field rules on compile path |
| `ci/ref_compile.hyp` | `ref` + shared struct fields on compile path |
| `ci/vectorize_compile.hyp` | `@vectorize` / `@parallel` compile |

## Loop control flow

`break` and `continue` lower on the compile path for every loop form. `while` sends `continue` back to the condition header; `for` and `for-in` route it through a dedicated increment block so the induction variable still advances. Both are rejected outside a loop and inside a `@parallel` `for` body — see [Known limitations](known-limitations.md).

## Not compiled (see limitations)

Generics, list/dict shared `ref` payloads, production GPU/SIMD for `@vectorize`, Python library interop — [Known limitations](known-limitations.md).
