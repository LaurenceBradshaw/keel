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

### 3.1 KIR's shape

**Not SSA.** Rust's MIR is not, and for what §8 specifies that is correct rather
than a compromise: the analysis tracks whether a *named local* is initialised or
moved at each point, which is a per-local lattice over the CFG. SSA renames every
assignment apart and would obscure exactly the thing being tracked, at the price
of phi nodes and dominance frontiers. If an LLVM backend ever wants SSA, LLVM's
own `mem2reg` produces it from allocas — the standard path, and free to us.

Two simplifications Keel gets that Rust does not, both taken deliberately:

- **No unwinding** (L14). In MIR every call is a *terminator* carrying an unwind
  edge. With no exceptions a call is an ordinary statement, blocks stay long and
  straight, and cleanup-on-unwind does not exist.
- **No lifetimes** (§8). The structural reference rule means no region
  inference, so a place needs no provenance and the dataflow is a bitset per
  local.

The vocabulary is five types. The value/place split is the one the C emitter
already discovered the hard way, as `lower` versus `lower_place`:

```
Place      a local, plus a projection path        x, p.y, (*q).z
Operand    Copy(Place) | Move(Place) | Constant   Move is where M4's checking hangs
Rvalue     Use | Binary | Unary | Cast | Call | Address_of | Struct_init
Statement  Assign(Place, Rvalue) | Drop(Place) | Storage_live/dead(Local)
Terminator Goto | Branch(Operand, Block, Block) | Return | Unreachable
Function   locals[] + blocks[];  Block = statements[] + exactly one terminator
```

Every statement and terminator carries a `Span` (§4), and every local a
`Type_id`. Each later milestone adds **one variant**, not a mechanism: `Drop`
and `Set_drop_flag` at M3, meaning for `Move` at M4, `Terminator::Switch` at M5,
`Rvalue::Checked_binary` for §12's runtime overflow checks, and a second
consumer of `Function` for an LLVM backend.

### 3.2 Keeping KIR from becoming another 2,000-line file

`sema/type_checker.cpp` reached 2,350 lines of code because **`Checker` is a
file-local class, and a class cannot span translation units**. Every
responsibility it gained — annotation resolution, the operator tables, the
constant folder, some twenty-five `visit_*`/`infer_*` methods — had to become a
private method in that one file. The in-source test convention then doubles it.
The size is a consequence of the organising unit, not of sprawl.

So KIR is organised the other way round, and this is the rule that matters:

> **A pass is a free function over the IR, not a method on a class.**
> Adding a pass adds a file, and cannot grow an existing one.

With `kir.h` holding data types and no logic, and payloads in side tables rather
than inline, the layout follows §11's reserved directories:

```
src/ir/
  kir.h          the vocabulary; no logic
  kir.cpp        accessors, arena
  builder.h/cpp  allocate locals and blocks, emit, wire terminators
  lower.h/cpp    typed AST -> KIR            the only large one
  print.h/cpp    --dump-kir, and the golden format
  verify.h/cpp   structural invariants
src/check/
  dataflow.h     the generic fixpoint; an analysis is a lattice + transfer function
  initialised.cpp
  drop.cpp
```

`verify` is worth having from the first commit rather than later: a malformed
CFG otherwise surfaces as a wrong program instead of a failed assertion, and it
is what makes the `--dump-kir` goldens worth trusting.

### 3.3 The order to build it in

M3 is where §13 warns that silent breakage compounds, so the path is chosen so
that only the last step changes what any program means.

1. `kir.h`, the builder, the printer, `--dump-kir`. Nothing consumes it; golden
   fixtures pin the dump.
2. Lower straight-line code — the `fib`/`gcd` subset. Still nothing consumes it.
3. A KIR -> C emitter behind a flag. The existing codegen goldens are then an
   equivalence check across both paths: the runner already compiles and executes
   each fixture, so "same exit code either way" is mechanical.
4. Flip the default; delete the AST emitter.
5. **Then** M3 proper: destructors, drop placement, cleanup edges.

**Steps 1 and 2 are done.** `kir.h`, the builder, the printer, `verify`,
`--dump-kir`, and a lowerer covering the whole v0 language: literals, locals and
names, operators, assignment and increment, `if`, `while`, `for`, `break`,
`continue`, `&&`/`||`, calls, casts, places (fields, derefs, and paths through
both), `&`, and struct literals. All 43 lowerable fixtures in the corpus lower
and pass `verify`; the other 29 fail the front end deliberately. `--dump-kir`
runs the verifier on every function it prints, so the whole corpus sits behind it.

Three things the lowering made concrete, each of which the C emitter had to work
harder for:

- **`break` and `continue` are edges.** No generated labels, no
  emit-the-label-only-if-used, no `-Wunused-label` to design around. That whole
  mechanism in `emitter.cpp` has no counterpart here.
- **Conversions are explicit.** §6.4's widening is a `Cast` rvalue rather than
  something a backend re-derives, which is also what an LLVM backend requires of
  an `add`. The case that forces it is a comparison, whose recorded type is
  `bool` while its operands still meet at their common type - a fact written
  nowhere in the AST.
- **A place is a local plus a path.** `( *p ).next` is one place with two
  projections rather than a chain of copies, which is what lets M4 ask whether
  that field specifically was moved out of.

Two debts the lowering opened, both small and both recorded here rather than in
the code: `Rvalue_kind::Cast` does not distinguish `cast` from `wrap`, which is
harmless while narrowing `cast` is rejected outright and becomes real when M3
can trap; and a file-scope variable has no representation, because `Place` is
always rooted in a `Local_id`.

Steps 1-4 are a refactor with a mechanical correctness check. What survives from
the current emitter is its C-spelling layer — `c_type`, the mangling, literal
spelling, struct emission and ordering; what it loses is its dispatch structure,
which KIR replaces.

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
either new notation or a hard error; none silently redefines valid C++ — with one
exception, marked `†` in §6.4, where sub-32-bit arithmetic keeps a narrower result
type than C++'s integral promotion gives. Values agree there until the result
overflows; what happens then is the open overflow question in §12.

