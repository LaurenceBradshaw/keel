# Keel — Implementation Plan

**Status (2026-10-07):** M0 to M8 are complete. **M9, error handling, is next**, opened by `print`;
then M9.1 (borrows), M9.5 (interfaces), M9.6 (the rest of the operators), M10 (compiler flags) and
M11 (KIR passes). §9 has the table and §15 the open work.

This file is the plan and the rules as they stand. **How each was reached** - the work log, the
measurements, superseded decisions and the text they replaced - is in `.claude/LOG.md`, which git
does not track. A rule here that cites a date can be looked up there.

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
| Standard library package | `kl` |

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
directory and keep KIR clean enough that an LLVM backend could consume it.

**LLVM is the intended backend, and the division of labour is decided (2026-09-28).** It is not
scheduled, but it is decided, because the answer changes what `ir/simplify.cpp` and M11 are for.
**keelc drives the linker itself by then at the latest**; until then `cc` links, and keelc supplies
the runtime.

**The rule: if an optimisation is available at the IR level, Keel performs it. LLVM is used for
machine-level work only.** Instruction selection, register allocation, machine scheduling, branch
layout, addressing-mode folding, calling-convention lowering - all of that is work Keel has no
reason to own and no ability to do better. Everything above it is Keel's, in KIR, where the
ownership and borrow invariants are still visible. A backend that optimises for us is a backend that
hides which of our own transformations are worth writing.

**The `-O` level is two knobs rather than one.** LLVM's command-line `-O` conflates the mid-level IR
pipeline with the codegen optimisation level; the API keeps them apart, and so should we.

- **Mid-level pipeline: empty.** `PassBuilder` with no passes added, plus the handful that are about
  legality rather than speed: `AlwaysInliner` if `alwaysinline` is ever emitted, `AtomicExpand`,
  `PreISelIntrinsicLowering`.
- **Codegen: `CodeGenOptLevel::Aggressive`.** Through `llc` that is `llc -O3` applied to
  unoptimised IR, which is the combination this rule describes and the one to test against.
- **Not `clang -O0`.** That sets both knobs to zero, which costs the greedy register allocator, the
  machine scheduler and machine block placement - machine-level work, given up for nothing.

**The rule is "no IR pass whose purpose is a speed-up KIR could have achieved", not "no IR pass
runs".** `CodeGenPrepare` is an IR-level pass that runs at any codegen level above `None`; its work
is target-driven addressing-mode and branch-shape rewriting, so it belongs on the machine side by
intent.

**Three boundary cases.** **Auto-vectorisation is LLVM's**, because the transformation depends on
the target's vector width, which KIR cannot know. **Tail-call elimination is Keel's**, because
turning self-recursion into a loop has nothing target-specific in it; the backend's sibling-call
optimisation stays the backend's. **Anything gated on undefined behaviour is nobody's**: no `nsw`,
`nuw`, `noalias`, `dereferenceable` or `!tbaa` is emitted unless Keel can prove the claim, because
those are permissions handed to an optimiser we have declined to run.

**What the rule obliges Keel to write**: promotion of non-address-taken locals to SSA values (the
big one - LLVM assumes SROA/mem2reg has run); inlining with a cost model; CSE and GVN; constant and
copy propagation; dead code and dead store elimination; LICM; loop unrolling; induction-variable
simplification and strength reduction. **And the ones LLVM could never do**, which are the reason
the rule is worth keeping: drop elimination, move elision, drop-flag elimination, redundant-borrow
removal (the `&(*x)` a borrow-returning call produces is the first entry), devirtualisation once
dynamic dispatch lands, and bounds-check elimination. M11 is the first of this list.

**The C backend cannot express the split.** The golden runner passes no `-O`, which happens to
match this rule, but a release build through C cannot ask for the machine half only, since
`cc -O2` is both knobs at once and C's undefined behaviour is a wider surface than LLVM IR's. That
is the clearest single argument for the eventual move.

### 2.3 Compiler implementation language: C++20

A compiler is overwhelmingly data structures: hash maps, dynamic arrays, string
interning, trees, worklists. Writing those from scratch in C costs months and
buys nothing.

We use C++20 in a **deliberately restrained subset**:

- No exceptions. No RTTI.
- No inheritance and no virtual functions **in this compiler**. This is a style rule about
  compilers, not about Keel: **Keel will have dynamic dispatch** through interfaces (M9.5), and
  refuses implementation inheritance.
- No templates beyond using the standard containers.
- Standard containers, `string_view`, `optional`, `span` — yes.
- Style is "C with containers and namespaces."
- **One class per file, and a class's members are defined in one file.** C++ lets a class's member
  definitions span translation units; Keel has no form for that, so a compiler written that way
  could not be translated when it compiles itself. When a class is too large, split the class, not
  the file (§3.2).
- **No operators on an enum.** Keel has no free operators (D33), no enum-to-integer conversion
  (D30) and no methods on an enum. A set of `Node_kind`s is a named predicate such as
  `is_function_like`, not `A | B`.

Two reasons for the restraint: it is the correct style for compilers, and every
line here will eventually be rewritten in Keel during bootstrap. Do not lean on
features Keel does not have — which for most of this list means *will never have*,
and for dynamic dispatch means *does not have yet*.

---

## 3. Architecture

```
  .kl source
      |
      v
  Loader           lexes and parses each file a program imports into one AST,
                   then the prelude after it; records the import graph
      |
      v
  AST              flat vectors, u32 handles, every node has a Span
      |
      v
  Resolver         names -> declarations; scopes, packages, import visibility
      |
      v
  Type checker     bidirectional; types every node into side tables, records
                   callees, constants and the instantiations generics need
      |
      v
  KIR lowering     -> basic blocks, three-address form, explicit temps;
                   monomorphises by worklist from the checker's instantiations
      |
      v
  simplify         threads empty blocks, folds constant branches
      |
      v
  Move check,      dataflow over the KIR CFG: use after move, definite
  definite         assignment (D9), and a call that moves and borrows one local
  assignment
      |
      v
  Drop             inserts drops and drop flags
  elaboration
      |
      v
  C emitter        the program's C, and the prelude's C beside it (D46)
      |
      v
  cc               links the generated .c against libkeel_rt.a
```

`--check` stops after drop elaboration, so an editor gets every diagnostic the compiler can give.
`verify` checks each function's KIR wherever it is dumped.

### Why KIR exists

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
assignment apart and would obscure exactly the thing being tracked. If an LLVM backend wants SSA,
Keel promotes locals itself (§2.2).

Two simplifications Keel gets that Rust does not, both taken deliberately:

- **No unwinding** (L14). In MIR every call is a *terminator* carrying an unwind
  edge. With no exceptions a call is an ordinary statement, blocks stay long and
  straight, and cleanup-on-unwind does not exist.
- **No lifetimes** (§8). The structural reference rule means no region
  inference, so a place needs no provenance and the dataflow is a bitset per
  local.

The vocabulary, in `ir/kir.h`:

```
Place      a local or a global, plus a projection path    x, p.y, (*q).z, the tag of an enum
Operand    Copy(Place) | Move(Place) | Constant
Rvalue     Use | Binary | Unary | Cast | Call | Address_of | Allocate | Release
           | Function_address | Indirect_call | Field_offset | Field_read | Literal_bytes
Statement  Assign(Place, Rvalue) | Drop(Place, flag) | Storage_live | Storage_dead
Terminator Goto | Branch(Operand, Block, Block) | Return | Unreachable | Panic
Function   locals[] + blocks[];  Block = statements[] + exactly one terminator
```

Every statement and terminator carries a `Span`, and every local a `Type_id`. Block 0 is the entry,
local 0 the return slot, and locals 1 to n the parameters. `Address_of` says why the address is
taken, `Borrow` or `Initialise` (a constructor's target, an `out` argument, a string literal's
`str`), and the three analyses read that rather than guessing. A `switch` lowers to branches on a
tag; there is no switch terminator. `Panic` ends the program with a message, for a failed
`assert`, a narrowing `cast` that does not fit (D28, D48) or a `panic`, whose message is an operand.
`Unreachable` follows a call of a `never` function.

### 3.2 How the code is organised

**One class, one file, and split by state ownership.** Before splitting a class, measure which
members touch which fields; a class's neighbourhoods are its fields, and a split along grammar or
construct lines that leaves every function able to reach every field changes nothing about
readability. Split the class, never only the file (§2.3).

**A KIR pass is a free function over a `Function`**, in a file of its own: `simplify`, `verify`,
`check_moves`, `check_assignment`, `elaborate_drops`. Adding a pass adds a file. The analyses return
structured errors and the driver words them. No generic dataflow framework: under §2.3 it would mean
templates or callbacks, and two thirty-line worklists are clearer.

**The checker is a DAG of classes, one question each**, wired by `Checker`'s constructor in
`sema/type_checker.cpp`, in dependency order: `Types_builder`, `Reporter` and `Callees` at the
bottom; then `Aggregates`, `Bounds`, `Annotations`, `Constant_folder`, `Places`,
`Generic_recursion`, `Literals`, `Operators`, `Overloads`, `Expressions`, `Coverage`, `Statements`
and `Signatures`. The rules that keep it a DAG:

- A class names only classes below it. Two classes on opposite sides that need one map get a
  level-0 collaborator (`Callees`, `Reporter`).
- **Nothing below `Expressions` re-enters the expression walk**: grep a class's file for `infer(`,
  `check(` and `visit(` and expect nothing. A class may read *declaration* subtrees.
- A lower class that would write a higher class's state returns the answer instead.
- Walk state belongs to the walk: `Expressions` owns `current_function_`, and classes below take it
  as a parameter.
- When inverting a call into the walk, look for what the re-entry *wrote*, not only for the call.
- A helper shared across a layer boundary goes with the layer its body is in, not its callers.

**Moving code is proved by what does not change**: the same assertion and golden counts before and
after, a multiset diff of every string literal, and a per-function diff read by eye. Delete the old
declaration first and work its callers one at a time; the compiler cannot check an inverted
predicate. A move can widen linkage and leave stale cross-references, which only reading finds.

**The file-local classes still growing** are `Parser` and `Lowering`, and `Expressions` is the
largest class. The readability work that addresses them is proposed in §15.

---

## 4. Core data structure decisions

These are load-bearing and painful to retrofit.

| Decision | Rule |
| --- | --- |
| **Memory** | Every structure is a `std::vector` addressed by handles, and nothing is freed: the compiler is a batch process and it exits. A long-lived language server would need this revisited (§12). |
| **Handles** | Refer to nodes by `u32` index wrappers (`struct Node_id { u32 v; };`), not pointers. Arrays grow without invalidating references; nodes stay small; dumping is trivial. |
| **Identifiers** | Interned at lex time into a `Symbol_id`. All name comparison is integer comparison. |
| **Spans** | Every AST node, type, and KIR instruction stores `{ File_id, u32 start, u32 end }` — half-open byte offsets, not line/column. Diagnostics are a large part of a real compiler, and retrofitting spans is miserable. |
| **AST shape** | A tagged struct — kind, span, a `u32` `aux` whose meaning depends on the kind, and a run of children in one shared array — not a class hierarchy with visitors. Everything a pass learns lives in side tables keyed by `Node_id`. |
| **Errors** | A `Diagnostics` sink collected and reported in batch. Never `exit()` from deep in the compiler. Parser and checker both recover and continue, and one mistake gives one error. |

---

## 5. Locked decisions

These are settled for v0. Reopening one requires a written reason. This section
exists specifically to prevent indefinite bikeshedding.

| # | Decision |
| --- | --- |
| L1 | Backend is C11. Frontend owns all semantics. No C++ output, ever. |
| L2 | Compiler is C++20, restrained subset (§2.3). |
| L3 | `u32` handles for all IR-ish data (§4). |
| L4 | Hand-written lexer and recursive-descent parser. No generators. Pratt parsing for expressions. |
| L5 | Bidirectional type checking (`check(expr, expected)` / `infer(expr)`). No Hindley-Milner, no global inference. |
| L6 | Function signatures and struct members are fully annotated. Inference exists for `auto` locals and for a call's type arguments (§6.7). |
| L7 | All lifetime/move/drop analysis runs on the KIR CFG, never on the AST. |
| L8 | **v0 has move checking but no borrow checker.** See §8. |
| L9 | Generics by monomorphisation. |
| L10 | Golden-file tests from the first commit. |
| L11 | **Surface syntax is C++'s** (§5.1). Statement-oriented, explicit `return`, no expression-blocks. |
| L12 | Mutable by default; `const` for immutable — as in C++ (D24). |
| L13 | **Two aggregate kinds** (D29). A `struct` is a transparent aggregate: all fields public, trivially copyable, no destructor, no owning members. A `class` has invariants: fields private by default, may own resources, may have constructors and a destructor, and is moved rather than copied. Neither inherits, ever; dynamic dispatch comes through interfaces (M9.5). |
| L14 | No exceptions in the language. Errors are `result` and `try` (M9). |
| L15 | Naming: `Capitalized_snake_case` types and `snake_case` values and functions in the compiler and in program code by convention; the library (`kl`) and the builtins are snake_case throughout, enum variants included (`result::ok`). The compiler enforces no case. |
| L16 | C declaration order (`i32 x`), so type constructors are **postfix**: `T*` and `T[*]`. References are not among them — `ref` is a binding mode written in front of the type (D32) — and `&` means address-of and nothing else. |
| L17 | `a * b;` is ambiguous between a pointer declaration and a discarded multiply under C declaration order. Resolved the Java/C# way: **an expression statement must have an effect** (D15), so the discarded-multiply reading is not a legal statement and the parser decides from syntax alone. No symbol-table feedback, so name resolution stays a separate pass after parsing. |

---

## 5.1 Syntax policy: C++ on the surface, Keel underneath

**Keel's surface syntax is C++'s.** A C or C++ developer should be able to read
Keel without a tutorial and write it after skimming one page. Declarations,
`struct`, destructors, `if`/`while`/`for`/`switch`, `auto`, and comments are all
spelled exactly as in C++.

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
copy. Nothing in the syntax warns them.

The containment rule is therefore:

> **Where Keel's semantics differ from C++'s, the syntax must differ too, or
> the compiler must reject the C++ reading outright.**

Never silently redefine a construct that already means something specific in
C++. Either give it new notation, or make the old meaning a hard error with a
diagnostic that explains the difference. §6.3 lists every current divergence;
that list is the audit surface for this rule, and every future syntax addition
must be checked against it. **Test it rather than assert it**: compile the same source with both
compilers and compare. That is how the only two silent redefinitions so far were found (D38).

---

## 6. v0 language surface

What the compiler accepts, and the rules it enforces. §6.3 is the audit surface for §5.1; §6.7
holds the settled designs that have no D-number.

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

