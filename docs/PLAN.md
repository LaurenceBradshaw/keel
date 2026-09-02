# Keel — Implementation Plan

**Status:** M0 complete. `keelc` parses the §6.1 sample to a tree with no errors.
**Companion document:** `MANIFESTO.md` (the vision doc). This file is the engineering plan.

---

## 0. Relationship to the manifesto

`MANIFESTO.md` describes the language we eventually want. It is deliberately
unresolved on syntax and semantics, and its §15 phase ordering says "design the
semantics before the syntax."

That advice is correct for an experienced language designer and wrong for a
first compiler. Unanswered design questions cannot be resolved by reasoning
alone; they are resolved by implementing something and writing programs in it.

**Therefore this plan inverts the ordering.** We build a small working compiler
first, on a deliberately impoverished language, and let the implementation
generate the evidence that answers the manifesto's open questions.

The manifesto's role from here on is a **tiebreaker document**: when two
implementation options are equally cheap, pick the one the manifesto prefers.
It is not a gate, a checklist, or a scope definition.

---

## 1. The name

**Keel.** The keel is the first structural member laid down in a ship; the
entire hull is built off it. This mirrors the manifesto's central claim that
the lifetime and ownership model must be designed first and everything else
derives from it.

| Thing | Value |
| --- | --- |
| Language | Keel |
| Source extension | `.kl` |
| Compiler binary | `keelc` |
| IR name | KIR (Keel IR) |
| Symbol prefix in generated C | `kl_` |
| Runtime prefix | `kl_rt_` |

Renaming is a `sed` across the repo. Do not spend a day on this.

---

## 2. Strategy

### 2.1 Transpile to C11

Keel compiles to portable C11, which is then handed to `cc`.

This is chosen not merely because it is easier than LLVM, but because it is
**semantically forcing**. C has no destructors, no generics, no sum types, no
move semantics, and no ownership. Every feature in the manifesto must therefore
be implemented by our frontend. There is nothing to offload.

Precedent: cfront, Nim, Vala, Chicken Scheme, GHC's unregisterised backend,
Zig's C backend.

**We emit C, never C++.** Emitting C++ would let us lower destructors onto C++
destructors and generics onto templates, which produces a syntax skin over the
C++ object model rather than a language. That is the exact failure this project
exists to avoid.

### 2.2 The C backend is not the portability boundary

KIR is. Do not build a backend abstraction layer now — keep all emission in one
directory and keep KIR clean enough that an LLVM backend could consume it in
year three. Realistically that day never comes and that is fine.

### 2.3 Compiler implementation language: C++20

A compiler is overwhelmingly data structures: hash maps, dynamic arrays, string
interning, trees, worklists. Writing those from scratch in C costs months and
buys nothing.

We use C++20 in a **deliberately restrained subset**:

- No exceptions. No RTTI. No inheritance. No virtual functions.
- No templates beyond using the standard containers.
- Standard containers, `string_view`, `optional`, `span` — yes.
- Style is "C with containers and namespaces."

Two reasons for the restraint: it is the correct style for compilers, and every
line here will eventually be rewritten in Keel during bootstrap. Do not lean on
features Keel will not have.

---

## 3. Architecture

```
  .kl source
      |
      v
  Lexer            hand-written, one switch, interns identifiers
      |
      v
  Parser           recursive descent + Pratt for expressions
      |
      v
  AST              arena-allocated, u32 handles, every node has a Span
      |
      v
  Resolver         names -> declarations; builds scope tree
      |
      v
  Type checker     bidirectional; annotates AST in place
      |
      v
  Monomorphiser    worklist; generic decls -> concrete instances   [M6+]
      |
      v
  KIR lowering     -> basic blocks, three-address form, explicit temps
      |
      v
  Move / drop      dataflow over the KIR CFG; inserts drops + drop flags
  analysis
      |
      v
  C emitter        declarations pass, definitions pass, bodies pass
      |
      v
  cc               generated .c -> native binary
```

### Why KIR exists (and why the manifesto's pipeline was wrong)

`MANIFESTO.md` §15 places the ownership checker directly after the type checker,
operating on the AST. That does not work.

Deciding *which destructors run at a given exit point* is a question about
control flow: it depends on which paths reach that point, what was initialised
on each of them, and what was moved out on each of them. That is a **dataflow
problem over a control-flow graph**, not a tree walk. Rust discovered this and
moved its borrow checker off the AST onto MIR.

So: lower to a CFG **before** doing any lifetime work. KIR is not optional
sophistication; it is the thing that makes RAII implementable at all.

---

## 4. Core data structure decisions

These are load-bearing and painful to retrofit. Decide once, at M0.