| # | Divergence | Why it is safe under the §5.1 rule |
| --- | --- | --- |
| D1 | Fixed-width primitives only: `i32`, `u64`, `f64`. No `int`, `long`, `char`, `unsigned`. | The C++ spellings are **rejected outright**, with a diagnostic naming the replacement. They are *not* reserved words — the lexer treats them as ordinary identifiers, and the suggestion is produced by sema's unknown-type path, which knows it is in type position and so gives a better message than the lexer could. No type name is a keyword: `i32`, `Point` and `Vector<T>` all resolve through one path. |
| D2 | **A bare by-value pass of an owning type is an error**; transferring ownership is written `consume( move b )`. A type is owning exactly when it has a destructor, directly or through a member: `struct Wrapper { Buffer b; }` is owning, because copying one would copy a `Buffer` and there is nothing to copy it with. That transitivity is forced rather than chosen, and it is Rust's `Copy` rule. A raw pointer owns nothing by itself — an address says nothing about who frees it. The query is a memoised walk over the containment graph M2 already builds and orders. Note the rule's real content is **"is it copyable"**; "has a destructor" is a proxy that coincides only because §6.6 puts copy constructors outside v0. `Weak<T>` will have a destructor and should still be copyable, so this needs restating when copy constructors arrive. Everything else copies exactly as in C++. | The earlier rule made `consume( Buffer b )` a move, which is C++'s syntax for a copy with different semantics — the one divergence §5.1 forbids — and excused it by claiming new notation was impossible. It is not. Copying by default is unavailable too: §6.6 puts copy constructors outside v0, so an owning type cannot be copied at all. That leaves moving silently or rejecting, and §5.1 says reject. The cost is a marker on every move; Rust pays none, but Rust has no C++ copy expectation to fight. **Inert until M3**: no type has a destructor yet, so nothing is owning and nothing changes. |
| D3 | Braces mandatory on every `if`/`while`/`for` body. | Braceless C++ is a parse error, not a reinterpretation. Kills the `goto fail;` class of bug. |
| D4 | `switch` has no fallthrough, needs no `break`, requires exhaustiveness, and matches sum-type payloads. | Payload patterns (`case Circle( r ):`) are new notation. A `switch` over a plain integer keeps C++ meaning minus fallthrough; missing cases are an error, never a silent skip. |
| D5 | No *lossy* implicit conversions. A binary operator widens both operands to the smallest type that losslessly holds both, and is a hard error whenever C++'s own result type would not hold them. Assignment is implicit only where the target holds the source type. int↔bool and pointer↔bool are never implicit. §6.4 has the table. | The second clause makes the §5.1 audit executable: where C++ is lossless Keel agrees with it, and where C++ silently loses information Keel refuses. Lossless widening is not a conversion anyone can get wrong, and requiring a cast for it trains authors to write casts reflexively — which is how the dangerous ones get waved through. **Provisional**: adopted for M1 to unblock the type checker, and expected to be re-judged once real code exists. |
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
| D19 | A declaration may not shadow another that is still reachable by the same unqualified name. A block local colliding with an enclosing local or with a parameter is an error. Scopes that do **not** carry outer names in — a function body relative to file scope, and later a lambda relative to its enclosing function — are *barriers*, and reusing a name across one is fine: a local may be called `count` alongside a top-level `count()`. | The wart is the *silent* failure: two locals of the same type mean picking the wrong one compiles, runs and returns a bad value. Shadowing a function with a variable is caught immediately by the type checker, so it needs no rule. Java (JLS §6.4) and C# (CS0136) reject exactly this and keep members shadowable for the same reason; C++ allows it and papers over it with `-Wshadow`. File scope is exempt so that adding a top-level function cannot break a function body far above it — action at a distance. Rust's shadowing is same-scope rebinding (`let x = validate(x);`), which we already reject as a duplicate declaration and which this does not revisit. |
| D20 | A character literal is a `u8` holding one byte: `'A'` is 65, `'\xFF'` is 255. A literal spanning more than one byte is an error, so a non-ASCII character needs its bytes written out. String literals lex and parse but have no type at all, and are rejected with a message saying so. | D1 leaves no `char` type to give them, and inventing one for literals alone would be a second spelling for `u8`. Treating a code point as the integer it is means §6.4's range rules apply unchanged: `u8 c = 'a';` and `u32 c = 'a';` both work, `bool c = 'a';` does not, and no new machinery is needed. C++ makes `'a'` an `int` and `'ab'` implementation-defined; both are rejected here rather than reinterpreted. Strings wait because their representation is an M7 question (§9) — choosing `u8*`, a slice or an array now would commit the language before the ownership model exists to judge it. |
| D21 | `main` returns `i32` and takes no parameters. Any other signature is a hard error. | C's `main` returns `int`, so a wider or narrower return type would either truncate in the emitted shim or fail to convert at all — and the shim calls `main` with no arguments, so a parameter would generate C that does not compile: a `cc` error pointing at generated code instead of a diagnostic pointing at the program. Command-line arguments wait for arrays and modules (M7), at which point this entry is what has to change. A program with no `main` is a library and stays legal. |
| D22 | `.` is the only member-access operator, and reaches through a pointer. `->` is a hard error naming `.` as the replacement. | C++ needs both because `.` on a pointer means nothing there; a language designed now does not. The decisive reason is generics (M6): `template<C T> ... thing.x` works whether `T` is a value, a borrow or a pointer, where `->` would force the template's author to know which. `->` distinguishes less than it appears to even in C++ — `.` on a reference is already spelled like `.` on a value — and L6 makes every signature fully annotated, so the declaration is in view. §5.1 constrains neither direction here, unusually: `.` on a pointer is a C++ error, so accepting it only adds meaning. The `Arrow` token is **kept** so the parser can reject it by name; deleting it would lex `p->x` as `-` then `>` and produce a message about arithmetic. |
| D23 | A struct literal must initialise **every** field. `Point { 1.0 }` for a two-field struct is an error, not a zero-fill. | C++ value-initialises the fields you leave out, so adding a field to a struct silently changes what every existing construction site builds. Requiring all of them turns that into a compile error at each site — noisy in the way that finds bugs. This rejects a program C++ accepts, which §5.1 permits outright. Mixing positional and named initialisers is also an error, but that needs no entry of its own: C++20 forbids it too, so Keel is simply following. |
| D24 | **Mutable by default**, as in C++. `const` is opt-in. | The manifesto's "safe by default" pulls the other way, and this is the one place the two goals genuinely conflict — but mutability is not what makes a program unsafe; aliasing and lifetime are, and those are M3-M4's business. Most values are written more often than they are read once, so const-by-default taxes the common case to annotate the rare one. And `i32 y = 0; y = 1;` failing would astonish exactly the developer §5.1 is written for. |
| D25 | `int`, `float` and `double` stay **hard errors**, never aliases. | D1's suggestion path is implemented and works: the message names the replacement, so the cost is one compile the first time. Accepting them would buy that same first hour at the price of a permanent second spelling for every type — every reader thereafter has to know both, and every code base picks one by accident. |
| D26 | `nullptr` is the null pointer, spelled as in C++, and it is a **literal** whose type comes from context: `u8* p = nullptr;` adopts, `auto p = nullptr;` is an error. | The spelling is C++'s because §5.1 has no reason to invent another for an identical concept. Making it a literal rather than a value of some `nullptr_t` reuses `check_literal` wholesale and keeps D5 intact — no conversion happens, the literal simply *becomes* that type, exactly as `42` becomes a `u32`. The `auto` case then falls out as an error for the same reason it does for any literal with nothing to adopt from. |
| D27 | **No pointer arithmetic on `*T`**, which points at exactly one `T`. Arithmetic belongs to a many-item pointer, `[*]T` in Zig's notation, which v0 does not have. | `p + 1` on a single-item pointer is not dangerous, it is *nonsense*, and a type distinction catches it statically at no cost. Note the performance argument for C's arithmetic does not hold: `*(p + i)` and `p[i]` compile to identical machine code, so what buys speed is the capability of touching raw memory, which survives either spelling. C's implicit scaling by element size is the wart — a named `offset` operation says what it does. §6.4 already answers nothing for a pointer, so rejection is the default rather than a rule to add. `==` and `!=` between two pointers of the **same** type are allowed, since that is how a null check is written; ordering is not, because comparing pointers into different allocations is meaningless. |
| D28 | Conversions are written `cast<T>( x )` and `wrap<T>( x )`, both **keywords**. `cast` preserves the value; `wrap` keeps the low bits. Neither converts float to integer, and neither converts integer to bool. §6.5 has the table. **Narrowing `cast` is refused until the run-time check exists.** | Two spellings rather than one because the failure policy is the interesting part, and an unqualified cast lets an author avoid stating it — which is how C's `(u8)x` silently truncates. Keywords because as identifiers they walk into §12's `a < b > ( c )` ambiguity; as keywords the `<` can only be a bracket. Both spellings are new notation, so §5.1 is satisfied for free. Float to integer is rejected because `cast<i32>( 1.9 )` has no obvious answer — truncate, round, floor and ceil are four operations and C picks one silently; they arrive as library functions at M6. Integer to bool is rejected because `x != 0` says it better and modular arithmetic down to one bit says something else again. Float *rounding* is accepted (`cast<f32>( some_f64 )`), because a float that cannot hold the value gives an infinity rather than a plausible wrong number — the line is that `cast` refuses to turn a value into a *different* value, not that it refuses to lose precision. |