**These samples are compiled.** Each milestone's sample is a fixture - `keelc/test/sema/sample_m1.kl`
to `sample_m5.kl`, `codegen/sample_m7.kl` and the directory `codegen/sample_m8` - which must compile
clean. A sample here that disagrees with its fixture is a bug in this file.

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

// --- M3 and M5.5: RAII. Owning types are classes (D29); allocation is a keyword in `unsafe` (D37). ---
class Buffer
{
    u8[*] data;
    u64   len;

    public Buffer( u64 n )
    {
        unsafe { data = alloc<u8>( n ); }
        len = n;
    }

    ~Buffer()
    {
        unsafe { free( data ); }
    }
};

// --- M4: ownership through call-site markers (D2, D31) ---
void consume( move Buffer b );      // called as consume( move b ) - b is dead afterwards
u64  inspect( Buffer b );           // called as inspect( b )      - read-only borrow, may not escape (§8)
void grow( ref Buffer b );          // called as grow( ref b )     - mutable borrow, may not escape

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
        case Shape::Circle( r ):
            return 3.14159 * r * r;
        case Shape::Rect( w, h ):
            return w * h;
    }
}

// --- M6: type parameters on the name, bounds in a `where` clause (D39, D40) ---
T larger<T>( T a, T b ) where T : Copyable & Comparable
{
    if( a > b )
    {
        return a;
    }
    return b;
}