| Decision | Rule |
| --- | --- |
| **Memory** | Arena-allocate the AST, types, and KIR. Never free. The compiler is a batch process; it exits. |
| **Handles** | Refer to nodes by `u32` index wrappers (`struct ExprId { uint32_t v; };`), not pointers. Arrays grow without invalidating references; nodes stay small; dumping is trivial. |
| **Identifiers** | Interned at lex time into a `SymbolId`. All name comparison is integer comparison. |
| **Spans** | Every AST node, type, and KIR instruction stores `{ File_id, u32 start, u32 end }` — half-open byte offsets, not line/column. Non-negotiable — retrofitting spans is miserable and diagnostics are ~40% of a real compiler. |
| **AST shape** | Tagged struct with a `Kind` enum + union, not a class hierarchy with visitors. Adding a node kind should not require touching eight visitor classes in a language we are still redesigning weekly. |
| **Errors** | A `Diagnostics` sink collected and reported in batch. Never `exit()` from deep in the compiler. Parser and checker both recover and continue. |

---

## 5. Locked decisions

These are settled for v0. Reopening one requires a written reason. This section
exists specifically to prevent indefinite bikeshedding.

| # | Decision |
| --- | --- |
| L1 | Backend is C11. Frontend owns all semantics. No C++ output, ever. |
| L2 | Compiler is C++20, restrained subset (§2.3). |
| L3 | Arena + `u32` handles for all IR-ish data. |
| L4 | Hand-written lexer and recursive-descent parser. No generators. Pratt parsing for expressions. |
| L5 | Bidirectional type checking (`check(expr, expected)` / `infer(expr)`). No Hindley-Milner, no global inference. |
| L6 | Function signatures and struct members are fully annotated. Inference exists only for `auto` locals. |
| L7 | All lifetime/move/drop analysis runs on the KIR CFG, never on the AST. |
| L8 | **v0 has move checking but no borrow checker.** See §8. |
| L9 | Generics by monomorphisation. |
| L10 | Golden-file tests from the first commit. |
| L11 | **Surface syntax is C++'s** (§5.1). Statement-oriented, explicit `return`, no expression-blocks. |
| L12 | Mutable by default; `const` for immutable — as in C++. See §12; this is the sharpest familiarity-vs-safety trade in the design. |
| L13 | One aggregate kind: `struct`. No classes, no inheritance, no virtual dispatch in v0. |
| L14 | No exceptions in the language. Errors are `Result` + `?`. |
| L15 | Naming: `Capitalized_snake_case` types (`String_view`, `Hash_map`), `snake_case` values and functions — the convention already used in `MANIFESTO.md` §12. |
| L16 | C declaration order (`i32 x`), so type constructors are **postfix**: `T&`, `const T&`, `T*` — as in C++. |
| L17 | `a * b;` is ambiguous between a pointer declaration and a discarded multiply under C declaration order. Resolved the Java/C# way: **an expression statement must have an effect** (D15), so the discarded-multiply reading is not a legal statement and the parser decides from syntax alone. No symbol-table feedback, so name resolution stays a separate pass after parsing. |

---

## 5.1 Syntax policy: C++ on the surface, Keel underneath

**Keel's surface syntax is C++'s.** A C or C++ developer should be able to read
Keel without a tutorial and write it after skimming one page. Declarations,
`struct`, destructors, references, templates, `if`/`while`/`for`/`switch`,
`auto`, and comments are all spelled exactly as in C++.

### This contradicts the manifesto, deliberately

`MANIFESTO.md` §14 says: *"Do not inherit C++ syntax and semantics merely
because programmers recognise them."* That guidance is overruled here, and the
reason is worth recording, because it is a real trade:

The manifesto's objection is sound about **semantics** and weak about
**spelling**. The accumulated damage in C++ is in its object model, its
lifetime rules, and its template substitution — not in the fact that a function
is written `T f( U a )`. Keel discards every one of those semantics (§8, §5).
What it keeps is the notation, which costs nothing and buys the entire adoption
story.

### The hazard this creates, and the rule that contains it

Familiar syntax with unfamiliar semantics is the one genuinely dangerous
combination. A C++ developer reading `void consume( Buffer b )` will assume a
copy; in Keel it is a move. Nothing in the syntax warns them.

The containment rule is therefore:

> **Where Keel's semantics differ from C++'s, the syntax must differ too, or
> the compiler must reject the C++ reading outright.**

Never silently redefine a construct that already means something specific in
C++. Either give it new notation, or make the old meaning a hard error with a
diagnostic that explains the difference. §6.3 lists every current divergence;
that list is the audit surface for this rule, and every future syntax addition
must be checked against it.

---