### 6.4 Numeric conversions (D5)

Result type of a binary operator. `--` is a compile error; `†` marks a cell where
C++ promotes to `i32` and Keel keeps the narrower type.

```
           i8  i16  i32  i64   u8  u16  u32  u64  f32  f64
  i8      i8† i16†  i32  i64 i16†  i32   --   --  f32  f64
  i16    i16† i16†  i32  i64 i16†  i32   --   --  f32  f64
  i32     i32  i32  i32  i64  i32  i32   --   --   --  f64
  i64     i64  i64  i64  i64  i64  i64  i64   --   --   --
  u8     i16† i16†  i32  i64  u8† u16†  u32  u64  f32  f64
  u16     i32  i32  i32  i64 u16† u16†  u32  u64  f32  f64
  u32      --   --   --  i64  u32  u32  u32  u64   --  f64
  u64      --   --   --   --  u64  u64  u64  u64   --   --
  f32     f32  f32   --   --  f32  f32   --   --  f32  f64
  f64     f64  f64  f64   --  f64  f64  f64   --  f64  f64
```

Assignment (`T x = expr`) is implicit exactly where `T` holds the source type:

```
  i8  -> i16 i32 i64 f32 f64        u8  -> i16 i32 i64 u16 u32 u64 f32 f64
  i16 -> i32 i64 f32 f64            u16 -> i32 i64 u32 u64 f32 f64
  i32 -> i64 f64                    u32 -> i64 u64 f64
  i64 -> (nothing)                  u64 -> (nothing)
  f32 -> f64                        f64 -> (nothing)
```

Signed→unsigned and float→int are never implicit. `bool` takes no part: it
appears only in `&&`, `||`, `==`, `!=` and as a condition.

Three consequences worth stating, since they are what the table is *for*:

- **`T op T` is `T`.** Forced, not chosen: if `u32 + u32` widened to `u64`, then
  `u32 n = 0; n = n + 1;` would need a cast. Any rule that widens same-type
  arithmetic makes D5 break assignment for every type but the widest.
- **Mixed signedness is safe exactly when the signed type is strictly wider.**
  `i32 + u8` cannot misinterpret anything and C++ agrees; `i32 + u32` is where
  C++ turns `-1` into 4294967295. Of the 32 mixed-sign pairs, 18 are accepted and
  14 rejected — and over 400k random comparisons the accepted cells never
  disagree with exact arithmetic, while the rejected ones disagree 46% of the
  time in C++.
- **`i64 + f64` and `f32 + i32` are errors.** C++ accepts both and silently loses
  precision past the mantissa. This is the table catching a bug C++ does not warn
  about.