// --- M9, planned: `result` in the prelude, `try` to propagate, an error union (§6.7) ---
result<Config, Io_error | Parse_error> load_config( const ref Path path )
{
    auto text   = try read_file( path );
    auto config = try parse_config( text );
    return result::ok( config );
}
```

### 6.3 Divergences from C++ — the complete list

This list is the audit surface for the §5.1 containment rule. Every entry is
either new notation or a hard error; none silently redefines valid C++ — with one
exception, marked `†` in §6.4, where sub-32-bit arithmetic keeps a narrower result
type than C++'s integral promotion gives.

**One entry diverges in the permissive direction, and is the only one**: D41 accepts comparisons
C++ compiles with the wrong answer. It removes a silent wrong answer rather than creating one, and
is listed because the audit surface is about *differences*, not severity.

| # | Divergence | Why it is safe under the §5.1 rule |
| --- | --- | --- |
| D1 | Fixed-width primitives only: `i32`, `u64`, `f64`. No `int`, `long`, `char`, `unsigned`. | The C++ spellings are **rejected outright**, with a diagnostic naming the replacement. They are *not* reserved words — the lexer treats them as ordinary identifiers, and the suggestion comes from sema's unknown-type path, which knows it is in type position. No type name is a keyword: `i32`, `Point` and `list<T>` all resolve through one path. |
| D2 | **Ownership transfers only where the call site says `move`**: `consume( move b )`. A bare argument never transfers — for a `class` it is a read-only borrow, for a `struct` a copy (D31). A type is **owning** exactly when it has a destructor, directly or through a member, and a raw pointer owns nothing. `struct` is legal exactly when the type is not owning (D29). The rule's real content is **"is it copyable"**; "has a destructor" is a proxy that holds only while copy constructors are outside v0 (§6.6), and needs restating if they arrive. | `consume( Buffer b )` as a move would be C++'s syntax for a copy carrying different semantics, which §5.1 forbids. A marker at the call site fixes that, and D31 closes the hole it left. The cost is a marker on every transfer. |
| D3 | Braces mandatory on every `if`/`while`/`for` body. | Braceless C++ is a parse error, not a reinterpretation. Kills the `goto fail;` class of bug. |
| D4 | `switch` requires exhaustiveness, matches sum-type payloads, and has no implicit fallthrough out of an arm with a body (D38). | Payload patterns (`case Shape::Circle( r ):`) are new notation. A `switch` over a plain integer keeps C++ meaning except that an arm must say how it ends; a missing case is an error, never a silent skip. |
| D5 | No *lossy* implicit conversions. A binary operator widens both operands to the smallest type that losslessly holds both, and is a hard error whenever C++'s own result type would not hold them. Assignment is implicit only where the target holds the source type. int↔bool and pointer↔bool are never implicit. §6.4 has the table. | Where C++ is lossless Keel agrees with it, and where C++ silently loses information Keel refuses. Requiring a cast for lossless widening trains authors to write casts reflexively, which is how the dangerous ones get waved through. **Provisional**: to be re-judged against real code. |
| D6 | **Error propagation is the prefix keyword `try`** (M9): `try read_file( path )`, not `read_file( path )?`. | `try expr` is a syntax error in C++, so it is new notation. A single `?` is easy to miss, and returning from the enclosing function is the loudest thing an expression can do; every other early exit is a keyword. L14 makes `try` free permanently: with no exceptions there is never a `catch` to expect (Zig's precedent). The counter-evidence is Rust's, which replaced `try!` with `?` because nested `try`s bury the happy path; whether Keel also refuses nesting is settled with M9. |
| D7 | **`enum` variants carry payloads.** A payload is a positional list of named fields, `Circle( f64 radius )`; it is constructed scoped, `Shape::Circle( 1.0 )`, and destructured in a `case`, `case Shape::Circle( r ):`. **A binding in a `case` is read-only** unless its `switch` consumes: an owning payload is borrowed in place, a copyable one copied, as D31 splits a bare parameter. There is no mutable form yet, and `case Shape::Circle( ref r ):` is refused. **While a binding borrows, the scrutinee's root may be neither written nor moved** until its `case` ends, and nothing takes a payload out of its enum: a copy, a `move` and a `return` of a binding are refused. **`switch( move h )` consumes**: `h` is moved where the `switch` is written, each binding owns its payload as a local owns its value (returned bare, taken elsewhere by `move`, writable) and drops when its arm ends, and an arm that binds nothing leaves the value whole, dropped when the `switch` ends. Only `move` consumes, a temporary included: `switch( make() )` borrows. Variants with and without payloads mix freely. **`switch` is exhaustive**: every variant appears, or `default` does. Directly adjacent labels share a body; anywhere else running on is spelled `fallthrough;` (D38). A variant takes an owning payload over, so a named one is passed with `move`. | A payload-free `enum` behaves exactly as C++'s `enum class`, which D30 makes the only spelling; payloads are new notation. The binding mode is D31's, the scoping D30's. A binding cannot be a copy (no copy constructors) and cannot be a move out of a borrowed value (it would tear a hole in a live object, which a whole-local drop flag cannot track); consuming the whole value leaves no hole, and is what `try` lowers to. The emitted C is a tag plus every variant's fields; the tag is the compiler's to maintain, which is why Keel has no `union` keyword (§6.6). |
| D8 | No headers, no preprocessor. `import geometry;` — C++20's spelling. **A module is a file and the unit of import; a package is the namespace** (§6.7). | `#include` is a hard error directing to `import`. |
| D9 | Reading an uninitialised variable is a compile error, and so is leaving one unassigned where something is owed: an `out` parameter, the return slot, and each field of the object a constructor builds. | Strictly rejects programs C++ accepts; never changes the meaning of an accepted one. |
| D10 | No `new` or `delete`. Allocation is `alloc` and `free` inside `unsafe` (D37); owning wrappers are library types. | Both words stay reserved, so `new P` is an error rather than a call. The message is the parser's generic one today; one naming `alloc` is owed (§15). |
| D11 | Generic constraints are checked **at definition, not at instantiation**. | Strictly stricter than C++20 concepts, which check at both. Errors point at the generic, not at the expansion — which is the whole reason to pay for it. The spelling is D39's. |
| D12 | `++` and `--` are **statements, not expressions**, and are written after the operand: `i++;` and `for( ...; ...; i++ )` are fine; `x = a[i++]` is a parse error, and `++i` is refused with a message saying to write `i++`. | Removes the pre/post distinction and every sequencing hazard in one move — `a[i++] = i++` is UB in C++ and is not expressible here. As a statement the two spellings mean the same, so the prefix form adds nothing. Rejects valid C++ outright rather than reinterpreting it. |
| D13 | Literal syntax: `_` is accepted as a digit separator alongside C++'s `'`; `\x` escapes take **exactly** two hex digits; unknown escapes and multi-character char literals are errors. | The separator is a pure addition. The rest reject what C++ accepts loosely: unbounded `\x` silently overflows, and `'ab'` is implementation-defined in C++. |
| D14 | A leading zero on a decimal literal is an error: `010` does not compile. | C++ reads it as octal. Keel has no octal, so accepting it would silently change the value of valid C++. |
| D15 | An expression statement must have an effect: calls, assignments, `++`/`--`, and the effectful builtins (`free`, `destroy`, `assert`). `a * b;`, `x.field;` and `arr[3];` are errors. | Rejects only statements that compute a value and discard it — already a bug, and already warned about by C++ (`-Wunused-value`). Java and C# enumerate the legal statement expressions for the same reason. |
| D16 | Operators whose relative precedence is a known C wart cannot be mixed without parentheses: bitwise (`&` `\|` `^`) with comparison, shift (`<<` `>>`) with arithmetic, and `&&` with `\|\|`. `a & b == c` is a compile error naming both readings. | Changing the precedence would silently alter valid C++; rejecting it fixes the wart *and* satisfies the containment rule. Zig takes the same approach. |
| D17 | `*` and `&` bind to the **type**, not the name, and must touch it: `u32* p;` declares a pointer, `u32 *p;` is an error. | C's declarator binds them to the name, so `int *p, q;` makes `q` an `int`. Adjacency is checked from the token spans. The rejected form gets a message naming the fix. |
| D18 | Top-level declarations are visible throughout the program, so mutual recursion needs no forward declaration. Inside a function statements are sequential, so using a local before its declaration is still an error. Import visibility narrows what each file sees (§6.7). | C++ requires a forward declaration at namespace scope; Rust, Java and C# do not. This accepts what C++ rejects and never reinterprets an accepted program. Declaration order carries no meaning between functions, and every meaning within one. A bare prototype is therefore an error, which is why `extern` (D36) is the one body-less function. |
| D19 | A declaration may not shadow another that is still reachable by the same unqualified name. A block local colliding with an enclosing local or a parameter is an error. Scopes that do **not** carry outer names in — a function body relative to file scope — are *barriers*, and reusing a name across one is fine. **Fields are visible unqualified inside a member body, and may be shadowed by a parameter but never by a local**: `Buffer( u64 len ) { this.len = len; }` is legal, `u64 len = 0;` against a field `len` is not. | Two locals of the same type mean picking the wrong one compiles and returns a bad value. Java (JLS §6.4) and C# (CS0136) reject exactly this; C++ papers over it with `-Wshadow`. File scope is exempt so adding a top-level function cannot break a body far away. A constructor parameter naming its field is the one shadow C++ programmers are forced into, so it is allowed with `this.len` as the disambiguation; a local has no such excuse. |
| D20 | A character literal is a `u8` holding one byte: `'A'` is 65, `'\xFF'` is 255. A literal spanning more than one byte is an error. A string literal is the prelude's `str` (D47). | D1 leaves no `char` type, and inventing one for literals alone would be a second spelling for `u8`. Treating a code point as the integer it is means §6.4's range rules apply unchanged. C++ makes `'a'` an `int` and `'ab'` implementation-defined; both are rejected rather than reinterpreted. |
| D21 | `main` returns `i32` and takes no parameters. Any other signature is a hard error. A program with no `main` is a library. | The emitted shim calls `main` with no arguments and returns its result as C's `int`, so any other signature would produce C that does not compile or that truncates. Command-line arguments have no spelling yet (§12). |
| D22 | `.` is the only member-access operator, and reaches through a pointer. `->` is a hard error naming `.`. | `.` on a pointer is a C++ error, so accepting it only adds meaning. Generics are the decisive reason: `thing.x` works whether `T` is a value, a borrow or a pointer. `->` stays lexed so the error can name it, and has a meaning only in type position, after a `fn` parameter list (§6.7). |
| D23 | A struct literal must initialise **every** field. `Point { 1.0 }` for a two-field struct is an error, not a zero-fill. Positional and named initialisers do not mix. | C++ value-initialises omitted fields, so adding a field silently changes every construction site. This turns that into an error at each one. |
| D24 | **Mutable by default**, as in C++. `const` is opt-in, and binds the name (§6.7). | Mutability is not what makes a program unsafe; aliasing and lifetime are. Const-by-default taxes the common case, and `i32 y = 0; y = 1;` failing would astonish exactly the developer §5.1 is written for. |
| D25 | `int`, `float` and `double` stay **hard errors**, never aliases. | D1's message names the replacement, so the cost is one compile the first time; an alias would be a permanent second spelling for every type. |
| D26 | `nullptr` is the null pointer, spelled as in C++, and a **literal** whose type comes from context: `u8* p = nullptr;` adopts, `auto p = nullptr;` is an error. | Making it a literal rather than a value of some `nullptr_t` reuses the literal machinery and keeps D5 intact — the literal *becomes* that type, as `42` becomes a `u32`. |
| D27 | **Two pointer types.** `T*` points at exactly one `T`, and has no arithmetic. `T[*]` points at many, and only it indexes and steps: `p[ i ]` is a place, the element itself, with any integer index; `p + i` counts elements and stays a `T[*]`; `&p[ i ]` is a `T*`. Only `+`, `+=` and `++` move a `T[*]`, the pointer on the left; there is no `p - q` and no ordering. `*p` and `p.x` on a `T[*]` are refused, naming `p[0]`. Indexing and `+` need `unsafe` (D35); dereferencing a `T*` does not. A `T[*]` carries no length and cannot be sliced. `T*` and `T[*]` never convert implicitly; many-to-one is `&p[ i ]`, one-to-many a `cast` in `unsafe`. `void*` is C's opaque pointer, for `extern`: nothing reads, writes or steps through it, and it converts only by `cast`; `void[*]` is refused. Suffixes apply to everything on their left, so `u8[*][*]` is many `u8[*]`. | `p + 1` on a single-item pointer is nonsense, and a type distinction catches it statically at no cost. `*(p + i)` and `p[i]` compile to identical machine code, so what buys speed is the capability of touching raw memory, which survives the split. `==` and `!=` between two pointers of the same type stay, since that is how a null check is written; ordering is refused because comparing pointers into different allocations is meaningless. A buffer with a length is a library type, which exposes `.slice( a, b )`. |
| D28 | Conversions are written `cast<T>( x )` and `wrap<T>( x )`, both **keywords**. `cast` preserves the value; `wrap` keeps the low bits. Neither converts float to integer, and neither converts integer to bool. **A narrowing `cast` is checked at run time**: a value that does not fit prints `file:line: cast out of range: <the cast>` and aborts, as a failed `assert` does; a constant that does not fit is refused at compile time. §6.5 has the table. | Two spellings because the failure policy is the interesting part, and an unqualified cast lets an author avoid stating it — which is how C's `(u8)x` silently truncates. Keywords because as identifiers they walk into the `a < b > ( c )` ambiguity. Float to integer has four reasonable answers and C picks one silently; integer to bool is better said `x != 0`. Float rounding is accepted, because a float that cannot hold a value gives an infinity rather than a plausible wrong number. |
| D29 | **Two aggregate kinds, split by one principle: a `struct` is a type whose representation is its interface; a `class` is a type whose interface hides its representation.** Both may have methods, with a **trailing `const`** saying a method does not modify its object, and static members (§6.7). A `struct` has all fields public, is trivially copyable, may not have a destructor, and may not contain an owning member (transitively); it is built from a struct literal (D23). A `class` has fields private by default, may own resources, may have constructors and a destructor, is moved rather than copied, and is built by a constructor; a class whose fields are all private cannot be built from a literal. Access markers are written on the member, `private i32 x;`, never as a section label; a `class`'s methods, constructors and destructor default to public. Neither kind inherits or is virtual. A `struct` with a destructor, and a `struct` with an access marker, are hard errors naming `class`. | C++ has two keywords for one job, kept so C headers compile; Keel pays no C-compatibility tax, so the second word is free to earn its keep. The line is **trivial copyability** rather than "may have methods", because only the first has consequences: a trivially copyable type cannot have a destructor (copy plus destructor is a double free), is never moved, and never enters drop analysis. Literals belong to structs and constructors to classes, so the two never compete. A class hides its *representation*, which is what it holds rather than how it is used, so the private default reaches fields only. A label is parser state that outlives the declaration it applies to; every other marker Keel has is written on the thing it changes. |
| D30 | **One `enum` keyword**, carrying `enum class`'s semantics: scoped (`Shape::Circle`), with no implicit conversion to an integer, and **scoped everywhere, including a `case` label**. The underlying type is spelled as in C++: `enum Shape : u8 { ... }`. `enum class E` is a hard error saying to drop the `class`. An enum with no variants is refused. **An enum is owning exactly when a variant's payload is**, and is destroyed by a synthesised destructor that tests the tag and drops only the live variant's payload. | C++'s plain `enum` leaked its names and converted to `int`; `enum class` fixed both and the broken spelling survives only for C. Every point where the two meanings diverge is an error rather than a reinterpretation. Ownership is inherited, not chosen, so there is nothing to annotate. Destroying one means destroying the active variant, chosen at run time; one generated function per owning enum keeps every drop site unchanged, and assignment then destroys the old variant for free. A variant-less enum is a type with no values, which nothing has designed (§12). |
| D31 | **Argument passing has five forms, and the call site names every one where something happens to the caller's variable.** Bare `f( x )` — a copy the callee owns if `x` is a `struct`, a read-only borrow if a `class`; either way the caller's object is alive and unchanged afterwards. A `const ref T` parameter is called bare too: a read-only borrow of either kind. `f( ref x )` — a mutable borrow. `f( out x )` — uninitialised, and the callee must assign it. `f( move x )` — the callee owns it and `x` is dead afterwards. **The same rule governs initialisation and assignment**: `Buffer b = a;` and `b = a;` between classes are hard errors naming `move a`. **A `return` needs no marker only when it names a local or a parameter** (D49). The marker appears in the signature as well as at the call site, and the two must agree; `const ref` takes none at the call. **A borrow — `ref`, `out` or `const ref` — takes an argument of exactly its type**; only a by-value parameter widens. | One principle generates the table: **you may modify what you own.** A bare `struct` parameter is a copy you own, so it is mutable, as in C++. A bare `class` parameter is a borrow, because copying a class needs a copy constructor and §6.6 has none. Reading a call site never requires knowing the kind. `move` on a `struct` is an assertion, not a transfer. `Buffer b = a;` has no safe reading — a silent move leaves `a` dead with nothing saying so, and a binding would give one resource two owners. The signature marker is forced: with `const T&` gone, `consume( Buffer b )` and `inspect( Buffer b )` would otherwise be indistinguishable. |
| D32 | **`ref` is a binding mode, not a type.** `ref T x` and `const ref T x` replace `T&` and `const T&` in every position — parameter, local binding, and return. `T&` in type position is a hard error naming `ref T`. A `ref` binding is **initialised at its declaration and never reseated**. `this` is a `ref T` binding, `const ref T` in a `const` method. Only `const ref` may be returned (§8). | Two spellings for one concept is the redundancy D25 and D30 refuse. As a *type*, §8's rule that a reference may not live in a struct needs a diagnostic; as a **mode**, a field is not a binding and the rule disappears into the grammar. `const ref i32` has one order where C++ has two. Prefix does not violate L16, because a mode is not a type constructor. `i32& r` becoming an error is the largest spelling departure the language has taken, and the diagnostic names the replacement. |
| D33 | **An operator is a method, not a free function.** `a == b` resolves to `operator==` on `a`'s type: one candidate, no overload resolution among operators, no ADL. An operator outside a type is refused. **Built: `operator==` and `operator[]`.** `operator==` is a `const` method returning `bool` and taking one more of its own type, declared at most once, and gives both `==` and `!=`; `operator!=` cannot be declared. **`operator[]` is one `const` declaration returning `T*` or `const T*`**, and `v[i]` is the element it points at; the result may be written exactly when the receiver may and the pointer is not to `const`. It is a place for the expression it is written in and **cannot initialise a `ref` or `const ref` local**; an element is kept as `T* e = &v[i];`. **Planned (M9.6)**: the arithmetic and bitwise operators, unary `-`, `!` and `~`, and ordering as **one `operator<=>`** answering a prelude `ordering` enum, from which `<`, `<=`, `>` and `>=` all come and none of which can be declared. Never overloadable: `&&`, `\|\|`, `,`, `?:` and unary `*`; `->` is not an expression operator (D22). A smart pointer lends a `T*` through a method, reached with `.`. | Defining an operator for your own type is in; choosing among operator overloads by argument type is not. One receiver type and one candidate collapses the cost to a name lookup — Rust's design by way of traits, reached here from D29's methods. Comparison is two declarations because `a < b > ( c )`'s disambiguation (§6.7) depends on a comparison yielding `bool`. `operator[]` returns a pointer, not a reference, because an element can dangle once its container grows, and `T*` is the type that says so; one declaration serves const and mutable receivers alike, with no constness overloading. Short-circuiting and sequencing cannot survive becoming a call. |
| D34 | **A range is syntax, not a type.** `a..b` is half-open and appears in **one** place: a `switch` label, `case 1..5:`, which lowers to a pair of comparisons. There is no `Range` type, no iterator, and no `step`. `a..b` with `a > b` is an error when both are constants. **Guards are refused**: a `case` matches shape, never a computed condition. | Exclusive because the real form is `0..size()`, where an off-by-one is a bug. As syntax it costs one parse rule and one lowering rule. A guard makes exhaustiveness undecidable — `case n if n > 0:` and `case n if n <= 0:` cover everything and no compiler can prove it — and exhaustiveness is the reason `switch` is worth having. The `for( i : 0..n )` forms are designed and not shipped: they are shorthand for what `for` already spells, and reverse and step want a value with methods, which arrives only if `Range` becomes a type. A `T[*]` has no length, so it has nothing to slice. |
| D35 | **`unsafe` is a block, and it permits operations rather than disabling checks.** `unsafe { ... }` is the only form: no `unsafe` function, no `unsafe` expression. Everything checked outside one is checked inside. The block permits an **enumerated** list: converting between pointer types, dropping `const` by `cast`, calling an `extern` function (D36), `alloc`, `free` and `destroy` (D37, D50), and indexing and `+` on a `T[*]` (D27). **A block that uses none of them is an error**, and so is **a block inside another**. An operation counts as used even when its operand is wrong, so a mistake is one error, not two. | The shape is Rust's. Unused is an error because a region that quietly grows wider than its operation makes the marker worthless for review. Nesting is an error because permission is not cumulative. The list stays enumerated: dereferencing a `T*` is deliberately off it, since gating it would break working programs and deserves its own decision. No `unsafe fn`: `extern` already marks the one thing unsafe to call, and if a marker ever arrives it should mark callers only, the lesson of Rust's 2024 edition. An unsafe block is a `Block` with `aux == 1`, so every pass that walks blocks handles it with no edit. |
| D36 | **`extern` declares a function defined in C, and calling one needs `unsafe`.** `extern i32 abs( i32 v );` — file scope only, terminated by `;`, and **the absent body is what marks it**. The name is **not mangled**. Binding modes work and are how a real C signature is spelled: `ref i32` and `out i32` both emit `int32_t*`. `main` may not be `extern`, and an `extern` cannot be overloaded. The emitted C declares each extern itself and never includes the C header. | D18 makes a bare prototype an error, so `extern` is the only rule that produces a body-less function and no flag is needed. With every extern unsafe to call, `unsafe extern` would carry no information. **Two guarantees rest on a promise**: `out` on an extern is discharged in a body that does not exist, and `move` hands ownership to a function whose destructor Keel never runs; both are what the `unsafe` at the call asserts. Declaring externs ourselves avoids a conflicting-declaration error whenever the Keel spelling differs, at the cost that a wrong declaration is an ABI bug at run time — the argument for a thin runtime whose signatures are ours. |
| D37 | **`alloc` and `free` are keywords, and both need `unsafe`.** `alloc<T>()` yields a `T*`; `alloc<T>( n )` yields a `T[*]` of `n` slots. `free( p )` takes either and yields `void`. They lower to the runtime, `kl_rt_alloc`, `kl_rt_alloc_many` (which refuses an overflowing `n * size`) and `kl_rt_free`. **Neither runs a constructor or a destructor**: `alloc` returns memory, not an object. Failure is `nullptr` until M9 gives `result`. | Keywords because `alloc` needs the element type to compute the size, and then `<` can only be a bracket. `free` is a keyword so it can check its operand is a pointer, and so freeing needs no `cast`. **`alloc` is gated because it hands back a pointer whose type says there is a `T` there, and there is not.** Nothing tracks double free, use after free, leaks or freeing a stack address; each is what the `unsafe` asserts. Open: whether both become library functions over one size intrinsic, which would give the two names back (§15). |
| D38 | **A `case` with a body says how it ends, and `fallthrough` is how it runs on.** An arm that can complete normally may not be followed by another — it ends with `return`, `break` or `fallthrough`, and the last arm is exempt. **`break` binds to the nearest enclosing loop *or* `switch`**, as in C; `continue` looks past a `switch` to the loop. **`fallthrough;`** runs the next arm's body: it must be the arm's last statement, may not appear in the last arm, and may not enter an arm that binds. **No stacked label may destructure**, and **a bare `case Shape::Circle:` matches a payload variant without destructuring it**. | **The only construct where Keel was found silently redefining valid C++**, twice: an arm running on into the next gave a different answer, and `break` inside a `switch` left the loop. Both now match C++ or are errors. `fallthrough` is what makes the rule satisfiable; it is a hard error in C++ today, so it is new notation. Stacked labels that bound every payload read fields of a variant that was not there; refusing destructuring there, and allowing a bare variant label, keeps stacking available. |
| D39 | **Type parameters are written on the name, and bounds in a trailing `where` clause.** `T larger<T>( T a, T b ) where T : Comparable`, several bounds joined with `&` (D40), and `class list<T>` for a type. `template` is a hard error naming the replacement. | **The declaration mirrors the call site**, where type arguments already sit beside the name — `larger<i32>( a, b )`, `cast<T>( x )`, `alloc<T>()`. `where` says what the clause is. `T id<T>( T a )` and `class W<T>` are hard errors in C++, so this is new notation. |
| D40 | **A bound is a promise about a type parameter, drawn from a fixed set, and written `where T : Copyable & Comparable`.** A second parameter takes a second clause, `where T : Comparable, where K : Hashable`. Bounds are **nominal**. The set is closed and compiler-provided: `Copyable`, `Equatable`, `Comparable`, `Numeric`, `Integral`, `Floating`. They **imply** one another, so `Integral` carries `Numeric`, `Comparable` and `Equatable`. **`Copyable` means trivially copyable** — what a `struct` is — and is what lets a body copy or mutate a bare `T`. **Planned (M9.5)**: `Equatable`, `Comparable` and `Hashable` become prelude interfaces a user type can implement; `Numeric`, `Integral`, `Floating` and `Copyable` stay in the compiler; every bound is spelled lower case. | **Bounds exist for D11**: monomorphisation needs no bound to emit code; checking a body once, before any instance exists, needs the bound to enumerate what is available — which is also why they are nominal. **An unbounded `T` is not copyable**, so `T b = a;` is refused at the definition and `twice<Counter>` at the call: each side reports its own half. `Owning` is absent because `move` is correct for owning and non-owning alike. `&` rather than `,`, because all must hold; `\|` would say "or". A conformance is declared, never inferred from shape, so the interfaces of M9.5 replace the operator check rather than extend it. |
| D41 | **Comparison is not arithmetic, and only arithmetic needs a common type.** `<`, `>`, `<=`, `>=`, `==` and `!=` accept any two numeric operands, mixed signedness included, and give the mathematically correct answer: `u64 a; i64 b; a < b` is legal. A `T` bounded `Numeric` compares against any concrete number the same way. Where no type holds both operands, the emitter writes a sign guard or a range-checked comparison rather than a bare C one. A comparison against a literal the other operand's type cannot hold is a warning, since it is always true or always false. Every other binary operator keeps D5's rule. | `a + b` must land somewhere, and for `u64` and `i64` nowhere holds both. `a < b` lands in `bool`, which holds every answer. C's `-1 < 1u` is false because it forces a common type where none is needed — the most-cited integer footgun in the language. Eight pairs have no common domain (mixed signedness against `u64`, and a 64-bit integer against a float), and they fall into three shapes, each one small function emitted where used. The float shapes cannot be one expression: truncating a `double` is undefined for a NaN or an out-of-range value, so the tests are what make it legal. Operands still have to be numeric. |
| D42 | **A generic may not be instantiated with a type built out of its own parameter, around a cycle.** `f<T>` calling `f<T>` is ordinary recursion; `f<T>` calling `f<T*>` is an error, as is any cycle through several generics carrying an edge that wraps a parameter. The same holds through an aggregate's fields and a method's signature. Detected at the definition and reported at the call that builds the type. | Monomorphisation works only if the set of instances is finite. A depth limit, C++'s answer, was refused: any number is arbitrary, the diagnostic lands at an expansion the author never wrote, and a program property would depend on a compiler setting. The shape is decidable from the source, which keeps it inside D11. One level with no cycle is fine — `f<T>` calling `g<T*>` stops — and the escape is to take the built type as a second parameter. |
| D43 | **`const` on a pointer's element is C++'s pointer to const, written one way.** `const T*` and `const T[*]` read what they point at, and nothing writes through them. `T* const` is a pointer that cannot be reseated, and `const T* const` is both. `const` goes before the type it applies to: **`T const*` is refused**, naming `const T*`; `const T const*` is refused as `const` written twice; `const` on a type argument is refused as applying to nothing. `T*` converts to `const T*` implicitly, one level down only, and dropping `const` is a `cast` in `unsafe`. `&` of a read-only place is a `const T*`: a `const` binding, a `const ref`, a bare borrow, a pattern binding, a `const ref` return, or a field reached through any of them. | The outer `const` already meant the binding (§6.7), so the element's `const` is the other half of the same C++ meaning. East const is valid C++, refused for the reason `ref const T` was: one order for one meaning. Two levels down is C's known unsoundness: `T**` to `const T**` lets a `const T*` be stored where a `T*` is read back. |
| D44 | **A `const` field is assigned once by each of its class's constructors, with `=`, and by nothing else** - a struct's, by its literal. Everywhere else it reads like any field and is refused as a write: from a method, a destructor, another object of the same class, a reference, or through a `T*`. Inside its constructor only `n = v` or `this.n = v` assigns it - not `+=`, `++`, a write to part of it, a non-`const` method, `ref` or `out` - and D9's constructor analysis refuses a second assignment on any path. Its address is a `const T*`. Replacing the whole object, `b = move c` or `p = P { ... }`, is not writing its fields and stays legal. | The binding `const` moved onto a field, with the one window a field needs to get its value. Java's blank `final` and Swift's `let` property are the same rule. Whole replacement follows Swift, not C++, which deletes assignment for a class with a `const` member and would make `str` impossible to reassign. `out` is left out so no address of a `const` field ever writes. |
| D45 | **The prelude is a package of its own, seen by every file without an import, and searched after the file's own package and before no other.** A declaration of the program's, or of a package's, with a prelude name shadows the prelude's within that package, and is not an error. **A function's overloads fall back rather than shadow**: an unqualified call tries its own package's set first, by both tiers of §6.7's selection and with the call's markers aside, and the prelude's only when its own set has no viable candidate; a call nothing matches lists both sets. A missing or extra marker is therefore reported against the call's own set rather than answered by a prelude candidate taking a copy. A qualified call names one package and never falls back. No qualifier names it: `prelude::x` is an undeclared `prelude`, and `--package prelude=<dir>` is refused. Its types read bare in diagnostics, and its symbols are mangled with the package like any other's. An error inside it is an ordinary diagnostic at `<prelude>:line:col`. Its source is embedded in keelc and parsed at startup; `keelc --print-prelude` writes it. | Shadowing is Rust's prelude rule, for Rust's reason: a refusal would make every name later added to the prelude a breaking change. Falling back keeps that promise for overloads, where a flat merge would not: a prelude signature equal to one the program declared would make every call ambiguous, and with widening a closer prelude overload would silently take a call the program's own set used to win. A program's `print( Point )` therefore leaves `print( "hi" )` working. A package of its own makes shadowing free, since the resolver already keeps one scope per package. The one name the compiler uses itself, a literal's `str`, is bound to the declaration rather than looked up (D47). |
| D46 | **The prelude's C is a file of its own, `<input>.kl.prelude.c`, which the program's C includes after the standard headers; and the prelude is parsed after the program.** Each half holds its own declarations, and a typedef both write is written twice, which C11 allows. `--emit-c` prints the program's half; a build writes both beside the executable. The dumps leave the prelude's declarations out. An instance of a prelude generic at a program type belongs to the program's half, and a comparison guard both halves use is written by the prelude's only. | Written into the program's C, the prelude changed every codegen golden whenever it grew. The include is valid because the prelude sees nothing of the program. Parsing it last keeps the program's node ids, and so its C names, stable however the prelude grows. |
| D47 | **A string literal is the prelude's `str`, built by `str`'s private constructor from the literal's bytes and their count.** The resolver binds each literal to the prelude's own declaration named `str`, so a program declaring a `str` of its own changes nothing about `"abc"`. The bytes are the escapes decoded, and `size` counts them, a `\0` included. Each literal is a C string literal at its use, every byte C could misread written as three octal digits. A literal is a value: `"abc".size` reads it and `&"abc"` is refused. Where two types in one mismatch print the same name, the hint says they are two types. | Binding by declaration is D45's promise kept. The private constructor keeps `str` ordinary Keel with one door, and a literal lowers to a construction the analyses already know. A C string literal per use, not one pooled array: C already stores each with its `0` and may merge identical ones. |
| D48 | **`assert( cond )` is a keyword expression of type `void`, always on, whose failure prints `file:line: assertion failed: <cond>` to stderr and aborts, running no destructor.** The condition is held to `bool` as an `if`'s is. `<cond>` is the source text with whitespace runs collapsed. It lowers to a branch whose failing block ends in KIR's `Panic`, which the emitter writes as a call to the runtime's `_Noreturn kl_rt_panic( file, line, message )`. | A keyword, not a library function, because only the compiler knows the caller's file, line and text. Always on because there is no debug mode to turn it off in (M10). Aborting without unwinding is C's `assert` and Rust's `panic = "abort"`: destructors run on state the program has just said is broken would do harm. A terminator rather than a call followed by `Unreachable`, so the analyses need nothing new. `panic` is a keyword for the same reason and shares its terminator (M9); a function reporting its caller's line rather than its own waits for caller location (M10). |
| D49 | **A `T*` points at a live `T`; a `T[*]` slot is raw storage. An owning value leaves a place only by `move`, and only a local, a parameter or a raw slot may be moved or returned bare.** Writing `*p`, and so `v[i]` through `operator[]`, drops the old value first; writing `data[i]` on a `T[*]` drops nothing. `return *p;`, `return field;` and `return data[i];` of an owning value are refused as *an owning value is transferred, not copied*. `move v[i]` through `operator[]` is refused, as `move *p` is. Moving out of a raw slot is allowed and, like any `T[*]` index, needs `unsafe`. `move` on a `ref` binding, on `this` or on a field is refused as moving out of a borrow. | The pointer types already split one-live-value from many-maybe-empty (D27), so that split also says whether a write replaces or initialises — what Rust says with `ptr::write` against `*p =`. `return` was exempt from D31's marker because the returned local dies; a pointee, a field or an element does not. An element moved out through `[]` would be dropped again by its container; a raw slot is the container's own storage, and keeping its count right is the author's part of `unsafe`. **Not closed**: `*p = move a` with `p == &a` leaves `*p` dead, the dangling `T*` §8 accepts. |
| D50 | **`destroy( p, n )` ends the values in the first `n` slots of a `T[*]` and releases nothing.** A keyword like `free`, beside it in `unsafe`. The pointer must be a `T[*]` that does not point to `const`; the count adopts `u64`; the result is `void`, and a statement of it is effectful. It lowers to a counted loop of drops through `p + i`, and to its operands alone when `T` owns nothing, so one generic body serves both. | Ending a lifetime and releasing storage are two jobs: `free` does not know how many slots are live, and only the container knows its count. C++ splits them the same way (`std::destroy`, then deallocate), as does Rust (`drop_in_place`, then `dealloc`). A count, because every caller wants a run; one is `destroy( p + i, 1 )`. |
| D51 | **`void` is a type: it has no size and one value, and may be a type argument.** A value of type `void` takes no storage, and a payload position of type `void` takes no argument and binds no name, so `result<void, E>` succeeds with `result::ok()`, matches `case result::ok:`, and `try` on it yields `void`; the arguments and names left pair with the positions left, in order. Inside a generic body `T` is not `void`, so `ok( v )` there keeps its written count for every instance. Written directly, a `void` parameter, field, payload or global is refused, since it passes or holds nothing and a field of it would make the empty-aggregate refusal untrue, and `void x = f();` is a warning, since `x` holds nothing; none is reported when the `void` arrived as a type argument, nor for `auto x = f();`, which did not write it. An instance left with no fields that way, `Box<void>`, is emitted with one placeholder byte, since standard C has no zero-size struct. | One rule in lowering, "a `void` value takes no storage", instead of a unit type beside `void` that means the same and must be spelled `ok( unit{} )` at every success. C++'s irregular `void` is why `std::expected<void, E>` needs its own specialisation. |
| D52 | **A statement may not discard a `result`; `_ = f();` discards one on purpose.** The rule belongs to the type, not to an attribute, and only `result` carries it. `_` is a keyword, and a discard target only on the left of `=`; the right side is evaluated, and an owning value is dropped at the end of the statement. | A dropped `result` is the one discard that loses what the language promised to bring to the caller's attention: an error. Refusing every discarded value was rejected, since it would refuse calls made for their effect and the drop of a discarded call's owning result (§15). |
| D53 | **`///` documents the declaration after it and `//!` the package of its file, both in Markdown.** The `///` lines between a declaration's first token and the token before it are its documentation, whatever plain comments and blank lines fall among them: a type's, function's, method's, constructor's, destructor's, field's, variant's or global's. Lines before anything else (a `}`, a statement, the end of the file) are ignored without a diagnostic, and `////` or more is a plain comment. A package's documentation is the `//!` lines of its `packageinfo.kl`, a file name every package reserves: it may hold only comments, so a declaration or `import` in it is an error, and `import packageinfo;` is refused, since it is not a module. A `//!` anywhere else is ignored. A block's first paragraph is its summary, and a top-level declaration's must read without its signature, since the package's page lists it alone: it names no parameter. `### Panics` and `### Errors` head the sections saying when a call aborts or fails. A private member may be documented; generated pages leave it out and hover shows it. They leave out an `extern` without a doc too, since it is C the reader either should not call or will find documented with its library. Hover shows a member as its use sees it: at an instance every type parameter is replaced by its argument, and inside the generic `T` stays `T`. | A comment, because documentation changes nothing the program means, so a misplaced one is never an error. A package is a directory and every file in it a module, so no module is the package's; a reserved file is, and it may later hold more than comments. Markdown rather than tags such as `@param`: the signature already names each parameter and its type. A side table beside the tokens rather than a token of its own, so no grammar rule has to skip one. |


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
  `u32 n = 0; n = n + 1;` would need a cast.