## 6. v0 language surface

Deliberately tiny. Everything below is what M1–M6 must support; nothing else.

### 6.1 Programs

```cpp
// Comments are C++'s, both forms.

i32 add( i32 a, i32 b )
{
    return a + b;
}

i32 main()
{
    auto x = add( 1, 2 );
    i32  y = 0;

    while( y < x )
    {
        y = y + 1;
    }

    for( i32 i = 0; i < 3; i++ )
    {
        y = y + i;
    }

    if( y == 6 )
    {
        return 0;
    }

    return 1;
}
```

Primitive types: `i8 i16 i32 i64 u8 u16 u32 u64 f32 f64 bool void`.

### 6.2 Structs, destructors, ownership, sum types, generics

```cpp
// --- M2: structs and value semantics ---
struct Point
{
    f64 x;
    f64 y;
};

Point origin()
{
    return Point { 0.0, 0.0 };
}

// --- M3: RAII. The destructor is spelled exactly as in C++. ---
struct Buffer
{
    u8* ptr;
    u64 len;

    Buffer( u64 n )
    {
        ptr = kl_rt_alloc( n );
        len = n;
    }

    ~Buffer()
    {
        kl_rt_free( ptr );
    }
};

// --- M4: ownership through the existing C++ parameter notation ---
void consume( Buffer b );           // by value: MOVES. The caller's b is dead afterwards.
u64  inspect( const Buffer& b );    // shared borrow; may not escape (§8)
void grow( Buffer& b );             // mutable borrow; may not escape

// --- M5: sum types. `enum class` gains payloads; `switch` gains destructuring. ---
enum class Shape
{
    Circle( f64 radius ),
    Rect( f64 width, f64 height )
};

f64 area( const Shape& s )
{
    switch( s )
    {
        case Circle( r ):    return 3.14159 * r * r;
        case Rect( w, h ):   return w * h;
    }
}

// --- M6: generics are C++20 constrained templates ---
template<Ord T>
T max( T a, T b )
{
    if( a > b )
    {
        return a;
    }
    return b;
}

// --- Error handling: Result plus the `?` propagation operator ---
Result<Config, Config_error> load_config( const Path& path )
{
    auto text   = read_file( path )?;
    auto config = parse_config( text )?;
    return Ok( config );
}
```

### 6.3 Divergences from C++ — the complete list

This list is the audit surface for the §5.1 containment rule. Every entry is
either new notation or a hard error; none silently redefines valid C++.