`i8 + u32` is stricter than safety alone requires — `i64` holds both operands
exactly, but C++ answers `u32`, so accepting it would silently change a valid C++
program. That cell is the containment rule's cost, not the type system's.

### 6.5 Explicit conversions (D28)

Rows are the source, columns the target. `cast` preserves the value, `wrap`
keeps the low bits.

| from \ to | int | float | bool | pointer | struct |
|---|---|---|---|---|---|
| **int**     | `cast`* / `wrap` | `cast` | — | — | — |
| **float**   | — | `cast` | — | — | — |
| **bool**    | `cast` | — | — | — | — |
| **pointer** | — | — | — | *unsafe* | — |
| **struct**  | — | — | — | — | — |

`—` is a hard error. `*` marks the one cell held back: a **narrowing** integer
`cast` (one where the target cannot hold the source type, which includes a
same-width change of signedness) is refused until the KIR can emit the run-time
check that makes it safe. `wrap` covers that cell today. Widening `cast` is
allowed now and does not change meaning when the block goes, so no program that
compiles today is affected by lifting it.

*unsafe* marks a real conversion held for the `unsafe` gate rather than rejected
as nonsense, so it gets its own diagnostic.

Two asymmetries are deliberate. `wrap` accepts a widening integer conversion —
it simply never wraps — so it is total over integer-to-integer rather than
carrying a rule about which direction is allowed. And `cast` gives a **literal**
its type from context rather than converting it: `cast<u8>( 300 )` is the
ordinary out-of-range error and `cast<u8>( 200 )` is simply a `u8` literal, both
falling out of `check_literal`. `wrap` must not do this — accepting a value the
target cannot hold is the whole point of it — so `wrap<u8>( 300 )` infers `i32`
and then wraps to 44.

### 6.6 Not in v0

Optionals, traits beyond generic bounds, closures, `namespace`, operator
overloading, copy constructors, inheritance, virtual dispatch, `Shared<T>`,
`Weak<T>`, concurrency, reflection, coroutines, and any standard library.
Modules arrive at M7. **String literals** lex and parse but have no type
(D20); they wait for `String`, which is M7 as well.

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

A suite holding a `RUN` file does not stop at the diff: its stdout is treated as
C, compiled with `$CC`, and executed, and the program's exit code is compared
against `<name>.kl.run` (0 when the file is absent). `tests/codegen/` has one.

This matters because a golden diff can only say the emitted C is *unchanged*,
never that it is *correct*, and the codegen fixtures are written to check their
own answers — each result is compared in Keel and a wrong one returns a distinct
code — which is worth nothing if nothing runs them. Both halves have already
earned their keep: the `( *p ).field` miscompile produced C that `diff` was
perfectly happy with and `cc` rejected outright, and a fixture deliberately
given a wrong expected value, with its golden regenerated to match, passes the
diff and is caught only by its exit code.

The C is built with `-Werror`, because a warning in generated code is the
compiler saying it means something other than intended. The `unused-*` family is
excluded: a local the Keel program never reads becomes a local the C program
never reads, which is faithful emission rather than a fault. `CC`, `KEEL_CFLAGS`
and `KEEL_ARTIFACTS` override the compiler, its flags, and where the `.c` and
binaries are kept — outside `tests/`, so the stray-file check stays meaningful,
and left behind on failure so a bad one can be read.

The older fixtures (`fib`, `gcd`, `calls`, `arithmetic`, `control_flow`) predate
the self-checking convention and return a computed value instead, which is why
each has a `.kl.run` recording it.

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
| Do we keep `?` for error propagation, or find a spelling from the C family? It is the only construct in the language with no C++ heritage (D6). | M5 |
| **What happens on integer overflow?** D5 is often mistaken for an answer here, and is not: it governs conversions *between* types, not arithmetic *within* one. `u32 a = 0; a - 1;` involves no conversion, so D5 is silent and the result is 4294967295. §7.7's `-fwrapv` currently makes signed overflow wrap silently too — defined, which is better than C++'s UB, but still a wrong answer delivered quietly. Underflow is not a separate question: `u32 a = 0; a - 1;` is overflow off the bottom, and one decision covers both. Float underflow to denormals is IEEE's business and is not a trap candidate. The options are wrap (status quo — fast, silent, a wrong answer delivered quietly); trap always (a predicted branch per operation, and some lost vectorisation); trap in debug only, as Rust does (free in production, but tests and production then compute different answers); saturate (surprising, and wrong for a systems language); or trap by default with `+% -% *%` to opt out. **Decided: wrap.** A branch per operation genuinely inhibits auto-vectorisation, and Keel's performance claim makes that a real cost rather than a theoretical one — and wrapping is what every programmer was taught happens. Silence is answered two ways. **Compile-time rejection of constant overflow** — `u32 d = 1 - 2;` is an error rather than 4294967295 — is free, backend-independent, and can land now. **Opt-in runtime checking** is a frontend feature, not a `cc` flag: passing `-fsanitize=signed-integer-overflow` through to `$CC` would work today and evaporate with the backend, which is precisely the coupling §2.2 forbids. The frontend decides where a check belongs and each backend spells it — `__builtin_add_overflow` in C, `llvm.sadd.with.overflow` in LLVM. That makes it **KIR work at M3**: a checked add is an instruction, and building it into the AST-walking emitter first means building it twice. A sanitiser flag pass-through is a fine convenience until then, but it is not the design. With no trapping default there is nothing to opt out of, so `+%` is not needed. Keep `-fwrapv`, so the wrap is defined rather than UB. A manifesto that claims safety by default cannot leave this at "whatever `-fwrapv` does". D5 now depends on the answer: `T op T` yields `T` (§6.4), so `u8 + u8` can overflow where C++'s promotion to `int` could not — that is the one `†` divergence in the §6.3 audit, and whether it traps or wraps decides whether the divergence is loud or silent. | **Decided: wrap.** Constant-overflow rejection is **done**; runtime checking is KIR work at M3. |
| **What, if anything, does `->` come to mean?** Free, with nothing assigned. D22 freed the token and the three call-site markers it was a candidate for are keywords instead: `move x` (transfers, D2), `out x` (the callee assigns it, and it need not be initialised first) and `ref x` (initialised, and may be modified) — C#'s distinction, which earns both. `->` stays a hard error naming `.`, and the token stays lexed so that error can be given by name. Rejected along the way: `socket -> connection` as a move expression (competes with `=`); a state-machine DSL (a domain feature in a language about ownership, and M5's exhaustive `switch` already makes illegal transitions a compile error); and scope injection, `user -> { greet( name ) }` — Pascal's and JavaScript's `with`, which JS deprecated in strict mode because you cannot tell a field from a local and adding a field silently changes the meaning of code that already compiled. That is D19's action-at-a-distance with a larger blast radius. | No deadline — it costs nothing to leave free |
| ~~**What is a cast?**~~ **Answered — D28.** `cast<T>( x )` preserves the value, `wrap<T>( x )` keeps the low bits, both keywords, table in §6.5. What remains open is narrower: `cast` is *defined* to check the value at run time and nothing can do that yet, so narrowing `cast` is refused rather than silently truncating. See the debt in §15. | M1 — done |
| `a < b > ( c )` — a call to a generic, or two comparisons? C++ needs `template` disambiguators, Rust needs turbofish (`a::<b>(c)`). D15 does not help: both readings are effectful. | M6 |
| Do we ever add lifetimes/borrow checking, or is the non-escaping rule permanent? | After M7, with real-program evidence |
| Are interfaces/traits the only form of polymorphism, or is there virtual dispatch? | M6 (generic bounds force a partial answer) |
| **What exactly is in an `unsafe` block, and what does it permit?** The shape is settled: Rust's model — `unsafe { }` blocks and `unsafe fn` — with Zig's `[*]T` beside it. What remains is the enumerated list. The property that makes Rust's version work, and the one most often misunderstood: **`unsafe` permits operations, it does not disable checks.** Move checking, type checking and D5 all still apply inside one; an unsafe block is not a different language. The two mechanisms are orthogonal rather than overlapping — `p + 1000000` on a `[*]T` is type-correct and catastrophic, so the type says the operation is *meaningful* while `unsafe` says the author *checked the invariant*. D's `@trusted` is deliberately **not** taken: a safe function containing an unsafe block already *is* one, so Rust's two levels encode D's three, and a standalone `@trusted` without D's full `@safe`/`@system` lattice would be an optional marker whose absence means either "safe" or "forgot" — the same defect that keeps `move` out of signatures under D2. | M3, when `Buffer` gives it something concrete to gate |
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