- **Mixed signedness is safe exactly when the signed type is strictly wider.**
  `i32 + u8` cannot misinterpret anything and C++ agrees; `i32 + u32` is where
  C++ turns `-1` into 4294967295. Of the 32 mixed-sign pairs, 18 are accepted and
  14 rejected.
- **`i64 + f64` and `f32 + i32` are errors.** C++ accepts both and silently loses
  precision past the mantissa.

`i8 + u32` is stricter than safety alone requires — `i64` holds both operands
exactly, but C++ answers `u32`, so accepting it would silently change a valid C++
program. That cell is the containment rule's cost, not the type system's.

**A literal takes the type its context expects**, and is range-checked against it: `u8 x = 300;` is
an error, and so is `f32 x = -1e40;`. In a chain of operations over literals, such as
`u32 a = 1 + 2 + 3;` or `i64 a = 0 - 3000000000 - 1;`, each operation is checked in the declared
type, so `i8 d = 100 + 100 - 1;` is refused at the `200`. A constant that overflows its type is an
error (`u32 d = 1 - 2;`); arithmetic on values wraps (§12).

**A comparison does not pay the containment cost (D41).** It has no result to protect, so the only
requirement is that the domain hold both operands exactly. That is a second, wider table, and it is
the one `<`, `<=`, `>`, `>=`, `==` and `!=` are checked against:

```
           i8  i16  i32  i64   u8  u16  u32  u64  f32  f64
  i8       i8  i16  i32  i64  i16  i32  i64    *  f32  f64
  i16     i16  i16  i32  i64  i16  i32  i64    *  f32  f64
  i32     i32  i32  i32  i64  i32  i32  i64    *  f64  f64
  i64     i64  i64  i64  i64  i64  i64  i64    *    *    *
  u8      i16  i16  i32  i64   u8  u16  u32  u64  f32  f64
  u16     i32  i32  i32  i64  u16  u16  u32  u64  f32  f64
  u32     i64  i64  i64  i64  u32  u32  u32  u64  f64  f64
  u64       *    *    *    *  u64  u64  u64  u64    *    *
  f32     f32  f32  f64    *  f32  f32  f64    *  f32  f64
  f64     f64  f64  f64    *  f64  f64  f64    *  f64  f64
```

`*` is **not an error**: it marks the eight pairs no type holds, where the comparison is answered by
cases instead. A comparison adopts only a bare literal into the other operand's type, so
`n < 0 - 1` on a `u64` is the warning *always false* rather than a refusal.

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

`—` is a hard error. `*` marks a **narrowing** integer `cast` (one where the target cannot hold the
source type, which includes a same-width change of signedness): it is checked at run time and aborts
when the value does not fit. A constant that does not fit is refused at compile time.

*unsafe* marks a real conversion rather than nonsense, so it is **gated rather than refused**: legal
inside an `unsafe` block, an error outside one naming that as the fix. It covers a conversion between
pointer types: `T*` to `T[*]`, to or from `void*`, and dropping `const`.

Two asymmetries are deliberate. `wrap` accepts a widening integer conversion — it simply never
wraps — so it is total over integer-to-integer. And `cast` gives a **literal** its type from context
rather than converting it: `cast<u8>( 300 )` is the ordinary out-of-range error and
`cast<u8>( 200 )` is simply a `u8` literal. `wrap` must not do this — accepting a value the target
cannot hold is the whole point of it — so `wrap<u8>( 300 )` infers `i32` and then wraps to 44.
Both work on a type parameter: `wrap<i32>( a )` for an `Integral` `T`, `cast<f64>( a )` for a
`Floating` one.

### 6.6 Not in v0

**Planned:** interfaces and dynamic dispatch (M9.5), the rest of the operators (M9.6), and
`result` with error unions (M9). `optional<T>` needs no language work — it is an ordinary generic
enum — and arrives as library code when something wants it.

**Not in v0:** closures, coroutines, concurrency, copy constructors (a class is duplicated by an
explicit `clone()` method), `Shared<T>` and `Weak<T>`, nested types and member type aliases, default
member initialisers, type aliases, runtime reflection.

**Never:** implementation inheritance, `protected`, `friend`, constructor member-initialiser lists
(Keel has no `const` or reference members to need one, and no bases), RTTI and downcasting, a
`namespace` keyword (the package is the namespace, §6.7), and unions — the one legitimate use of
one, a tagged variant, is what a payload-carrying `enum` is, with the tag maintained by the compiler.

### 6.7 Settled designs