| # | Divergence | Why it is safe under the §5.1 rule |
| --- | --- | --- |
| D1 | Fixed-width primitives only: `i32`, `u64`, `f64`. No `int`, `long`, `char`, `unsigned`. | The C++ spellings are **rejected outright**, with a diagnostic naming the replacement. They are *not* reserved words — the lexer treats them as ordinary identifiers, and the suggestion is produced by sema's unknown-type path, which knows it is in type position and so gives a better message than the lexer could. No type name is a keyword: `i32`, `Point` and `Vector<T>` all resolve through one path. |
| D2 | **Passing by value moves; it does not copy.** Copying is explicit (`b.clone()`). | New notation is impossible here, so the compiler enforces it instead: using a moved-from value is a hard error (§8) pointing at the move. This is the most dangerous divergence and the one M4 exists to police. |
| D3 | Braces mandatory on every `if`/`while`/`for` body. | Braceless C++ is a parse error, not a reinterpretation. Kills the `goto fail;` class of bug. |
| D4 | `switch` has no fallthrough, needs no `break`, requires exhaustiveness, and matches sum-type payloads. | Payload patterns (`case Circle( r ):`) are new notation. A `switch` over a plain integer keeps C++ meaning minus fallthrough; missing cases are an error, never a silent skip. |
| D5 | No implicit conversions at all — not narrowing, not int↔float, not int↔bool, not pointer↔bool. | Always a hard error with the required cast in the message. |
| D6 | `?` postfix operator for error propagation. | No meaning in C++; pure addition. The one borrow from outside the C family, kept because no C++ notation exists for it. |
| D7 | `enum class` variants carry payloads. | Payload-free `enum class` behaves exactly as C++. Payloads are new notation. |
| D8 | No headers, no preprocessor. `import graphics;` — C++20's spelling. | `#include` is a hard error directing to `import`. |
| D9 | Reading an uninitialised variable is a compile error. | Strictly rejects programs C++ accepts; never changes the meaning of an accepted one. |
| D10 | No `new`/`delete` in safe code; `Owned<T>`, `Shared<T>`, `Weak<T>` instead. | `new` and `delete` are hard errors outside `unsafe`. |
| D11 | `template<Ord T>` constraints are checked at definition, not at instantiation. | Same syntax as C++20 concepts, strictly stricter behaviour. Errors point at the template, not the expansion. |
| D12 | `++` and `--` are **statements, not expressions**. `i++;` and `for( ...; ...; i++ )` are fine; `x = a[i++]` is a parse error. | Removes the pre/post distinction and every sequencing hazard in one move — `a[i++] = i++` is UB in C++ and is simply not expressible here. Rejects valid C++ outright rather than reinterpreting it. |
| D13 | Literal syntax: `_` is accepted as a digit separator alongside C++'s `'`; `\x` escapes take **exactly** two hex digits; unknown escapes and multi-character char literals are errors. | The separator is a pure addition — `1'000'000` keeps its C++ meaning. The rest reject what C++ accepts loosely: unbounded `\x` silently overflows, and `'ab'` is implementation-defined in C++. |
| D14 | A leading zero on a decimal literal is an error: `010` does not compile. | C++ reads it as octal, so `010` is 8 there and would be 10 here. Keel has no octal at all, so accepting it would silently change the value of valid C++ — exactly what §5.1 forbids. Rejected outright with a message naming the cause. |
| D15 | An expression statement must have an effect: only calls, assignments, and `++`/`--`. `a * b;`, `x.field;` and `arr[3];` are errors. | Rejects only statements that compute a value and discard it — already a bug, and already warned about by C++ (`-Wunused-value`). Java and C# both enumerate the legal statement expressions for exactly this reason. Multiplication inside an expression (`x = a * b;`) is untouched. |
| D16 | Operators whose relative precedence is a known C wart cannot be mixed without parentheses: bitwise (`&` `\|` `^`) with comparison, shift (`<<` `>>`) with arithmetic, and `&&` with `\|\|`. `a & b == c` is a compile error naming both readings. | C reads it as `a & (b == c)` — a mistake Ritchie acknowledged and every compiler warns about. Changing the precedence would silently alter valid C++, which §5.1 forbids; rejecting it fixes the wart *and* satisfies the containment rule, since no accepted program changes meaning. Zig takes the same approach. |
| D17 | `*` and `&` bind to the **type**, not the name, and must touch it: `u32* p;` declares a pointer, `u32 *p;` is an error. | C's declarator syntax binds them to the name, so `int *p, q;` makes `q` an `int` — a wart every C style guide works around. Adjacency is checked from the token spans, so no lexer change is needed. The rejected form gets a message naming the fix rather than the generic D15 one, since it is exactly what a C++ programmer writes from habit. Independent of D15: `a * b;` remains a useless statement either way. |
| D18 | Top-level declarations are visible throughout the file, so mutual recursion needs no forward declaration: `even` may call `odd` above it. Inside a function this does **not** apply — statements are sequential, so using a local before its declaration is still an error. | C++ requires a forward declaration at namespace scope; Rust, Java and C# do not. This accepts what C++ rejects and never reinterprets an accepted program, so it is safe under §5.1. The asymmetry between file and block scope is deliberate: declaration order carries no meaning between functions, and every meaning within one. |
| D19 | A declaration may not shadow another that is still reachable by the same unqualified name. A block local colliding with an enclosing local or with a parameter is an error. Scopes that do **not** carry outer names in — a function body relative to file scope, and later a lambda relative to its enclosing function — are *barriers*, and reusing a name across one is fine: a local may be called `count` alongside a top-level `count()`. | The wart is the *silent* failure: two locals of the same type mean picking the wrong one compiles, runs and returns a bad value. Shadowing a function with a variable is caught immediately by the type checker, so it needs no rule. Java (§6.4) and C# (CS0136) reject exactly this and keep members shadowable for the same reason; C++ allows it and papers over it with `-Wshadow`. File scope is exempt so that adding a top-level function cannot break a function body far above it — action at a distance. Rust's shadowing is same-scope rebinding (`let x = validate(x);`), which we already reject as a duplicate declaration and which this does not revisit. |

### 6.4 Not in v0

Optionals, traits beyond generic bounds, closures, `namespace`, operator
overloading, copy constructors, inheritance, virtual dispatch, `Shared<T>`,
`Weak<T>`, concurrency, reflection, coroutines, and any standard library.
Modules arrive at M7.

---

## 7. C backend contract

The emitter is where most avoidable bugs will live. These rules are not
optional.

1. **Three-address form before emission.** Every subexpression gets its own
   temporary on its own statement. This makes C's unspecified evaluation order
   irrelevant, and makes destructor insertion for temporaries tractable.
   Skipping this is the single most common transpiler mistake.
2. **Scope exit uses `goto` cleanup chains.** Early `return`, `break`,
   `continue`, and `?` must run destructors for everything live at that point in
   reverse construction order. Emit Linux-kernel-style labels
   (`goto kl_cleanup_3;`). *This is why KIR exists* — "what is live here" is a
   dataflow query.
3. **Drop flags where static analysis cannot decide.** A value conditionally
   initialised or moved on only one branch gets a hidden `bool` checked at
   cleanup. Rust does exactly this. Do not attempt to be cleverer at first.
4. **Three emission passes:** type forward-declarations, then type definitions
   in topological order (C requires complete types for by-value members; detect
   cycles and require indirection), then function prototypes, then bodies.
5. **Mangle every symbol** as `kl_<module>_<name>__<argtypes>`. Reserve the
   `kl_` prefix so user identifiers can never collide with C keywords or the
   runtime.
6. **Emit `#line` directives** mapping back to `.kl` files. Free debugger and
   error-location support for almost no work.