### M1 — done

Type checking and C emission. The pipeline gains its first pass that asks what a
program *means* rather than how it is shaped, and `keelc` starts producing
output rather than dumps.

1. **Resolver** — *done*. Names to declarations, scope tree. §3 puts this after
   parsing and L17 keeps it there: nothing about parsing needs a symbol table.
   `Resolution` is a side table keyed by `Node_id` — the same shape the type
   checker's result should take. D18 and D19 landed here. Its golden corpus is
   `tests/sema/`, whose `FLAGS` is empty: with no dump flag the whole pipeline
   runs, so the fixtures are compared on stderr and exit code rather than stdout.
2. **Type checker** — in progress. `sema/type.{h,cpp}` is done: `Type_table`
   interns the builtins behind a `Type_id` handle, and D5's §6.4 rule is four
   pure functions on it — `holds`, `common`, `cpp_result`, `arithmetic_result` —
   with the whole table pinned cell by cell in tests. `sema/type_checker.{h,cpp}` is
   most of the way there: signatures, names, calls, both operator families,
   assignment, returns and conditions all check, with one function per construct
   dispatched from `visit`/`infer` and the operator rules held as a table beside
   them — the shape `parse_*()` and `binding_power()` already use. D1's
   suggestion table landed on the unknown-type path, so its promise is real
   rather than aspirational. Literals are bidirectional (L5), so
   `u32 x = 42;` needs no suffix and `u8 x = 300;` does not compile: the lexer
   records values into a `Literals` pool that the token's unused `symbol` slot
   indexes, and `check` measures them against the target type. The negation case
   is handled explicitly — `-2147483648` is a negation of a value that does not
   itself fit an `i32`, so the expectation is pushed through the minus. Range-checking a literal against its
   target (`u8 x = 300;`) needs the literal's value, which does not currently
   survive lexing — see the debts below; it is the one part of the checker that
   cannot be written yet.
3. **C emitter** — done. `codegen_c/emitter.{h,cpp}` and `mangle.{h,cpp}`,
   three-address form straight from the AST. `keelc` writes the `.c` beside the
   executable, keeps it, and invokes `$CC` (or `cc`) with §7.7's flags.
   `--emit-c` prints it instead, which is what `tests/codegen/` goldens.

   Two emission choices are worth knowing before M3 changes them. **Loops emit
   as `while ( true )` with a guarded break**, because lowering a condition emits
   statements and those must run every iteration — C's loop headers cannot hold
   them. And **`&&` / `||` lower their right operand inside the guard**, which is
   the only expression that emits control flow. Both are exactly the shapes KIR's
   basic blocks replace.

**M1 is complete**: `fib(20)` compiles and returns 6765.

### M2 — done

Structs, value semantics, field access, struct literals, by-value passing and
returning. A `Point` program computing a distance compiles and runs.

Each stage of the pipeline gained the same three things: the parser gained
`Struct_decl`, `Field_decl`, `Field_expr`, `Struct_literal` and `Field_init`; the
resolver declares struct names in its first pass and resolves type annotations —
paying the debt §15 has carried since M1 — while leaving fields in a member
namespace of their own, since declaring them into `scopes_` would let a field
named `Point` shadow the type; the type table interns struct types **by
declaration**, so two modules' `Point`s are two types; and the emitter orders
definitions topologically, because C needs a complete type for a by-value member
and D18 lets a struct be declared after the one that contains it.

The ordering is not computed twice: the checker's cycle walk **is** a topological
sort, and its post-order is what the emitter consumes. Being in that order is
also what "already proved acyclic" means, so there is one structure rather than
two that must agree.