Rules decided and built (or decided and scheduled) that have no D-number, because they are new
territory rather than divergences from a C++ meaning.

**Modules and packages.** `import name;` loads `name.kl` from the importing file's package, and
loading is transitive, deduplicated by canonical path, so cycles and diamonds are free. Imports come
before every declaration. **A module is the unit of import; a package is the namespace**: the
program's own package is unnamed and rooted at the input file's directory, and another package is
named on the command line, `--package kl=<dir>`, as one identifier that is not a keyword or
`prelude`. `import kl::list;` loads a module from a package, and its declarations are written through
the package, `kl::list<i32>`, never through the module. **Inside a package the qualifier is
optional; across packages it is required**, and there is no `using`. **A file sees only what it
imports**, and imports are not transitive; a name from a loaded module the file did not import is
refused with the `import` to write. A type reached through a signature needs no import to *use* —
only to *write its name*. Each package has its own file scope, so two packages may declare one name;
overload sets merge within a package, never across one, so an import elsewhere can never change
which overload a call picks. `::` resolves left to right — package, type, static member or variant —
and a type may not take a package's name. A package's symbols mangle with the package, so the
program's `Point` and `kl::Point` are two C types.

**`const` is a property of a binding.** A declaration is `const` when the outermost node of its
annotation is `const`: `const i32 x` cannot be assigned, and `i32* const p` cannot be reseated though
what it points at can be written. `const ref T` is the read-only borrow. The pointer-to-const half is
D43, and a `const` field is D44. A `const` on a by-value parameter or return is not part of a
function's type.

**Static members.** `static` before a method's return type declares one with no receiver, called
`Type::name( args )` and refused as `value.name( args )`; a generic type's static call names its
instance, `list<i32>::with_capacity( 8 )`. `static` before a field declares one variable shared by
every object, named `C::count` (or bare inside the type) and refused as `c.count`; it follows a
file-scope variable's rules — a constant initialiser, folded once; no struct or class, which would
need building; nothing owning, since nothing drops one; and no error union, whose zero tag need not
name a member — and is absent from the emitted struct. A generic type gets **one storage per instantiation**, as C++, D, Zig and C# do, and the
static's own type may not name a type parameter. A static and a member of one name collide.

**Access control.** `private` and `public` are written on a member (D29). Every decision is about a
*qualified* access, and there are five: `obj.f`, `obj.f( ... )`, `Type::f`, `C( ... )` and
`C { ... }`. A member is visible from inside the aggregate that declares it, static methods
included. A class can hide its constructor and offer a static one instead; the refusal suggests a
static method only when a visible one returns the type, and says that only a literal makes a `str`.

**Overloading.** Functions, methods and constructors overload; operators do not (D33). A parameter's
identity is the pair **(declared type, marker the call must write)**: `f( i32 )` and
`f( const ref i32 )` are one signature, since a `const ref` takes no marker, while `f( ref i32 )` and
`f( i32* )` coexist. Two declarations differing only in return type or a trailing `const` are refused,
an `extern` cannot be overloaded, and there is one `main`. **Selection is by exact type first, and
widens only when nothing matches exactly.** A literal narrows to its family, integer or float, and no
further: `f( 1 )` against `f( i32 )` and `f( i64 )` is ambiguous, and an ambiguity is never settled by
widening. Arithmetic on literals of one family, `2000000000 + 2000000000` or `1 << 40`, narrows the
same way and is then typed at the chosen parameter, as one candidate's argument is; a mix, or a
`true` among them, is typed on its own. Among exact matches a non-generic candidate beats a generic one, and two generics are
ambiguous. **With no exact match**, a candidate is viable when each numeric argument to a by-value
parameter widens by §6.4's assignment table and every other argument matches exactly; a borrow never
widens (D31). The closest target for one argument is the one that keeps its kind (integer or float),
then the narrowest, then the one that keeps its signedness: a `u8` against `i16` and `u32` takes
`i16`, against `i16` and `u16` takes `u16`, and an `i32` against `i64` and `f64` takes `i64`. An
integer reaches a float only where the float holds it exactly, as it does through one candidate, so
an `i32` against `f32` and `f64` takes `f64` and an `i64` against them is refused. One
candidate beats another when no argument's target is further and one is closer; a set with no
candidate beating every other is ambiguous. So `print( i64 )` and `print( u64 )` take every integer
type, and every program accepted before this rule means the same under it. **A call that no
candidate takes, and exactly one would take with its markers set aside,** chooses that one and reports
the marker, so `print( out v )` says to remove `out` rather than that nothing matches. Two members that some substitution could make identical — `at( T )` beside
`at( i32 )` on a `Box<T>` — are refused at the declaration.

**Type-argument inference.** A call deduces its type arguments **from its arguments, and the
expectation fills only what they left**, so `i64 x = id( a );` on an `i32` instantiates at `i32` and
widens the result. Deduction is structural: `Box<T>` against a `Box<i32>` gives `T = i32`. A literal,
`nullptr` and a struct literal contribute nothing. The list is all or nothing: if any parameter
cannot be deduced, every one is written. The expectation is taken and cleared by the call it belongs
to, so in `f( g() )` what `f` is wanted for says nothing about `g`. A constructor deduces against the
aggregate's open form, so `Box<i32> b = Box( 7 );` works. A generic static call writes its type
arguments.

**A generic aggregate's or enum's instance comes from context.** `Box<i32> b = Box { 7 };` and
`Opt<i32> o = Opt::Some( 7 );` take their instance from the expectation, and with none —
`auto x = Opt::Some( 7 );` — it is an error. `Box<i32> { 7 }` and `Opt<i32>::Some( 7 )` are not
spellings.

**Literals in generic bodies.** An integer literal adopts a `Numeric` `T`, a float literal needs
`Floating`, and `true` and `nullptr` never adopt. Its range is checked at the definition against
every type the bound admits — under `Integral`, `0` to `127` — and past that the author writes
`cast<T>` or `wrap<T>`. Checking against the instances actually used was rejected: adding a call
elsewhere would break a function that compiled yesterday.

**The conditional expression is `?:`, and `if` has no value.** An expectation reaches both arms;
with none, the arms must match exactly, with no widening between them. It may be the root of a
place that is read, `( c ? a : b ).t`.

**Writing into a temporary is refused.** `g().t = 1;` and `( c ? a : b ).t = 1;` on by-value
results are refused, and a place rooted in a call that returns a reference is read-only, since only
`const ref` may be returned.

**An aggregate with no fields is refused**, naming a one-variant `enum` as the tag-only
alternative; standard C has no zero-size struct. A class with only methods counts as empty.

**Function types and pointers.** A function type is `fn( i32, i32 ) -> i32`, arrow included for
`void`. `&f` takes a function's address; `fn( ... )` with modes spells them, `fn( ref i32 ) -> i32`,
and the return can be `-> const ref T`. A mode is part of the type; a `const` on a by-value
parameter or return is not. On an overloaded name the expected type selects, and `auto g = &f;` is
refused. A generic's address names its instance, `&id<i32>`. Taking the address of an `extern`
needs `unsafe`. **A method pointer shows its receiver**: `&C::get` is `fn( ref C ) -> T`, or
`fn( const ref C ) -> T` for a `const` method, an ordinary function pointer, since Keel has no virtual dispatch to adjust for; a static method's is `fn( args ) -> ret`.
**A field offset** is `field( C ) -> T`, from `&C::x`, applied as `p( obj )` and read-only; it is an
offset, not an address, so it has no arithmetic, and it copies, so `T` must be `Copyable`.

**`a < b > ( c )` is a call to a generic.** The comparison reading would need `operator>` on
`bool`, which nothing can declare, and Keel has no comma operator, so the parser commits to the
generic reading with no symbol table. This depends on comparison yielding `bool` (D33).

**No `Boolean` bound**: it would admit exactly one type, which is that type spelled longer.

**The anonymous error union (M9).** `result<Config, Io_error | Parse_error>` — no
combining enum is declared, and `switch` matches leaf members directly. `A | B` is a type wherever
one is written. **Each member is an `enum`, written once**, and none is a type parameter, so a union
is closed where it is written. A union is canonical: nested unions flatten, members sort by
`Type_id` and deduplicate, so `A | B` and `B | A` are one type, interned on the member set. Every
member gets a global tag when a union first holds it, available because Keel has no separate
compilation, so widening keeps the tag and exhaustiveness is a subset test. **A member converts
into a union holding it** wherever a value meets a known type (an initialiser, an argument, a
`return`, a payload, an arm of `?:`), as `i32` converts to `i64`; a copyable member needs no `move`
even where the union owns, since the caller gives nothing up. **Widening one union into another is
confined to `try` and is not a subtype relation**: everywhere else a union matches by identity, so
overloading never needs subset ordering. The error set is declared, never inferred, so adding a
`try` for a new error type is a compile error at the contract, naming the member to add. A
`switch` over a union lists every member's variants, and a missing one is named with its enum. In
C a union is a `u32` tag and its members overlaid; one that owns gets a synthesised destructor that
tests the tag, as an owning enum does (D30).

**Planned for M9.5: interfaces.** One `interface` declaration, method signatures only, serves both
uses: `where T : Shape` is monomorphised with no vtable, and `ref Shape`, `Shape*`, a bare parameter
and `kl::box<Shape>` dispatch through one. No `dyn`. **Conformance is declared** —
`class Circle implements Shape`, after any type parameters and before `where` — and never
structural; a missing method is reported at the `implements`. A type declared elsewhere conforms
through a block, `implements hashable for i32 { ... }`, under Rust's orphan rule. **An interface type
is never held by value**: a `Shape` local or field and `kl::list<Shape>` are refused, and a closed
set of shapes is an `enum`. What the compiler depends on (`result`, `box`, the bound interfaces)
lives in the prelude; everything else in `kl`. Open with the row: how an interface names its
implementing type, how `box<Shape>` is built from `box<Circle>`, generic interfaces, and whether a
string literal may adopt a library type that opts in through a prelude interface.

---

## 7. C backend contract

The emitter is where most avoidable bugs will live. These rules are not
optional.

1. **Three-address form before emission.** Every subexpression gets its own
   temporary on its own statement. This makes C's unspecified evaluation order
   irrelevant, and makes destructor insertion for temporaries tractable.
2. **Every exit runs the destructors of everything live**, in reverse construction order: the
   end of a scope, an early `return`, `break`, `continue`, and (M9) `try`. Drops are KIR statements
   placed by drop elaboration, and the C is a `goto` between blocks. A temporary an owning value
   lands in is dropped at the end of its statement, and a discarded call's owning result with it.
   A `try` leaves mid-statement, so a local or temporary is listed for dropping only once it holds a
   value, and an aggregate's values are all built before any field is written.
3. **Drop flags where static analysis cannot decide.** A value conditionally
   initialised or moved on only one branch gets a hidden `bool` checked at
   cleanup. Rust does exactly this.
4. **Emission order:** type forward declarations, then type definitions in containment order (C
   requires complete types for by-value members; a cycle needs a pointer and is refused otherwise),
   then function prototypes, then bodies. The prelude's half is a separate file (D46).
5. **Mangle every symbol**, length-prefixed so the encoding is injective: `kl__main__`,
   `kl_2kl_area__3i32` for a package's function, a member tagged by its type, an instance carrying its
   type arguments and substituted parameters, and each parameter encoded with the marker a call writes.
   `extern` names are not mangled (D36). The `kl_` prefix keeps user identifiers away from C keywords
   and the runtime.
6. **Emit `#line` directives** mapping back to `.kl` files. Free debugger and
   error-location support for almost no work.
7. **Compile generated C with `-fwrapv -fno-strict-aliasing -std=c11`.**
   C's UB becomes Keel's UB otherwise.
8. `bool` is `<stdbool.h>`'s. Integers are `<stdint.h>` exact-width types.
9. **The C compiles under `-Wall -Wextra -Werror` whatever the program leaves unread.** A local or
   parameter nothing reads is marked used with `( void ) name;`; the dead store stays until M11's
   dead-store elimination removes it in KIR.

