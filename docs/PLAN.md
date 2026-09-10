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
| L13 | **Two aggregate kinds** (D29). A `struct` is a transparent aggregate: all fields public, trivially copyable, no destructor, no owning members. A `class` has invariants: fields private by default, may own resources, may have a constructor and a destructor, and is moved rather than copied. Neither inherits and neither is virtual in v0. |
| L14 | No exceptions in the language. Errors are `Result` + `?`. |
| L15 | Naming: `Capitalized_snake_case` types (`String_view`, `Hash_map`), `snake_case` values and functions — the convention already used in `MANIFESTO.md` §12. |
| L16 | C declaration order (`i32 x`), so type constructors are **postfix**: `T*`. References are not among them — `ref` is a binding mode written in front of the type (D32), so `T*` is the only one left and `&` means address-of and nothing else. |
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

// --- M3: RAII. Owning types are classes (D29); the destructor is spelled as in C++. ---
class Buffer
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

// --- M4: ownership through call-site markers (D2, D31) ---
void consume( Buffer b );           // called as consume( move b ) - b is dead afterwards
u64  inspect( Buffer b );           // called as inspect( b )      - read-only borrow, may not escape (§8)
void grow( Buffer& b );             // called as grow( ref b )     - mutable borrow, may not escape

// --- M5: sum types. `enum` gains payloads; `switch` gains destructuring. ---
enum Shape
{
    Circle( f64 radius ),
    Rect( f64 width, f64 height )
};