Two bugs were worth the trouble they caused. Field assignment lowered its target
as a *value* — `p.x = 3.0` read the field into a temporary and assigned to that,
which compiles, runs, and does nothing. `lower()` produces a value; an assignment
target needs a place, and for a `Name_expr` those are the same string, which is
why it survived all of M1. And a coverage audit by mutation found three tests
that asserted `errors() >= 1`, each of which passed against deliberately broken
code because *some* other error still fired.

Writing the emitter, and then writing real programs to emit, paid for itself
immediately: between them they exposed four type-checker bugs that 3,300 unit
assertions had not. All four were the same mistake — `infer` used where `check`
belonged, so a literal settled on its default type before anything told it what
was wanted. The worst, `u32 bits; bits != 0`, made unsigned code very nearly
unwritable, since D13 leaves no suffix to write around it. The lesson is that
bidirectional checking fails silently in exactly the places nothing looks — compound assignment inferred its right-hand
side instead of checking it, so `u8 x; x += 3;` settled the literal on `i32` and
was then refused. Nothing that only inspects the front end can see that.

**KIR is not part of M1.** §9 places it at M3, where RAII is what needs a CFG;
straight-line C for `fib(20)` does not. Building it earlier would mean designing
it against guesses rather than against the drop-placement problem that defines
it.

The remaining v0 syntax (`struct`, `enum`, `match`, generics, `?`) is M2 onward;
none of it is needed for `fib(20)`.

### Ahead of M3

With the folder in place the file-scope initialiser rule widened from "a
literal" to "a constant expression": `i32 seconds_per_hour = 60 * 60;` and
`u32 all_ones = wrap<u32>( 0 - 1 );` now work. The emitter needed no folding for
it — C accepts an arithmetic constant expression at file scope too, so
`constant_text` *prints* the tree with the same operand casts `lower_binary`
uses, and nothing has to run before main. The two agree on the answer because
constant rejection has already proved the result fits, so C's wider
intermediates cannot reach a different one; and C truncates division toward zero
and takes the remainder's sign from the dividend, which is what the folder's
sign-magnitude arithmetic does on its own.

`&&` and `||` stay out. Their ordinary lowering emits control flow, and keeping
them out means the file-scope path never has to answer whether printing both
sides is sound. They buy nothing in an initialiser.

Constant overflow is rejected at compile time. §12 decided arithmetic *wraps*
at run time, which leaves the case where the answer is known before the program
runs — and wrapping it there is a wrong answer delivered in silence. A small
folder over literal arithmetic answers what an expression's value is, and the
value is measured against the type **the operation happens in**, at every node
rather than only at the end: `i32 d = 2000000000 + 2000000000 - 2000000000;`
fits an i32 comfortably and the addition on the way does not.

Three things it catches beyond overflow proper, all undefined behaviour in C
rather than merely wrong: division and remainder by a constant zero, and a shift
count that is not less than the width. Float constants are folded too, where an
infinity from finite operands is the same failure — `fits_float` answers true
unconditionally for f64, so the finiteness test is what catches it.

Only `+ - * / % << >>` fold. `&`, `|`, `^` and `~` cannot take a value outside
the type their operands came from, so there is nothing for them to overflow and
"not constant" is the correct answer for them rather than a shortcut. The folder
carries `overflowed` separately from `constant`, because
`18446744073709551615 * 2` is entirely constant and has left what u64 can carry
— collapsing the two would let the largest constants through in silence, which
is backwards. `const` variables are deliberately not constants: that needs
`const` to be enforced, which is M4.

Deliberate wrapping keeps its spelling, and needed no special case:
`wrap<u32>( 0 - 1 )` is legal because `wrap` **infers** its operand instead of
pushing the target type in, so the fold happens in i32 where -1 fits.
`cast<u32>( 0 - 1 )` pushes the type in and is correctly refused. The two
operators already disagreed in exactly the right way — that falls out of D28.

This is **not** constant folding for codegen. The emitter still emits `1 - 2`;
the value is computed only to decide whether to complain. The optimisation is
KIR's, later.

Doing it turned up a fourth instance of the inferring-where-checking-belonged
family: a shift's result type is its *left* operand's (§6.4), and no expectation
flowed there, so `u32 d = 1 << 4;` settled the literal on i32 and was then
refused for being one. Fixed with the shift work, since the shift rule would
otherwise have been measuring against the wrong type.

File-scope variables work end to end. Their initialisers must be **literals** —
not because nothing else could be computed, but because there is no constant
folder, and that is the honest statement of what the compiler can evaluate. The
restriction removes two problems rather than deferring them: a global cannot
name another global, so initialisation order never exists as a question, and no
global can hold a type with a destructor, so M3 never has to sequence global
teardown. It widens later to "folds to a constant" as a one-predicate swap once
constant-overflow rejection builds the folder, and nothing compiling under the
narrow rule changes meaning when it does.

The parser tells a function from a variable by scanning past the type and name
and looking for `(`; a failed scan takes the variable path deliberately, so
`i32 = 1;` reports a missing name rather than a missing parameter list. Reading
a global needed no emitter change at all — `lower_name` already mangles from the
declaration node, and a global's declaration is a `Var_decl` like any other.
Writing one needed `emit_globals` to use `lower_literal` rather than `lower`,
because `lower` emits the statements a value needs first and file scope has
nowhere to put them.

Two traps, both from the same shape: a `Source_file` case added to a `visit`
whose `default:` used to recurse must **keep recursing** for everything it does
not handle. In the resolver, omitting the pre-pass exclusion makes every global
report "already declared" against itself. In the type checker, omitting the
fallthrough silently stops every function body from being checked at all — a
program with three mistakes in `main` reported one, from the resolver. Both are
the recurring lesson: a switch that replaces a recursing default inherits the
obligation to recurse.

`break` and `continue` work end to end. The checker scopes a loop-depth counter
to the loop *body*, so `break` in a bare block, after a loop has closed, or
inside an `if` that is not itself in a loop are all rejected — and the cases live
in `visit` explicitly rather than falling through its `default:`, which recurses
into children and would have let `break;` compile anywhere at all.