**The runtime is `keel_rt`, a thin C floor**, linked into every program as `libkeel_rt.a`:
`kl_rt_alloc`, `kl_rt_alloc_many`, `kl_rt_free`, `kl_rt_panic` with `kl_rt_panic_message` (a `panic`'s `str`, as bytes and a count) and `kl_rt_unreachable` (after a `never` call, which C cannot see through a pointer), and the writers `kl_rt_write`,
`kl_rt_write_i64`, `kl_rt_write_u64` and `kl_rt_write_f64`, each taking a stream, 1 for stdout and
2 for stderr. A write to stderr, and a panic, flush stdout first, so the two streams read back in
the order they were written. A float is written in the fewest digits that read back as the same
value, a whole number keeps its `.0`, and a NaN is `nan` whatever its sign. The
runtime proper is Keel above it, as the library is. libc is the floor; raw syscalls would couple the
C backend to one compiler's `__asm__`. **Keep it under 200 lines for as long as possible.**

keelc links the runtime itself: by default from `${CMAKE_INSTALL_PREFIX}/lib/keel/libkeel_rt.a`,
fixed at configure time, or from `--runtime <path>`. A missing runtime is keelc's own error, before
anything is written; `--check`, `--emit-c` and the dumps link nothing. `$CC` chooses the compiler and
`$CFLAGS` is appended to keelc's own. One `.c` per program: monomorphisation and the error union's
tags both need the whole program, so there is no separate compilation.

---

## 8. Ownership: start far smaller than the manifesto

The manifesto's §24 asks whether the model should resemble Rust's borrow
checker. **v0's answer is: no, and we defer the question** (§12).

### v0 implements move checking

A flow-sensitive dataflow analysis over the KIR CFG. Each local is in one of:

```
Uninitialised -> Live -> Moved
                   \-> MaybeMoved   (join of Live and Moved at a merge point)
```

Using a `Moved` value is an error, and so is borrowing one, writing into part of one, or one call
that both moves a local and borrows it (`two( ref b, move b )`, `h.eat( move h )`). `MaybeMoved` at
a drop point emits a drop flag, and so does a temporary built in one arm of `?:` or on the right of
`&&` or `||`, which lowering records. Only `move` of a local, a parameter or a raw slot is allowed (D49).
This buys use-after-move detection, double-free prevention and correct RAII — most of manifesto
§3.6, for a fraction of the work.

**Assignment replaces.** `=` on an owning place drops what it held first, reading the new value
before the drop so `o = move o` survives; through a `T*` or `operator[]` likewise (D49), and through
a `T[*]` slot not at all, since raw storage holds nothing yet. **A constructor initialises**: its
first write to each field is an initialisation, every field is assigned before it is read, before
`this` is used whole and before any return (D9), and an owning field may not be written where it may
already hold a value.

### v0 does not implement lifetimes

References obey one blunt structural rule instead:

> A reference may appear as a function parameter or a local binding.
> It may **not** be stored in a struct or captured. It may be **returned only as a
> `const ref` derived from one of the function's own reference parameters** — the
> receiver counts as one, so a method may return a reference into its object.

A **reference parameter** is one that travels by address: `ref T`, `const ref T`, `this`, and a
bare parameter of an owning type, which D31 makes a read-only borrow. A local `ref` binding does not
count, even where its referent is a parameter: the rule traces the returned expression to its root,
and a binding is not a parameter. That conservatism is deliberate and relaxing it is additive.

**Why returning one needs no lifetimes.** D32 makes a `ref` binding initialised at its declaration
and never reseated. Everything nameable at a binding's declaration lives in an enclosing-or-same
scope, so **anything alive at a call site outlives any `ref` binding declared at that call site** —
whichever parameter the result came from. The question Rust answers with lifetime parameters does
not arise. What remains is a syntactic check with no dataflow.

**Raw pointers are outside this rule, and that is the point.** `ref` is the spelling that cannot
dangle; `T*` is the one that can. The one pointer escape refused is the certain one: `return &x`
where `x`'s storage ends at that `return` — a local, a by-value parameter or a pattern binding. An
address stored through an `out` parameter, into a global or inside a returned struct is the author's
problem.

**The holes, and where they close.** A `ref` binding to a call's result whose argument is a
temporary — `const ref A r = pick( A( 1 ), b );` — is accepted today and dangles once the temporary
is dropped at the end of the statement: a use-after-free in safe code. With it, a `T*` held across
the end of what it points at, a `ref` binding held across a `move` of its referent, and two
arguments aliasing one place (`f( out v, out v )`, `f( ref v, ref v[0] )`) are M9.1's work. M9.1's
question is whether these can be checked without lifetimes in the syntax.

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

**A deferral is real only when a row carries it.** "Waits for M8", "post-M6" or "with the library"
in a sentence reads like a schedule and is not one: no row picks it up and the milestone closes
without it. Every item deferred to a milestone goes into that milestone's row, with its acceptance.

| M | Deliverable | Acceptance | Teaches |
| --- | --- | --- | --- |
| **M0** | Lexer, parser, AST, pretty-printer, diagnostics, interner. No types. **Done.** | Parse the §6.1 sample and print an AST that round-trips. Malformed input produces a spanned error, not a crash. | Parsing, spans, error recovery |
| **M1** | Integers, bools, C-style function and variable declarations, `auto`, `if`, `while`, `for`, calls, arithmetic. Type check + emit C. **Done.** | `fib(20)` compiles and returns the right exit code. | The whole pipeline works end to end |
| **M2** | Structs, value semantics, field access, struct literals, by-value passing and returning. **Done.** | A `Point` program computing a distance. | Type layout, declaration ordering |
| **M3** | KIR + CFG. Constructors and `~Dtor()`. Scope-exit cleanup. **Done.** | A `Buffer` with `~Buffer()` frees exactly once, at the right place, including on early `return`, under valgrind and ASan. | **RAII — the core of the language** |
| **M4** | Move checking, `ref`/`out` bindings (D32), the non-escaping rule, drop flags, D2. **Done.** | Use-after-move is a compile error with a good message; a conditionally-moved value drops correctly. | Ownership, dataflow analysis |
| **M5** | `enum` (D30) payload-free, then payloads (D7). `switch` with destructuring, `default`, stacked labels and exhaustiveness. Range labels (D34). **Done.** | The `Shape`/`area` sample. Non-exhaustive `switch` is a compile error naming the missing variant. | Sum types, tagged variants |
| **M5.5** | Methods on `struct` and `class`, `unsafe` blocks (D35), `extern` (D36), `alloc`/`free` and `kl_rt` (D37). **Done.** | A linked list whose nodes are allocated one at a time, walked, and freed — valgrind-clean. | Whether the language can express a real data structure |
| **M6** | Generics (D39, D11), bounds (D40), the monomorphisation worklist and D42, for functions, aggregates and enums; D41; `cast`/`wrap` on a type parameter; overloading; type-argument inference. **Done (2026-09-18).** | `max<i32>` and `max<f64>` both work; a generic `Box<T>` with a destructor drops correctly; `u32 < i32` and `i64 < f64` compile and answer correctly; `wrap<i32>( a )` works for an `Integral` `T`; `C( i32 )` and `C( f64 )` coexist; `i32 x = id( 5 );` needs no written type argument. | Instantiation, mangling |
| **M6.5** | Debts: the KIR simplification pass, the empty-aggregate and variant-less-enum rejections, the conditional expression, writes into temporaries, the mangling category tag, and splitting the type checker into the class DAG of §3.2. **Done (2026-09-24).** | `struct Empty { };` refused naming a one-variant `enum`; `while( true ) { return 7; }` needs no `return` after it; `a > b ? a : b` compiles and runs only one arm; `struct foo__ { };` beside `i32 foo()` mangles to two names; `( c ? a : b ).t = 1;` is refused. | Paying debts before they compound |
| **M7** | Static methods, access control, function pointers, method pointers and field offsets (§6.7). **Done (2026-09-28).** | `codegen/sample_m7.kl`: a named constructor called `Type::make( args )` and refused as `value.make( args )`; a private field refused outside its type; a function's address stored and called through. Refusals in `sema/errors_sample_m7.kl`. | Whether a type is a namespace, and whether a function is a value |
| **M8** | `T[*]` (D27), modules and packages (§6.7), the editor extension with diagnostics, semantic tokens and go-to-definition, keelc linking the runtime, static fields, pointer to const (D43), `const` fields (D44), the prelude (D45, D46), string literals (D47), `assert` (D48), `operator==` and `operator[]` (D33), owning elements (D49, D50), and `kl::list` and `kl::string` written in Keel. **Done (2026-10-07).** | `codegen/sample_m8`: `alloc<T>( n )`, `p[i]` and `p + i` on a `T[*]`, a module imported bare and a package named by `--package`, a static field per instantiation, `&Gauge::make`, `kl::string` from a literal compared with `==`, a `kl::list<i32>` grown through `operator[]`, a passing `assert`, a checked `cast`, and a `kl::list` of an owning type through every operation, valgrind-clean. Refusals in `sema/errors_sample_m8.kl`. | **Whether the design actually works** |
| **M9** | **Error handling**, in this order. **Overloads first**, since `print` is an overload set every program calls: selection widens when nothing matches exactly, and a borrow takes its exact type (§6.7, D31), **done**; then the prelude's set falls back behind the program's (D45), **done**. **Then `print`**: the runtime takes a stream, bytes and a count, `kl_rt_write`, since a `str` carries its length, and formats numbers itself; the prelude adds `print`, `println`, `eprint` and `eprintln`, each over `str`, `i64`, `u64`, `f64` and `bool`, with no `unsafe` at the caller; `print( 5 )` is ambiguous, as any literal is between two of its family; and `kl_rt_panic` flushes `stdout` before `abort`; **done**. The writers' `extern`s are prelude declarations, so a program sees them, and calling one still needs `unsafe`. `kl::string` prints at M9.5, through an interface it opts into (§12). **Then owning enum payloads**: the synthesised destructor that switches on the tag (D30), which an error carrying a `kl::string` needs; **done**. **Then moving a payload out**, by `switch( move h )` (D7), which `try` lowers to; **done**. **Then `result<T, E>`** as an ordinary generic enum in the prelude, and **`try`** (D6), which returns through the drops the function owes; step 4d (§15) gains its `try` clause here. `try` may nest. **Done**, sema and lowering; a local joins a drop list only once it holds a value, since `try` can leave mid-statement. Before it lowered, **`void` becomes a type** (D51), so a function that can fail but yields no value returns `result<void, E>`, and **a discarded `result` is refused**, with `_ = f();` to discard one on purpose (D52); both **done**. **Then a temporary built on one path of an expression**: an owning temporary in one arm of `?:`, or on the right of `&&` or `\|\|`, drops only if it was: lowering lists it as built on one path, drop elaboration gives it a flag, and the end of its statement ends its storage, which clears the flag for the next time round a loop; **done**. **Then a runtime-cost audit**: every cost the emitted C pays that the program did not write — drop flags and their writes, hidden temporaries and the copies into and out of them, merge locals — checked against the least it could be; **done**, and M11's row lists the passes it owes. **Then the anonymous error union** (§6.7); **done**. **Then `panic` and `never`**, **done**. `panic( message )` is a keyword expression taking a `str`, for D48's reason: only the compiler knows the caller's line. It prints `file:line: panic: <message>` and aborts, running no destructor. `never` is a return type and nothing else (a function's, an `extern`'s, a function pointer's): a call of one ends its block, a `switch` arm ending in one needs no `break`, and a `never` function that writes `return` or can reach its end is refused. A call of type `never` is a statement only and stands nowhere a value is wanted (§12). `assert`, a failed `cast` and `panic` end in one KIR terminator, `Panic`; a call of a `never` function is followed by `Unreachable`, and every `never` function is `_Noreturn` in C. **Then the library, before anything else in M9**: `kl::optional<T>`, with `unwrap`, `expect`, `value_or`, `is_some` and `is_none`, and `kl::pair<A, B>`, **done**; on `kl::list`, `insert`, `truncate`, `first`, `last`, `try_pop`, `reverse`, `shrink_to_fit` and `append`, **done**. On `kl::string`, `starts_with`, `ends_with`, `find`, `substring`, `pop`, `clear`, `string::from` over `i64` and `u64`, and `parse_i64` answering a `result`, **done**. The three searches take a `str`, since a `kl::string` reaches them through `as_str()` at M9.5, and `find` answers `optional<u64>`, the first match; `substring( start, count )` copies and asserts the run is in range; `pop` asserts as `kl::list`'s does; `reserve` and `capacity` are not offered, since a string's storage is not its interface; `parse_i64` takes an optional `-` and decimal digits only, and answers `result<i64, kl::parse_error>`, whose variants are `no_digits`, `invalid_digit( u64 index )` and `overflow`, reporting the first byte that cannot continue a number in range. A static call `T::f( … )` and a bare sibling call choose among overloads as `p.f( … )` does, and all three check `static` and visibility on the method chosen. Open: `kl::ring<T>`. Each takes an owning element wherever it takes an element. **And a function-pointer field called by its name**: `h.cb( 1 )`, `this.cb( 1 )` and, in a method, `cb( 1 )` call through the field, where today the first two are refused as *not a method* and the third as *not callable*. | An `i8` and a `u32` reach `print( i64 )` and `print( u64 )`, and `f( out x )` with an `i32 x` against `out i64` is refused. A program prints a string literal, an integer and a float to stdout and to stderr, and its golden compares both; a failed `assert` after a `print` still shows what was printed. A function returning `result<Config, Io_error \| Parse_error>` propagates with `try` from a callee returning `result<kl::string, Io_error>`, with no conversion written. `switch` over the union matches leaf variants, and a missing arm is a compile error naming it. `A \| B` and `B \| A` select the same instantiation and mangle to one symbol. `c ? B( 1 ).n : 2` and `c && B( true ).n` run no destructor when `c` is false. `panic( "empty" )` ending a `switch` arm prints its line and message after what was printed, and aborts dropping nothing; `i32 x = panic( "no" );`, `never f() { }` and a `never` local are refused. `unwrap` on an empty `kl::optional` panics, and every library addition has a fixture in `keel_stl/test/`, valgrind-clean with an owning element. `cb( 1 )` in a method calls the field. | **The largest type-system addition in the document** |
| **Docs** | **Documentation comments, hover and generated pages**, between M9's `kl::string` and `kl::ring` (D53). In this order. **`--names` names the overload chosen**, which every call but `p.f( … )` did not, **done**. **The lexer keeps `///` and `//!` blocks** in a side table, and the parser gives each `///` block to the declaration after it; the loader reads a package's `packageinfo.kl` and refuses anything in it but comments. **Done.** **A signature printer** writes a declaration as its source writes it, or at an instance, **done**. **`--names` gains each use's signature** as D53 says hover shows it, and its declaration's doc, and **`--declarations`** prints one record per package doc and per declaration outside the prelude: location, kind, visibility, signature, parent and doc, **done**. **The extension's hover** shows a use's signature and its declaration's doc, still on save, **done**. **The doc tool**, `keeldoc`, a fourth project in `keeldoc/` with its own tests, a binary that runs keelc for `--declarations` on a root importing every module of the package, writes a package's HTML pages, public declarations only, overloads together, rendering the Markdown itself; it is not run on save, and pre-commit regenerates into a scratch directory and fails when the committed pages differ. The `kl` pages live in `keel_stl/docs/`. **Done**: `index.html` with the package doc and each module's declarations and summaries, one page per module (a type's members under Variants, Fields, Constructors and Methods), a shared `style.css`; signatures mark keywords and link the package's types; a doc's heading of any level is one section heading; each page's footer is the copyright and SPDX lines opening `packageinfo.kl`. **The library documented**: the stl and its `packageinfo.kl`, **done**, then the prelude, **done**. The prelude's pages come with every package's: keelc's `--with-prelude` keeps the prelude in `--declarations`, and keeldoc writes `prelude.html` beside each package's pages, the primitives first and then the prelude's declarations, and links `str`, `result` and the primitives in every signature to it. The primitives have no declaration, so their docs are `keeldoc/src/primitives.md`, one `#` heading per primitive, compiled into keeldoc as the prelude is into keelc; a unit test fails when it and keeldoc's list of primitives disagree. **Then the two empty test projects**: the `codegen/kl_*` fixtures move to `keel_stl/test/`, one directory per program, with a `run_tests.sh` of its own that hands keelc's runner `--tests`, which CTest and pre-commit run, **done**; then `keel_rt/test/` gains a C test binary for the runtime's edge cases (the lowest `i64`, the largest `u64`, a panic's message), run by CTest and pre-commit, **done**. | Hovering `xs.push` on a `kl::list<i32>` shows `public void push( move i32 value )` and its doc; inside `list.kl` the same hover shows `T`. Go-to-definition on `Number::from( b )` with a `u64 b` lands on `from( u64 )`. A `///` before a `}` is accepted without a diagnostic, and `////` documents nothing. The `kl` pages hold no private member, and `str`, `result` and `i64` in their signatures link to their `prelude.html`. A doc change without regenerated pages fails pre-commit. A declaration in `packageinfo.kl` is refused, and so is `import packageinfo;`. Every `kl_*` fixture passes from `keel_stl/test/`, under valgrind too, and none is left in `keelc/test/`. | Documentation as data the compiler already has |
| **M9.1** | **Borrows that outlive or alias their object.** **A borrow kept past its object**: a `const ref` bound through a call to a temporary argument (`const ref A r = pick( A( 1 ), b );`, a use-after-free accepted today), `&V()[0]` on a temporary, `&v[0]` held across a `push`, `&b` held across `move b`, and a `ref` binding held across a `move` of its referent. **Aliasing within one call**: `f( out v, out v )`, and `f( ref v, ref v[0] )`, where the callee reaches `v` both whole and through an element. **The loan's freezing half**: while a borrow of `v[i]` is live, nothing may reach `v`, which lets the refusal of `ref i32 x = v[0];` be lifted. **A view kept past a change to its object**: a value of a type tagged as a view by a prelude interface with no methods (`str_view` the first) takes a `ref` binding's rules, so §8's argument covers it: initialised once and never reassigned, never a field except of another view, never a type argument, returned only as §8 traces a `ref`. Returned from a call, it is tied to the receiver and every borrowed argument, transitively through views of views; a later use is refused if, on any path between, the object was passed to a non-`const` method, assigned, moved, passed `ref` or `out`, or dropped. A view of a temporary is the temporary case above. The tag needs M9.5's interfaces, so this row builds the tracking over the borrows that exist and M9.5 attaches the tag. The row's first decision is which pointer cases are refused and which stay the `T*` type's own warning, since a `T*` already says it can dangle and a `ref` does not. After M9, since nothing here waits on `try`; before M9.5, since an interface's `ref Shape` is one more borrow these rules must cover. | The temporary case is refused or its temporary lives as long as the binding; `f( out v, out v )` and `f( ref v, ref v[0] )` are refused, each naming both arguments; `ref i32 x = v[0];` is accepted and `v.push( 9 )` while `x` is live is refused; a `ref` binding used after a `move` of its referent is refused; and each pointer case is either refused or recorded as accepted on purpose. | **Whether a borrow can be checked without lifetimes in the syntax** |
| **M9.5** | **Generic methods first**: a method with type parameters of its own beside its aggregate's, `list<T>::map<U>( fn( const ref T ) -> U f )`, instantiated per call within the aggregate's instance; today `map<U>` is a parse error that does not say so. Before interfaces, because whether an interface method may be generic (it cannot be dispatched through a vtable) is decided with them. **Interfaces** (§6.7): `interface`, `implements`, static use through a bound and dynamic use through `ref Shape`, `Shape*` and `kl::box<Shape>`. D40's `Equatable`, `Comparable` and `Hashable` become prelude interfaces, built-in types satisfying the first two intrinsically and `hashable` through a prelude conformance; every bound is spelled lower case. A type declared elsewhere conforms through an `implements ... for` block under the orphan rule. A type lends text by implementing the prelude interface `textual`, requiring `as_str()` returning a prelude `str_view`, which `print` and its kin accept, so `kl::string` prints (§12); `str_view` implements the view tag M9.1's tracking reads, and `str_view`'s constructor carries D35's caller-only `unsafe` marker. **Before building the marker, investigate a built-in slice `T[]` in its place (§12).** **The library waiting on interfaces**: `kl::box<T>`, which lives in the prelude once, beside the `box<Shape>` above, and no `kl` copy precedes it; `kl::string::hash` and `kl::map` and `kl::set` over a `hashable` key, refused until then rather than built over `Integral` keys. Open: how an interface names its implementing type, how `box<Shape>` is made from `box<Circle>`, generic interfaces, and a string literal adopting a library type that opts in. | `list<T>::map<U>` called at two `U`s from one `list<i32>`, each its own instance. A user `interface` used as a bound, with an instance calling the implementer's method directly; a type missing a method is refused at its `implements`; a `kl::list<kl::box<Shape>>` holding two implementers, each called through its vtable and dropped through it, valgrind-clean; `kl::list<Shape>` and a `Shape` local are refused. | Polymorphism without inheritance |
| **M9.6** | **The rest of D33's operators**: arithmetic and bitwise, unary `-`, `!` and `~`, and ordering as one `operator<=>` over a prelude `ordering` enum. `kl::string` gains `operator+` over another `kl::string`, the spelling `push_str` would have been. After M9.5, so a user type's operators and its `equatable` and `comparable` conformance land together. | `a + b`, `-a` and `a < b` on a user type, each a call of the left operand's operator; `operator<` refused with a pointer to `<=>`; a `where T : comparable` generic instantiated at a type declaring `operator<=>`. | **Whether one candidate per operator is enough** |
| **M10** | **Compiler flags.** Which diagnostics are warnings, which are errors, which can be silenced; **a debug mode**, so `assert` has something to be off in; **caller location**: a function marked to receive its caller's file and line without the caller writing them, so a `panic` inside `kl::optional`'s `unwrap` reports the line that called `unwrap` (open: the marker's spelling, what a pointer to a marked function carries, and a chain of marked functions passing one location through); and the rule that **a `-W` flag reaches `$CC` only when Keel was asked for the same thing**, so the C compiler never refuses code Keel accepted (today `i32 f( i32 a ) { return 0; }` fails under the golden runner's `-Werror` for an unused parameter Keel said nothing about). | `keelc --werror=unused-parameter` refuses that program with Keel's own diagnostic, and without the flag both Keel and the generated C accept it. The golden runner's C flags follow what each fixture asked for rather than one global default. `unwrap` on an empty `kl::optional`, called through a second marked function, reports the line of the outermost call. | Diagnostics as an interface |
| **M11** | **KIR passes** (§2.2), written against KIR so every backend inherits them. First what M9's runtime-cost audit found, each a cost Keel removes itself whatever the C compiler would do (§2.2): **move coalescing**, which merges a copy `a = b` that is `b`'s last use into one local where the two lives do not overlap. That covers a constructor's temporary moved into its declared local, a conditional's merge local and each arm's result, a `switch( move … )` or `try` scrutinee's copy, and a moved local returned, which becomes the return slot; a reassignment keeps its temporary, since the old value drops after the new is built. Then the same into a field place, so an aggregate literal or `result::ok( … )` builds its value where it ends up, and out of a payload, so a `switch( move … )` binding names the payload in place. **Drop elaboration by maybe-initialised dataflow**: a drop stays unconditional where its local is certainly built, goes where certainly not, and only the rest keep a flag, which also removes the flag writes no path reads (a clear beside a `storage_live`, a clear before a return). **The `&(*x)` peephole** a borrow-returning call produces. Then dead store elimination, copy and constant propagation, then **inlining**, run after drop elaboration so a callee's drops are already explicit — a borrowed parameter becomes the caller's place, a `move` parameter a fresh local, the return slot the call's destination; its first cost model is size alone, never a recursive cycle or a call through a pointer — and then the drop-flag dataflow again over the inlined bodies. The rest of §2.2's list waits for a backend that needs it. | `codegen/drop_flags.kl` keeps a flag only where paths differ, and emits each flag's false write once; a new `codegen/moves.kl` emits no whole-value copy out of a temporary at its last use, except into a reassigned local; `codegen/borrows.kl` emits no `&( *`, `codegen/inlining.kl` calls a small function nowhere and a recursive one still, with the same output, and every other golden changes only by lines removed, except where inlining moves a body into its caller. Each pass is its own commit with its own fixture, passes under `KEEL_VALGRIND=1`, and has a unit test showing it leave alone the case that looks like its target and is not. | Transformations that must keep the ownership invariants |