7. **Compile generated C with `-fwrapv -fno-strict-aliasing -std=c11`.**
   C's UB becomes Keel's UB otherwise.
8. `bool` is `<stdbool.h>`'s. Integers are `<stdint.h>` exact-width types.

A tiny `runtime/kl_rt.{h,c}` provides allocation, abort/panic, and `print`.
Keep it under 200 lines for as long as possible.

---

## 8. Ownership: start far smaller than the manifesto

The manifesto's §24 asks whether the model should resemble Rust's borrow
checker. **v0's answer is: no, and we defer the question.**

### v0 implements move checking only

A flow-sensitive dataflow analysis over the KIR CFG. Each local is in one of:

```
Uninitialised -> Live -> Moved
                   \-> MaybeMoved   (join of Live and Moved at a merge point)
```

Using a `Moved` value is an error. `MaybeMoved` at a drop point emits a drop
flag. This alone buys use-after-move detection, double-free prevention, and
correct RAII — most of manifesto §3.6, for a fraction of the work.

### v0 does not implement lifetimes

References obey one blunt structural rule instead:

> A reference may appear as a function parameter or a local binding.
> It may **not** be stored in a struct, returned from a function, or captured.

This makes dangling references unrepresentable by construction rather than by
analysis. It is roughly Hylo's (formerly Val) approach, and it is a defensible
permanent design, not merely a shortcut. Rust's borrow checker is the single
hardest part of Rust and non-lexical lifetimes took years; attempting it as a
first compiler project is how this repository dies.

Revisit only after M7, with real programs as evidence.

### Related work to read before designing the final model

- **Hylo / Val** — mutable value semantics. Closest existing language to this
  manifesto's actual spirit. Read their design docs first.
- **Rust's MIR + NLL** — for the dataflow machinery, not necessarily the model.
- **C++ Core Guidelines lifetime profile** — the "80% without a borrow checker"
  attempt from the C++ side.

---

## 9. Milestones

Each milestone is defined by a program the compiler can build and run. No
milestone is complete until its acceptance program is a passing golden test.

| M | Deliverable | Acceptance | Teaches |
| --- | --- | --- | --- |
| **M0** | Lexer, parser, AST, pretty-printer, diagnostics, arena, interner. No types. | Parse the §6.1 sample and print an AST that round-trips. Malformed input produces a spanned error, not a crash. | Parsing, spans, error recovery |
| **M1** | Integers, bools, C-style function and variable declarations, `auto`, `if`, `while`, `for`, calls, arithmetic. Type check + emit C. | `fib(20)` compiles and returns the right exit code. | The whole pipeline works end to end |
| **M2** | Structs, value semantics, field access, struct literals, by-value passing and returning. | A `Point` program computing a distance. | Type layout, declaration ordering |
| **M3** | KIR + CFG. Constructors and `~Dtor()`. Scope-exit `goto` cleanup. | A `Buffer` with `~Buffer()` frees exactly once, at the right place, including on early `return`. Verify under valgrind/ASan. | **RAII — the core of the language** |
| **M4** | Move checking, `const T&` / `T&` params, the non-escaping rule, drop flags. Enforcement of D2. | Use-after-move is a compile error with a good message; a conditionally-moved value drops correctly. | Ownership, dataflow analysis |
| **M5** | Payload-carrying `enum class`, `switch` destructuring, exhaustiveness checking. | The `Shape`/`area` sample. Non-exhaustive `switch` is a compile error naming the missing variant. | Sum types, tagged unions |
| **M6** | `template<C T>` generics, monomorphisation worklist, name mangling with type args. | `max<i32>` and `max<f64>` both work; a generic `Box<T>` with a destructor drops correctly. | Instantiation, mangling |
| **M7** | Modules (`import`), multi-file compilation, then begin `Vector` and `String` **in Keel**. | A two-module program. Then a `Vector<i32>` that grows and frees. | **Whether the design actually works** |