The emitter is where the work is. `break` needs nothing: a loop is emitted as
`while ( true )` with the condition guarded inside, so C's own keyword already
means "leave this loop", and there is no `switch` for it to bind to instead.
`continue` splits. In a `while` it is C's own `continue`, which returns to the
top and re-runs the condition statements — exactly right. In a `for` it must not
be: the update is emitted *after* the body, so C's `continue` would skip it and
the loop would never advance. So a `for` emits a label before its update and
`continue` jumps there.

The label is named eagerly and written lazily — `-Wunused-label` is in `-Wall`
and the golden runner builds with `-Werror`, so a label nothing targets fails the
build. The body is emitted before the label's position, so a "was it used" flag
on the loop stack settles it with no pre-scan. That stack carries a break target
as well, unused today and deliberately present: at M3 `break` has to run the
destructors for every scope it leaves, so it becomes a jump to a cleanup label,
which is the mechanism §7 already commits to.

None of this survives M3 as written — the emitter is re-pointed at KIR, where
`break` and `continue` are ordinary CFG edges rather than statements. What
survives is the checker's rule and the shape of the loop stack.

Four larger fixtures now exercise the three features together rather than one at
a time: `linked_list.kl` (a stack-allocated list, rewired in place through
pointers), `hashing.kl` (FNV-1a, xorshift32 and an LCG, all built on `wrap`),
`geometry.kl` (structs nested and passed by value, with the one cast the
arithmetic forces — an i32 width times an i32 height needs i64), and
`numbers.kl` (integer-only algorithms, including an integer square root written
as one *because* `cast` refuses float to integer). Their expected values are the
standard ones for those algorithms, computed independently, so a wrong answer
means the compiler disagreed with the algorithm rather than with a number
invented to match it.

Writing them immediately found a **miscompile**: `( *p ).field` as an assignment
target lowered to `*p.field`, and `.` binds tighter than unary `*` in C, so that
means `*( p.field )`. Every existing fixture had used a deref *or* a field
access as a place, never one composed into the other, so nothing caught it — and
where the field is itself a pointer the wrong form still compiles. This is the
same lesson §15 already records twice: the front end's assertions cannot see
what the back end gets wrong, and only real programs go looking.

`cast` and `wrap` work end to end (D28, table in §6.5): both parse as one
`Cast_expr` whose `aux` says which and whose children are the target type and
the operand, the checker enforces the table, and the emitter lowers each to a C
cast in three-address form. Narrowing `cast` is refused for now — see the debts.
`wrap` covers that cell, and `tests/codegen/conversions.kl` checks the wrapping
arithmetic at run time rather than only pinning the emitted C.

Raw pointers work end to end: `*T` annotations, `&x`, `*p`, writing through a
pointer, pointers to structs and to pointers, `nullptr` as a context-typed
literal (D26), and equality against it. No arithmetic and no indexing (D27), and
no `unsafe` gate yet — that waits for M3, where `Buffer` gives it something
concrete to gate.


`move`, `out` and `ref` lex, parse and dump as one `Marker_expr` whose `aux` says
which — D2's call-site marker, and the two the §12 arrow question resolved to.
Sema reports "not supported yet" for all three: `move` has nothing to enforce
until a type has a destructor, and `ref`/`out` have nothing to mean until
references exist. Their **signature** forms are deliberately unwritten —
`Param_decl` is `aux = name, children = { type }`, with nowhere to put a passing
mode, and choosing one before M4 defines what `ref` and `out` do would be picking
a spelling with nothing to check it against.

Taking those three keywords costs `out` and `ref` as identifiers, both of which
are legal C++ and common names. §5.1 permits it — rejecting is always safe — and
the cost is one diagnostic naming the problem rather than a confusing one about a
stray `;`.

Error-type absorption now covers literals as well as nodes. Six places walked an
expression purely to surface the errors *inside* it, in a context that had
already failed — `check`'s error-expected guard, `infer_binary`'s literal
adoption, three in `visit_assign`, one in `visit_return`, one in
`infer_struct_literal`. All of them now go through `Checker::absorb`, which
skips a literal: there is nothing inside one to be wrong, and its only possible
complaint is that nothing told it what type to be, which is exactly what the
error that got us there already said. `nullptr` was where this showed, being the
only literal with no default type to fall back on, so `missing() != nullptr`
reported both the unknown name and a second line about the literal. The rule is
that the *cause* is the diagnostic worth printing.

### Debts to pay along the way

- **A global's initialiser should be folded to a value in the front end, not printed as an
  expression by each backend.** Today `Spelling::constant_expression` walks the AST and prints C,
  which works because C accepts an arithmetic constant expression at file scope.

  It cannot be lowered into KIR as things stand, and the reason is structural rather than
  incidental: KIR is three-address form, so `60 * 60` becomes a statement writing into a temporary,
  and a C file-scope initialiser must be one expression with nowhere to put one. LLVM answers this
  with a separate `Constant` sub-language that is explicitly not an `Instruction`; adding the
  equivalent to KIR would be a second representation rather than a unification.

  The better destination is clang's: **evaluate it in the front end and record the value.** Most of
  the machinery exists - `fold_integer` and `fold_float` were written for constant-overflow
  rejection, and D-rule already requires a global's initialiser to be a constant expression, so
  folding is always possible in principle. It would delete the expression printer from every
  backend and make an LLVM backend's globals a `ConstantInt` rather than a tree to translate.

  Two gaps to close first: the folder returns "not constant" for `&`, `|`, `^` and `~` because
  those cannot overflow and it was written to detect overflow rather than to evaluate; and there is
  nowhere to record the result, which wants a small value type on `Types`.

  **Wanted sooner rather than later**, but after the KIR backend lands - it removes backend code,
  so doing it while a second backend is half-written would mean writing what it deletes.