**M3 is where this stops being a toy** — it is the first thing C cannot do for us. **M4 is where we
learn whether the ownership model is real.** **M8 is where the language writes its own library**,
and M9 to M9.6 are what that library showed it needs: errors, sound borrows, interfaces and
operators.

**A milestone of debts is legitimate** (M6.5). Small things found while building a feature that
belong to none of it get carried indefinitely when no milestone owns them; giving them one means they
are finished rather than remembered.

---

## 10. Testing

Two suites, with a clean split of responsibility.

### Unit tests live in the source file they test

Tests sit at the bottom of the `.cpp` file they exercise, inside `#ifdef ENABLE_UNIT_TESTS`, with
the Catch2 includes inside the same guard. `keel_tests` is the whole source set recompiled with that
define; `main()` is excluded from it by `#ifndef ENABLE_UNIT_TESTS` so Catch2WithMain can supply its
own.

The cost is compiling every source twice. The benefit is decisive for a compiler: a test can reach
file-local statics and anonymous-namespace helpers directly, so the lexer's tables, the parser's
recovery predicates and the dataflow lattice joins are all testable without widening a public
header. Use unit tests for anything with an invariant expressible in C++. A test belongs in the file
that emits the diagnostic it asserts. **Test what the compiler calls**: a suite that is cheap to
write is not evidence that the code under it is used.

### Golden-file tests cover language behaviour

`keelc/test/<suite>/`, one directory per stage: `lex`, `parse`, `sema`, `kir`, `codegen`, `driver`.
`run_tests.sh <keelc>` runs them all; it is a shell script, not a framework, and the directory holds
only `.kl` files and expectations. Every compiler works this way (rustc's UI tests, clang's
`lit`/FileCheck).

- Each suite has a `FLAGS` file naming the compiler flags it runs with; a suite without one is an
  error, so a new directory cannot quietly test the wrong thing.
- For each `<name>.kl`, stdout is compared against `<name>.kl.expected`, stderr against
  `<name>.kl.stderr` and the exit code against `<name>.kl.exit`; an absent file means empty, or 0.
- **A directory holding `main.kl` is one program**: `main.kl` is the test and its other files are
  the modules and packages it imports.
- **A suite holding a `RUN` file builds and runs each program**: keelc links it against the runtime
  itself, and the exit code is compared against `<name>.kl.run`, and stdout and stderr against
  `<name>.kl.out` where one exists. A golden diff can say the emitted C is unchanged, never that it
  is correct, so codegen fixtures check their own answers and return a distinct code for each wrong
  one. A fixture expected to exit 134 aborts on purpose and is not run under valgrind.
- The C is built with `-Werror` except the `unused-*` family, since a local the program never reads
  is faithful emission. `CC`, `KEEL_CFLAGS`, `KEEL_RT`, `KEEL_ARTIFACTS`, `KEEL_JOBS` and
  `KEEL_RUN_TIMEOUT` override the compiler, its flags, the runtime library, where artefacts go, the
  parallelism and the run timeout.
- **`KEEL_VALGRIND=1` runs every program under valgrind** and fails on any error it reports; a
  double free or a leak still exits 0, so the exit code alone cannot see M3's acceptance. The `asan`
  preset needs `KEEL_RT` naming a runtime built without sanitizers.
- **Never regenerate expectations wholesale.** `--update` records what the compiler does, not what
  it should do. An expectation is regenerated one fixture at a time, with keelc and the suite's
  `FLAGS`, from `keelc/test`, and the diff is read.
- **The `kl` package's fixtures are `keel_stl/test/`**, one program per directory, run by the same
  runner: `keel_stl/test/run_tests.sh <keelc>` passes it `--tests` and an artefacts directory of its
  own. A program testing the package belongs there; keelc's corpus imports `kl` only to test the
  compiler.
- **The runtime's edge cases are `keel_rt/test/`**, a C binary, `keel_rt_tests`, linking
  `libkeel_rt.a` as a program does. Each case runs in a forked child with stdout and stderr on pipes,
  so a panic's message and its abort are checked as well as a writer's output.

**A bug fix without a regression test — in either suite — does not count as fixed.** Run every
suite on debug, debug under valgrind, release, and asan.

**What the tests are for, learned by mutation.** A count is not an assertion about a diagnostic: a
rule that says one mistake gives one message is pinned by `errors() == 1` and the message, never by
`!clean()`. Both halves of a two-part invariant need their own mutation. A surviving mutation of new
code often finds missing coverage of old code. Probe before declaring a mutant equivalent: count how
often the line runs, with a positive control.

---

## 11. Repository layout

One repository, four projects, three languages:

```
keel/
  keelc/                  the compiler, C++
    src/
      main.cpp            the driver: options, the pipeline, writing outputs, invoking cc
      common/             Interner, Span, Source_manager, Diagnostics, Literal_pool, Imports
      lex/                lex(), Token and the token tables
      parse/              Parser (file-local), its Lookahead, the loader, C++-habit hints
      ast/                Ast, Node and Node_kind's shape table, the dump
      sema/               Resolver; the checker's class DAG (§3.2), one class per file; collect_names for the editor
      ir/                 KIR (kir.h), Builder, Lowering (file-local) and monomorphisation, simplify, verify, print
      check/              check_moves, check_assignment, elaborate_drops: one pass per file, over kir.h's successors()
      codegen_c/          Kir_emitter, Spelling, mangling, linking the runtime
      prelude/            prelude.kl, embedded into keelc at build time
    test/                 golden corpus and run_tests.sh (§10)
  keel_rt/                the C runtime every program links against (§7); its edge cases in test/
  keel_stl/               the kl package, written in Keel, in src/; its generated pages in docs/, its programs in test/
  keeldoc/                the doc tool, C++: runs keelc for --declarations and writes a package's HTML pages
  editors/vscode/         the editor extension: grammar, diagnostics, semantic tokens
  examples/
  docs/
    MANIFESTO.md          the vision
    PLAN.md               this file
  CMakeLists.txt  CMakePresets.json  vcpkg.json
```

Each project keeps its own include root, so keelc's sources say `#include "lex/lexer.h"` and never
name the project. `keel_rt` is separate from keelc because it is not part of the compiler: it is data
the compiler ships, in a different language, linked into programs keelc produces. One repository is
right because a runtime change and the compiler change that needs it are one commit, and one build
proves both. `.claude/` (untracked) holds the work log, `LOG.md`.

Build: CMake + Ninja, C++20, clang-18, configured by `CMakePresets.json` rather than VS Code kits,
which pick up unusable Windows toolchains under WSL.

```
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```

Presets: `debug`, `asan` (ASan + UBSan), and `release` (RelWithDebInfo, `-Werror`). Each gets its
own `build/<preset>/bin`. `-Werror` is off in `debug` on purpose: a mid-refactor unused variable
should not stop the build.

Dependencies are held to three, all via vcpkg, and each has to justify itself:

| Package | Why |
| --- | --- |
| `fmt` | clang-18 over libstdc++ 12 has no `<format>`. Drop it if the toolchain gains one. |
| `cxxopts` | Compiler drivers accumulate flags quickly. |
| `catch2` | Unit tests, inline in the sources (§10). Test-only; `-DKEEL_TESTS=OFF` builds with two deps. |

Nothing else gets added without deleting something. The compiler writes its own interner and
containers — those are the parts we are here to understand, and they are rewritten in Keel at
bootstrap. `keelc` and `keel_tests` are built from the same glob of `src/**/*.cpp`, the latter with
`ENABLE_UNIT_TESTS`. Includes are path-qualified from `src/`.