**M3 is where this stops being a toy** — it is the first thing C cannot do for
us. **M4 is where we learn whether the ownership model is real.**

Write no standard library before M6. Write no `Shared<T>` before M7.

---

## 10. Testing

Two suites, with a clean split of responsibility.

### Unit tests live in the source file they test

Following the convention already in use in `../rtlblogv4-gnss-sdr`: tests sit at
the bottom of the `.cpp` file they exercise, inside `#ifdef ENABLE_UNIT_TESTS`,
with the Catch2 includes inside the same guard. `keel_tests` is the whole source
set recompiled with that define; `main()` is excluded from it by `#ifndef
ENABLE_UNIT_TESTS` so Catch2WithMain can supply its own.

The cost is compiling every source twice. The benefit is decisive for a
compiler: a test can reach file-local statics and anonymous-namespace helpers
directly, so the lexer's classification tables, the parser's recovery
predicates, and the dataflow lattice joins are all testable without widening a
single public header.

Use unit tests for anything with an invariant expressible in C++: the arena,
the interner, span arithmetic, token boundaries, lattice joins.

### Golden-file tests cover language behaviour

From the first commit. Without these, things break silently and constantly.

```
tests/
  lex/          .kl + expected token dump
  parse/        .kl + expected AST dump
  typecheck/    .kl + expected types or expected error
  errors/       .kl + expected diagnostic text (span, message)
  run/          .kl + expected stdout and exit code
  run_tests.sh
```

Every compiler works this way (rustc's UI tests, clang's `lit`/FileCheck). A
50-line shell runner is sufficient; do not adopt a framework. `tests/` holds
only `.kl` files and expectations — no C++ lives there.

**A bug fix without a regression test — in either suite — does not count as
fixed.**

From M3 onward, run the `run/` suite under ASan and valgrind. Destructor bugs
are silent otherwise.

---

## 11. Repository layout

```
keel/
  CMakeLists.txt
  CMakePresets.json       # generator, compilers, vcpkg toolchain, build types
  vcpkg.json
  README.md
  docs/
    MANIFESTO.md          # the vision doc
    PLAN.md               # this file
    GRAMMAR.md            # the v0 grammar, kept in sync with the parser
    DECISIONS.md          # append-only log of decisions and their reasons
  src/
    main.cpp              # driver: parse args, run pipeline, invoke cc
    common/               # arena, interner, span, diagnostics, small containers
    lex/
    parse/
    ast/
    sema/                 # resolver, type checker
    ir/                   # KIR, lowering, CFG construction
    check/                # move/drop dataflow
    codegen_c/
  runtime/
    kl_rt.h
    kl_rt.c
  tests/                  # golden corpus only: .kl files + expectations
    run_tests.sh
  examples/
```

Build: CMake + Ninja, C++20, clang-18. Configuration is driven by
`CMakePresets.json`, not by VS Code kits — the kit scanner on this machine
picks up Windows toolchains from `/mnt/c` (clang-cl, MinGW) that have no usable
generator under Linux, and selecting one yields "No usable generator found" and
a broken kit menu. Presets also keep the compiler and vcpkg toolchain paths in
the repo instead of in machine-local settings.

```
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```

Presets: `debug`, `asan` (ASan + UBSan, required from M3 onward per §10), and
`release` (RelWithDebInfo, `-Werror`). Each gets its own `build/<preset>/bin`,
so a sanitized build can never overwrite the Debug binaries. `-Werror` is off
in `debug` on purpose: a mid-refactor unused variable should not stop the
build.

Dependencies are held to three, all via vcpkg, and each has to justify itself:

| Package | Why |
| --- | --- |
| `fmt` | This toolchain is clang-18 over libstdc++ 12, which has no `<format>`. Diagnostics need formatted output; this is a real constraint, not a preference. Drop it if the toolchain ever gains `<format>`. |
| `cxxopts` | Compiler drivers accumulate flags quickly (`--dump-tokens`, `--dump-ast`, `--emit-c`, `-o`). |
| `catch2` | Unit tests, inline in the sources (§10). Test-only; `-DKEEL_TESTS=OFF` builds with two deps. |

Nothing else gets added without deleting something. In particular the compiler
writes its own arena, interner, and containers — those are the parts we are
here to understand, and they are rewritten in Keel at bootstrap.