- **Four files have grown past what one file should hold.** Code lines, excluding the in-source
  tests that roughly double each: `sema/type_checker.cpp` 2350, `parse/parser.cpp` 1675,
  `codegen_c/emitter.cpp` 1247, `lex/lexer.cpp` 824.

  The mechanism is identified in §3.2 and is not sprawl: each is built around a **file-local class**
  — `Checker`, `Parser`, `Emitter` — and a class cannot span translation units, so every new
  responsibility becomes another private method in the same file. The tests then double it.

  KIR is being built the other way round (§3.2) to avoid repeating it, which also makes it the
  natural moment to judge whether the free-function-pass discipline is actually pleasanter to work
  in before retrofitting it. **To be revisited after KIR lands**, when there is evidence rather
  than preference. The likely shapes: split `type_checker.cpp` along its seams — annotation
  resolution, the operator tables, the constant folder — into free functions over `Ast` and
  `Type_table`; and split the parser by grammar section. Neither should be attempted mid-M3.

- **`const` is parsed and discarded.** `type_of_annotation` unwraps `Const_type` and returns the
  inner type, and `Type` carries no const bit, so every one of these compiles today: assigning to
  a `const` local, incrementing one, compound-assigning one, assigning to a `const` parameter,
  writing through a `const T*`, and mutating a field of a `const` struct.

  This is a live **§5.1 violation**, and one of the worse shapes of it: `const i32 x = 1; x = 2;`
  is identical syntax to C++ with the opposite meaning, silently — a reader writes `const` and
  believes it. §5.1's own prescription would be to reject the keyword outright until it is
  enforced, which is cheap; that was considered and deliberately declined, on the grounds that the
  churn is not worth paying twice.

  The enforcement belongs at **M4**, where `const T&` and `T&` parameters arrive: the moment
  pointers are involved the change stops being one bit on `Type` and becomes a decision about
  `const i32*` versus `i32* const`, which is entangled with references. Until then this is a known
  hole, not an oversight.

  One consequence to respect meanwhile: **do not emit C `const`** for anything, including a
  `const` global. A Keel-legal write would then fail in `cc` against generated code, and the
  golden runner's `-Werror` build would surface it as `assignment of read-only variable
  'kl_c_1'` — the C compiler doing the checking Keel declined to do, with the worse message.

- **Narrowing `cast` is blocked, not implemented.** D28 defines `cast` as checking the value at
  run time and trapping when it does not fit, and nothing in the pipeline can emit that check yet.
  Rather than let a narrowing `cast` silently truncate — which is `wrap`'s behaviour wearing
  `cast`'s name, exactly the silent wrong answer D5 exists to remove — the checker refuses it. The
  block is one named constant, `k_narrowing_cast_needs_a_run_time_check` in `type_checker.cpp`,
  and the single branch that reads it; deleting both is the whole change once the KIR can trap.
  Nothing that compiles today changes meaning when it goes, because the cell is currently empty.
  The three tests under `type_checker_holds_back_a_narrowing_cast` go at the same time.

- `mangle_function` is not injective. Argument types are spelled with `Type_table::name()`, the
  plain Keel spelling, so `i32*` becomes `i32p` and a struct genuinely named `i32p` collides with
  it. Two separate improvements are wanted, and only the second is complete:
  - **A category tag** — `kl_struct_`, `kl_func_`, `kl_local_` — closes collisions *between* the
    three schemes, which exist today: `mangle_struct( "", "foo__" )` and
    `mangle_function( "", "foo", {} )` both give `kl__foo__`, and at M7 a module named `foo` makes
    `mangle_local( "foo", 7 )` and `mangle_struct( "foo", "7" )` both `kl_foo_7`.
  - **Embedding each argument type's own mangled name** rather than its spelling — so a struct
    argument reads `kl_struct__Point`, distinct from any builtin's spelling. This makes a collision
    require a deliberately perverse name, but is still not injective: `_` is the separator and may
    appear inside a name, so `f( struct A, struct B )` and `f( struct A_kl_struct__B )` still
    coincide. Only **length-prefixing**, as the Itanium ABI uses (`6Vector3i32`), is injective by
    construction. M6 forces the question anyway, since `Vector<i32>` contains characters that are
    not identifier characters at all.
- `can_start_expression()` and `parse_prefix`'s Keyword case are two halves of one list, and three
  features in a row have needed both edited together — `true`/`false`, then `move`/`out`/`ref`,
  then `nullptr`. A keyword missing from the first parses correctly in an argument or an
  initialiser and fails only in statement position, which is why it keeps getting through. They
  want to share one predicate.
- `Node_kind::Null_literal` had to be added to four separate switches - `infer`, `check`,
  `is_literal_expression` and the emitter's `lower` - and missing any one of them failed silently
  rather than loudly. A literal kind is currently a five-place edit.
- `expect_keyword()` will need a spelling that `token_kind_spelling()` cannot
  give. Every keyword shares one `Token_kind::Keyword`, so the spelling function
  can only answer "keyword" — *which* one lives in `Token::symbol`. A message
  like `expected \`return\`` therefore has to go through the keyword spelling
  table in `interner.cpp`, not through `lex/token.cpp`. `error_expected` avoids
  this today only because it quotes the source text for the *found* half.
- `found_text()` returns a formatted message fragment, not raw text: source text
  quoted, end-of-file as prose. Its name says otherwise, and a call site added
  later that supplies its own backticks would double-quote with nothing to catch
  it. `expectation()` beside it draws the same distinction for the expected half —
  punctuation is quoted because it is what the author would type, a category is
  prose because "identifier" is not something you can write.
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
- Float literals are not range-checked, and **should be**, for the same reason
  `u8 x = 300;` fails. `f32 x = -1e40;` compiles and yields infinity today:
  `check_literal` asks only whether the target is a float, and
  `Type_table::fits` treats floats as a mantissa bound on *integer* magnitudes,
  never looking at a float literal's value. It wants a `fits_float( f64, Type_id )`
  beside it, checking the magnitude against the target's maximum.
- A constant expression that overflows is not caught: `u32 d = 1 - 2;` gives
  4294967295 rather than an error, because both literals legitimately fit `u32`
  and nothing folds the subtraction. Catching it needs either constant folding or
  the §12 overflow decision, and is the same question either way.
- Prefix `++` is unreachable — `can_start_expression` rejects it, so `++i;` says
  *expected a statement*. D12 left the postfix/prefix choice open; decide it.