---

## 12. Open questions

Each has a milestone by which it must be decided, or a stated reason it has none.

| Question | Decide by |
| --- | --- |
| **Do we ever add lifetimes or a borrow checker, or is the structural rule permanent?** §8's rule plus M9.1's checks are the experiment. | After M9.1, with real-program evidence |
| **Runtime overflow checking.** **Decided: arithmetic wraps** (`-fwrapv` keeps it defined), and a constant that overflows is a compile error. A branch per operation inhibits vectorisation, and wrapping is what every programmer was taught. Open: an **opt-in** runtime check, which is a frontend feature — a checked operation in KIR that each backend spells (`__builtin_add_overflow`, `llvm.sadd.with.overflow`) — never a sanitiser flag passed to `$CC`. **And a variable shift count**: `a << b` with `b` past the width is undefined behaviour in the emitted C, the one place C's UB reaches a Keel program that compiled cleanly; the answers are masking the count (what x86 does, and Java and C# define) or a checked shift. It cannot stay undefined. | The shift: before M11's passes. The opt-in check: with M10's flags |
| **What, if anything, does `->` mean in an expression?** It is a hard error naming `.` (D22) and means the return in a `fn` type. Rejected uses: a move expression, a state-machine DSL, and scope injection (`user -> { greet( name ) }`), which is `with`'s action at a distance. | No deadline — it costs nothing to leave free |
| **How does a library type lend text?** Only a literal makes a `str`, since its constructor is private, so `kl::string` cannot reach `print`. **Direction decided**: a literal stays `str`, tied to nothing and free to be a field; a view is a separate prelude `str_view`, tagged a view (M9.1), made by `as_str()` on any type implementing the prelude interface `textual`, and `print` accepts any implementer, `str` included. The three live in the prelude, because `print` must name them. `str_view`'s constructor is callable only inside `unsafe`, which is D35's caller-only marker arriving: any function, method or constructor may carry it, its body gains no permission, and taking its address is unsafe or carries the marker in the type. Before building that, investigate a built-in slice `T[]` instead: made from a `T[*]` and a count as a D35 operation, with `str_view` safely built from one. A view's object is invalidated by any non-`const` method rather than only by one touching what the view reads: `const` is the declared contract, and reading bodies would make a caller's validity change with an implementation. | M9.5 |
| **Returning a mutable `ref`.** Refused (D32), so a smart pointer lends mutable access as a `T*`, which says it can dangle and that the pointer does not own the object. Open: whether §8's tracing argument covers a mutable return, and whether M9.1's checks make it worth having. | After M9.1 |
| **Uninhabited types.** `enum Nothing { };` is refused because nobody designed a type with no values; Rust's `!` is the real concept. **Decided for M9**: `never` is a return type only. Open: whether it becomes a type a value has, which the next row needs, and `enum Nothing { };` with it. | With the next row |
| **A `never` call where a value is wanted.** `return panic( "no" );` and `c ? 1 : panic( "no" )` are refused (M9); the spelling is a statement, with the local declared first. Lifting it: `never` converts to every type, and lowering ends the block mid-expression as `try` does, tested in each position (`?:` arms, the right of `&&` and `\|\|`, arguments after an owning temporary, initialisers, `return`, assignment). Refusing `auto x = panic( … );` stays. Only accepts more programs. | When the statement form is felt in real code |
| **Is there a construct for "do several unconnected things to one value"?** Rejected for `switch`, where it would be action at a distance (D38), but what it describes — a value and a list of independent guarded actions, each running if it matches — is not a `switch`: validation accumulating findings is the customer. A construct with no exhaustiveness claim could allow predicate guards. Possibly not a feature at all. | With real code to judge it against; no milestone |
| **Does a pattern nest?** `case err( Io( e ) ):` is what anyone writes once errors are enums of enums. Nesting needs exhaustiveness over a product of variants and a diagnostic for a case missing three levels down; one layer per `switch` composes by hand. The error union removes most of the pressure, since `switch` matches its leaves directly. | M9, with the error union |
| **Lambdas.** The comparator algorithms (`sort`, `index_of`, `contains`, `binary_search`, `remove_if`, `dedup`) wait for them or for interfaces: without either, comparing means declaring a function and passing its address, which is the spelling they should not ship with. Open: capture (by `const ref`, `ref` or `move`, under §8's non-escaping rule) and whether one without captures is a plain `fn` value. | Before the comparator algorithms; unscheduled |
| **A one-argument constructor as a conversion.** `s == "abc"` and `s += "abc"` on a `kl::string` would go through `operator==` and `operator+` if a `str` converted to a `kl::string` through `string( str )`, so the library writes no `equals( str )` or `push_str( str )`. Open: implicit by default, or opted into per constructor; and the cost, an allocation the call site does not show. | Before `kl::string` takes a `str` anywhere `operator==` or `operator+` could |
| **Custom allocators — visible in the type system or not?** Nothing has asked yet; `kl::list` allocates through `alloc`. | When a second allocation strategy is written |
| **Command-line arguments.** `main` takes nothing (D21); the obvious spelling is `i32 main( const str[*] args, u64 count )` or a library type. | When a program needs them |
| **`for( var : collection )`.** Needs an iteration protocol, which should be designed against `kl::list` rather than ahead of it, and D34's `for( i : 0..n )` is the same syntax, so the two are settled together. | After M9.5, since a protocol is an interface |
| **Standard library naming.** Settled so far: the package is `kl`, names are snake_case, `list` rather than `vector`, `string`, `map`, `box`. Open: the rest as they are written. | As each type is written |
| **A build tool.** The scope that stays small: find the sources from `import`, build the graph, invoke the C compiler, cache. The scope that eats projects: package management, versioning, cross-compilation, a configuration language. Start with the smallest thing that builds a multi-package program, and treat every addition as a decision. | After M9.6 |
| **The rest of the editor: completion.** Diagnostics, semantic tokens and go-to-definition run `keelc --check` on save. Completion needs the checker's type for the left of a `.` on unsaved text that may not parse, which needs either a long-lived server or a re-entrant compiler (§4's memory policy assumes a batch process). Try the subprocess first. | With the build tool |
| **Compile-time evaluation and reflection.** **Direction decided: compile-time only**; runtime reflection needs metadata in every binary, which §4's no-RTTI refuses. The uses are a test framework that discovers tests, and `enum` to string as a generated table. Open: the surface (a `@` builtin, a `__traits`-style call, attributes) and how much general compile-time evaluation sits under it. A field offset (§6.7) is the runtime half of field access and stays read-only. | After M9.6 |
| **ABI stability: is there one at all?** | After the bootstrap |

---

## 13. How this project fails

Named explicitly, because these are the actual risks and they are all
self-inflicted:

1. **Bikeshedding syntax instead of writing the compiler.** §5 is locked for a
   reason. Syntax is the cheapest thing to change later; semantics are not.
2. **Building the standard library ahead of the language it needs.** Library code is the best
   evidence there is, and should be written as soon as the language can express it — but not by
   working around a missing feature that should be built instead.
3. **Attempting Rust's borrow checker.** Lifetimes in the syntax and non-lexical lifetimes took Rust
   years. M9.1 asks how far structural rules go; it is not permission to build Rust's.
4. **Optimising before the language exists.** C is not the bottleneck; the frontend is. The LLVM
   division of labour (§2.2) is decided so that it is not argued later, not so that it is built now.
5. **Rewriting the parser because it is not elegant.** It only has to be correct and produce good
   errors; split it by state ownership when it is too large (§3.2), which is not a rewrite.
6. **Skipping tests during the exciting parts.** Ownership work is exactly where silent breakage
   compounds.
7. **Treating the manifesto as a scope commitment.** It is a direction, not a
   backlog.
8. **Deferring to a phrase instead of a row** (§9).

---

## 14. Bootstrap

Long-term goal: `keelc` written in Keel, compiled by `keelc`.

Do not design for this now. Compiler #1 is disposable scaffolding — that is the
entire reason for the restrained C++ subset in §2.3 and the one-class-one-file rule. The
bootstrap becomes realistic once `kl::list`, `kl::string`, `kl::map`, a file API and error handling
exist in Keel, and it will be a full rewrite rather than a port.

---

## 15. Open work

What is owed and not yet in a milestone row, checked against the compiler on 2026-10-07. A finished
item leaves this list for `.claude/LOG.md`, with what it cost.

### Readability work (proposed 2026-10-07, not yet agreed)

From `.claude/READABILITY_AUDIT.md`, chosen by what M9 to M9.6 will edit. Done: R19's table, R7's
and R14's small fixes, `node.h`'s shape table, and R1: `Ast`'s typed `aux` readers and named-child
accessors, which every pass reads through (only `Field_type` and the tests stay positional), R20's
`Types_builder::poison`, R24's one namespace spelling, `keel::sema`, R2's single take-and-clear
discipline for `Expressions::expected_`, R17's `successors()` in `kir.h`, R14's `lower_arm_body`,
R7's tree queries as `Ast` members, R18's `Lookahead`, R13's `Annotations::resolve` with its long
cases as members, and R9's `ir/instances`, which owns monomorphisation (`instances_to_emit`,
`bindings_for`). A query that reads only the tree and the nodes it is given is a member of `Ast`; one
that needs anything more (`parameter_mode_of`, which answers in sema's `Param_mode`, and the
ownership and binding-type questions) stays in sema. R22 (`const Types&` past the checker) is
declined: lowering and emission intern types the checker never named, and `table()` is `Types`' only
mutable member. **At M9.1**: the dataflow wording out of `main.cpp` (R10) and one test fixture that
runs the real pipeline, prelude included (R11). **At M9.5**: the parser's cursor and recovery as
classes (R3, with R5 and R18), `infer_call`'s phases and a call-site struct (R6, R15), and the
resolver's long cases (R12). **Later, or when next touched**: `Addresses` out of `Expressions` (R4),
the `Types` renames (R8) once files stop moving, R16 and R21, and R19's comment sweep of
`ir/lower.cpp`, `check/` and `parse/parser.cpp`.

### Carried from M8

- **4d. Refill in one statement**: `v[i] = f( move v[i] );`, `state = step( move state );`. Moving
  out of a place is allowed when the same statement's assignment refills it. Sound because Keel never
  unwinds. Rules: the moved place is the target, written identically, with no call in its index, and
  evaluated once (the lowering takes its address once); the right side names the target's root
  nowhere else; the write-back initialises rather than assigns, so D49's drop is skipped for it; a
  global root is refused. **M9's `try` adds a clause**: `v[i] = try f( move v[i] );` returns between
  the move and the write-back, leaving slot `i` empty for the return's drops. The danger is an exit
  *after* the move, so a `try` evaluated before it, `v[i] = f( try g(), move v[i] );`, is fine.
- **Whether `alloc` and `free` become library functions** over one size intrinsic, which would give
  the two names back (D37). Taken whenever it is forced.
- **Growing `kl::list` by one `realloc`**, since a Keel move is a byte copy. Needs a
  `kl_rt_realloc_many`; only an optimisation.

### Soundness and ownership

- **`out` cannot take an owning type.** Assigning one destroys nothing, so it would leak what the
  caller held; the fix is a conditional drop of the argument's place before the call, which drop
  flags already know how to emit. Until then the diagnostic says so.
- **Definite assignment is per local**, so `P p; p.x = 1; return p.y;` is accepted and a struct `out`
  parameter may be left half-written. Per-field state fixes this and lets a field be moved on its
  own; neither is worth it alone, and together they are one change. Pinned as accepted in the tests.
  Until then `kl::pair`'s halves are read and replaced in place, and leave only with the whole pair.
- **A temporary cannot reach a `move` parameter bare**: `consume( Buffer( 16 ) )` is refused, and
  `consume( move Buffer( 16 ) )` is the spelling. Worth lifting with M9.1's temporary-lifetime rule.
- **A payload enum is a struct of every variant's fields, not a union**, so it is as large as all
  its payloads together. Invisible while nothing guarantees an enum's layout; the fix is confined to
  the emitter.

### Diagnostics

- **A bound's help names a method, not its type**: `left == other` in a method of
  `class pair<T> where T : Copyable` says *write `where T : Equatable` on `same`*, but a method writes
  no `where`; the clause goes on `pair`.
- **Copying through `*p` suggests `move *p`**, which the next line refuses. `[]` was given the
  container help instead; `*p` wants the same.
- **`unsafe { free( nope ); }` gives two errors**: the undeclared name, and *an `unsafe` block that
  does nothing unsafe*. A gated operation should count as used whatever its operand, as indexing does.
- **`new P` gets the parser's generic *expected an expression*** rather than a message naming
  `alloc` (D10).
- **`parse_block` reports twice when the opening brace is missing**: it proceeds into its loop and
  then consumes the enclosing `}`. Carrying on is right; it should tell a missing brace from one that
  was never coming.
- **Chained assignment, `a = b = c;`, says *expected `;`, found `=`*.** Correct but unhelpful; it
  deserves a targeted message.
- **A `switch` arm ending in `while( true ) { }` is reported as falling out**: `completes_normally`
  answers over the AST and treats every `while` as finishing. It needs to ask whether the condition is
  the literal `true` and no `break` binds to the loop. Pinned as accepted in
  `type_checker_reports_an_arm_that_falls_out`.
- **A receiver's borrow in a move-and-borrow error underlines the whole call** (`h.eat( move h )`),
  since KIR operands carry no span. The `TODO` on `Statement` in `kir.h` is the fix.

### Language rules not yet stated

- **`f( 1 + 2 )` against `f( i32 )` and `f( i64 )` picks `f( i32 )`**, because `argument_shape` types
  an arithmetic of literals as its default. A bare literal is ambiguous there; whether a chain of them
  should be too is undecided.

### Compiler structure

- **A node that carries a name must not be built without one**: if `aux` is a `Symbol_id`, the parser
  guarantees it is valid, so no reader needs a guard.
- **`can_start_expression()` and `parse_prefix`'s keyword case are two halves of one list**, and
  three features in a row needed both edited. A keyword missing from the first fails only in statement
  position. They want one predicate.
- **A literal kind is a five-place edit** (`infer`, `check`, `is_literal_expression`, lowering, the
  emitter), and missing one fails silently.
- **`found_text()` returns a formatted fragment, not raw text**, which its name hides; a caller adding
  its own backticks would double-quote.
- **A block with one predecessor is not merged into it**, so `if( false ) { ... }` before a `return`
  still emits a label and a jump. An M11 candidate.
- **The golden runner has no UBSan mode.** `KEEL_VALGRIND=1` earns its place; UBSan catches a
  different class (the shift count in §12) and runs today only when someone sets `KEEL_CFLAGS`.