`keelc` and `keel_tests` are built from the same glob of `src/**/*.cpp`, the
latter with `ENABLE_UNIT_TESTS` defined — see §10 for why this is a second
compilation rather than a shared library. Includes are path-qualified from
`src/` (`#include "lex/lexer.h"`), because a compiler naturally grows
same-named files in different directories (`ast/node.h`, `ir/node.h`).

---

## 12. Open questions

Deliberately unresolved. Each has a milestone by which it must be decided, so
none of them can block work indefinitely.

| Question | Decide by |
| --- | --- |
| **Mutable-by-default (C++) or const-by-default (safer)?** L12 currently follows C++, because `i32 y = 0; y = 1;` failing would astonish exactly the developer §5.1 is written for. But "safe by default" is a manifesto core principle, and this is the one place the two goals point in opposite directions. | M2, once real code exists to judge how often `const` gets forgotten |
| Do we keep `?` for error propagation, or find a spelling from the C family? It is the only construct in the language with no C++ heritage (D6). | M5 |
| **What is a cast?** D5 makes every conversion explicit and promises "the required cast in the message", but no cast syntax exists — not in the grammar, the lexer, or the parser. Until one does, D5's errors name a remedy the language cannot express. C-style `(u64)x` is ambiguous with parenthesised expressions and is itself on §5.1's list of C++ warts; `static_cast<u64>(x)` is unambiguous and familiar but verbose enough to discourage the widening that D5 makes routine. | M1 — the type checker's first error message needs it |
| Should `int`/`float`/`double` be accepted as aliases after all (D1), or stay hard errors? Aliases ease the first hour and cost a permanent second spelling for every type. | M1 |
| `a < b > ( c )` — a call to a generic, or two comparisons? C++ needs `template` disambiguators, Rust needs turbofish (`a::<b>(c)`). D15 does not help: both readings are effectful. | M6 |
| Do we ever add lifetimes/borrow checking, or is the non-escaping rule permanent? | After M7, with real-program evidence |
| Are interfaces/traits the only form of polymorphism, or is there virtual dispatch? | M6 (generic bounds force a partial answer) |
| What exactly is in an `unsafe` block, and what does it permit? | M3 (raw pointers appear in `Buffer`) |
| Optionals: `T?`, `Optional<T>`, or a nullable-reference type — and how does it interact with `&`? | M5 |
| Does `class` exist at all, or is `struct` the only aggregate? (v0 says struct only) | M7 |
| Custom allocators / arenas — visible in the type system or not? | M7 |
| Module granularity: file, directory, or explicit declaration? | M7 |
| Standard library naming. `MANIFESTO.md` §12 already refuses to mirror `std`, but the specific names are unsettled: one `Hash_map` rather than `map`/`unordered_map`, and a better name than `vector` for a dynamic array. Note the one real trap — `List` reads as a *linked* list to a C++ programmer (it is `List<T>` in C#/Java but `std::list` in C++), so a familiar name would carry the wrong semantics. Not a §6.3 divergence: those cover syntax and semantics the compiler enforces, and no library exists yet. | M7, when the first containers are written in Keel |
| Compile-time evaluation: how much, and is there reflection? | Post-M7 |
| ABI stability: is there one at all? | Post-M7 |

---

## 13. How this project fails

Named explicitly, because these are the actual risks and they are all
self-inflicted:

1. **Bikeshedding syntax instead of writing the compiler.** §5 is locked for a
   reason. Syntax is the cheapest thing to change later; semantics are not.
2. **Building the standard library too early.** It is the most fun part and the
   least informative. Nothing before M6.
3. **Attempting a borrow checker.** See §8. This is the most likely single cause
   of abandonment.
4. **Adding an LLVM backend "for real performance."** Performance is irrelevant
   until the language exists. C is not the bottleneck; the frontend is.
5. **Rewriting the parser because it is not elegant.** It will not be elegant.
   Parsers never are. It only has to be correct and produce good errors.
6. **Skipping tests during the exciting parts.** M3 and M4 are exactly where
   silent breakage compounds.
7. **Treating the manifesto as a scope commitment.** It is a direction, not a
   backlog.

---

## 14. Bootstrap

Long-term goal: `keelc` written in Keel, compiled by `keelc`.

Do not design for this now. Compiler #1 is disposable scaffolding — that is the
entire reason for the restrained C++ subset in §2.3. The bootstrap becomes
realistic somewhere after M7, once `Vector`, `String`, `Hash_map`, and a file
API exist in Keel, and it will be a full rewrite rather than a port.

---

## 15. Where the work is

### M0 — done

| | |
| --- | --- |
| `common/` | `Span`, `Source_manager`, `Diagnostics`, `Interner`, `Arena` |
| `lex/` | `Token`, the lexer, `token_kind_name`/`token_kind_spelling` |
| `ast/` | `Node`, `Ast`, the dumper |
| `parse/` | declarations, statements, and expressions with C++ precedence |
| `tests/` | in-source unit tests plus golden corpora for `lex/` and `parse/` |
| `keelc` | `--dump-tokens`, `--dump-ast`, spanned diagnostics, error recovery |

`examples/hello.kl` — the §6.1 sample — parses with zero errors, which was M0's
acceptance criterion. It is pinned as a golden fixture.

Rules that landed as code rather than prose: D3 (mandatory braces, enforced for
free by the body being a block), D12, D13, D15, D16, D17, D18 (whose
corollary — a prototype is a parse error, not a declaration — is enforced in
`parse_function_decl`), D19, and L17 — the
declaration/expression ambiguity is resolved by a speculative *scan* with no
symbol table, exactly as §5.1 intended.

### M1 — in progress

Type checking and C emission. The pipeline gains its first pass that asks what a
program *means* rather than how it is shaped, and `keelc` starts producing
output rather than dumps.

1. **Resolver** — *done*. Names to declarations, scope tree. §3 puts this after
   parsing and L17 keeps it there: nothing about parsing needs a symbol table.
   `Resolution` is a side table keyed by `Node_id` — the same shape the type
   checker's result should take. D18 and D19 landed here. Its golden corpus is
   `tests/sema/`, whose `FLAGS` is empty: with no dump flag the whole pipeline
   runs, so the fixtures are compared on stderr and exit code rather than stdout.
2. **Type checker** — next. Bidirectional (L5), so a literal's type comes from
   context and `u32 x = 42;` needs no suffix. This is where D1's suggestion table
   lives. Decide before writing any of it: how a type is represented — a
   `Type_id` handle into a type table, mirroring the AST's handles, versus a
   plain enum while everything is still builtin. The enum is cheaper now and
   painful at M2 when structs arrive.
3. **C emitter** — §7's rules, three-address form, straight from the AST.

**KIR is not part of M1.** §9 places it at M3, where RAII is what needs a CFG;
straight-line C for `fib(20)` does not. Building it earlier would mean designing
it against guesses rather than against the drop-placement problem that defines
it.

The remaining v0 syntax (`struct`, `enum`, `match`, generics, `?`) is M2 onward;
none of it is needed for `fib(20)`.

### Debts to pay along the way

- `expect_keyword()` will need a spelling that `token_kind_spelling()` cannot
  give. Every keyword shares one `Token_kind::Keyword`, so the spelling function
  can only answer "keyword" — *which* one lives in `Token::symbol`. A message
  like `expected \`return\`` therefore has to go through the keyword spelling
  table in `interner.cpp`, not through `lex/token.cpp`. `error_expected` avoids
  this today only because it quotes the source text for the *found* half.
- D1's suggestion table (`int` → `i32`, `double` → `f64`, ...) belongs on sema's
  unknown-type path. Nothing produces that message today, and it is what makes
  D1's promise real rather than aspirational.
- `Interner` should hold its strings in an `Arena` rather than in the map's keys.
  That deletes `Sv_hash` and `std::equal_to<>` — the lookup type becomes the key
  type again — and drops one heap allocation per symbol. Deferred: it is not a
  bottleneck and the API does not change.
- The `Arena` still has no caller. It earns its place at M6 (monomorphised
  instances) or in KIR payloads, whichever arrives first.
- `parse_block` reports twice when the opening brace is missing: it proceeds into
  its loop and then consumes the enclosing `}`. Bailing out immediately would
  give one error instead of two. Carrying on is deliberate — the statements after
  a dropped brace are still worth parsing — so the fix is to distinguish a brace
  that is *missing* from one that was never coming, as `parse_function_decl` now
  does for `;`.
- Chained assignment (`a = b = c;`) is not expressible, since assignment is a
  statement. It currently fails with *expected `;`, found `=`*, which is correct
  but unhelpful; it deserves either a targeted message or a D-entry.
- Literal values do not survive lexing. `Token` carries only kind, span and
  symbol, and `Int_literal` nodes store `aux = 0`, so the digits the lexer
  already scanned and validated are recoverable only by re-reading the source
  text. The type checker needs the value to reject `u8 x = 300;` and the emitter
  needs it to print. `aux` is a `u32` and so cannot hold an `i64`/`u64`: this
  wants a literal pool with `aux` as the index. Note that sign lives in a
  separate `Unary_expr`, so `-2147483648` is a negation of a value that does not
  itself fit in `i32` — the range check has to happen after the negation.
- Prefix `++` is unreachable — `can_start_expression` rejects it, so `++i;` says
  *expected a statement*. D12 left the postfix/prefix choice open; decide it.