f64 area( Shape s )
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
| D2 | **Ownership transfers only where the call site says `move`**: `consume( move b )`. A bare argument never transfers — for a `class` it is a read-only borrow, for a `struct` a copy (D31). A type is **owning** exactly when it has a destructor, directly or through a member: `class Wrapper { Buffer b; }` is owning, because destroying one destroys a `Buffer`. That transitivity is forced rather than chosen, and it is Rust's `Copy` rule. A raw pointer owns nothing by itself — an address says nothing about who frees it. The query is a memoised walk over the containment graph M2 already builds and orders, and D29 reuses it unchanged: `struct` is legal exactly when the type is **not** owning. Note the rule's real content is **"is it copyable"**; "has a destructor" is a proxy that coincides only because §6.6 puts copy constructors outside v0. `Weak<T>` will have a destructor and should still be copyable, so this needs restating when copy constructors arrive. | The earlier rule made `consume( Buffer b )` a move — C++'s syntax for a copy, carrying different semantics, which is the one divergence §5.1 forbids. A marker at the call site fixes that. D31 then closes the hole it left: with copy constructors outside v0 a class cannot be copied at all, so a bare argument had no meaning available to it except *borrow*. The cost is a marker on every transfer; Rust pays none, but Rust has no C++ copy expectation to fight. **Inert until M3**: no type has a destructor yet, so nothing is owning and nothing changes. |
| D3 | Braces mandatory on every `if`/`while`/`for` body. | Braceless C++ is a parse error, not a reinterpretation. Kills the `goto fail;` class of bug. |
| D4 | `switch` has no fallthrough, needs no `break`, requires exhaustiveness, and matches sum-type payloads. | Payload patterns (`case Circle( r ):`) are new notation. A `switch` over a plain integer keeps C++ meaning minus fallthrough; missing cases are an error, never a silent skip. |
| D5 | No *lossy* implicit conversions. A binary operator widens both operands to the smallest type that losslessly holds both, and is a hard error whenever C++'s own result type would not hold them. Assignment is implicit only where the target holds the source type. int↔bool and pointer↔bool are never implicit. §6.4 has the table. | The second clause makes the §5.1 audit executable: where C++ is lossless Keel agrees with it, and where C++ silently loses information Keel refuses. Lossless widening is not a conversion anyone can get wrong, and requiring a cast for it trains authors to write casts reflexively — which is how the dangerous ones get waved through. **Provisional**: adopted for M1 to unblock the type checker, and expected to be re-judged once real code exists. |
| D6 | `?` postfix operator for error propagation. | No meaning in C++; pure addition. The one borrow from outside the C family, kept because no C++ notation exists for it. |
| D7 | `enum` variants carry payloads. | A payload-free `enum` behaves exactly as C++'s `enum class`, which D30 makes the only spelling. Payloads are new notation. |
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
| D19 | A declaration may not shadow another that is still reachable by the same unqualified name. A block local colliding with an enclosing local or with a parameter is an error. Scopes that do **not** carry outer names in — a function body relative to file scope, and later a lambda relative to its enclosing function — are *barriers*, and reusing a name across one is fine: a local may be called `count` alongside a top-level `count()`. **Fields are visible unqualified inside a member body, and may be shadowed by a parameter but never by a local.** `Buffer( u64 len ) { this.len = len; }` is legal; `~Buffer() { u64 len = 0; }` against a field `len` is not. | The wart is the *silent* failure: two locals of the same type mean picking the wrong one compiles, runs and returns a bad value. Shadowing a function with a variable is caught immediately by the type checker, so it needs no rule. Java (JLS §6.4) and C# (CS0136) reject exactly this and keep members shadowable for the same reason; C++ allows it and papers over it with `-Wshadow`. File scope is exempt so that adding a top-level function cannot break a function body far above it — action at a distance. Rust's shadowing is same-scope rebinding (`let x = validate(x);`), which we already reject as a duplicate declaration and which this does not revisit. The member clause draws the line at whether the shadow was *forced*. A constructor parameter naming the field it initialises is the one place C++ programmers shadow constantly, and every alternative is worse — `new_len`, `len_`, a convention nobody agrees on — so it is allowed, with `this.len` as the disambiguation they already write. A local has no such excuse: nothing forces the collision, so it is exactly the silent-wrong-value hazard this entry exists to kill. Slotting members into the same rule keeps one sentence rather than an exception carved beside it. Stricter than C++, which permits both and papers over it with `-Wshadow`, and safe under §5.1 because it rejects rather than reinterprets. **Inert until constructors**: a destructor's only parameter is `this`, which is a keyword and cannot collide with a field, so until member functions take arguments only the restrictive half is reachable. |
| D20 | A character literal is a `u8` holding one byte: `'A'` is 65, `'\xFF'` is 255. A literal spanning more than one byte is an error, so a non-ASCII character needs its bytes written out. String literals lex and parse but have no type at all, and are rejected with a message saying so. | D1 leaves no `char` type to give them, and inventing one for literals alone would be a second spelling for `u8`. Treating a code point as the integer it is means §6.4's range rules apply unchanged: `u8 c = 'a';` and `u32 c = 'a';` both work, `bool c = 'a';` does not, and no new machinery is needed. C++ makes `'a'` an `int` and `'ab'` implementation-defined; both are rejected here rather than reinterpreted. Strings wait because their representation is an M7 question (§9) — choosing `u8*`, a slice or an array now would commit the language before the ownership model exists to judge it. |
| D21 | `main` returns `i32` and takes no parameters. Any other signature is a hard error. | C's `main` returns `int`, so a wider or narrower return type would either truncate in the emitted shim or fail to convert at all — and the shim calls `main` with no arguments, so a parameter would generate C that does not compile: a `cc` error pointing at generated code instead of a diagnostic pointing at the program. Command-line arguments wait for arrays and modules (M7), at which point this entry is what has to change. A program with no `main` is a library and stays legal. |
| D22 | `.` is the only member-access operator, and reaches through a pointer. `->` is a hard error naming `.` as the replacement. | C++ needs both because `.` on a pointer means nothing there; a language designed now does not. The decisive reason is generics (M6): `template<C T> ... thing.x` works whether `T` is a value, a borrow or a pointer, where `->` would force the template's author to know which. `->` distinguishes less than it appears to even in C++ — `.` on a reference is already spelled like `.` on a value — and L6 makes every signature fully annotated, so the declaration is in view. §5.1 constrains neither direction here, unusually: `.` on a pointer is a C++ error, so accepting it only adds meaning. The `Arrow` token is **kept** so the parser can reject it by name; deleting it would lex `p->x` as `-` then `>` and produce a message about arithmetic. |
| D23 | A struct literal must initialise **every** field. `Point { 1.0 }` for a two-field struct is an error, not a zero-fill. | C++ value-initialises the fields you leave out, so adding a field to a struct silently changes what every existing construction site builds. Requiring all of them turns that into a compile error at each site — noisy in the way that finds bugs. This rejects a program C++ accepts, which §5.1 permits outright. Mixing positional and named initialisers is also an error, but that needs no entry of its own: C++20 forbids it too, so Keel is simply following. |
| D24 | **Mutable by default**, as in C++. `const` is opt-in. | The manifesto's "safe by default" pulls the other way, and this is the one place the two goals genuinely conflict — but mutability is not what makes a program unsafe; aliasing and lifetime are, and those are M3-M4's business. Most values are written more often than they are read once, so const-by-default taxes the common case to annotate the rare one. And `i32 y = 0; y = 1;` failing would astonish exactly the developer §5.1 is written for. |
| D25 | `int`, `float` and `double` stay **hard errors**, never aliases. | D1's suggestion path is implemented and works: the message names the replacement, so the cost is one compile the first time. Accepting them would buy that same first hour at the price of a permanent second spelling for every type — every reader thereafter has to know both, and every code base picks one by accident. |
| D26 | `nullptr` is the null pointer, spelled as in C++, and it is a **literal** whose type comes from context: `u8* p = nullptr;` adopts, `auto p = nullptr;` is an error. | The spelling is C++'s because §5.1 has no reason to invent another for an identical concept. Making it a literal rather than a value of some `nullptr_t` reuses `check_literal` wholesale and keeps D5 intact — no conversion happens, the literal simply *becomes* that type, exactly as `42` becomes a `u32`. The `auto` case then falls out as an error for the same reason it does for any literal with nothing to adopt from. |
| D27 | **No pointer arithmetic on `*T`**, which points at exactly one `T`. Arithmetic belongs to a many-item pointer, `[*]T` in Zig's notation, which v0 does not have. | `p + 1` on a single-item pointer is not dangerous, it is *nonsense*, and a type distinction catches it statically at no cost. Note the performance argument for C's arithmetic does not hold: `*(p + i)` and `p[i]` compile to identical machine code, so what buys speed is the capability of touching raw memory, which survives either spelling. C's implicit scaling by element size is the wart — a named `offset` operation says what it does. §6.4 already answers nothing for a pointer, so rejection is the default rather than a rule to add. `==` and `!=` between two pointers of the **same** type are allowed, since that is how a null check is written; ordering is not, because comparing pointers into different allocations is meaningless. |
| D28 | Conversions are written `cast<T>( x )` and `wrap<T>( x )`, both **keywords**. `cast` preserves the value; `wrap` keeps the low bits. Neither converts float to integer, and neither converts integer to bool. §6.5 has the table. **Narrowing `cast` is refused until the run-time check exists.** | Two spellings rather than one because the failure policy is the interesting part, and an unqualified cast lets an author avoid stating it — which is how C's `(u8)x` silently truncates. Keywords because as identifiers they walk into §12's `a < b > ( c )` ambiguity; as keywords the `<` can only be a bracket. Both spellings are new notation, so §5.1 is satisfied for free. Float to integer is rejected because `cast<i32>( 1.9 )` has no obvious answer — truncate, round, floor and ceil are four operations and C picks one silently; they arrive as library functions at M6. Integer to bool is rejected because `x != 0` says it better and modular arithmetic down to one bit says something else again. Float *rounding* is accepted (`cast<f32>( some_f64 )`), because a float that cannot hold the value gives an infinity rather than a plausible wrong number — the line is that `cast` refuses to turn a value into a *different* value, not that it refuses to lose precision. |
| D29 | **Two aggregate kinds, split by one principle: a `struct` is a type whose representation is its interface; a `class` is a type whose interface hides its representation.** A `struct` has all fields public, is trivially copyable, may not have a destructor, and may not contain an owning member (transitively); it is built from a struct literal (D23). A `class` has fields private by default, may own resources, may have a constructor and a destructor, is moved rather than copied, and is built by a constructor. **Both may have methods.** Neither inherits and neither is virtual in v0. A `struct` with a destructor, and a `struct` with a `private:` label, are hard errors naming `class` as the fix. | C++ has two keywords for one job — the only difference is default access, kept so that C headers would compile — and Keel pays no C-compatibility tax, so the second word is free to earn its keep. The line is drawn at **trivial copyability** rather than at "may have methods", because only the first has semantic consequences: a trivially copyable type cannot have a destructor (copy plus destructor is a double free, which is why Rust makes `Copy` and `Drop` mutually exclusive), is never moved, and never enters §8's drop analysis. "May have methods" has no consequences at all, and the motivating examples for restricting it — `Node_id::is_valid()` — need them anyway. The two initialisation syntaxes stop competing as a side effect: literals belong to structs, constructors to classes, so `Buffer { ... }` versus `Buffer( 16 )` never has to be disambiguated. Enforcement is free: `struct` is legal exactly when D2's owning query says no. Safe under §5.1 because both rejected spellings are errors rather than reinterpretations. Prior art cuts both ways and is worth recording: the languages that keep two aggregate keywords (C#, Swift, D) split on value-versus-reference semantics, and the C++ successors that exist (Carbon, Cpp2, Hylo) collapse to one kind. This splits on copyability, which is the ownership-language analogue of the first — Keel has no garbage collector, so "reference type" has nothing to mean. |
| D30 | **One `enum` keyword**, carrying `enum class`'s semantics: scoped (`Shape::Circle`), with no implicit conversion to an integer. The underlying type is spelled as in C++: `enum Shape : u8 { ... }`. `enum class E` is a hard error saying to drop the `class`. | The same C-compatibility tax as D29, with the opposite answer, and the asymmetry is the point: `struct`/`class` are two words for one job, so the job gets split; `enum`/`enum class` are two words for one job where only one of them does it correctly, so there is nothing to split. C++'s plain `enum` leaked its variant names into the enclosing scope and converted implicitly to `int`; both were mistakes, `enum class` fixed them in C++11, and the broken spelling survives only for C. Safe under §5.1 because every point where the two meanings diverge is an error rather than a reinterpretation: `Shape s = Circle;` is an unknown name, and `i32 x = Circle;` and `if ( s == 0 )` have no conversion to reach for. Rejecting `enum class` follows D22's pattern — keep the spelling recognised so the diagnostic can name the fix. **Open at M5**: a payload-carrying variant can hold an owning type, so an `enum` inherits D29's question of which kind it is. |
| D31 | **Argument passing has five forms, and the call site names every one where something happens to the caller's variable.** Bare `f( x )` — the callee gets a copy it owns if `x` is a `struct`, a read-only borrow if `x` is a `class`; either way the caller's object is alive and unchanged afterwards. A `const ref T` parameter is called bare as well, and is the fifth form: a read-only borrow of *either* kind, which is how a large `struct` is passed without copying it. `f( ref x )` — a mutable borrow. `f( out x )` — uninitialised, and the callee must assign it. `f( move x )` — the callee owns it and `x` is dead afterwards: for a `class` because the resource left, for a `struct` because the author said so. `const T&` does **not** survive as a spelling; D32's `const ref T` replaces it in every position. **The same rule governs initialisation and assignment, not just arguments**: `Buffer b = a;` is a hard error naming `Buffer b = move a;` as the fix, and so is `b = a;` between two existing classes. A `return` is the one exempt position — `return b;` needs no marker, because `b` is going out of scope regardless and there is no later use for the marker to warn about. | One principle generates the whole table: **you may modify what you own.** A bare `struct` parameter is a copy you own, so it is mutable — which is also exactly what C++ does, so this costs no audit row and keeps `i32 factorial( i32 n ) { ... n--; }` legal. A bare `class` parameter is a borrow you do not own, so it is not; that is forced rather than chosen, because copying a class needs a copy constructor and §6.6 puts those outside v0, leaving *borrow* as the only meaning available. The uniformity that matters is caller-side and holds in both rows — after a bare argument the object is alive and unchanged — so reading a call site never requires knowing the kind. The variation is callee-side, concerns a type named on the same line under L6, and surfaces as a compile error rather than a silent difference in meaning. The result is C#'s behaviour for value and reference types, arrived at from ownership rather than from a garbage collector. `move` on a `struct` copies the bytes and marks the source dead in the checker: an assertion, not a transfer, which keeps the keyword's user-visible meaning identical across kinds and costs almost nothing, since structs are already in §8's dataflow for the `Uninitialised -> Live` half and drop elaboration still never looks at one. **Obligation at M6**: a generic that mutates a bare parameter is legal only when `T` is a `struct`, so definition-checked generics (D11) need a bound that permits it — `is_trivially_copyable`, alongside `is_numeric` and the rest — rather than deferring the error to the instantiation site as C++ does. The initialisation case is where the rule earns most: C++ would call a copy constructor for `Buffer b = a;`, and with none available the two remaining readings are to move silently — leaving `a` dead with nothing in the source saying so — or to bind `b` as a reference to `a`, which is worse, because two names would own one resource and the second destructor would be a double free. Rejecting is the only safe answer, and it rejects valid C++ outright rather than reinterpreting it, which §5.1 permits. Note that `b = move a;` must also destroy whatever `b` held first; that is drop elaboration's job at M3, not a separate rule. **The marker appears in the signature as well as at the call site**, and the two must agree: `void consume( move Buffer b )` is called as `consume( move b )`, `void grow( ref Buffer b )` as `grow( ref b )`, `void init( out Buffer b )` as `init( out b )`, and an unmarked parameter as `inspect( b )`. `const ref` is the one that takes no marker: `void peek( const ref Point p )` is called as `peek( p )`. This is forced: with `const T&` gone, `void consume( Buffer b )` and `void inspect( Buffer b )` would otherwise be indistinguishable, and the callee has to know whether it owns its argument. Marking both sides is C#'s design rather than C++'s, and it removes `&` from parameter lists entirely — a reference type still exists (L16), but a parameter never spells one, because each of the three things `&` was doing in a C++ signature now has its own keyword. The redundancy is only apparent: the signature states the contract and the call site acknowledges it, which is the whole point of D2 — a reader of the call site should not have to find the declaration to learn that a variable just died. **Amended once `const ref` was implemented.** This entry first said `const T&` did not survive *because a bare argument already means it*. That is true of a `class`, where §6.6 leaves borrowing as the only meaning available, and false of a `struct`, where bare is a copy — so there was no way to pass a large `struct` read-only without copying it, and the entry counted four forms where the language needed five. D32's `const ref T` fills the gap. That it takes **no marker at the call site** follows from this entry's own argument rather than being an exception to it: the marker exists so a reader knows something happened to the variable, and after a `const ref` argument nothing has — it is alive and unchanged, exactly as after a bare one. The agreement rule therefore asks about *mutation* rather than about modes, and `const ref` sits with bare. What the fifth form changes is callee-side only: what the call costs, and whether the callee may write. |

| D32 | **`ref` is a binding mode, not a type.** `ref T x` and `const ref T x` replace `T&` and `const T&` in every position — parameter, local binding, and return. `T&` in type position is a hard error naming `ref T`. `&` therefore means address-of and nothing else, and `T*` is the only postfix type constructor left (L16). A `ref` binding is **initialised at its declaration and never reseated**. | Two spellings for one concept is the redundancy D25 and D30 already refuse, and D31 had removed `const T&` from parameters — where the great majority of references appear — leaving `T&` alive only for local bindings. Finishing it costs little more and stops the language carrying both. The reframe is what earns it: as a *type*, §8's rule that a reference may not live in a struct is a restriction needing a diagnostic; as a **mode**, a field simply is not a binding and the rule disappears into the grammar. Reading order improves too — `const ref i32` has one order where C++ has `const i32&` and `i32 const&` meaning the same thing. Prefix does not violate L16, because a mode is not a type constructor. Taken at M4 rather than later because references were still *unimplemented* — `Ref_type` parsed and the checker said "not supported yet" — so the change cost a parser branch and an enum entry rather than a migration. `Ref_type` accordingly becomes a flag on `Param_decl`/`Var_decl` rather than a node wrapping a type. The §5.1 cost is real and is the largest departure in spelling the language has taken: `i32& r` is idiomatic C++ and becomes a syntax error. §5.1 permits rejecting outright, and the diagnostic names the replacement. **Answered by the implementation**: the receiver and a `ref` parameter are one mechanism - a pointer local that `place_for` derefs - so making `this` spell `ref T` is now a change of spelling rather than of design, and can wait for whoever wants to write it. |


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
**Unions are a decided non-goal, not a deferral**: a union is only safe when the
tag is maintained by hand and correct every time, and the one legitimate use of
one — a tagged variant — is what M5's payload-carrying `enum` (D7, D30) is, with
the tag maintained by the compiler and `switch` exhaustiveness checked.
Modules arrive at M7. **Reflection** is listed here for v0 only — §12 records its direction: compile-time reflection, yes; runtime reflection, never. **String literals** lex and parse but have no type
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
> It may **not** be stored in a struct or captured. It may be **returned only as a
> `const ref` derived from one of the function's own reference parameters** — the
> receiver counts as one, so a method may return a reference into its object.

This makes dangling references unrepresentable by construction rather than by
analysis. It is roughly Hylo's (formerly Val) approach, and it is a defensible
permanent design, not merely a shortcut.

**Why returning one needs no lifetimes.** D32 makes a `ref` binding initialised at
its declaration and never reseated, and that single property carries the whole
argument. Everything nameable at a binding's declaration lives in an
enclosing-or-same scope: in the same scope it was declared *earlier*, so it is
destroyed *later*, and in an enclosing scope later still. Therefore **anything
alive at a call site outlives any `ref` binding declared at that call site** — and
it does not matter which parameter the result derived from, because all of them
outlive it. The question Rust answers with lifetime parameters, and which was the
reason to consider refusing a function more than one reference parameter, simply
does not arise here. Keel is stronger than Rust on this one point precisely
because it gave up reseating. What remains is a syntactic check with no dataflow:
trace the returned expression to its root and require a reference parameter, not a
local, a by-value parameter, or a temporary.

The honest cost of allowing it: dangling then stops being *unrepresentable* for
returns and becomes *rejected*. The check is one pass and cheap, but it is a check,
and this paragraph is where that concession is recorded.

**The hole is temporaries.** `const ref A r = pick( make_a(), b );` binds into a
value with no scope of its own — C++'s famous case, where binding a `const T&`
directly to a temporary extends its life but binding through a call does not. The
options are to extend the temporary to the binding's scope, to reject passing one
where the result is bound by reference, or to give temporaries the enclosing
scope's lifetime outright. **This is the same decision drop placement already
owes**: an owning temporary is currently never dropped (see the debts), and M4 needs
a temporary-lifetime rule for that regardless. One answer serves both. Rust's borrow checker is the single
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
| **M4** | Move checking, `ref`/`out` bindings (D32), the non-escaping rule, drop flags. Enforcement of D2. | Use-after-move is a compile error with a good message; a conditionally-moved value drops correctly. | Ownership, dataflow analysis |
| **M5** | Payload-carrying `enum` (D30), `switch` destructuring, exhaustiveness checking. | The `Shape`/`area` sample. Non-exhaustive `switch` is a compile error naming the missing variant. | Sum types, tagged variants |
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
| ~~Does `class` exist at all, or is `struct` the only aggregate?~~ **Answered — D29.** Both exist, split at trivial copyability: `struct` is a transparent aggregate that cannot own, `class` is a type with invariants that can. Answered early, at M3 rather than M7, because the cost is asymmetric — one keyword now, versus a breaking change to every program that declared a `struct` that should have been a `class`. | M3 — decided |
| Custom allocators / arenas — visible in the type system or not? | M7 |
| Module granularity: file, directory, or explicit declaration? | M7 |
| Standard library naming. `MANIFESTO.md` §12 already refuses to mirror `std`, but the specific names are unsettled: one `Hash_map` rather than `map`/`unordered_map`, and a better name than `vector` for a dynamic array. Note the one real trap — `List` reads as a *linked* list to a C++ programmer (it is `List<T>` in C#/Java but `std::list` in C++), so a familiar name would carry the wrong semantics. Not a §6.3 divergence: those cover syntax and semantics the compiler enforces, and no library exists yet. | M7, when the first containers are written in Keel |
| **What does the runtime look like, and how does Keel call into it?** §7 promises a `runtime/kl_rt.{h,c}` under 200 lines for allocation, abort/panic and `print`, and none of it exists - no Keel program can allocate anything. **Deferred past M3**, deliberately: M3's acceptance names freeing, but what makes drop placement hard is *where*, and that is proven by a fixture whose destructor increments a counter on every path out, early `return` included. The blocker underneath is that Keel cannot declare what it does not define - a body-less function is a parse error, D18's corollary - so an FFI declaration has no spelling. The likely answer is **`extern`**, which keeps that corollary intact (a bare prototype stays an error, with `extern` as the fix it names) and should mean what C++'s `extern "C"` means, **including suppressing mangling**: `kl_rt_alloc` has to emit under its own name. Four sub-questions, with leanings rather than answers. **Is an `extern` call `unsafe`?** Probably, as Rust's `unsafe extern` - the FFI boundary is where the assertion belongs, and being the safe wrapper over it is the point of `Buffer`. That contradicts §6.2's sample, which calls `kl_rt_alloc` bare, so the sample changes when this lands; to be reviewed rather than assumed. **What does allocation return?** Spelled `alloc<f64>( count )` - a keyword in D28's mould rather than a generic, so the `<` can only be a bracket, and the element type gives the compiler the size, so no `sizeof` is needed. Its type is `[*]f64`, which means this waits on the many-item pointer D27 leaves out of v0: the spelling is settled, the prerequisite is not. **What does failure do?** `nullptr` for now, which D26 already makes adopt from context, replaced by `Result` at M5. Nothing forces the check until then, and that is the stopgap's whole cost. **What can `print` print?** Nothing worth having - D20 leaves string literals with no type until `String` arrives - so it waits for M7 alongside them rather than shipping an integers-only version that has to be unbuilt. **And what language is it written in?** A thin C floor, with the runtime proper in Keel above it - the same instinct that already puts `Vector` and `String` in Keel at M7. That is what every comparable language converges on: Rust's `std` is Rust over libc, Zig's is Zig over a small `os` layer, D's druntime is D over a little C. Nobody writes a runtime in C by preference; they write it in their own language over the smallest floor they can manage. Raw syscalls, as Go and Zig do, buy static binaries free of libc versioning - genuinely appealing, and rejected here for two reasons: per-architecture assembly stubs, and that reaching them from generated C means `__asm__` blocks, which couples the C backend to one compiler's extensions and is exactly what §2.2 forbids. libc is the right floor for v0. Note also that with `alloc<T>` a keyword, the backend could simply spell it `malloc` and no runtime file would be needed at all - cheaper than `extern`, but it bakes an allocation policy into the compiler, sits badly beside D10's `Owned<T>` direction, and gets unbuilt at M6. `extern` costs more and is never thrown away, because it is also how Keel talks to any C library. | M4 at the earliest; `print` waits for M7 |
| **Compile-time evaluation: how much, and is there reflection?** **Direction decided: yes, and compile-time only.** The word usually evokes C#'s runtime reflection — `typeof( T ).GetProperties()` — and that is the one version Keel cannot afford: it requires type metadata for every type in every binary, which is the cost §4's *No RTTI* already refuses. Compile-time reflection has neither problem, and both motivating uses are compile-time by nature. **A testing framework in the standard library** needs to enumerate test functions and report their names, which is discovery over the program's own declarations: Zig builds this into the language (`test "name" { }`) with `@typeInfo` for the general case, D has `__traits`, and Rust reaches the same place from the other side with derive macros — compile-time code generation rather than reflection proper. **`enum` to string** is the canonical example, and the one C++ programmers have wanted for twenty years; C++26's `std::meta` finally delivers it. In Keel it lowers to a generated static table — the variant names are known at compile time, the enum is closed and scoped (D30), and the run-time cost is one array index. Because both uses are pure code generation, nothing need survive into the binary that the program does not use. What stays open is the surface — a `@` builtin as in Zig, a `__traits`-style call, or attributes as in Rust — and how much general compile-time evaluation sits underneath it. M5 must land first: payload-carrying variants change what printing a value even means. | Surface post-M5; the framework needs the standard library, so **M7** |
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

A file-scope initialiser is now **evaluated by the checker**, not printed as an
expression by the backend. `Types::constant_of` carries the value and
`global_definition` emits a literal, which deleted the last place a backend
walked an AST expression - and is what makes an LLVM backend's globals a
`ConstantInt` rather than a tree to translate. It also settles the question KIR
could not answer: three-address form has nowhere at file scope to put a
temporary, so a constant expression can never be lowered into it. LLVM answers
this with a separate `Constant` sub-language; evaluating in the front end, as
clang does, avoids needing one.

The folder became a real evaluator to do it. It had been written to *detect*
overflow, so it answered "not constant" for every operator that cannot overflow;
it now folds comparisons, `&`, `|`, `^`, `~` and casts. The last four needed
`to_bits`/`from_bits` - sign-magnitude to two's complement in the type's width
and back - because a bitwise operator is defined on the representation and
sign-magnitude has no bit pattern of its own. That machinery is what §12's
runtime overflow checking will want at M3 for the cases it can discharge
statically.

Doing it exposed a defect the goldens caught and the unit tests did not:
`fold_integer` read the node's recorded type for the width, but `check_constant`
runs *before* that type is recorded - so every width-dependent operator folded to
nothing exactly where overflow was being checked. The type is now passed in by
the caller that already knows it; only the outermost node needs the hint, since
the children were checked first.

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

**Storage markers are emitted.** `Storage_live`/`Storage_dead` existed in `kir.h`
from the start and the verifier and printer handled them, but nothing produced
one. The lowerer now brackets every declared local, which is the first piece of
M3 proper: the markers are not instructions — the C backend skips them and
declares every local up front — they exist so drop elaboration has an anchor,
and so the scope-exit machinery it needs is built and proven against something a
reader can check by eye.

The scope stack is flat with marks (`scope_locals_` plus `scope_marks_`) rather
than a stack of vectors, because the operation wanted is not "pop one scope" but
**unwind to a depth**: `return` unwinds to zero and `break`/`continue` unwind to
the depth recorded on `Loop_targets`. Markers come out in reverse declaration
order, which is not cosmetic — it is the order destructors run in, so drop
elaboration inherits it by reusing the same loop. Parameters, the return slot and
temporaries get no markers: the first two are live on entry and dead on exit by
construction, and a temporary has no scope to leave. Whether an owning value can
live in a temporary — `consume( make_buffer() )` — is a real question, and it
belongs to drop elaboration rather than here.

Two things went wrong, and both are worth recording because drops will meet them
again. **A `break` must not unwind the loop's own scope.** The exit block is a
join reached by both the break edge and the condition's false edge, and the local
is live on the second, so the exit block has to end it — which means ending it on
the break edge too produces two. Since the exit block is emitted before the
scope is popped, a break edge never actually leaves that scope, so `break` and
`continue` unwind exactly the same set and one depth field suffices. The general
form of that rule is what MIR needs drop flags for: **one drop at the join, not
one per predecessor**. And the C backend emitted its `#line` directive before
dispatching on statement kind, so a skipped marker left a directive with nothing
under it; the skip has to come first. That was caught by the codegen goldens
moving when they should not have — the useful kind of golden failure.

**`class` and destructors parse.** D29's first slice is in the parser, and two
shape decisions carry it. `Struct_decl` and `Class_decl` are **separate node
kinds** with an `is_aggregate` predicate beside them, because the two differ in
rules and not in shape — field layout, containment ordering, member resolution
and C emission all treat them identically, and only the rule checks read the
kind directly. Eight sites that compared against `Struct_decl` now ask the
predicate, so a third aggregate would be one edit rather than eight. The
alternative — one kind with a flag — had nowhere to put it: `aux` is a whole
`u32` holding the name, and packing a bit beside a `Symbol_id` is a trap waiting
on a program with enough symbols.

`Destructor_decl` is shaped as a `Function_decl` **minus its return type**, with
an always-invalid first child holding that place: `{ <none>, Param_list, Block }`.
That buys the thing that matters at lowering — `lower()` scans every node
linearly for a function, so a destructor nested inside a class body is found by
the same loop and read from the same indices, with no traversal change and no
second layout for any function-shaped pass to learn. `aux` carries the name
written after the `~` rather than the enclosing type's, so `~Wrong()` inside
`class Buffer` is a comparison the checker makes rather than a parse failure.

The parser deliberately accepts more than D29 allows: `~Point()` on a `struct`
parses, and so does a mismatched name. Both are the checker's to reject, because
the parser has no business knowing D29 and a parse error would give a worse
message than one naming `class` as the fix. `Tilde` needed no lexer work — it
was already the bitwise-NOT token, and it cannot start a field, so one token of
lookahead separates the two member shapes with no backtracking.

**D29's rules and D2's owning query are enforced.** Three checker passes, split
out rather than folded into the existing ones because the third cannot run
before the second has: `check_aggregate_members` (a struct may not have a
destructor; the name after the `~` must match; there may be only one),
`compute_owning`, and `check_struct_ownership`.

The query cost almost nothing, because the graph was already there.
`contains_itself` walks exactly what D2 describes — by-value aggregate members,
skipping pointers, since *"a pointer to a struct is finite"* — so the rule that a
raw pointer owns nothing arrived for free rather than as a case. And
`struct_order_` is a DFS post-order, dependencies first, so `compute_owning` is a
**single forward pass**: every aggregate is reached after everything it contains,
which makes a member's answer already known and leaves no recursion, no
memoisation and no cycle handling to write. A type on a cycle never enters the
order and never needs to, `order_structs` having already reported it.

The answer is recorded in `Types` beside the folded constants, keyed by `Type_id`
for the caller and by declaration `Node_id` in storage, because drop elaboration
runs on KIR long after the checker has returned. D2's own note that it is *inert
until M3* is now testable rather than asserted: no existing fixture has a
destructor, so `owning_` is empty for all of them and not one of the 74 goldens
moved.

One diagnostic per mistake, which needed two suppressions rather than one. A
struct with a destructor silences both the name and duplicate rules on that
declaration, and silences the ownership pass entirely — `struct Bad { Buffer b;
~Bad() { } };` is one decision to reverse, not three. An owning *field*, though,
is reported once per field, because each is a separate edit.

**Diagnostics render in source order.** They had been rendering in emission
order, which is pass structure — the lexer speaks before the resolver speaks
before the checker — and the driver runs all three before rendering once, so a
file with several kinds of mistake came out shuffled. `errors_literals` was the
clearest case: four range errors on consecutive lines, rendered `7, 4, 5, 6`,
because one of them was the lexer's and three were the checker's.

The question worth asking first was whether emission order was *telling* the
reader something — cause before consequence is worth preserving even when it
reads out of order. It is not, and the reason is a property the code already
has: **error-type absorption exists so that every diagnostic is an independent
mistake** rather than a consequence of one above it. A diagnostic that were only
meaningful because of an earlier one is exactly what absorption suppresses. With
no causal chain to preserve, emission order is an implementation detail the
reader should not have to know, and source order is strictly better. The sort
carries that reasoning in a comment, because it stops being correct the moment
Keel starts emitting consequence diagnostics deliberately.

Two mechanical notes. It sorts a local index vector rather than `items_`, so
`render` stays `const` — every caller holds a `Diagnostics` by const reference —
and rendering twice gives the same answer. And it is `std::sort` with an explicit
tiebreak on the emission index rather than `std::stable_sort`: same result, but
libstdc++'s `stable_sort` reaches for a deprecated `get_temporary_buffer`, which
the release build's `-Werror` rejects. Saying "at one span, keep emission order"
in the comparator is clearer than leaving it implicit in the algorithm anyway —
at an identical span the pass order really is the causal one.

**`this` exists, and a destructor body sees its fields.** `this` is a keyword, and
the parser turns it into an ordinary `Param_decl` typed `T*` at the front of the
destructor's parameter list — C++ writes it implicitly, Keel writes it down. That
one choice is why nothing downstream needed a concept for it: the resolver
declares it like any parameter, the checker types it from its annotation, and it
will be local 1 when lowering arrives. `this` in expression position is a plain
`Name_expr` carrying the keyword's `Symbol_id`, and because keywords share the
symbol space with identifiers the ordinary lookup finds the parameter. There is no
`this` case anywhere in sema.

The member scope was not where it first looked. Two requirements pull apart: a
parameter must beat a field at lookup, as in C++, which puts the field scope
*below* the destructor's barrier; but `declare`'s shadowing walk stops at that
barrier, so it can never see the fields from inside the body. They cannot both be
one mechanism, so D19's member clause is an **explicit check** against a field-name
set the resolver carries, and the scope exists only for lookup. The fields also go
into it directly rather than through `declare`, because that walk reads a barrier
scope's *names* before noticing it is a barrier — so a field sharing a name with a
top-level declaration would have been reported as a shadow.

**D22 turned out to be unimplemented.** *"`.` reaches through a pointer"* has been
recorded since M1 and `infer_field` required a struct outright, so only `( *p ).x`
ever worked — which is why every fixture is written that way and why nobody
noticed. `this` is a `Buffer*`, so `this.ptr` was the first thing to need it. The
checker now unwraps one level, and `lower_place` takes the pointer's value,
dereferences, then projects — the same shape as unary `*`, which `p.x` is the
implicit form of. No golden moved, since the explicit spelling takes the unchanged
path.

The AST dump needed one adjustment for the same reason. It had assumed a
`Named_type`'s span text is always its name, which the synthetic receiver breaks —
its span is the `~` it stands for. Annotating every `Named_type` would have printed
`"f64"  name=f64` across twenty-one fixtures to fix one synthetic case, so the
annotation appears only when it differs from the span text, which is what that
column is documented to be for: what no span can carry. Doing it surfaced two dump
fixtures building a `Named_type` with `aux = 0` — the very mistake the comment
beside them warns about, since `Symbol_id` 0 is `Keyword::If` rather than "no name".

One bug worth recording because it explains a symptom seen all week: a diagnostic
added for `this` called `Ast::kind` on the result of `declaration_of` without
checking it, so `1 = 2;` aborted the compiler. Catch2 reports a *partial* count on
SIGABRT, so the suite appeared to shrink from 358 cases to 111 rather than
reporting a failure — a red run that looks like a smaller green one.

**Destructors lower.** A destructor becomes an ordinary KIR `Function` whose
`declaration` happens to be a `Destructor_decl`, and nothing in KIR knows what a
destructor is. Almost all of it was free, which is the synthetic-receiver decision
paying out: the constructor's parameter walk turns `this` into local 1 with no
change, `run()` lowers `children[2]` for both kinds, and the return slot is already
`void` because the signature pass recorded it. Three things were needed — the
linear scan in `lower()` asking `is_function_like`, `place_for` learning that a
name bound to a `Field_decl` is `field( deref( receiver ), field )`, and a symbol
to emit under.

That last one is why the receiver is captured from the parameter walk rather than
assumed to be local 1: the assumption is true today and would break silently the
day anything adds a local before the walk runs.

`Spelling::function` branches on the declaration kind rather than gaining a second
method, so the backend can hold a `Function` and ask for its symbol without knowing
which kind produced it. `mangle_destructor` spells `kl_<module>_<Type>__dtor`,
putting the marker where argtypes go — the same non-injectivity §7.5 already
carries, and the same fix when it matters.

The prototype loop was walking the AST's top-level declarations while the
definition loop walked `functions_`, which had been two sources of truth agreeing
by coincidence. A destructor is nested inside its class, so the coincidence ended:
the body was emitted with no prior declaration. Both now read `functions_`.

Proof it is real rather than merely well-formed: `tests/codegen/destructors.kl`
carries three destructor shapes - a field read passed to a call, both spellings of a
field written, and control flow with an early return and a loop - and the suite
compiles its output with `-Werror` and runs it. Emitted as
`void kl__Buffer__dtor( struct kl__Buffer* kl_this_1 )`. The exit code proves only
that the program links and runs today; when drop elaboration lands, the fixture's
`released` flag becoming 1 is what it should assert instead.
**Nothing calls one yet** - that is drop elaboration, which needs the dataflow first.

The KIR dump prints one as `fn ~Buffer`. Its `aux` is the *type's* name, so without the tilde it
reads as a free function of that name - and the tilde goes at the header rather than in `name_of`,
which globals, fields and callees share and where it would be wrong.

**Drops are emitted at scope exit.** The rule is one line — wherever `storage_dead`
ends an owning local, drop it first — and it works because `unwind_to` is the
single place any `storage_dead` comes from, so early `return`, `break`, `continue`
and normal fall-through are covered by construction rather than by four cases. That
is the storage-marker step paying for itself: the hard part was the scope
machinery, and it was already done and already pinned by `tests/kir/storage.kl`.

It is unconditional, which is only correct because **move checking is M4**. Nothing
can be moved yet, so no local is ever `MaybeMoved` and no drop flag is needed. When
that changes the placement becomes an analysis and earns a pass of its own; today
re-deriving it over the finished CFG would be structure invented ahead of the need.

`Statement_kind::Drop` deliberately means **"call this place's own destructor"**
rather than MIR's "destroy this place". The lowerer expands a compound into
per-field drops before emitting any, so `class Wrapper { Buffer inner; }` — owning
through a member with no `~Wrapper` — lowers to `drop _3.inner` rather than a call
to a function that does not exist. The narrower meaning is what makes every actual
free visible in the dump, which is what M3's acceptance is about. Three locals are
deliberately never dropped: parameters (a bare class parameter is a borrow under
D31), the return slot (moved out), and temporaries (`into_temp` does not enter
`scope_locals_`). The last is a real hole, currently benign because it leaks rather
than double-frees, and it closes when M4 gives passing an owner a meaning.

Doing it found a bug that had been there since destructors became members:
`infer_struct_literal` took `ast_.children( decl )` as the field list, so a
destructor counted as a field — demanding an extra initialiser and misaligning
every positional one after it. Nothing caught it because no test built a class with
a destructor from a literal, and that spelling is only reachable at all because
D29's constructor half is deferred.

Two notes on the harness. `run_tests.sh --update` rewrites a suite's `.run` file as
well as its `.expected`, so it will happily bless a wrong exit code — the exit
assertion has to be checked by perturbing it, not by watching the suite pass. And
**ASan cannot run the generated program in this environment**: it loops on
`DEADLYSIGNAL`, the known WSL2 ASLR interaction, and `vm.mmap_rnd_bits` is not
writable. `build/asan` still sanitises `keelc` itself. M3's acceptance names
*valgrind or ASan*, and valgrind runs the generated program clean.

**The golden runner can run fixtures under valgrind**, with `KEEL_VALGRIND=1`. Off
by default because it roughly doubles the suite (4s to 9s) and most fixtures
allocate nothing for it to check. It exists because an exit code cannot see M3's
acceptance: a double free and a leak both still exit 0.

Two things had to be got right, and the first attempt got neither. Judging it by
`--error-exitcode` misses a program killed by a signal, which reports the signal
instead — so a segfault passed. And valgrind's output shared the run log with the
program's own stdout, so it could not be tested for directly. It now writes its own
`--log-file`, which `-q` leaves empty unless there is something to say, and a
non-empty file is the failure. Confirmed by deliberately writing through a null
pointer: silent before, `Invalid write of size 1` after.

**Constructors, and M3's acceptance.** A constructor is a `Constructor_decl` with
`Function_decl`'s arity and a synthesised `this`, so it reuses everything the
destructor built: `is_function_like` puts it in `lower()`'s scan, the member scope
puts the fields in its body, and the signature pass types its parameters. What is
new is the **call**, and it is the one place the shape genuinely differs. A
constructor returns nothing and writes through its receiver, so
`Buffer b = Buffer( 16 );` is not an assignment - it is a void call taking `&b`:

    storage_live _1
    _2 = &_1
    _3 = call Buffer(copy _2, const 4)

Constructing into a temporary and copying would have been simpler and wrong: for
an owning type it makes two where the program said one, and only one is dropped.

Three things fell out that are worth recording. **The receiver capture was keyed
on `Destructor_decl`**, so a constructor's `this` was invalid and every field write
addressed local `0xFFFFFFFF` - caught immediately because the dump prints the id.
The test is now "not a free function" rather than a list of kinds. **`infer_call`
had conflated three things** that a construction pulls apart: what the arguments
are checked against (the constructor), what the call produces (the type, not the
constructor's `void`), and how many leading parameters the author does not supply
(one - the receiver is an ordinary parameter, so failing to skip it would make
every call site owe an extra argument). **`check_aggregate_members` is now table
driven**: a constructor and a destructor obey the same three rules - not on a
struct, name must match, at most one - and differ only in a noun and a `~`, so the
rules are written once over a description of the kind.

D29's temporary exception is closed: a class with a constructor rejects the literal
form, naming `Buffer( ... )` as the fix, and returns the declared type rather than
the error type so the assignment does not cascade a second complaint.

**M3's acceptance now runs.** `tests/codegen/constructors.kl` acquires in a
constructor and releases in a destructor across a plain scope, four loop
iterations, and both exit paths of an early `return`, and exits 42 - which is 33
plus a counter that is zero only if every constructor was matched by exactly one
destructor, plus a field proving the constructor body ran through a parameter that
shadows it. Clean under `KEEL_VALGRIND=1`. The resource is a counter rather than an
allocation because the runtime is deferred (§12), and what that costs is precisely
what valgrind would otherwise be checking - placement is proven, freeing is
simulated.

### M4 — in progress

**`move` reaches KIR.** The first step was not the dataflow: `move x` parsed into a
`Marker_expr` that the checker rejected outright, so nothing in the compiler ever
produced `Operand_kind::Move` and an analysis would have had nothing to read. The
plumbing came first.

It cost two cases. The checker types a `move` as its operand's type and requires
that operand to be a **place** - reusing `is_assignable` rather than writing a
second answer to "is this a place", so the two cannot drift on what counts as one.
The lowerer reads that place with `move()` instead of `copy()`. `ref` and `out`
still report, in their own words, that they wait on D32's binding modes.

Two properties worth recording. `move` is legal on **any** type and in **any**
expression position - D31 makes it an assertion on a struct rather than a transfer,
and governs initialisation and assignment as well as arguments - so neither a
kind check nor a position check belongs here. And because the C backend already
maps `Move` and `Copy` to the same text (a move *is* a byte copy there),
**nothing observable changed**: all 79 goldens passed untouched, which is the whole
test that this step did what it claimed and no more.

**A marker binds to the whole argument, not as a unary operator.** It had been
parsed at unary power, so `f( move a + 1 )` was `f( ( move a ) + 1 )` - and the
dump made the consequence plain:

    _2 = move _1 + const 1
    _3 = call f(copy _2)

The argument reaches the callee **by copy**, and the `move` at the call site
describes a subexpression consumed before the call happens. That defeats the one
thing D2 exists to guarantee: *a reader of the call site should not have to find
the declaration to learn that a variable just died.* `ref` and `out` make it
plainer still - a mutable borrow of a sum is not a reading of anything.

The cause is an asymmetry with D28: `cast<T>( x )` has mandatory parentheses and
cannot have this problem, while D2 chose `move x` over `move( x )` because the
latter reads like a call. The fix is to parse the operand at the lowest power
rather than unary power, so the marker takes the expression to its boundary. The
surprising form then hits the place check that already exists - *only a variable or
a field can be moved*, with the caret on `a + 1` - and no new rule is needed.
Everything useful still parses, because postfix and prefix both bind tighter:
`move a.b.c` and `move *p` take the whole place. `( move a ) + 1` remains available
for anyone who means it, and now has to be written down.

The framing that makes this feel right rather than arbitrary: **a marker is not an
operator, it is an annotation on a whole argument.** Unary precedence was the
accident.

**`move` applies only to a plain named variable or parameter.** A field on its own -
`move w.inner` - is refused, and not merely because tracking it is hard. The object
still has a scope exit, and drop elaboration emits a drop for its owning fields, so
a partly-moved object would free a field that has already gone. Making that correct
needs **per-field drop flags**, which is the move-path machinery Rust's MIR carries.
So this is a case the compiler currently *cannot* handle rather than one it declines
to track, and refusing it is a restriction - liftable later without breaking a single
program that compiles today. `move *p` goes the same way and for the same reason: it
names something this function does not own. The payoff is that the language rule and
the analysis agree **by construction** rather than by approximation, because the
dataflow tracks whole locals and nothing else can be moved.

The alternatives were both worse. Marking the root local moved is conservative and
turns `move q.x; use q.y` into a false error, which blocks valid programs; ignoring
projected moves silently misses `move q.x; use q.x`. A restriction is the only
option that is neither wrong nor lenient.

After this, `move a` compiles, lowers, and **means nothing** - `a` is still
readable. That is the point: it gives the dataflow something real to read before
the dataflow exists.

**The move dataflow.** `check/move_check.cpp`, §8's lattice over the KIR CFG, and
the first thing in that directory. Its header is two names - `Move_error` and
`check_moves( const Function& )` - and everything else is file-local: `verify()`'s
shape, and the reason it can be tested by handing it a function and nothing else.

Four things about it are load-bearing rather than incidental.

**It reports on a second walk, not during the fixpoint.** A block on a back edge is
transferred several times; reporting inside that loop would emit one error per
visit. Both walks share `transfer_block`, distinguished only by whether the
`errors` pointer is null, so they cannot drift.

**`Uninitialised` is the bottom, not a fourth peer.** Joining it with anything
yields the other side, which is what lets every block start empty and be filled by
its predecessors - and is what makes the worklist converge from below.

**The worklist pushes into successors rather than pulling from predecessors**, so no
predecessor map is needed at all. Termination is four states, a join that never
moves a local back down the lattice, and finitely many blocks.

**Span joins are deterministic.** Two paths can agree a local is moved and disagree
about where; picking by worklist order would make the message depend on scheduling.
It takes the earlier span.

Two of the tests were wrong in ways worth keeping. `a = move a + 1` is now
`move ( a + 1 )` after the precedence fix, so the self-assignment case has to be
written `( move a ) + 1` - the same trap the precedence change was made to expose,
found in my own test. And a loop containing both a move and a later read reports
**two** errors, not one: the read sees what the move killed, and the move sees what
the back edge left moved. Both are real, and the property worth pinning is that
neither site is reported twice, which the test now checks by comparing spans rather
than counting.

**Operand spans: not yet, and here is the trigger.** KIR operands carry no span, so
every argument of a call reports at the statement. Measured rather than guessed:
`two( a, b )` after `two( move a, move b )` gives two errors whose `use` and `moved`
spans are *identical*, leaving the variable's name in the message as the only thing
telling them apart. That is imprecise rather than wrong. Adding `Span` to `Operand`
would fix it in one place - `Rvalue::a`, `Rvalue::b`, `Terminator::condition` and
`Function::operands` are all `Operand`, so no other struct changes - but it costs 12
bytes there and 24 on every `Statement`, which kir.h already flags as 100 bytes with
a TODO to move rvalues to a side table. **Do it when that refactor happens**: the
layout is being touched anyway, and 72 bytes leave as 12 arrive.

**Use-after-move is a compile error.** Half of M4's acceptance, and the wiring
turned up a structural problem worth fixing on the way: `lower()` was being called
**twice**, once for `--dump-kir` and once for the emitter - two sources of truth for
the same lowering, agreeing by coincidence. It is hoisted, and the move check,
`--dump-kir` and the emitter now read the same functions.

**The check runs above the `--check` early return**, which is the one real decision
here. Move checking is part of the front end rather than of emission: `--check` is
what an editor wants, and an editor wants use-after-move underlined. The cost is
that `--dump-kir` now refuses a program with a move error, which is consistent - it
is a compile error like any other - but does remove the ability to dump the KIR of a
program you are debugging the move checker against.

`Diagnostics` carries one span and a help string, not a second underlined snippet,
so the move site renders as a `line:col` in the help - the shape the resolver's
previous-declaration note already uses. And the errors need no sorting of their own:
`check_moves` returns them in block order, which is not source order, but diagnostics
are sorted by span at render time. That sort earning its keep in a case it was not
written for is a good sign it was the right change.

The two messages are deliberately different, because `Maybe_moved` exists to say the
compiler is refusing an ambiguity rather than reporting a certainty:

    error: `a` is used after it was moved
     --> t.kl:7:5
      |
    7 |     sink( a );
      |     ^^^^^^^^^ moved at 6:5

    error: `a` may already have been moved
       |     sink( a );
       |     ^^^^^^^^^ moved at 10:9 on some path to here

`tests/sema/errors_moves.kl` covers five failing shapes and **three passing ones** -
the arm that did not move, a local assigned again after being moved, and
`a = ( move a ) + 1`. Without those the fixture would not show the analysis is doing
anything more than refusing every program that says `move`. The subtlest case comes
out right on its own: a move inside a loop body is reported as a *maybe*, because the
header joins "first entry, live" with "back edge, moved" - correct, since the first
iteration is fine and later ones are not.

**Drop flags.** The other half of M4's acceptance, and smaller than expected once
one thing was clear: **a drop flag needs no dataflow.** It is a runtime value - false
while storage is empty, true once the local holds something, false again once moved -
so it records what *happened* rather than what statically might have, and is exact on
every path by construction. `move_check`'s lattice answers a different question, for
diagnostics. Rust uses dataflow here only to *eliminate* flags it can prove
unnecessary, which §7 explicitly says not to attempt first.

**The flag rides on the `Drop` statement rather than becoming control flow.** MIR
splits the block and branches; that means new blocks, reachability implications, and a
CFG that stops matching the source. A `Local_id drop_flag` on the statement is four
bytes and leaves the graph alone - the backend emits the `if`, which it was going to
have to do either way. Consistent with §3.1 already making drops statements rather
than terminators, since there is no unwinding to make them diverge.

Three things came out of building it.

**A constructed local is never an `Assign` target**, so keying "now initialised" off
assignment left the flag false and the drop unreachable. `Owned o = Owned( 1 );` lowers
to `_3 = &_2` and a call writing through `_3`. Taking a local's address is therefore
what marks it live. The looseness is deliberate and bounded: `Owned o; Owned* p = &o;`
marks an uninitialised local as needing a drop, which is exactly what happens today
without flags, so it is no worse than the status quo for a program D9 will reject.

**A flag guards a drop, so a local with no drop needs none.** Flagging on "was moved"
alone gave every `move` of an `i32` a bool and four assignments guarding nothing -
caught because `kir/calls.kl` moved when it had no business to. The set is moved *and*
dropped.

**`for_each_operand` moved into `kir.h`.** It was about to be written a second time,
having already been inlined into `move_check`'s `read_rvalue`. Where operands live is
vocabulary, not policy, and this is the third two-sources-of-truth of the milestone
after `lower()` being called twice and the prototype loop walking the AST.

`tests/codegen/drop_flags.kl` runs the same function down both paths. Kept: the flag is
still set, the destructor runs, `live` returns to 0. Moved: the flag is clear, the
caller does not drop - and `live` stays 1, because ownership went to the callee and a
callee does not yet drop its parameter. That 1 is the D2 debt, asserted rather than
worked around, so the fixture's answer will change to 0 by itself the day that lands.

**M4's acceptance is met.** D31's `move` parameter closes it, and the two halves were
inseparable: making a callee drop its parameter *without* modes would double-free
every bare pass, because the caller drops too. So the mode had to come first, and it
brings D2's enforcement with it - a bare argument is a borrow, so there is nothing to
enforce, and a transfer cannot be silent because the signature has to say so as well.

`Ref_type` became **`Mode_type`** rather than a new node kind: the same shape - a node
in type position wrapping a type - generalised from one keyword to three, with the
keyword in `aux`. That mirrors `Marker_expr` at the call site exactly, one node on
each side of the call carrying the same three keywords. It is unwrapped in
`type_of_annotation` and nowhere else, so the declaration records the underlying type
and nothing downstream meets the wrapper.

**The agreement rule applies only to owning types.** Requiring it everywhere made
`sink( move a )` on an `i32` an error - caught immediately by three existing fixtures.
For a non-owning type a bare argument is a copy and `move` is the caller's own
assertion that the source is dead afterwards (D31), which the callee neither sees nor
cares about. Demanding the two agree would have made the marker mean something
different depending on the type, which is exactly what D31 was written to avoid.

**An owned parameter is dropped by a scope enclosing the body.** Not by the
constructor's parameter walk, which was the obvious place and is wrong: `scope_marks_`
is empty until a scope is pushed, so the body's own `push_scope()` would record its
mark *above* the parameters and `return`'s `unwind_to( 0 )` would never reach them.
`run()` pushes a scope first, puts the owned parameters in it, and pops it after the
body - which also covers the fall-off-the-end path, since `unwind_to` is a no-op on a
block a `return` already terminated.

`tests/codegen/drop_flags.kl` now exits **0**, and its `.run` file is gone - the runner
takes an absent one as zero. Freed exactly once on both paths: by the caller when the
move did not happen, by the callee when it did. Clean under `KEEL_VALGRIND=1`.

Two diagnostics improved on the way. D32 takes `&` out of type position, and the
adjacency check was firing first - so `i32 &r` gave two errors for one mistake, the
first of them advice about how to space a spelling that no longer exists. And neither
`u32 &r;` nor `u32 & r;` reaches the type parser at all, because L17 scans them as a
bitwise and; the D17 path that already rescued `u32 *p;` from the generic
"no effect" message now covers `&` too, so all three spacings name `ref T`.

**An owning value is never copied.** Chasing the temporary leak turned up a
**double free** that had been shipping: `B make() { B b = ...; return b; }` freed
twice, because `_0 = copy _1` copied the resource out and then the callee's scope
exit dropped `_1`. Once one of these is found the rest follow from the same
sentence, and there were four:

- **`return`** transfers out, so the value is read as a move. §8 exempts `return`
  from a written marker because there is no later use for one to warn about.
- **An initialiser and an assignment** do too, where the source is a temporary -
  which is the common case, since D31 requires a written `move` from a named one.
- **A struct literal's fields**: `Wrapper { Buffer { ... } }` copied the inner
  temporary into the field, leaving both holding one resource and both dropping it.
- **A `move` parameter** takes its argument by move whatever the syntax said, so KIR
  records what happens rather than what was written.

The general rule, and the one to reach for when the next case appears: **any read of
an owning value that transfers ownership is a move, and the lowerer is what knows
which reads those are.**

Three consequential fixes came with it.

**A projected assignment initialises the whole local.** Nothing can be partially
moved, so `_2.p = x` is a step in *building* `_2`. Both passes had been ignoring
projected targets, which meant a struct literal - assembled entirely through
projections - was never `Live`, so in a loop its temporary reported a false
use-after-move on the second iteration and its drop flag never came back on.

**A projected *drop* counts as dropping the local.** A compound with no destructor
of its own has its drop expanded into per-field drops, so `drop _8.inner` is how
`_8` is dropped. Requiring an unprojected drop left such a local unflagged, and
moving it out then freed the field twice.

**A temporary can be moved, so a move error need not name anything the author
wrote.** `report_move_errors` asserted otherwise and aborted the compiler; it now
says "this value" when the local is unnamed.

`consume( move Buffer( 16 ) )` also needed the lowerer to materialise the temporary
before moving it - `Marker_expr` was calling `lower_place` on a construction, which
is not a place.

Every fixture's exit code is unchanged; only `codegen/destructors.kl`'s emitted C
moved, gaining the flags and drops that make it correct rather than accidentally
balanced. Clean under `KEEL_VALGRIND=1`.

**D31's initialisation and assignment clause is enforced.** `Buffer b2 = b1;` from a
named owning local is now an error naming `move b1`, in an initialiser, an `auto`
initialiser and an assignment alike. It was the last place a transfer could happen
without being written down - the lowerer moved it anyway, so it freed exactly once,
but silently, and a silent transfer is the one thing D2 exists to prevent.

The rule needs no new machinery: `is_assignable` is already exactly the "names a
place" test, which is what separates a named source from a temporary. A temporary
needs no marker because it has no other owner and leaves no variable behind for a
reader to wonder about - the same reasoning that lets `return` go unmarked.

It needed its own fixture rather than a section of `errors_moves.kl`, and the reason
is worth recording: these are **type** errors, so the driver stops before lowering
when one is reported, and the move analysis never runs. Adding them to that file
silently deleted its five move diagnostics from the golden. One fixture per pass,
where a pass can gate the one after it.

**D32's `ref` arrives as a parameter mode.** `void bump( ref i32 n )`, called as
`bump( ref n )`, and inside the body `n` is an ordinary `i32` in every position -
`n = n + 1`, `n.field`, `&n`. A class travels the same way, which is what finally
makes one passable without giving it away: with copy constructors outside §6.6, a
borrow was the only meaning available and there was no spelling for it.

The decision that shaped the implementation: **the declaration records `T`, not
`T*`.** D32 forces it - a mode is not a type, so `ref i32 n` has to support
`n = n + 1` rather than `*n = *n + 1`. The pointer exists only below sema.

That left lowering needing a `T*` id it cannot make: `Types::table()` hands out a
`const Type_table&` and `pointer_to` interns. So the checker records the address
type **on the `Mode_type` annotation node**, which nothing else types - one free
slot with exactly one meaning, *what this binding travels as*. `parameter_type()`
is the single reader, shared by lowering and by the emitter's `parameter_types_vector`,
because the local, the prototype and the mangled name must all agree or C sees a
definition that does not match its own declaration.

Below sema nothing knows the mode exists. The local is a pointer, `place_for`
derefs it, and that one line is what makes every use work at once - reads, writes,
fields, and forwarding, since `lower_place` is where all four already met. It is
the shape the receiver has had since D22, so **D32's open question answers itself**:
`this` and a `ref` parameter are now the same mechanism, and making the receiver
`ref T` would be a spelling change rather than a design one. verify, the move
analysis and drop elaboration needed nothing - a borrow's local is a pointer, so it
is not owning, so it is never dropped.

Two rules the agreement check needed. It had been exempting any disagreement on a
non-owning type, which is right for `move` - the caller's own assertion that the
source is dead, which the callee never sees - and wrong for `ref`, which changes
what the callee is holding whatever the type is. And a `ref` argument is **not
converted**: a binding is the caller's object itself, so there is no conversion step
for a widened copy to live in, and `bump( ref my_u8 )` against `ref i32` would
otherwise have the callee writing 32 bits through an 8-bit place. That check has to
sit *above* the agreement test rather than below it, because both sides agreeing is
its precondition, not its exit - the first version sat below and could never fire.

`is_assignable` is the test for what may be lent, deliberately the same one that
says what may be assigned, so the two cannot drift. It is looser than `move`'s:
a field and a pointee can both be borrowed, because unlike a move a borrow leaves
nothing partly gone.

`out` is still refused. It needs definite assignment in the callee, which is §8's
lattice read from the `Uninitialised` end rather than the `Moved` one - a second
analysis rather than a second keyword.

No existing golden moved, which is the claim worth keeping: `ref` added a spelling
and took nothing away.

**Noted for whenever overloading arrives:** `void f( ref i32 n )` and
`void f( i32* n )` mangle identically, both to `i32p`. Harmless in v0, where a
second `f` is a redeclaration caught by name long before mangling matters, but the
mangler will need to distinguish a borrow from a pointer the day two functions may
share a name.

**A bare parameter of an owning type is a read-only borrow.** D31's last unimplemented
row, and it was not a missing feature but a **double free that had been shipping**:

    B pass( B b ) { return b; }

freed one resource twice. The KIR said why - `_5 = call pass(copy _1)`, a struct copy
of an owning value, which is the one thing *an owning value is never copied* forbids.
Two `B`s then held one resource and both were dropped. `void f( Buffer b ) { b.len = 5; }`
was accepted for the same reason.

`ref` had already built the by-address path, so most of this was wiring it to a second
predicate. What it needed of its own was the **read-only** half.

Three things worth keeping.

**The rule is a pass of its own** because `is_owning_type` only answers after
`compute_owning`, and `declare_signatures_member_functions` runs *before* it - so a
constructor's parameters would never have been borrows. `record_borrowed_parameters`
runs last and establishes one invariant: **the annotation node carries a type exactly
when the parameter travels by address.** `parameter_type` is then the only reader and
no longer asks about modes at all.

**A borrow is *bare* and owning.** The first version of the predicate asked only
`!is_ref_parameter && is_owning_type`, which classifies a `move` parameter as a
borrow - it would have travelled by address and never been dropped by the callee.
`drop_flags_guards_a_conditionally_moved_local` caught it. Both the pass and
`is_borrow_binding` now test `parameter_mode() == Keyword::Count`: `ref` is the
mutable borrow, `move` is not a borrow at all, and bare-and-owning is the read-only one.

**`is_assignable` was left alone.** It answers *is this a place*, which a borrow is,
and `check_owning_source` and `infer_marker` depend on that reading. *May I write here*
is a second question with its own walk: `place_root` follows a field path to the
variable it roots in and **stops at a pointer**, because what a pointer points at was
never part of the object that was lent. That is C++'s shallow `const`, arrived at from
ownership rather than inherited.

The refusals are four, and they are the four ways to give away what you were lent:
write to it, move it, lend it mutably, return it. Returning is gated on the *return
type* owning something - reading a value out of a borrow and returning that copies
nothing anyone else holds. The write check runs **after** the target is inferred,
because `place_root` has to ask whether a field's object is a pointer and nothing has
typed it before then.

`u64 forward( B b ) { return peek( b ); }` needs no marker and no copy: the address is
simply forwarded, and lowering emits `&(*_1)`. No existing golden moved.

**Correcting an earlier entry:** the debt below used to record that an owning temporary
still leaks in `f( B( 1 ) )`. It does not - `statement_temporaries_` emits the drop, and
`tests/codegen/borrows.kl` exits 43, whose second digit is three destructor runs for
three constructed objects. That entry has been narrowed to what is actually still true,
and the *bare parameter is not yet a borrow* debt beside it is now paid and removed.

**D32's `ref` reaches its second position: local bindings.** `ref i32 r = x;`, and
inside the body `r` is an ordinary `i32` - the same split the parameter already had, so
the checker and the lowerer needed one predicate each rather than a second mechanism.
`is_borrowed_parameter`/`parameter_type` became `is_borrowed_binding`/`binding_type`,
because the invariant they read - *the annotation carries the address type exactly when
this is a borrow* - was never about parameters. A `Var_decl`'s children are
`{ type, init }`, so the annotation is `children[0]` there too and the same pair works
unchanged.

**It needs no analysis to be safe, and that is the whole design.** A binding is
initialised at its declaration and never reseated, so its referent was declared *before*
it - same scope or an enclosing one - and therefore outlives it. That is §8's
no-lifetimes argument applied to locals, and it holds by construction rather than by
checking. The single escape is binding to a temporary, and one rule refuses it:
`is_assignable`, the same "names a place" test a `ref` argument already uses, so the two
cannot drift apart.

Four rules in `visit_var`: it must be initialised, it must bind to a place, the type
must match exactly (a binding is the variable itself, so there is no conversion step for
a widened copy to live in), and it may not rebind something held read-only.

Three things this cost that were not obvious.

**Binding is not a transfer.** `check_owning_source` fired on `ref B r = a;` and demanded
`move a`. Nothing is copied and nothing is given away - which is what a borrow *is* - so
the mode has to be consulted before D31's question is asked at all.

**A mode keyword does not, by itself, start a declaration.** `move i32 x = 1;` should be
a declaration so sema can say *`move` is not a binding mode*; `move b;` should stay an
expression so D15 can say *this statement has no effect*. Deciding on the first token
gets one of them wrong whichever way it is written, and the first attempt turned `move b;`
into a parse error about a missing identifier. `looks_like_binding` scans for a type and
a name and restores the cursor, which is what `looks_like_declaration` beside it has
always done.

**`const` is inert everywhere in Keel.** `const i32 x = 1; x = 2;` compiles clean today,
which is why `const ref` is not in this step: it would be the first place `const` meant
anything, and that is a language decision rather than a wiring job. It is also the real
prerequisite for `const ref` returns.

**`const` is enforced, and `const ref` is the fifth argument form.** The model is
D32's shape applied to constness: **`ref` is a binding mode, `const` is a binding
property.** Both are about the name rather than the data, which is what let the whole
thing reuse the read-only machinery the borrow rule already had instead of putting
constness into the type.

A declaration is `const` when the **outermost** node of its annotation is `Const_type`.
`const i32 x` and `i32* const p` both are, with C++'s meaning intact - reseating a
const pointer is refused, writing through it is not. A `Const_type` that is *not*
outermost is a pointer to const, `const i32* p`, which is a promise about data nobody
named here; it is refused rather than quietly given the other meaning, because silently
reinterpreting valid C++ is the one divergence §5.1 forbids.

**What the binding model gives up, and why that was affordable.** For one value,
`const ref T` replaces `const T*` entirely - the same trade D31 already made when it
deleted `const T&`. What has no replacement is a read-only *buffer* (`const u8* data`),
which needs the many-item pointer D27 defers past v0, and a nullable read-only pointer,
which needs M5's `Optional`. Both gaps land on features already deferred, which is what
made the decision cheap. **`[*]T` is the trigger to revisit it**: it is the first
feature that genuinely needs pointer-to-const, and the moment const either moves into
the type or read-only buffers get some other spelling.

`check_not_borrowed` became `check_writable`, with two reasons to refuse and a message
for each. Three of its four callers then did new work with nothing written for them:
`ref x` at a call site, `ref T r = x` as a binding, and `x++` all refuse a const now.

**`const ref` cost almost nothing on top.** Both halves already existed - it travels by
address because `is_borrowed_binding` says so, and it is unwritable because
`is_const_binding` says so - and the step was putting them in contact. KIR shows
`const ref` and `ref` as one shape, which is the point: a read-only borrow is not a
third calling convention.

Two things it did need. The annotation is `Const_type( Mode_type( T ) )`, so every
question about a mode had to see through a wrapper - `unwrap_const`, added *before* the
parser learned the spelling, because otherwise a `const ref` parameter would silently
have become a by-value one with no diagnostic anywhere. And the agreement rule had to
stop asking about modes and start asking about **mutation**, which is what D31's
amendment above records.

`ref const T` is a hard error naming `const ref T`. One order, where C++ has two
spellings meaning the same thing - the redundancy D25, D30 and D32 all refuse. Without
it the wrong order parses as an inner const and gets reported as a pointer to const:
the right refusal for the wrong reason.

**A note on acceptance fixtures:** `tests/codegen/const_ref.kl` first encoded its answer
as `total * 10 + freed` = 261, and an exit code is a byte - 261 & 0xFF is 5, which is a
number the fixture could plausibly have meant. Encodings there have to fit in 255 with
the halves in ranges that cannot carry into each other.

### Debts to pay along the way

- **A pointer to const has no spelling, and `[*]T` is when that stops being affordable.** `const`
  binds the name, so `const i32* p` is refused. For one value `const ref T` covers it; for a
  read-only *buffer* nothing does, and the many-item pointer is the first feature that will need
  one. That is the moment to decide whether `const` moves into the type or read-only buffers get
  another spelling - and it is worth noticing then rather than discovering it under deadline.


- **`consume( Buffer( 16 ) )` is still not expressible.** `move` requires a plain named variable,
  so a temporary cannot reach a `move` parameter without a `move` marker written on the
  construction itself. The leak this entry used to record is gone - a temporary handed to a borrow
  is dropped after the statement - but the restriction is worth lifting, and needs the
  temporary-lifetime rule §8 already owes for `const ref` returns. One answer serves both.

- **D2 stopped being inert at M3, and is not enforced.** Its entry says *"inert until M3: no type
  has a destructor yet, so nothing is owning and nothing changes"*. Destructors now exist, so that
  sentence has expired. Two defects follow, and they are one expression and one fix:

  ```keel
  void consume( Owned o ) { }
  consume( Owned( 1 ) );          // live == 1 at exit: the destructor never runs
  ```

  **The temporary is never dropped.** `Owned( 1 )` constructs into a local that is not in
  `scope_locals_`, so nothing ends its storage and `drop_place` never sees it. C++'s answer is
  destruction at the end of the full expression; Keel has no rule yet.

  **And the argument is a bare by-value pass of an owning type**, which D2 says is an error
  requiring `move`. It currently copies.

  Fixing the leak *alone* would be worse than leaving it: if the temporary were destroyed at
  end-of-statement while the callee's copy were also dropped, a leak becomes a double free. It is
  benign today only because parameters are not dropped either — every path errs toward not freeing,
  which is the safe direction to be wrong in. Both belong to **M4**, whose row is exactly this work,
  and which the milestone table already calls *"where we learn whether the ownership model is
  real"*.

- **D29 is only half implemented at M3, deliberately.** The entry specifies four
  differences between a `struct` and a `class`; M3's acceptance test — a `Buffer`
  freeing exactly once, including on early `return` — needs two of them. A `class`
  may have a destructor and a `struct` may not, and a `struct` may not contain an
  owning member: both are drop machinery, and the second *is* D2's owning query,
  which drop elaboration needs regardless. The other two defer, and each leaves a
  gap worth naming rather than discovering later.

  **Constructors — no longer deferred.** They are being taken before M3 closes,
  scoped to what M3 needs: a parameter list, a body that runs at initialisation,
  and fields set from it. Nothing more. The argument that settled it is that the
  *acquisition* half of RAII is only ever a statement in the body, so proving the
  body runs proves the mechanism — that the statement cannot yet call an allocator
  is the runtime's gap, not the constructor's. Until they land, a `class` has no
  other way to be built, so the struct literal works on one: a temporary and
  explicit exception to D29, and already the cause of one bug, where
  `infer_struct_literal` counted a destructor as a field. Closing it means deciding
  the call syntax and making the literal form an error on a type that has a
  constructor.

  **Access control.** Fields are private by default in a `class`, and that needs
  member lookup to carry visibility — a resolver feature, with nothing in M3
  depending on it. Until then a class's fields are public in effect, and
  `struct` versus `class` is distinguishable by what it may *contain* rather than
  by what may reach into it.

  Neither gap is load-bearing for M3, and both are cheap to add once the drop
  machinery is proven. Recording them here keeps §6.3's audit honest: D29 as
  written is not yet what the compiler enforces.

- **Three files have grown past what one file should hold.** Code lines, excluding the in-source
  tests that roughly double each: `sema/type_checker.cpp` 2350, `parse/parser.cpp` 1675,
  `lex/lexer.cpp` 824. `ir/lower.cpp` at 768 is the one to watch.

  The mechanism is identified in §3.2 and is not sprawl: each is built around a **file-local class**
  — `Checker`, `Parser` — and a class cannot span translation units, so every new responsibility
  becomes another private method in the same file. The tests then double it.

  `codegen_c/emitter.cpp` was a fourth at 1247 and is gone: KIR replaced it with `emit_kir.cpp` at
  540 and `spelling.cpp` at 207, because the lowerer had already done the conversions, the
  short-circuits, the loop labels and the temporaries. That is evidence for the §3.2 discipline
  rather than proof of it — the win came from moving work earlier in the pipeline as much as from
  how the files are laid out.

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
