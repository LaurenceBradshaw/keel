# Keel — Implementation Plan

**Status:** M0–M6 complete (2026-09-18). Generic functions, bounds, literal adoption and
generic aggregates work, methods, constructors and destructors included; §12's M6-deadline
questions are **all decided**, and the four that carried implementation — D41's
mixed-signedness comparison, `cast`/`wrap` on a type parameter, **overloading** and
**type-argument inference** — are all done. The last M6-adjacent row, **function pointers**,
is **decided**: FFI is the surviving customer, and member pointers come with them — where a
member *function* pointer turns out not to be a second feature at all, because L13 has no
inheritance for one to adjust for. **M6.5 is in progress**, and it is debts rather than a
feature. The two KIR passes carried from M5.5 are **done (2026-09-18)** as one pass,
`ir/simplify.cpp`, which closes the one *wrong answer* the tree held —
`while( true ) { return 1; }` reported as a path with no return. §12's **empty-aggregate
rejection** is **done (2026-09-19)**, and it kept the `kl_rt_alloc` guard it was expected to
delete — see §15. §12's **conditional expression** is **decided and built (2026-09-19)**: `?:`
ships, `if`-as-expression is **rejected** rather than deferred, and the type rule is that an
expectation decides and with none the arms must match exactly.
**M6.5's original list is now empty, and it has been given a second one (2026-09-19)**: splitting
`sema/type_checker.cpp`, the mangling category tag, two one-rule holes — `g().t = 1;` and
`enum Nothing { };` — and a decision on the `Arena`, which still has no caller after both of the
ones §15 predicted were decided against. **The split is done (2026-09-19)**: `Checker` now lives
in `sema/checker.h` with its definitions across nine files, none over 1,373 code lines, and no
change to a single assertion or golden. **M6.5's second list is now empty too (2026-09-24)**: the
tag, both holes, and the `Arena` are all closed, and the `Arena` was **deleted** rather than wired —
its third predicted caller was measured and did not pay, see §15. **Function pointers moved to M7**, not M8: M8 is the
library, and §12's own timing rule already put member pointers with access control, which is M7.
**M7 is now static methods and the old M7 is M8** (2026-09-19) — §9 records why the debt got
a milestone rather than a place in the library's. §15 is the live tracker.
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

- No exceptions. No RTTI.
- No inheritance and no virtual functions **in this compiler**. This is a style rule about
  compilers, not a statement about Keel: **Keel will have inheritance and the dynamic dispatch
  that comes with it** — virtual methods, overriding, abstract bases. It is not in v0 (L13, D29)
  and is not yet scheduled to a milestone, so the bootstrap will reach this code before it
  exists; but "Keel will not have it" was never the reason and this list should not be read as
  saying so. Corrected 2026-09-19, after the reading had been used to justify a design.
- No templates beyond using the standard containers.
- Standard containers, `string_view`, `optional`, `span` — yes.
- Style is "C with containers and namespaces."

Two reasons for the restraint: it is the correct style for compilers, and every
line here will eventually be rewritten in Keel during bootstrap. Do not lean on
features Keel does not have — which for most of this list means *will never have*,
and for inheritance means *does not have yet*.

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
  AST              flat vectors, u32 handles, every node has a Span
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
  kir.h          the vocabulary; no logic - the accessors are inline, so there is no kir.cpp
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

**Correction (2026-09-19), found while auditing the split this section's premise
justifies.** *"A class cannot span translation units"* is false as written, and
the rule built on it is narrower than it looks. A class **declaration** must
appear in one place; its **member function definitions** may live in as many
translation units as you like, which is exactly how Clang's `Sema`
(`SemaExpr.cpp`, `SemaDecl.cpp`, `SemaOverload.cpp`, …) and Go's
`types2.Checker` (`expr.go`, `stmt.go`, `call.go`, …) are organised. What is
actually true is the narrower claim: a *file-local* class cannot, because
nothing outside the file can name it — and `Checker` is one, declared inside an
anonymous namespace that spans lines 16 to 7,642 of the `.cpp`, nearly the whole
file. So this entry's *conclusion* holds and only its *reason* was wrong: the
obstacle is internal linkage plus a declaration that was never written into a
header, not a property of classes. Moving the declaration into one, and out of
the anonymous namespace, removes it. **This does not retire the free-function
rule for KIR**, which was
chosen for passes that are genuinely independent of one another and has been
vindicated by `simplify.cpp`. It does mean the rule was justified here by a
constraint that does not exist, and that the checker's split is a choice about
what is clearest rather than an accommodation of the language.

**The working document was `docs/TYPE_CHECKER_SPLIT.md`** — the measurements, the file layout,
the step sequence, and what Go's `types2` was read for. It did its job and is deleted; everything
that outlived the work is in §15 and in the rest of this section.

**Stage A landed on 2026-09-19.** `Checker` is declared in `sema/checker.h` and its 127 member
definitions are spread over `sema/type_checker.cpp` (the spine: `run`, `visit`, `record`,
`error_at`, `absorb`, `type_of_annotation`) and eight `sema/check_*.cpp`, one per construct.
No implementation file exceeds 1,373 code lines against a ceiling of 1,500 — where Go's
`types2` holds, and tighter than M6.5's stated 3,000. Behaviour is unchanged: the same 7,072
assertions in 602 test cases and the same 172 goldens pass, before and after.

**Stage A met M6.5's acceptance and did not improve the code, and that is the finding that
matters.** Splitting the file changed nothing about readability, because the cut was along the
grammar and the entanglement is in the state: every function could still reach every field. Two
measurements say what the shape should have been. Of `Checker`'s 26 private fields, four are
broadly shared (`ast_` 104 members, `table_` 75, `types_` 36, `interner_` 31) and **twenty-two
are touched by twelve members or fewer, most by two or three** — `bounds_` by three,
`unsafe_depth_` by two. And eleven of the fourteen classes that partition implies draw 75% or
more of their lines from a single Stage A file. So the neighbourhoods were right and the walls
were missing.

**Stage B was therefore not a lift of free functions; it is fourteen classes, one per file,
with a DAG between them** — `Types_builder`, `Reporter`, `Aggregates`, `Bounds`, `Annotations`,
`Constant_folder`, `Places`, `Generic_recursion`, `Literals`, `Operators`, `Overloads`,
`Expressions`, `Coverage`, `Statements`, `Signatures`, `Checker`, plus `Callees`, which the design
forced out as a third level-0 collaborator because `Places` reads what `Overloads` writes and
neither may name the other. A service takes types and spans rather than a `Node_id` it intends to
walk, which is what removes the callbacks and what stops a rule function from wandering into one
more check. Rescoped 2026-09-19, run through B0–B14, and **closed out on 2026-09-24**.

**B0 is done (2026-09-19)**: `sema/types_builder.{h,cpp}` owns `table_` and `types_`,
`sema/reporter.{h,cpp}` owns `sm_` and `diags_`, and `Checker` is down four fields with no
assertion or golden changed. It also settled the thing that made the rest look expensive: a
**shared** field does not have its readers rewritten, only its owner — `table_` becomes a
`Type_table&` bound once, exactly as `ast_` already is, so all 370 of its call sites move
untouched at B1–B13. `Reporter::previous_declaration_note` took a `Span` instead of a
`Node_id` on the way, which is the service inversion arriving early because it was free.

**B1 is done (2026-09-19)**: `sema/aggregates.{h,cpp}` owns `struct_order_` and `owning_` and
answers the shape questions — containment order, the owning set, member and field lookup,
`bindings_of`, `field_type`. `is_owning_type` split into `Aggregates::owns` (concrete
aggregates, asks nothing) and a dispatcher on `Checker` that dies at B2. B0's shared-field rule
held exactly: fifteen call sites gained a prefix and nothing else moved. Two things it added.
A lower class that needs to write a *higher* class's field should **return** the answer instead
— `order_structs` hands back the aggregates it found on a cycle and `Checker` seeds `expanding_`
from that — which is §4.3's inversion again, and it will recur. And a class that takes no nodes
it walks can be unit-tested with a parsed tree and nothing else: writing those five tests found
that no golden and no unit test covered leaving a generic aggregate out of the struct order, a
gap that predates the split.

**B2 is done (2026-09-20)**: `sema/bounds.{h,cpp}` owns `bounds_` — D40's table, the closure, the
bound queries, `check_bounds`, the admissible-set machinery and `declare_type_parameters`.
`Checker::is_owning_type` is gone; `!bounds_.satisfies( t, Bound::Copyable )` is the whole of it.
Three things it added. The **vocabulary leaves with the class**: `Bound`, `Bound_set` and
`Conversion` could not stay in `checker.h` once `checker.h` includes `bounds.h`, so §5.2's claim
that they stay there forever is corrected — expect one header move per remaining step.
`current_function_`, the field §5.2 could not place, **became a parameter** for the first time,
which is what that section said to do until B8 decides. And **inverting a predicate is where a
pure move stops being pure**: six of the ten `is_owning_type` sites already carried a `!`, two
came out inverted, and only the suites caught it — the compiler cannot check a sign, so delete the
old declaration first and work its list one site at a time. Ten direct `Bounds` tests came with
it, and one `Typed` case moved because its subject did.

**B3 is done (2026-09-20)**, in two commits. `sema/annotations.{h,cpp}` owns `type_of` (was
`type_of_annotation`) and `resolve_type_arguments` — the annotation subtree, the one walk that is
not the expression walk. It is the first of the fourteen that **owns no state**: §4.2 put the two
members together because they share one recursion, `Box<Vec<i32>>` being both at once.

The first commit was not the move. B2 had given `check_bounds` a `current_function` parameter so
its help line could say which declaration to put a `where` clause on, and that was the wrong
question: the declaration to name is the one that introduced *the argument's* type parameter,
which for a field of a generic aggregate is the aggregate and not any function. `struct Box<U>
where U : Numeric { U v; }; struct Pair<T> { Box<T> b; };` aborted in `Ast::aux`, because the
signature pass has no current function. `Bounds::declare_type_parameters` already knows the
pairing, so it records it and the parameter is gone. Doing that first is what kept B3 a pure move
instead of threading a field through twelve call sites and unthreading it at B8. **§5.2's
ownerless field is forced less often than it looks** — ask what the reader actually needs first.

**B4's first commit is a bug fix (2026-09-20).** `fold_float` handled three node kinds where
`fold_integer` handles seven, so a file-scope float whose initialiser was not built purely from
float literals folded to nothing, and `visit_global` recorded no value — emitting the global
uninitialised with no diagnostic. `f64 a = 1;` compiled clean and compared unequal to `1.0` at run
time; every cast dropped the same way; and `f64 x = 1.0 / 0;` was accepted while `1.0 / 0.0` was
refused, because the operator check asks the float pool about a divisor only the integer pool
holds. Two cases close all three. **Which pool a literal lives in is the lexer's decision, and the
checker never moves it** — adopting a float type does not carry an `Int_literal`'s value across,
so anything reading one has to ask by node kind rather than by recorded type. `checker.h`'s claim
that a missing entry in `constants_` is an internal error had been false for as long as the folder
existed, and no golden covered it: every file-scope float in the suite is a bare float literal.

**B4's second commit is the move (2026-09-20).** `Constant_folder` takes five collaborators — the
tree, the literal pools, `Types_builder`, `Bounds` and `Reporter`. `Interner` was on the predicted
list and turned out unused, so the class does not take one. `constants_` moved with it behind
`record_value` / `value_of` / `take_values`, because `Expressions`, `Coverage` and `Statements` all
read the map and none of them may name `Checker`. `check_constants.cpp` is gone — the first Stage A
file fully consumed, which is what "100% aligned to one file" was supposed to mean. Behaviour
unchanged: 7,197 assertions in 635 test cases, 172 goldens in debug and release, all identical to
before the move, which is the only evidence a pure move can offer.

**A move can widen linkage, and no test will say so.** The nine folding helpers arrived at
`keel::sema` scope instead of the anonymous namespace they left, giving `folded`, `equals` and
`less_than` external linkage under names generic enough to collide later — B2's finding a second
time, at a step that had already recorded it. The same reading found a `#include` of the new header
duplicated with two different spellings, and `<limits>` left behind while `std::numeric_limits`
came along. §6 says to grep for the old names before calling a step done; that catches a renamed
reference and none of these. **A lift is also not finished when its code compiles** — all thirteen
`TEST_CASE`s stayed in a file that then held nothing else, though §6's own rule says a `Typed` case
moves when the member under test is the one moving, and here every one of them was.

**B5 is `Places`, in two commits (2026-09-21).** Ten members leave `check_statements.cpp`,
`check_declarations.cpp` and `check_aggregates.cpp` for `sema/places.{h,cpp}` — place roots,
borrows, writability and the two passes that record which parameters travel by address. Seven
collaborators, and no field of its own: the first rule class in the stack that owns no state,
which is worth saying because §2 justified the whole redesign by state ownership and this one is
a counterexample the design accommodates rather than a hole in it. Behaviour unchanged, 7,197
assertions in 635 test cases and 172 goldens in debug and release.

**The DAG stopped a step for the first time, and only when the class was being written.**
`is_assignable` has to know which callable a call resolved to, and `callees_` was assigned to
`Overloads`, four levels below `Places`. Nothing in the plan predicted it, and nothing in the
tree would have shown it: a dependency that runs the wrong way up the DAG is invisible until the
class that must not have it exists. The answer is the one that produced `Reporter` — a third
level-0 collaborator, `Callees`, owned by `Checker` and read by both sides, which is the shape to
reach for whenever two classes on opposite sides of the order need one map.

**The first commit's own tests are the ones worth having here.** `Callees` is thirty lines and
its mutations are lopsided: dropping the write and emptying the hand-off both *abort the
compiler* in lowering rather than producing a diagnostic, so 66 goldens fail and the unit binary
dies partway. Blanking the lookup is the interesting one — two unit cases catch it and all 172
goldens pass, because no golden program both returns a `const ref` from a method and binds the
result. A survivor on one side of a two-sided battery is the reason for running both.

**B6 is `Generic_recursion`, in one commit (2026-09-21).** The five members of
`check_generics.cpp` — `record_generic_uses`, `record_instantiation`, `expands_forever`,
`wraps_a_parameter` and what was `check_generic_recursion` — move to `sema/generic_recursion.{h,cpp}`
with `generic_calls_`, `instantiations_` and `expanding_`. Two members are new and neither is a
rule: `record_call` replaces two raw `push_back`s into the graph and owns the `is_generic` guard
that used to sit at both of them, and `mark_reported` gives B1's inversion a name where
`check_declarations.cpp` had been reaching into `expanding_` directly. It is the first plain lift
since B2 — no state had to be threaded, no collaborator had to be invented, and every caller was
already below it in the DAG. It is also the first step that **empties a Stage A file of code**:
`check_generics.cpp` is now tests only, and drains as B7 to B10 take the classes those tests pin.

**A class that only needs level 0 can sit anywhere in the DAG.** `Generic_recursion` names `Ast`,
`Interner`, `Type_table`, `Reporter` and the free `is_generic`, and nothing else — it would be
legal at #1 as readily as at #6. That is worth writing down because §5's ordering has so far read
as a constraint discovered late, and here it constrains nothing. It is also the first class to take
`const Type_table&` rather than `Types_builder&`: it asks the table three questions and records no
type, so taking the builder would be an unused dependency dressed as a real one — the same argument
that deleted `Checker::parameter_mode` at B5, arriving before the code was written rather than after.

**The survivor was the guard, not the walk.** Four mutations. Blanking `wraps_a_parameter` dies to
three unit cases and two goldens, and dropping `record_instantiation`'s deduplication dies to one
unit case — `type_checker_deduces_type_arguments`, which asserts a deduced call and a written one
are one instantiation — while passing all 172 goldens. The prediction that nothing covered
deduplication was wrong, and the test that covers it was written for the overload rules rather than
for this. The genuine survivor is `record_call`'s `is_generic` guard: removing it passes 636 unit
cases and 172 goldens. It records edges out of `main` that are not steps towards anything, and
nothing downstream currently looks. The guard is still right — the walk reads every edge as a step —
so the answer is a test rather than a deletion, and
`generic_recursion_records_an_edge_only_from_inside_a_generic` is it. Second time in Stage B that a
lift's mutation battery has found a pre-existing gap rather than a new defect; B1's was the same shape.

**`keel::Literals` became `Literal_pool` first, in its own commit (2026-09-23).** Row 7's class
is `Literals`, and `keel::Literals` is the lexer's value store — two classes of one name, one of
them in an enclosing namespace of the other. C++ resolves that quietly and wrongly: inside
`class Literals` the injected-class-name wins, so `const Literals& literals_` declares a reference
to itself, and in `checker.h` the `Checker` constructor's own parameter changes meaning the moment
the new header is included. Both are caught by the build, neither by reading. The file said
`Literal_pool` in its prose already — its class comment calls it a pool and its test was named
`literals_pool_round_trips_values` — so the rename only wrote down a name that was in use. Singular,
to match `Literal_id` beside it. `common/literals.{h,cpp}` moved to `common/literal_pool.{h,cpp}`,
which also removes a second, quieter collision: two `literals.cpp` in one build. Four comments that
say "literals" about values rather than about the class were left alone, which is the part a
whole-word substitution gets wrong.

**B7 is `Literals`, in one commit (2026-09-23).** `is_literal_expression`, `infer_literal`,
`check_literal`, `standalone_literal_type` and `warn_if_constant_comparison` move to
`sema/literals.{h,cpp}` — the membership test that counts `-5` as a literal, the no-context
fallbacks, the 204-line core that pushes an expectation through a unary minus so
`i32 x = -2147483648;` range-checks as a negative and asks of a type parameter whether the value
fits every type `T` may become, and D41's two halves: the type a literal takes when the operand
beside it cannot hold its value, and the warning that the comparison this made legal was never in
doubt. A second plain lift — the bodies reach only `Ast`, `Type_table`, `Literal_pool`, `Bounds`,
`Constant_folder` and `Reporter`, make no `infer` or `check` call, and every caller was already
below them. It is the first step to **delete** a Stage A file rather than empty one:
`check_literals.cpp`'s nine tests all pin this class's own rules, so they moved with it.

**A class can be earned by a shared question rather than by shared state.** `Literals` carries no
fields at all — seven references and nothing else — which is the first time in Stage B, and the
measurement §15 leans on would have said to leave it in `Checker`. It is one class for the reason
`Annotations` is: its members answer one question, "what type does this literal take, and from
where", and splitting them would put the membership test in one file and the rule it gates in
another. It is also the first class **not** to take the `Interner` — `interner_` appears nowhere in
the five bodies, and taking one anyway would have been an unused reference that no warning catches.
`Bounds` and `Constant_folder` are held `const` for B6's reason: every method asked of either is.

**A field can die of the move that reads it.** `Checker::literals_` looked like shared state and was
not: `bounds_` and `constant_folder_` bind the pool from the constructor *parameter*, so the five
moving members were its only readers and the move left it dead. `Checker` now holds no literal pool
at all and only forwards the parameter. Worth doing at every step — ask what actually reads a field
before assuming the class keeps it — and it is §5.2's question asked about one field rather than
about `current_function_`. The battery is five, one per member, and all five die to unit cases: the
negated-literal arm of the membership test, the `negative` flag, the standalone-type escape hatch,
asking both operands rather than one, and a char literal defaulting as a `u8` code point. No
survivor, unlike B1's battery and B6's.

**B8 is `Operators`, in one commit (2026-09-23), and it is the first inversion.** `infer_binary`,
`infer_unary`, `infer_cast` and `infer_conditional` stop taking a node: the caller infers the
operands and asks for a result type. `sema/operators.{h,cpp}` answers five questions — where a
binary operator's result type comes from, what a binary operator answers for two operand types,
what the four rule-table unary operators answer, what a ternary answers when both arms are typed,
and which conversions `cast` and `wrap` permit with the help for the ones they do not. Five `const`
references and no fields, the second such class after `Literals`. 483 lines leave
`check_expressions.cpp`, which goes 3,023 to 2,295.

**An inversion cannot be proved the way a lift can, so prove something else.** Every step before
this one came out byte-identical under two or three mechanical rewrites; this one rewrites every
`return` in the bodies it moves, because `record( id, T )` becomes `return T`. What replaced the
diff, and what the remaining inversions should copy: first a multiset diff of every string literal
across the old file and the two new ones — 82 in, 82 out, none lost, none gained, none altered —
which is the mechanical half and catches a diagnostic that silently became a different diagnostic;
then a normalised structural diff per function, read by eye, confirming every remaining hunk is one
of the intended rewrites. The thing being looked for is an early return that used to record the
error type and now returns a valid one, or the reverse, which is a failure mode the plain lifts
did not have.

**A rule's machinery can have a reader the members do not.** The plan predicted which *members*
had to invert and was right about all four; it missed that `check()` reads the rule table directly,
to know which operands an expectation may flow into. That is neither a node question nor a type
question, so no inversion removes it — it needed a query, `result_source`, which then lets the
table itself stay private. **Before inverting, grep for every reader of the machinery, not only of
the members.**

**Threading beats a shared field, and B8 is where that stops being an open question.** The field
with no owner, `current_function_`, is read by `Operators::result_of` for one help line at one call
site. Across all of Stage B it has now been threaded to nine call sites in three classes, no class
has wanted the shared-field alternative, and the one time it looked forced it turned out to be a
bug. A class that takes it holds an `Ast&` to render it and never walks it, which is worth a line
in the class comment, because an `Ast&` on a service class otherwise reads as the inversion having
failed.

**An inversion is the first time a rule's test coverage is visible, and B8's battery is the proof.**
Eleven mutations. Seven died to unit cases. Two died only to a golden — a shift answering its right
operand's type, and a refused generic conversion dropping the witness clause — which says the unit
suite had no direct hold on either. **Two survived everything**: `-u32( 1 )` was accepted with the
signed-only check removed, and nothing anywhere noticed, though it is D5's founding example; and
having `check()` treat a shift like ordinary arithmetic pushed the expectation into the shift
*count* without failing the test that exists for exactly that rule, because both operands of
`1 << 4` are literals and the assertion is about the left one. Three new `TEST_CASE`s close all
four. The earlier batteries were not weaker — the rules simply were not reachable on their own
before, so their coverage could not be measured. **B9 confirmed it**: four survivors again, two of
them caught by nothing at all. Expect B11 to find the same thing.

**Testing a rule directly costs eleven lines, and the DAG is why.** Four of `operators.cpp`'s
`TEST_CASE`s construct the class and ask it about types with no source string anywhere — the first
tests in `sema/` that do not run the whole front end. The fixture is an `Ast`, an `Interner`, a
`Source_manager`, a `Diagnostics` and a `Literal_pool`, all default constructed, then a `Reporter`,
a `Types_builder`, an `Aggregates` and a `Bounds` to reach an `Operators`. Every one of them is a
level below, which is the entire payoff of insisting a class may only name classes above it: the
stack assembles without the checker. Eleven lines is cheap enough that a rule about types alone now
has no excuse for being unpinned.

**Private vocabulary leaves the header rather than moving to the new one.** `Operands` did not go
from `checker.h` to `operators.h`; it went into `operators.cpp`'s anonymous namespace, because its
only reader, `accepts`, reads no field either and became a free function over the type table. The
class's public surface is five members. The whole of the old anonymous namespace moved with it, 102
lines, with no reader anywhere else — a boundary the file already had.

**B9 lifted overloading out, and it is the largest class in the stage.** Sixteen members — ten from
`check_calls.cpp` and five from `check_aggregates.cpp`, plus `instantiation_of_` — into
`sema/overloads.{h,cpp}`: which callable a name means at a call site, what its type arguments are,
and whether two declarations of one name could ever be told apart. `check_calls.cpp` falls from
2,538 lines to 1,388 and keeps only what a call *expression* is; `check_aggregates.cpp` from 1,043
to 749. The class takes nine collaborators, the most of any in the stage, and holds six of them
`const` by B6's rule. It does not name `Places`, which the plan had assumed it would.

**§4.3's rule had to be narrowed rather than extended.** "A service takes types and spans, not
nodes" is unachievable for this class — it reads parameter lists, renders signatures and walks the
root for the overload sets — and it was never what the acyclicity needed. The rule that holds is
that a class may read *declaration* subtrees and may not re-enter the expression walk, which is
checkable: grep the new file for `infer(`, `check(` and `visit(` and expect nothing. `Places` has
been obeying that version since B5.

**A lazy call into the walk inverts into two entry points, never one.** Both selectors called
`argument_shape`, which infers, and both called it only after arity had failed to settle the
candidate set — because typing an argument cannot be taken back. Building the shapes eagerly would
have inferred arguments that today are checked against a parameter's type, changing diagnostics in a
way no assertion count would show. So each selector became a pair with the caller's walk in between.
And `check_call_arguments` needed splitting for a second reason the plan had not seen: its `ref`
rule reads the type the `check()` two lines above it had just written, so batching the walk left
that read empty. **When inverting a call into the walk, look for what the re-entry wrote, not only
for the re-entry.** The suite caught it in two cases, immediately.

**The B9 battery is thirteen and the shape of the finding has changed.** Seven died to unit cases at
once; four survived the unit suite and two of those survived everything, and all four are now
closed. The two the goldens caught were the `move` exemption on a non-owning type and deduction
forgetting which argument bound each parameter; the two nothing caught were an argument past the
last parameter never being walked, and a duplicate `main` losing its "previous declaration" note.
Two mutants still survive and both are equivalent rather than unpinned — and the second is equivalent
only because of a real defect it uncovered: **a method candidate list names `T` where the author
wrote `Box<i32>`.** `no `pick` takes 0 arguments / the ones declared take `( T )` and `( T, T )``
should read `( i32 )`. `signature_of` declines to substitute because a method's `type_param_list`
yields the aggregate's parameters and the receiver's bindings are not keyed by them. Not fixed in
B9, which changes no behaviour; it is a diagnostic naming a type the author never wrote, which is
the thing §5's substitution rule exists to prevent.

**B10 lifted the expression walk out, and it was the cheapest step in the stage.** Twenty-four
members and 1,564 lines into `sema/expressions.h` with definitions across `expressions.cpp` and
`expressions_calls.cpp` — one class, two translation units, because 1,564 code lines plus 2,063 of
tests is larger than the monolith Stage A set out to cut up. Both `check_expressions.cpp` and
`check_calls.cpp` are gone. A normalised per-function diff makes **twenty-six of the thirty-one
members byte-identical to their originals and none changed**. The first nine steps were expensive
because they were lifting rules *out* of the walk; a class that *is* the walk needs nothing
inverted to become one, which is the cost curve the plan had exactly backwards.

**Inverting a callee makes its caller longer, and nothing had said so.** The split plan predicted
`infer_call` would "shrink substantially" once B9 took selection and deduction out of it. Measured,
it went **259 → 295**, with `infer_method_call` 88 → 103 and `infer_implicit_method_call` 62 → 79.
A two-phase selector puts the sequencing in the caller by construction, so those 68 lines are the
price of B9's inversion, charged to the walk rather than saved anywhere. A plan that books a
shrink on both sides of an inversion has double-counted.

**The field with no owner had one: it belongs to whoever is the walk, not to whoever asks.**
`current_function_` was the one of 26 the split plan could not place, and it offered a threaded
parameter or a shared `Walk_context`. Both were wrong: every reader is either inside `Expressions`
or above it in the DAG, and the three classes below already take it as an argument. It is a private
field of `Expressions` with a scoped enter/leave pair the statement walk calls — which is exactly
what the other four walk-state fields do. **Ask which walk a piece of state belongs to before
asking which class**; the `Walk_context` is struck.

**One helper crossed the boundary the wrong way, and the compiler found it.** `check_condition`
was shared by `if`, `while`, `for` and the ternary, so it read as a statement rule. Its body infers
one node and reports one message, which makes it an expression rule that statements call, and it
moved *down* to `Expressions` rather than leaving a cycle. **When a helper is shared across a layer
boundary, ask which layer its body is in, not which layer calls it.**

**B11 inverted `Coverage`, and the inversion was two lines.** `sema/coverage.{h,cpp}`, seven members
and 510 lines of code out of `check_statements.cpp`, plus `Interval` out of `checker.h` and
`quoted_list` out of the statement file's anonymous namespace. The two lines are the
`visit( arm_children.back() )` that sat at the bottom of the enum and numeric arm loops. They could
not simply be hoisted above the loop: an arm's pattern declares names its body then reads, so labels
and bodies interleave, and hoisting would have reordered every diagnostic a `switch` produces. So
**the loop moved to the caller instead** — `Statements::visit_switch` drives it, and `Coverage`
answers one arm at a time through `begin_switch`, `check_arm_labels` and `finish_switch` over a
`Switch_coverage` the caller holds. Two members became six. The rule generalises past this class:
**when a callee's loop interleaves with something only the caller may do, move the loop, not the
call.**

**Who calls a helper settles where it goes, when the helper reads nothing.** `completes_normally`
was booked to `Statements` by association with the `visit_*` family. It reads only the `Ast` and has
exactly one caller, `check_arm_structure`, so it is `Coverage`'s — and taking it is what makes the
class answerable without naming the walk. That is the same question B10 asked of `check_condition`
and got the opposite answer to, because there the body was in a different layer from every caller.
Ask about the body first; ask about the callers when the body settles nothing.

**A class's collaborator list is readable off its last ten lines, not off its description.** The
reading that planned B11 predicted six collaborators for `Coverage` and the build named eight:
`check_variant_pattern` ends in `record( bindings[i], aggregates_.field_type( type, payload[i] ) )`,
so it needs `Types_builder` and `Aggregates` too. Third step in a row where the compiler found a
dependency the reading missed, which is an argument for doing the wiring before the prose rather
than after.

**Two shipped defects in D34's numeric coverage, both found by mutation and neither by reading.** A
single `case` label is stored as the one-wide interval `[v, v+1)` and overlap is a symmetric
pairwise test. Dropping the width, so the interval is `[v, v)` and empty, means `case 1: case 1:` is
no longer reported as a repeat. Dropping the second conjunct of the overlap test means
`case 5: case 1..3:` *is* reported as overlapping, because the test stops being symmetric and source
order starts to matter. Both survived 649 unit cases and 172 goldens. Both now have a `TEST_CASE`.
The shape to remember is that **both halves of a two-part invariant need their own mutation** — one
mutation of the pair would have found neither.

**B12 is the first step in Stage B where nothing had to be restructured.** `sema/statements.{h,cpp}`,
twelve members and 537 lines of code — the eleven `visit_*` out of `check_statements.cpp`, which
the step deletes, and `Checker::visit` out of `type_checker.cpp` — plus the `record` and `error_at`
forwarders every class in the split carries. **All twelve moved byte-identical**
modulo the class rename, against B10's twenty-six of twenty-eight and B11's five of seven. The
reason is the row itself: a class that is only dispatch reaches nothing except collaborators it is
allowed to name, so there is nothing to invert and nothing to split. The corollary is worth having
before B13 — **a step's cost is set by what its members reach, not by how many lines they are.**

**The DAG claim that `Statements` sits above everything now has its second grep.** B10 settled
`visit(` appearing nowhere in `Expressions`; B12 settles the other direction — the only caller of
`visit` outside the class is `Checker::run`, and `statements.{h,cpp}` name nothing in rows 13 or 14.
Eleven collaborators, five of them `const`.

**A save/restore pair is one fact per construct, not one per field, and mutating the second field
re-measures the first.** `Statements::visit_function` saves and restores `current_return_` and
`current_function_` on adjacent lines, both against nested functions the language does not have
until M6's lambdas. B10 measured `current_function_` and found `enter_function` called 2,972 times
with the field invalid every time. B12 mutated the other one and got the same answer from a
different counter: `visit_function` is entered **3,089 times** across 652 unit cases and 172 goldens
(2,207 and 882) with the enclosing return type valid **zero times**, while a positive control on the
same line fired 3,089. One equivalent mutant out of sixteen, and it is the same insurance measured
twice — which is a reason to record the *construct* as probed rather than the field.

**An unearned-include probe has to be a name grep, not a build.** Removing `<algorithm>` from
`type_checker.cpp` still compiled, because something it includes provides it; the file names
`std::find` and the include is earned. `lex/token.h` in `checker.h` failed both tests and is gone —
`Token_kind` and `Keyword` left with the `visit_*` family. **Compiling without an include proves
only that the include is redundant, which is not the same question.**

**Attributing a test by the file that emits its message caught a fixture in the wrong class, and
the gap behind it.** The unruled-`fallthrough` branch is `Statements::visit`'s, but its only case
was a section of `Coverage`'s `type_checker_checks_fallthrough` asserting nothing more than
`REQUIRE_FALSE( p.clean() )` — and the three placements the help text actually names, inside a
block, an `if` and a loop, had no case at all. Honest about what found it: **mutation did not.**
Flipping the guard either way dies to the section that was already there, so this is a gap in what
the suite *says* rather than in what it covers, and only reading the attribution surfaces it.

**A class may not be split across two files, because Keel cannot express it.** C++ lets a class be
declared once and have its members defined in as many translation units as you like; that device is
what `checker.h` existed for, and it is what let `Expressions` become `expressions.cpp` plus
`expressions_calls.cpp` at B10. Keel has no such form. So a compiler written this way cannot be
written in the language it compiles, and every file that relies on the device is a piece that would
have to be redesigned rather than translated. **This outranks the 1,500 code-line ceiling.** B13 was
built as two files for exactly the reason B10 was — one file would be large — and merged into one at
609 code lines the moment the rule was named. Where the two constraints conflict, the *class* gets
split, not the file. `Expressions` is the one place still outstanding, and it is a real split into
two classes rather than a merge into one 1,716-line file.

**B13 restructured nothing either: fourteen of fourteen byte-identical**, after B12's twelve of
twelve. `sema/signatures.{h,cpp}` took 572 lines from `check_declarations.cpp` and
`check_aggregates.cpp`, both deleted, modulo the class rename and one mechanical infix drop —
`declare_signatures` became `declare`, which turns `signatures_.declare_signatures()` into
`signatures_.declare()` and follows `generic_recursion_.check()`. Two comments in other modules
named the old names and now say "the declaration pass", which the DAG wanted anyway: a row-3 class
should not be naming a row-13 one even in prose.

**Ten collaborators and only two of them `const` — the fewest of any row, and the reason is what the
row does.** Every other class in the split holds several things read-only. A declaration pass holds
almost nothing that way, because it *writes*: it records types, declares type parameters, computes
ownership, records binding addresses, checks overload sets. Probed one field at a time, nine of ten
refuse `const`; only `Ast` and `Interner` survive. **How much of a class's state is read-only is a
fact about what the class is for**, not a hygiene score, and a pass that only writes should look
like one.

**"One mistake, one diagnostic" is invisible to `REQUIRE_FALSE( p.clean() )`, and three of B13's four
survivors were that shape.** A rule that suppresses a *second* diagnostic cannot be caught by an
assertion that the program is dirty, because it is still dirty with two. A struct with two
constructors, and a struct that declares its own destructor beside an owning field, both reported
once and would have reported twice — and every existing section asserted only cleanliness. The
assertion that catches this class of rule is `REQUIRE( p.errors() == 1 )`, and it should be the
default wherever a comment says one mistake yields one message.

**A silent mis-ordering is worse than a loud one, and `record`'s discipline does not catch it.**
Moving `declare_enum_decls()` after `declare_field_decls()` in the eleven-step sequence leaves a
field that names an enum **typed as nothing, with no diagnostic at all** — 653 unit cases and 172
goldens pass, and the compiler then aborts in the emitter on an invalid `Type_id`. The rule that
"every infer branch ends in a `record`, so forgetting to record a type is hard rather than silent"
holds for the *branch* and not for the *ordering*: the record happened, with a type the annotation
had resolved to nothing. The test that catches it asserts the recorded type, not cleanliness.

**An absorption downstream of a gate the error itself closes cannot be mutation-tested.** B13's one
equivalent mutant drops `underlying = i32` after "an `enum` must be backed by an integer". It is
unobservable by construction, not by measurement: the line runs only immediately after that error is
reported (once in the whole suite, against a control of 116 enum declarations), the only readers of
an enum's underlying type are the emitter and the speller, and `main.cpp` returns before lowering
whenever there are errors. The right response is to say so in the comment rather than write a test
that cannot exist — and the previous comment claimed a cascade it could not have prevented.

**The class a test's topic names is not always the class its fixture can live in.**
`type_checker_types_a_generic_aggregate` is about aggregates and went to `aggregates.cpp`, where it
would not compile: that file's fixture is `Shapes`, which parses and types fields *without* the
checker on purpose, and importing `Typed` beside it would undercut the point the fixture exists to
prove. It belongs with the class that emits its messages. **Attribution by emitting file survives
the case where attribution by topic does not**, which is the second time that rule has paid — the
first was B12's misplaced `fallthrough` section.

**The three `TEST_CASE`s owed since B6, B7 and B8 are paid, and `check_generics.cpp` is deleted.**
It was 637 lines of tests and no code. Six fixtures came out of it and three out of
`type_checker.cpp`, every one attributed by the file that emits the message it asserts; assertion
and case counts were unchanged by each move, which is the check that a move was only a move. The
method is mechanical where a test name is not: take the longest run of an asserted message with no
backticked name in it, and grep the tree for that run.

**Probe before declaring an equivalent mutant.** B10's battery is sixteen: eight died to units, four
only to goldens, one to nothing, and five new `TEST_CASE`s took it to thirteen killed by units. Of
the three that survive, all three would have been plausible to argue equivalent — so they were
measured instead, with an instrumented build and a positive control. `infer`'s `default:` arm is
reached **zero times** across 649 unit cases and 172 goldens, because the two expression kinds with
no case of their own only ever appear as `case` labels. `enter_function` is called **2,972 times
with no enclosing function**, because the language has no nested functions until M6's lambdas. Both
are insurance against a language that does not exist yet, which is a defensible thing for code to
be — but arguing it is how a real gap gets filed as a non-gap. B11's one survivor was measured the same
way: growing the coverage vector by a slot changes nothing, because its bound is reached 172 times
across 651 unit cases and 172 goldens and never once fires, and a control with the bound one lower
fired 46 times at the same two sites.

**Comments are reviewed per finished class**, after the suites are green rather than during the
lift — a move that rewrites its comments is no longer a move. `aggregates`, `types_builder`,
`reporter`, `bounds` and `annotations` were trimmed on 2026-09-20, and `constant_folder`,
`callees`, `places` and `generic_recursion` on 2026-09-21 and `literals`, `operators` and
`overloads` on 2026-09-23 and `expressions` and `coverage` on 2026-09-24 — **nothing is currently
unswept**; the rest of `sema/` follows B14.
The same pass catches surface that widened during the move: B2's five file-local helpers had
become public in `bounds.h` and went back to internal linkage. It also catches cross-references
the move invalidated — B3's rename left four comments in other files naming the old function, and
nothing but a grep finds those; B9 left one, `check_aggregates.cpp` naming `check_overload_sets`
after it had become `Overloads`'.

**B14 is a deletion (2026-09-24).** `class Checker` moved into `sema/type_checker.cpp` and
`sema/checker.h` is gone: one translation unit defines its members now, so the header's whole
reason for existing had left with the fourteenth class. What remains of `Checker` is a constructor
and a 33-line `run()`. Nine fixtures were rehomed to the files that emit their diagnostics and
`check_generics.cpp` — 637 lines, tests and no code — was deleted with them, which paid the three
`TEST_CASE`s owed since B6, B7 and B8. Three mutations over `run()` all die.

**Stage B is closed out (2026-09-24), and the honest accounting is in §15.** Fourteen classes plus
three level-0 collaborators, one class per file, and the DAG is **checked mechanically at zero
violations across all seventeen** rather than argued. All nine `check_*.cpp` and `checker.h` are
deleted. Two claims were made for the split and **one of them held**: the DAG is real; the tests did
not get simpler, and `Expressions` went *up* rather than down. What the split actually bought, and
what nothing predicted, is mutation testing per class instead of per compiler — every step from B10
on found a real gap, and the close-out found two more.

**One class in one file is a self-hosting constraint, not a preference.** B10 split `Expressions`
across two translation units because one file would have been large; B13 was about to do the same
and merged instead. C++ permits it, **Keel has no form for it**, so a class written that way could
not be translated when this compiler compiles itself. The close-out merged `expressions_calls.cpp`
back in. This outranks the 1,500-line ceiling the split set itself: when a class is too large, split
the *class*.

**The measurement that decides the shape.** This entry proposed splitting the
checker *into free functions over `Ast` and `Type_table`*. Propagating state use
through `Checker`'s internal call graph — 127 member definitions, 6,258 lines of
bodies — says that reaches **22 functions and 432 lines, 7% of the file**.
Another 45 functions (1,794 lines) touch only the accumulators that become
`Types`, and 60 functions (4,032 lines, 64%) touch the transient walk state —
`expected_`, `naming_variant_`, `current_return_`, the depth counters — and
cannot become free functions without threading that state by hand through every
call. The free-function plan is right for the 7% and wrong for the rest.

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
| **Memory** | ~~Arena-allocate the AST, types, and KIR. Never free.~~ **Amended (2026-09-24).** Every one of the three is a `std::vector` and always was; the `Arena` never acquired a caller and is now deleted. What survives is the half that was load-bearing: *never free*, because the compiler is a batch process and it exits. See §15. |
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
| L3 | ~~Arena +~~ `u32` handles for all IR-ish data. **Reopened and amended (2026-09-24)**, with the written reason in §15: the handles are the part that was load-bearing and they are untouched; the arena was never what stood behind them. |
| L4 | Hand-written lexer and recursive-descent parser. No generators. Pratt parsing for expressions. |
| L5 | Bidirectional type checking (`check(expr, expected)` / `infer(expr)`). No Hindley-Milner, no global inference. |
| L6 | Function signatures and struct members are fully annotated. Inference exists only for `auto` locals. |
| L7 | All lifetime/move/drop analysis runs on the KIR CFG, never on the AST. |
| L8 | **v0 has move checking but no borrow checker.** See §8. |
| L9 | Generics by monomorphisation. |
| L10 | Golden-file tests from the first commit. |
| L11 | **Surface syntax is C++'s** (§5.1). Statement-oriented, explicit `return`, no expression-blocks. |
| L12 | Mutable by default; `const` for immutable — as in C++. See §12; this is the sharpest familiarity-vs-safety trade in the design. |
| L13 | **Two aggregate kinds** (D29). A `struct` is a transparent aggregate: all fields public, trivially copyable, no destructor, no owning members. A `class` has invariants: fields private by default, may own resources, may have a constructor and a destructor, and is moved rather than copied. Neither inherits and neither is virtual **in v0** — inheritance and dynamic dispatch are planned, unscheduled, and not a thing v0 forecloses. |
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

**These samples are compiled.** Every milestone's section is a fixture -
`tests/sema/sample_m1.kl` and its siblings - which must compile clean. They exist
because the samples are the first thing a reader sees and were the only part of
this document with nothing checking them: the M4 section carried `Buffer&` for two
milestones after D32 made it a hard error. A milestone's sample lands with the
milestone, so M5's arrives with M5.

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

// --- M5.5: the runtime floor. `extern` declares what C defines (D36). ---
extern u8*  kl_rt_alloc( u64 n );
extern void kl_rt_free( u8* p );

// --- M3: RAII. Owning types are classes (D29); the destructor is spelled as in C++. ---
class Buffer
{
    u8* ptr;
    u64 len;

    Buffer( u64 n )
    {
        // D36: an FFI call is unsafe, and being the checked wrapper over it is what this class is.
        unsafe { ptr = kl_rt_alloc( n ); }
        len = n;
    }

    ~Buffer()
    {
        unsafe { kl_rt_free( ptr ); }
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
        case Shape::Circle( r ):   return 3.14159 * r * r;
        case Shape::Rect( w, h ):  return w * h;
    }
}

// --- M6: type parameters on the name, bounds in a `where` clause (D39) ---
T max<T>( T a, T b ) where T : Ord
{
    if( a > b )
    {
        return a;
    }
    return b;
}

// --- Error handling: Result plus the `?` propagation operator ---
Result<Config, Config_error> load_config( const ref Path path )
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
type than C++'s integral promotion gives.

**One entry is a deliberate divergence in the permissive direction, and is the
only one**: D41 accepts comparisons C++ compiles with the wrong answer. Thirteen
pairs are involved. Five are pairs C++ accepts today and answers wrongly — `i8`,
`i16` and `i32` against `u32`, where a negative side becomes a large positive
one, and `i32` and `u32` against `f32`, where an integer past the mantissa is
rounded before it is compared; `-Wsign-compare` exists for the first three and
nothing warns about the other two. The remaining eight are pairs no type holds at
all, which C++ also accepts and also answers wrongly, and which Keel answers by
cases. It is listed here because the audit surface is about *differences*, not
about severity, and a reader checking Keel against C++ needs to find it — but it
removes a silent wrong answer rather than creating one, which is the opposite of
what this list usually guards against.

**That claim has been false twice, and both times in `switch`** — an arm running on
into the next, and `break` inside a `switch`. Both are fixed (D38) and the claim
holds again, but it is a property to *test* rather than assert: the way to check it
is to compile the same source with both compilers and compare, which is what found
these. A divergence that produces a different answer rather than an error is exactly
what this list exists to prevent, and a list cannot notice its own omissions. Values agree there until the result
overflows; what happens then is the open overflow question in §12.

| # | Divergence | Why it is safe under the §5.1 rule |
| --- | --- | --- |
| D1 | Fixed-width primitives only: `i32`, `u64`, `f64`. No `int`, `long`, `char`, `unsigned`. | The C++ spellings are **rejected outright**, with a diagnostic naming the replacement. They are *not* reserved words — the lexer treats them as ordinary identifiers, and the suggestion is produced by sema's unknown-type path, which knows it is in type position and so gives a better message than the lexer could. No type name is a keyword: `i32`, `Point` and `Vector<T>` all resolve through one path. |
| D2 | **Ownership transfers only where the call site says `move`**: `consume( move b )`. A bare argument never transfers — for a `class` it is a read-only borrow, for a `struct` a copy (D31). A type is **owning** exactly when it has a destructor, directly or through a member: `class Wrapper { Buffer b; }` is owning, because destroying one destroys a `Buffer`. That transitivity is forced rather than chosen, and it is Rust's `Copy` rule. A raw pointer owns nothing by itself — an address says nothing about who frees it. The query is a memoised walk over the containment graph M2 already builds and orders, and D29 reuses it unchanged: `struct` is legal exactly when the type is **not** owning. Note the rule's real content is **"is it copyable"**; "has a destructor" is a proxy that coincides only because §6.6 puts copy constructors outside v0. `Weak<T>` will have a destructor and should still be copyable, so this needs restating when copy constructors arrive. | The earlier rule made `consume( Buffer b )` a move — C++'s syntax for a copy, carrying different semantics, which is the one divergence §5.1 forbids. A marker at the call site fixes that. D31 then closes the hole it left: with copy constructors outside v0 a class cannot be copied at all, so a bare argument had no meaning available to it except *borrow*. The cost is a marker on every transfer; Rust pays none, but Rust has no C++ copy expectation to fight. **Inert until M3**: no type has a destructor yet, so nothing is owning and nothing changes. |
| D3 | Braces mandatory on every `if`/`while`/`for` body. | Braceless C++ is a parse error, not a reinterpretation. Kills the `goto fail;` class of bug. |
| D4 | `switch` has no fallthrough, needs no `break`, requires exhaustiveness, and matches sum-type payloads. | Payload patterns (`case Circle( r ):`) are new notation. A `switch` over a plain integer keeps C++ meaning minus fallthrough; missing cases are an error, never a silent skip. |
| D5 | No *lossy* implicit conversions. A binary operator widens both operands to the smallest type that losslessly holds both, and is a hard error whenever C++'s own result type would not hold them. Assignment is implicit only where the target holds the source type. int↔bool and pointer↔bool are never implicit. §6.4 has the table. | The second clause makes the §5.1 audit executable: where C++ is lossless Keel agrees with it, and where C++ silently loses information Keel refuses. Lossless widening is not a conversion anyone can get wrong, and requiring a cast for it trains authors to write casts reflexively — which is how the dangerous ones get waved through. **Provisional**: adopted for M1 to unblock the type checker, and expected to be re-judged once real code exists. |
| D6 | `?` postfix operator for error propagation. | No meaning in C++; pure addition. The one borrow from outside the C family, kept because no C++ notation exists for it. |
| D7 | **`enum` variants carry payloads**, and the shape follows from rules Keel already has. A variant's payload is a positional list of named fields: `Circle( f64 radius )`. It is constructed by naming it scoped, `Shape::Circle( 1.0 )`, as D30 requires of every variant name. It is destructured in a `case`, where the scope may be dropped — `case Circle( r ):` — because the scrutinee's type is known there and a bare name can collide with nothing. **A binding in a `case` is a read-only borrow of the payload in place**, exactly as a bare parameter of an owning type is (D31): the enum is still alive and still owns what is inside it, so the binding cannot be a copy — §6.6 has no copy constructors — and cannot be a move, which would tear a hole in a live object. `case Circle( ref r ):` is the mutable form, and reads the same as everywhere else. Variants with and without payloads mix freely: `enum Optional { None, Some( i32 ) }`. **`switch` is exhaustive**: every variant appears, or `default` does. Fallthrough between **directly adjacent labels** — `case Red: case Green: return 1;` — is implicit; anywhere else it is spelled `fallthrough;`, and falling out of a non-empty arm is an error (**D38**, which is where these two became rules rather than intentions). | A payload-free `enum` behaves exactly as C++'s `enum class`, which D30 makes the only spelling; payloads are new notation. Each decision above is the existing rule applied rather than a new one, which is the test this entry had to pass: the binding mode is D31's, the scoping is D30's, and the exhaustiveness is what makes the feature worth having at all. **Fallthrough is restricted rather than refused** because stacked labels are the overwhelming majority of its use in C and C++ and cannot go wrong — an empty label binds nothing — while falling *into* a case with bindings would read payload fields that were never assigned, which is a class of bug C++ never faced because it has no destructuring. **That last sentence described a hazard this entry then failed to prevent**: stacked labels bound every label's payload unconditionally and with no tag test, so the bug was live until D38 refused the construct. The emitted C is a tag plus a union; §6.6 records that this is the one legitimate use of a union and precisely why Keel has no `union` keyword — the tag is the compiler's to maintain, never the author's. **The first cut refuses an owning payload** (see D30), which costs nothing usable: a variant worth owning needs allocation, and that is M5.5. |
| D8 | No headers, no preprocessor. `import graphics;` — C++20's spelling. | `#include` is a hard error directing to `import`. |
| D9 | Reading an uninitialised variable is a compile error. | Strictly rejects programs C++ accepts; never changes the meaning of an accepted one. |
| D10 | No `new`/`delete` in safe code; `Owned<T>`, `Shared<T>`, `Weak<T>` instead. | `new` and `delete` are hard errors outside `unsafe`. |
| D11 | Generic constraints are checked **at definition, not at instantiation**. | Strictly stricter than C++20 concepts, which check at both. Errors point at the generic, not at the expansion — which is the whole reason to pay for it. **The spelling moved to D39**: `T max<T>( T a, T b ) where T : Comparable`, not `template<Ord T>`. This entry is about *when* the check happens and is unaffected by that. |
| D12 | `++` and `--` are **statements, not expressions**. `i++;` and `for( ...; ...; i++ )` are fine; `x = a[i++]` is a parse error. | Removes the pre/post distinction and every sequencing hazard in one move — `a[i++] = i++` is UB in C++ and is simply not expressible here. Rejects valid C++ outright rather than reinterpreting it. |
| D13 | Literal syntax: `_` is accepted as a digit separator alongside C++'s `'`; `\x` escapes take **exactly** two hex digits; unknown escapes and multi-character char literals are errors. | The separator is a pure addition — `1'000'000` keeps its C++ meaning. The rest reject what C++ accepts loosely: unbounded `\x` silently overflows, and `'ab'` is implementation-defined in C++. |
| D14 | A leading zero on a decimal literal is an error: `010` does not compile. | C++ reads it as octal, so `010` is 8 there and would be 10 here. Keel has no octal at all, so accepting it would silently change the value of valid C++ — exactly what §5.1 forbids. Rejected outright with a message naming the cause. |
| D15 | An expression statement must have an effect: only calls, assignments, and `++`/`--`. `a * b;`, `x.field;` and `arr[3];` are errors. | Rejects only statements that compute a value and discard it — already a bug, and already warned about by C++ (`-Wunused-value`). Java and C# both enumerate the legal statement expressions for exactly this reason. Multiplication inside an expression (`x = a * b;`) is untouched. |
| D16 | Operators whose relative precedence is a known C wart cannot be mixed without parentheses: bitwise (`&` `\|` `^`) with comparison, shift (`<<` `>>`) with arithmetic, and `&&` with `\|\|`. `a & b == c` is a compile error naming both readings. | C reads it as `a & (b == c)` — a mistake Ritchie acknowledged and every compiler warns about. Changing the precedence would silently alter valid C++, which §5.1 forbids; rejecting it fixes the wart *and* satisfies the containment rule, since no accepted program changes meaning. Zig takes the same approach. |
| D17 | `*` and `&` bind to the **type**, not the name, and must touch it: `u32* p;` declares a pointer, `u32 *p;` is an error. | C's declarator syntax binds them to the name, so `int *p, q;` makes `q` an `int` — a wart every C style guide works around. Adjacency is checked from the token spans, so no lexer change is needed. The rejected form gets a message naming the fix rather than the generic D15 one, since it is exactly what a C++ programmer writes from habit. Independent of D15: `a * b;` remains a useless statement either way. |
| D18 | Top-level declarations are visible throughout the file, so mutual recursion needs no forward declaration: `even` may call `odd` above it. Inside a function this does **not** apply — statements are sequential, so using a local before its declaration is still an error. | C++ requires a forward declaration at namespace scope; Rust, Java and C# do not. This accepts what C++ rejects and never reinterprets an accepted program, so it is safe under §5.1. The asymmetry between file and block scope is deliberate: declaration order carries no meaning between functions, and every meaning within one. |
| D19 | A declaration may not shadow another that is still reachable by the same unqualified name. A block local colliding with an enclosing local or with a parameter is an error. Scopes that do **not** carry outer names in — a function body relative to file scope, and later a lambda relative to its enclosing function — are *barriers*, and reusing a name across one is fine: a local may be called `count` alongside a top-level `count()`. **Fields are visible unqualified inside a member body, and may be shadowed by a parameter but never by a local.** `Buffer( u64 len ) { this.len = len; }` is legal; `~Buffer() { u64 len = 0; }` against a field `len` is not. | The wart is the *silent* failure: two locals of the same type mean picking the wrong one compiles, runs and returns a bad value. Shadowing a function with a variable is caught immediately by the type checker, so it needs no rule. Java (JLS §6.4) and C# (CS0136) reject exactly this and keep members shadowable for the same reason; C++ allows it and papers over it with `-Wshadow`. File scope is exempt so that adding a top-level function cannot break a function body far above it — action at a distance. Rust's shadowing is same-scope rebinding (`let x = validate(x);`), which we already reject as a duplicate declaration and which this does not revisit. The member clause draws the line at whether the shadow was *forced*. A constructor parameter naming the field it initialises is the one place C++ programmers shadow constantly, and every alternative is worse — `new_len`, `len_`, a convention nobody agrees on — so it is allowed, with `this.len` as the disambiguation they already write. A local has no such excuse: nothing forces the collision, so it is exactly the silent-wrong-value hazard this entry exists to kill. Slotting members into the same rule keeps one sentence rather than an exception carved beside it. Stricter than C++, which permits both and papers over it with `-Wshadow`, and safe under §5.1 because it rejects rather than reinterprets. **Inert until constructors**: a destructor's only parameter is `this`, which is a keyword and cannot collide with a field, so until member functions take arguments only the restrictive half is reachable. |
| D20 | A character literal is a `u8` holding one byte: `'A'` is 65, `'\xFF'` is 255. A literal spanning more than one byte is an error, so a non-ASCII character needs its bytes written out. String literals lex and parse but have no type at all, and are rejected with a message saying so. | D1 leaves no `char` type to give them, and inventing one for literals alone would be a second spelling for `u8`. Treating a code point as the integer it is means §6.4's range rules apply unchanged: `u8 c = 'a';` and `u32 c = 'a';` both work, `bool c = 'a';` does not, and no new machinery is needed. C++ makes `'a'` an `int` and `'ab'` implementation-defined; both are rejected here rather than reinterpreted. Strings wait because their representation is an M8 question (§9) — choosing `u8*`, a slice or an array now would commit the language before the ownership model exists to judge it. |
| D21 | `main` returns `i32` and takes no parameters. Any other signature is a hard error. | C's `main` returns `int`, so a wider or narrower return type would either truncate in the emitted shim or fail to convert at all — and the shim calls `main` with no arguments, so a parameter would generate C that does not compile: a `cc` error pointing at generated code instead of a diagnostic pointing at the program. Command-line arguments wait for arrays and modules (M8), at which point this entry is what has to change. A program with no `main` is a library and stays legal. |
| D39 | Type parameters go on the name and bounds in a trailing `where`: `T max<T>( T a, T b ) where T : Comparable`. `template<...>` is a hard error naming the replacement. | Both new forms are **hard errors in C++** — `T id<T>( T a )` and `class W<T>` do not parse there — so this is new notation, not a reinterpretation. The rejected spelling is refused outright with the fix named, which is D1's and D22's pattern. |
| D22 | `.` is the only member-access operator, and reaches through a pointer. `->` is a hard error naming `.` as the replacement. | C++ needs both because `.` on a pointer means nothing there; a language designed now does not. The decisive reason is generics (M6): `template<C T> ... thing.x` works whether `T` is a value, a borrow or a pointer, where `->` would force the template's author to know which. `->` distinguishes less than it appears to even in C++ — `.` on a reference is already spelled like `.` on a value — and L6 makes every signature fully annotated, so the declaration is in view. §5.1 constrains neither direction here, unusually: `.` on a pointer is a C++ error, so accepting it only adds meaning. The `Arrow` token is **kept** so the parser can reject it by name; deleting it would lex `p->x` as `-` then `>` and produce a message about arithmetic. |
| D23 | A struct literal must initialise **every** field. `Point { 1.0 }` for a two-field struct is an error, not a zero-fill. | C++ value-initialises the fields you leave out, so adding a field to a struct silently changes what every existing construction site builds. Requiring all of them turns that into a compile error at each site — noisy in the way that finds bugs. This rejects a program C++ accepts, which §5.1 permits outright. Mixing positional and named initialisers is also an error, but that needs no entry of its own: C++20 forbids it too, so Keel is simply following. |
| D24 | **Mutable by default**, as in C++. `const` is opt-in. | The manifesto's "safe by default" pulls the other way, and this is the one place the two goals genuinely conflict — but mutability is not what makes a program unsafe; aliasing and lifetime are, and those are M3-M4's business. Most values are written more often than they are read once, so const-by-default taxes the common case to annotate the rare one. And `i32 y = 0; y = 1;` failing would astonish exactly the developer §5.1 is written for. |
| D25 | `int`, `float` and `double` stay **hard errors**, never aliases. | D1's suggestion path is implemented and works: the message names the replacement, so the cost is one compile the first time. Accepting them would buy that same first hour at the price of a permanent second spelling for every type — every reader thereafter has to know both, and every code base picks one by accident. |
| D26 | `nullptr` is the null pointer, spelled as in C++, and it is a **literal** whose type comes from context: `u8* p = nullptr;` adopts, `auto p = nullptr;` is an error. | The spelling is C++'s because §5.1 has no reason to invent another for an identical concept. Making it a literal rather than a value of some `nullptr_t` reuses `check_literal` wholesale and keeps D5 intact — no conversion happens, the literal simply *becomes* that type, exactly as `42` becomes a `u32`. The `auto` case then falls out as an error for the same reason it does for any literal with nothing to adopt from. |
| D27 | **No pointer arithmetic on `*T`**, which points at exactly one `T`. Arithmetic belongs to a many-item pointer, `[*]T` in Zig's notation, which v0 does not have. | `p + 1` on a single-item pointer is not dangerous, it is *nonsense*, and a type distinction catches it statically at no cost. Note the performance argument for C's arithmetic does not hold: `*(p + i)` and `p[i]` compile to identical machine code, so what buys speed is the capability of touching raw memory, which survives either spelling. C's implicit scaling by element size is the wart — a named `offset` operation says what it does. §6.4 already answers nothing for a pointer, so rejection is the default rather than a rule to add. `==` and `!=` between two pointers of the **same** type are allowed, since that is how a null check is written; ordering is not, because comparing pointers into different allocations is meaningless. |
| D28 | Conversions are written `cast<T>( x )` and `wrap<T>( x )`, both **keywords**. `cast` preserves the value; `wrap` keeps the low bits. Neither converts float to integer, and neither converts integer to bool. §6.5 has the table. **Narrowing `cast` is refused until the run-time check exists.** | Two spellings rather than one because the failure policy is the interesting part, and an unqualified cast lets an author avoid stating it — which is how C's `(u8)x` silently truncates. Keywords because as identifiers they walk into §12's `a < b > ( c )` ambiguity; as keywords the `<` can only be a bracket. Both spellings are new notation, so §5.1 is satisfied for free. Float to integer is rejected because `cast<i32>( 1.9 )` has no obvious answer — truncate, round, floor and ceil are four operations and C picks one silently; they arrive as library functions at M6. Integer to bool is rejected because `x != 0` says it better and modular arithmetic down to one bit says something else again. Float *rounding* is accepted (`cast<f32>( some_f64 )`), because a float that cannot hold the value gives an infinity rather than a plausible wrong number — the line is that `cast` refuses to turn a value into a *different* value, not that it refuses to lose precision. |
| D29 | **Two aggregate kinds, split by one principle: a `struct` is a type whose representation is its interface; a `class` is a type whose interface hides its representation.** **Both may have methods** (M5.5), written as C++ writes them, with a **trailing `const`** saying the method does not modify its object. A `struct` has all fields public, is trivially copyable, may not have a destructor, and may not contain an owning member (transitively); it is built from a struct literal (D23). A `class` has fields private by default, may own resources, may have a constructor and a destructor, is moved rather than copied, and is built by a constructor. **Both may have methods.** Neither inherits and neither is virtual **in v0**; inheritance and dynamic dispatch are planned and unscheduled, and D29's split is drawn so as not to foreclose them — the line is copyability, which says nothing about whether a `class` may later have a base. A `struct` with a destructor, and a `struct` with a `private:` label, are hard errors naming `class` as the fix. | C++ has two keywords for one job — the only difference is default access, kept so that C headers would compile — and Keel pays no C-compatibility tax, so the second word is free to earn its keep. The line is drawn at **trivial copyability** rather than at "may have methods", because only the first has semantic consequences: a trivially copyable type cannot have a destructor (copy plus destructor is a double free, which is why Rust makes `Copy` and `Drop` mutually exclusive), is never moved, and never enters §8's drop analysis. "May have methods" has no consequences at all, and the motivating examples for restricting it — `Node_id::is_valid()` — need them anyway. The two initialisation syntaxes stop competing as a side effect: literals belong to structs, constructors to classes, so `Buffer { ... }` versus `Buffer( 16 )` never has to be disambiguated. Enforcement is free: `struct` is legal exactly when D2's owning query says no. Safe under §5.1 because both rejected spellings are errors rather than reinterpretations. Prior art cuts both ways and is worth recording: the languages that keep two aggregate keywords (C#, Swift, D) split on value-versus-reference semantics, and the C++ successors that exist (Carbon, Cpp2, Hylo) collapse to one kind. This splits on copyability, which is the ownership-language analogue of the first — Keel has no garbage collector, so "reference type" has nothing to mean. |
| D30 | **One `enum` keyword**, carrying `enum class`'s semantics: scoped (`Shape::Circle`), with no implicit conversion to an integer. **Scoped everywhere, including a `case` label** - `case Shape::Circle( r ):`, never a bare `Circle`. The underlying type is spelled as in C++: `enum Shape : u8 { ... }`. `enum class E` is a hard error saying to drop the `class`. | The same C-compatibility tax as D29, with the opposite answer, and the asymmetry is the point: `struct`/`class` are two words for one job, so the job gets split; `enum`/`enum class` are two words for one job where only one of them does it correctly, so there is nothing to split. C++'s plain `enum` leaked its variant names into the enclosing scope and converted implicitly to `int`; both were mistakes, `enum class` fixed them in C++11, and the broken spelling survives only for C. Safe under §5.1 because every point where the two meanings diverge is an error rather than a reinterpretation: `Shape s = Circle;` is an unknown name, and `i32 x = Circle;` and `if ( s == 0 )` have no conversion to reach for. Rejecting `enum class` follows D22's pattern — keep the spelling recognised so the diagnostic can name the fix. **Answered at M5, and the answer is smaller than the question looked.** Ownership is **inherited, not chosen**: an enum is owning exactly when any variant's payload is owning, which is D2's query extended to variants with no new rule. That is why D29's question does not really transfer - D29 forced `struct` against `class` because the *author* had to say which they meant, and here there is nothing to say, since every variant is visible in the declaration and the compiler already knows. No annotation, no `enum class` equivalent, no way to get it wrong.

What is genuinely new is *how* one is destroyed. A struct's destructor is a fixed sequence - field 1, field 2, field 3 - and every drop in the language today is static. An enum has **one live variant, chosen at run time**, so destroying it means reading the tag and destroying only that payload; destroying the wrong one frees memory that was never allocated. The answer is a **synthesised destructor that switches on the tag**, one generated function per owning enum. That is the whole of the new machinery, and it is affordable precisely because nothing else moves: `drop` still means "call this type's destructor", so drop elaboration, drop flags, the move analysis and the emitter are untouched. Expanding the switch inline at every drop site instead would multiply both the code and the flags.

Two consequences follow without further decisions. A **payload binding is a borrow** - `case Ok( b )` cannot copy (§6.6 has no copy constructors) and cannot move (that would leave the enum live with a hole, which is why a struct field cannot be moved on its own either). And **assignment destroys the old active variant**, which the generated destructor makes free.

The first cut still refuses an owning payload. Destroying an enum means destroying the **active** variant, chosen at run time - a switch on the tag inside the destructor - and deferring that costs nothing anyone can use yet, because a variant worth owning needs allocation, which is M5.5. `Result<i32, Error_code>` works without it. |
| D31 | **Argument passing has five forms, and the call site names every one where something happens to the caller's variable.** Bare `f( x )` — the callee gets a copy it owns if `x` is a `struct`, a read-only borrow if `x` is a `class`; either way the caller's object is alive and unchanged afterwards. A `const ref T` parameter is called bare as well, and is the fifth form: a read-only borrow of *either* kind, which is how a large `struct` is passed without copying it. `f( ref x )` — a mutable borrow. `f( out x )` — uninitialised, and the callee must assign it. `f( move x )` — the callee owns it and `x` is dead afterwards: for a `class` because the resource left, for a `struct` because the author said so. `const T&` does **not** survive as a spelling; D32's `const ref T` replaces it in every position. **The same rule governs initialisation and assignment, not just arguments**: `Buffer b = a;` is a hard error naming `Buffer b = move a;` as the fix, and so is `b = a;` between two existing classes. A `return` is the one exempt position — `return b;` needs no marker, because `b` is going out of scope regardless and there is no later use for the marker to warn about. | One principle generates the whole table: **you may modify what you own.** A bare `struct` parameter is a copy you own, so it is mutable — which is also exactly what C++ does, so this costs no audit row and keeps `i32 factorial( i32 n ) { ... n--; }` legal. A bare `class` parameter is a borrow you do not own, so it is not; that is forced rather than chosen, because copying a class needs a copy constructor and §6.6 puts those outside v0, leaving *borrow* as the only meaning available. The uniformity that matters is caller-side and holds in both rows — after a bare argument the object is alive and unchanged — so reading a call site never requires knowing the kind. The variation is callee-side, concerns a type named on the same line under L6, and surfaces as a compile error rather than a silent difference in meaning. The result is C#'s behaviour for value and reference types, arrived at from ownership rather than from a garbage collector. `move` on a `struct` copies the bytes and marks the source dead in the checker: an assertion, not a transfer, which keeps the keyword's user-visible meaning identical across kinds and costs almost nothing, since structs are already in §8's dataflow for the `Uninitialised -> Live` half and drop elaboration still never looks at one. **Obligation at M6**: a generic that mutates a bare parameter is legal only when `T` is a `struct`, so definition-checked generics (D11) need a bound that permits it — `is_trivially_copyable`, alongside `is_numeric` and the rest — rather than deferring the error to the instantiation site as C++ does. The initialisation case is where the rule earns most: C++ would call a copy constructor for `Buffer b = a;`, and with none available the two remaining readings are to move silently — leaving `a` dead with nothing in the source saying so — or to bind `b` as a reference to `a`, which is worse, because two names would own one resource and the second destructor would be a double free. Rejecting is the only safe answer, and it rejects valid C++ outright rather than reinterpreting it, which §5.1 permits. Note that `b = move a;` must also destroy whatever `b` held first; that is drop elaboration's job at M3, not a separate rule. **The marker appears in the signature as well as at the call site**, and the two must agree: `void consume( move Buffer b )` is called as `consume( move b )`, `void grow( ref Buffer b )` as `grow( ref b )`, `void init( out Buffer b )` as `init( out b )`, and an unmarked parameter as `inspect( b )`. `const ref` is the one that takes no marker: `void peek( const ref Point p )` is called as `peek( p )`. This is forced: with `const T&` gone, `void consume( Buffer b )` and `void inspect( Buffer b )` would otherwise be indistinguishable, and the callee has to know whether it owns its argument. Marking both sides is C#'s design rather than C++'s, and it removes `&` from parameter lists entirely — a reference type still exists (L16), but a parameter never spells one, because each of the three things `&` was doing in a C++ signature now has its own keyword. The redundancy is only apparent: the signature states the contract and the call site acknowledges it, which is the whole point of D2 — a reader of the call site should not have to find the declaration to learn that a variable just died. **Amended once `const ref` was implemented.** This entry first said `const T&` did not survive *because a bare argument already means it*. That is true of a `class`, where §6.6 leaves borrowing as the only meaning available, and false of a `struct`, where bare is a copy — so there was no way to pass a large `struct` read-only without copying it, and the entry counted four forms where the language needed five. D32's `const ref T` fills the gap. That it takes **no marker at the call site** follows from this entry's own argument rather than being an exception to it: the marker exists so a reader knows something happened to the variable, and after a `const ref` argument nothing has — it is alive and unchanged, exactly as after a bare one. The agreement rule therefore asks about *mutation* rather than about modes, and `const ref` sits with bare. What the fifth form changes is callee-side only: what the call costs, and whether the callee may write. |

| D32 | **`ref` is a binding mode, not a type.** `ref T x` and `const ref T x` replace `T&` and `const T&` in every position — parameter, local binding, and return. `T&` in type position is a hard error naming `ref T`. `&` therefore means address-of and nothing else, and `T*` is the only postfix type constructor left (L16). A `ref` binding is **initialised at its declaration and never reseated**. | Two spellings for one concept is the redundancy D25 and D30 already refuse, and D31 had removed `const T&` from parameters — where the great majority of references appear — leaving `T&` alive only for local bindings. Finishing it costs little more and stops the language carrying both. The reframe is what earns it: as a *type*, §8's rule that a reference may not live in a struct is a restriction needing a diagnostic; as a **mode**, a field simply is not a binding and the rule disappears into the grammar. Reading order improves too — `const ref i32` has one order where C++ has `const i32&` and `i32 const&` meaning the same thing. Prefix does not violate L16, because a mode is not a type constructor. Taken at M4 rather than later because references were still *unimplemented* — `Ref_type` parsed and the checker said "not supported yet" — so the change cost a parser branch and an enum entry rather than a migration. `Ref_type` accordingly becomes a flag on `Param_decl`/`Var_decl` rather than a node wrapping a type. The §5.1 cost is real and is the largest departure in spelling the language has taken: `i32& r` is idiomatic C++ and becomes a syntax error. §5.1 permits rejecting outright, and the diagnostic names the replacement. **Answered, and then done at M5.5**: `this` is a `ref T` binding, `const ref T` when the method carries a trailing `const`. It was a change of spelling rather than of design, as predicted - the receiver and a `ref` parameter were already one mechanism - and it paid for itself immediately: §8's clause that a method may return a reference into its own object stopped being fiction the moment the receiver became a reference parameter. |
| D33 | **An operator is a method, not a free function.** *(2026-09-18: this entry's line that constructors "do have arg types because they are overloadable" contradicted the compiler, which refused a second constructor. §12 settled the contradiction in this entry's favour and M6 shipped it — the line is now simply true, and the sentence below saying Keel has no function overloading is the part that is out of date. Operator overloading and function overloading remain different features, and the argument below for why the first is cheap does not depend on the second being absent: an operator still has one receiver type and one candidate.)* `a + b` resolves to `operator+` on `a`'s type: exactly one candidate, no overload resolution, no ADL. Overloadable: the arithmetic and bitwise operators, comparison, `[]`, and unary `-`/`!`/`~`. **Arrives at M8**, with `Vector` and `String`, not with methods. **Not** `&&`, `\|\|`, `,` or `?:` — short-circuiting and sequencing cannot survive becoming a call, which is the C++ mistake worth not repeating. | Keel has no *function* overloading — two functions of one name is a redeclaration error — so the expensive half of C++'s operators, resolution, simply does not arise. The two features share a word and are not the same thing: defining `operator+` for your own type is in, and picking between `f( i32 )` and `f( f64 )` by argument type is out. The second being out is precisely what makes the first cheap. One receiver type, one candidate, and the cost collapses to a name lookup. That is Rust's design by way of traits, reached here from D29's methods rather than from a trait system. It is needed rather than merely wanted: `String` comparison spelled `a.equals( b )` fights §5.1 harder than any syntax Keel has rejected, and `Vector` without `[]` is not a container anyone will use. **Timing**: methods at M5.5 are the prerequisite, not the trigger. The trigger is M8, where `Vector` and `String` are written *in Keel* and `v[i]` and `a == b` stop being conveniences and start being the difference between a container people use and one they do not. Nothing before M8 needs an operator it cannot spell as a method call, so this waits, and waiting keeps M5.5 to two things instead of three. **M4 made the interesting half expressible**: `v[i] = 5` needs `operator[]` to hand back a *mutable binding*, and `ref T` is now something a function can return (§8). **Three sub-questions, deadline missed.** They were marked "open until M5.5" and M5.5 shipped without them: whether `==` auto-derives `!=`, whether comparison is six operators or one `<=>`-shaped answer, and whether an overload may be declared for a type it does not belong to (it may not — that is what keeps lookup to one candidate, and it is the only one of the three with an answer). **The second is now load-bearing** and no longer only about ergonomics: §12's `a < b > ( c )` disambiguation depends on a comparison yielding `bool`, so "comparison returns whatever the author says" is not available. Decide all three with D33 at M8, and decide the second first. |
| D34 | **A range is syntax, not a type.** `0..n` is a half-open range, exclusive of the upper bound. It appears in **one** place: a `switch` label, `case 1..5:`, which lowers to a pair of comparisons. There is no `Range` type, no iterator, and no `step`. The `for` forms — `for( 0..n )` to repeat and `for( i : 0..n )` to bind — are **designed and deliberately not shipped**; see below. | A library range needs generics (M6) and an iteration protocol Keel has neither designed nor needs yet; as a desugaring it costs one parse rule and one lowering rule and ships now. Exclusive because the form that appears in real code is `0..size()`, where an off-by-one is a *bug*, while `1..10` against `0..10` is a spelling learned once — Rust settles it identically, and leaves `..=` available for the rare inclusive case. The same meaning in both positions is not a nicety: two readings of `..` would be exactly the silent divergence §5.1 forbids. **The honest cost** is `step` and reverse iteration: Python's `range( 10, 0, -1 )` has no spelling here, and the escape hatch is the ordinary `for`, which is not going anywhere. A library `Range<T>` after M6 is not precluded by any of this. **Reverse and step are deliberately absent, and their absence is the signal.** Every punctuation that could carry them - `0..n..step`, `0..step..n`, `for( i : 0:n:step )`, direction encoded by which side the binding sits on - either collides with an existing meaning of `:` or makes one syntactic slot carry two features, which is the conflation D25 and D30 refuse. What reverse and step actually want is a *value with methods*: `( 0..n ).rev()` and `.step_by( 2 )` read better than any of those and need no new syntax at all. So the day the need bites is the day `Range` becomes a type, which is after M6 - and until then the ordinary `for` spells both, and is not going anywhere. **`a..b` where `a > b`**: a compile error when both are constants, because it is certainly a mistake; zero iterations otherwise, because inventing a run-time check for it would be the only run-time check in the language. Catch what is statically knowable, and no more. **Why the `for` forms wait.** They are the sugar half: an ordinary `for` already spells every loop they would, so shipping them buys brevity and nothing else, while every question they raise — reverse, step, whether the counter is a binding or a value — is a question a *type* answers better than syntax. The `switch` label is different in kind: without it a range of integer cases has no spelling at all, so it adds capability rather than shorthand. **The one use that would change this is slicing** — `buf[ 0..n ]` for a subslice, which arrives with the many-item pointer `[*]T` (D27) and `Vector`/`String` at M8. That is a second consumer of the same notation, and a strong one; if `for` is reconsidered it should be then, so that one spelling lands across all three positions at once rather than accreting. **Slicing is also what forces the type question**, and it is worth seeing now: `buf[ 0..n ]` going through an overloaded `operator[]` (D33) means the range has to be a *parameter*, and a parameter has a type. So `a..b` stays syntax exactly as long as `switch` is its only consumer; the moment slicing arrives, `Range` becomes a real type and the `for` forms come with it for free. That is not a contradiction of this entry — it is this entry's own prediction, written down before it happens. **One route avoids the promotion entirely, and is worth weighing first**: if `[*]T` is the only consumer, slicing belongs to the *many-item pointer* rather than to `T`, and `buf[ 0..n ]` can be a built-in operation on that type — exactly as `a..b` is built-in here — with no overloadable `operator[]` and no `Range` parameter anywhere. It still needs `T` to scale the offset, which it already does for indexing. **Answered for containers**: a container exposes `.slice( a, b )` for a range and `operator[]` for a single element, so the overload never has to take a range and the language route above stands. (The container will not be called `Vector` — the name is a C++ wart, a growable array that says nothing about growing or about arrays. Naming it is M8's problem.) So the fork is: **slicing as a language operation on `[*]T`, or as an overload that forces `Range` to become a type.** Decide it with `[*]T`, not before. **A `case` needing a step is not a range**, and reaching for one is the wrong instinct: "every third value" is a *predicate*, and the construct that expresses a predicate in a match is a **guard** — `case n if n % 3 == 0:` — — **and guards are refused**, which is the second half of the answer. A guard makes exhaustiveness *undecidable*: `case n if n > 0:` and `case n if n <= 0:` cover every value, and no compiler can prove it. Exhaustiveness is the whole reason `switch` is worth having over `if`, so a feature that silently converts it back into `default`-or-nothing costs more than the predicate it buys. A `case` therefore matches **shape**, never a computed condition, and the question of what happens when two patterns overlap does not arise: with ranges the checker can reject an overlap outright, which it could not do with arbitrary expressions. |
| D35 | **`unsafe` is a block, and it permits operations rather than disabling checks.** `unsafe { ... }` is the only form: no `unsafe` function, no `unsafe` expression, no statement form — so the extent of the permission is always a pair of braces the reader can see. Everything the compiler checks outside one it still checks inside: types, `const`, move checking, definite assignment, exhaustiveness, `break` placement. What a block changes is a short **enumerated** list of *operations*, and at M5.5 that list has four entries — **converting between pointer types**, the one `unsafe` cell of §6.5's table; **calling an `extern` function** (D36); and **`alloc<T>()` and `free( p )`** (D37). Two rules come with it: **a block that uses none of them is an error**, and **a block inside another one is an error**. Raw-pointer dereference is deliberately *not* on the list. | The shape was settled in §12 and is Rust's; what this entry adds is the two rules and the list, and each follows from an entry Keel already has. **Unused is an error rather than a warning** because that is D15's shape — a construct with no effect is an error — and because the failure mode it prevents is the one that makes `unsafe` worthless for review: a region that quietly grows wider than the operation it was opened for, until the marker no longer tells a reader where to look. The compiler emits no warnings at all today, so "warning" would also mean deciding what a warning does to the exit code, which is a larger decision than this one. **Nesting is an error** because permission is not cumulative: an inner marker can only mislead about which region is the guarded one. That is the same instinct as the rule against nesting `try` in §12. **The list stays enumerated rather than inferred**, which is why dereference is absent: gating it is a language change that breaks working programs — `codegen/pointers.kl` and five other fixtures deref in safe code — and it deserves its own decision rather than arriving as a side effect of building the gate. §12 named `Buffer` and `[*]T` as what would force the full list, and neither exists yet. **No `unsafe fn`, for now.** The only construct that needs "unsafe to *call*" is `extern`, which got its own representation in the next slice — **D36, and it needed no marker at all**, because the absent body already says everything a flag would have — and a declaration marker with no customer is precisely the defect D2 records for `move` in signatures — an optional marker whose absence means either "safe" or "forgot". Worth recording for whenever it lands: Rust shipped `unsafe fn` bodies as implicit unsafe blocks and **moved away from it** in the 2024 edition, because the marker conflated "callers must check" with "the body need not say where" — so if Keel adds one it should mark callers only. **Representation, and why it is worth an entry**: an unsafe block is a `Block` carrying `aux == 1`, not a `Node_kind` of its own. Every pass that already walks blocks — scoping, lowering, drop placement, both dataflow checks — then handles it with no edit at all. That is not a micro-optimisation but a defence: a new `Node_kind` missing from one dispatch compiles cleanly and makes a rule *silently absent*, which is the most common bug in this compiler's history. |
| D36 | **`extern` declares a function defined in C, and calling one needs `unsafe`.** `extern i32 abs( i32 v );` — file scope only, terminated by `;`, and **the absent body is what marks it**: D18's corollary already makes a bare prototype an error, so `extern` is the only rule that can produce a body-less function and no flag is needed to read it back. The name is **not mangled** — `kl__abs__i32` would link against nothing — which makes `extern` mean what C++'s `extern "C"` means. Every call site needs an `unsafe` block (D35's second gated operation), and `extern` alone carries that: there is no `unsafe extern` spelling and no way to declare a safe one. `main` may not be `extern`. Binding modes work and are how a real C signature is spelled: `ref i32` and `out i32` both emit `int32_t*`. | The representation is the entry's best part and was not designed so much as noticed. `Function_decl`'s `aux` is already the name, so a flag had nowhere to live — the same wall D35 hit — and the absent body turned out to carry the information for free. Four passes then needed **no change at all**: the resolver registers the name in its file-scope sweep and guards `!id.is_valid()` before visiting the body, the checker's `visit` guards identically, `declare_signatures` reads only the return type and parameters, and the three dataflow checks run on KIR, which an extern never enters. The total cost was one predicate, one `&& !is_extern` in `lower`, one early return in `Spelling::function`, and one new emitter pass. **That last one is the only piece that does not fall out**, and it is worth knowing why: prototypes are emitted by walking the *lowered KIR functions*, and an extern is not one — so it needs a second pass over the AST, and omitting it produces C that does not compile rather than C that is subtly wrong, which is the good failure. **Why `extern` implies unsafe rather than `unsafe extern`.** §12 leaned toward Rust's two-word form, and Rust needs both because its 2024 edition lets individual items be marked `safe` inside an `unsafe extern` block. Keel has no such escape and should not invent one: with every extern unsafe to call, the second keyword carries no information, which is the redundancy D25 and D30 refuse. **The FFI boundary is where the assertion belongs** — being the checked wrapper over an unchecked one is what §12 says `Buffer` is for, and the rule is what makes that wrapper a real boundary rather than a convention. **Two guarantees now rest on a promise rather than a proof**, and both are deliberate: `out` on an extern parameter is discharged in a body that does not exist, so the caller's variable is believed initialised on the C function's word; and `move` hands ownership to a function whose destructor Keel will never run. Neither needs a new rule, because the `unsafe` at the call site is precisely the author asserting them — but they are the first places in the language where that is true, and they are listed in §15 rather than left implicit. **The emitted file declares its externs itself and never includes the C header.** That avoids a conflicting-declaration error whenever the Keel spelling and the real one differ, and makes the Keel declaration the single source of truth; the cost is that a wrong declaration is an ABI bug at run time rather than a compile error. It is also the strongest argument for the runtime being a *thin* C floor whose signatures are ours. |
| D37 | **`alloc<T>()` and `free( p )` are keywords, and both need `unsafe`.** `alloc<T>()` yields `T*` and takes no count — D27's `[*]T` is what will give the count somewhere to go, so the parens are there to be filled rather than added. `free( p )` takes any `T*` and yields `void`. Both lower to the runtime, `kl_rt_alloc( sizeof( T ) )` and `kl_rt_free( p )`, whose prototypes the backend emits itself when a program uses them. **Neither runs a constructor or a destructor**: `alloc` returns memory, not an object. Both are reserved words, so nothing may be named `alloc` or `free`. | **Keywords, because `alloc` needs the element type to compute the size** — that is D28's argument for `cast<T>` reused: the `<` can only be a bracket, so §12's `a < b > ( c )` ambiguity cannot arise, and no `sizeof` is ever written in Keel to be got wrong. **`free` is a keyword for a different reason, and the reason is a hole**: Keel has no `void*`, so an `extern void kl_rt_free( u8* p )` would force `unsafe { kl_rt_free( cast<u8*>( node ) ); }` at every call — an `unsafe`, a `cast`, and a pointer conversion that is *itself* a gated operation, to release one node. A keyword also *checks* its argument is a pointer, which an extern taking `u8*` cannot. Allocation and release are one decision and should be one mechanism. This does not waste D36: `extern` is still how any C library is reached and is still never thrown away; what it is not is how the *runtime* is reached, and that is right, because `kl_rt_alloc` is not a library the author chose to call. **Why `alloc` is gated, and the reason is sharper than "it can fail."** It hands back a pointer whose type says there is a `T` at that address, and there is not — the bytes are uninitialised and no invariant has ever held. That is a false statement the type system makes the instant the expression evaluates, and nothing else will catch it, because definite assignment does not track through pointers. The nullptr stopgap is the second reason and the weaker one. This is exactly the difference between `alloc` and Rust's `Box::new`, which is safe *because it takes an initialised value* — Keel has no such form yet, so there is nothing safe to expose, and that gap is what D10's `Owned<T>` exists to close. **What the pair deliberately does not do** is track anything: double free, use after free, leaking, and freeing a stack address all type-check. Each is what the `unsafe` at the site is asserting, and each is pinned as *accepted* in the tests so that the day `Owned<T>` makes one a compile error, the change is visible. **The cost, paid immediately**: reserving `free` broke two existing tests the day it landed. Worth recording as the price of the spelling rather than discovering it again later. |
| D38 | **A `case` with a body says how it ends, and `fallthrough` is how it runs on.** Four rules, all about the shape of an arm rather than its labels. An arm that can complete normally may not be followed by another — it ends with `return`, `break` or `fallthrough`, and the last arm is exempt because nothing follows it. **`break` binds to the nearest enclosing loop *or* `switch`**, as in C; `continue` still looks past a `switch` to the loop. **`fallthrough;`** runs the next arm's body: it must be the arm's last statement, may not appear in the last arm, and may not enter an arm that binds. **No stacked label may destructure** — `case Circle( r ): case Rect( w, h ):` is refused — and **a bare `case Shape::Circle:` matches a payload variant without destructuring it**, which is what keeps stacking available. | **This is the only construct where Keel was found silently redefining valid C++**, and it did so twice. `case A: t = 1; case B: t = t + 10;` gave 1 where C++ gives 11, and `break` inside a `switch` inside a loop left the *loop* where C++ leaves the switch — both compiling in both languages with no diagnostic. §6.3's audit claims none of Keel's divergences do that; these were unrecorded exceptions, and the claim is only worth having if it is tested rather than asserted. Both are now hard errors or match C++. **`fallthrough` is what makes the first rule enforceable.** Without it the diagnostic for a run-on arm has no fix to name: `break` was spoken for, and an arm that must end but cannot say so is a rule nobody can satisfy. Every language that keeps C's `case X:` and removes implicit fallthrough keeps an explicit terminator — C# requires one, Go inserts it and lets you write `fallthrough` — and that is not a coincidence, it is the only escape hatch the syntax offers. `fallthrough;` is a *hard error* in C++ today (an undeclared identifier), so §5.1 permits it outright as new notation, and C++17's `[[fallthrough]]` already makes the word familiar. **The binding rules exist because destructuring is a hazard C++ never had.** D7 named it — *"falling into a case with bindings would read payload fields that were never assigned"* — and then nothing enforced it: stacked labels bound every label's payload unconditionally, with no tag test, so `case Circle( r ): case Rect( w, h ): return r;` read a field a `Rect` has not got. That is worse than either divergence above, because it is not a different answer but a meaningless one. **`case Shape::Circle:` is the relaxation that keeps the rule cheap.** Naming a variant and producing one are different things, and only the second needs a payload — so the same flag that lets a pattern and a construction write the bare path now lets a `case` label do it. Two rejected alternatives: a `_` wildcard, which is not needed until *partial* ignoring (`Rect( w, _ )`) is wanted and does not solve the mixed-binding case anyway; and stacking labels that bind matching names and types, which is a rule nobody would predict for a case nobody writes. |
| D39 | **Type parameters are written on the name, and bounds in a trailing `where` clause.** `T max<T>( T a, T b ) where T : Comparable`, several joined with `&` (D40), and `class Vector<T>` for a type. `template` keeps its reservation and becomes a hard error naming the replacement, as `->` does under D22. This is spelling only — D11's definition-checking and L9's monomorphisation are untouched. | **The declaration now mirrors the call site**, which is the argument that outranks familiarity. Type arguments already sit beside the name everywhere else in Keel — `max<i32>( a, b )`, `cast<T>( x )`, `alloc<T>()`, `wrap<u8>( n )` — and C++'s `template<typename T>` prefix is the one place they do not. Putting them on the name makes the language more internally consistent rather than less, and removes a keyword from the common case. **`where` says what the clause is.** An inline `<Ord T>` reads as though `Ord` were part of the parameter's name; `where T : Comparable` reads as a restriction on what `T` may be, which is what it is. It also keeps a long signature's first line to the signature, and leaves room for more than one bound per parameter without inventing punctuation for it — the case an inline form has to solve with `+` or a `requires` clause, both of which C++20 shows getting ugly. **§5.1 permits both outright.** `T id<T>( T a )` and `class W<T>` are *hard errors* in C++ — checked, not assumed — so this is new notation rather than a reinterpretation, and the rejected `template` spelling gets a diagnostic naming the fix. The cost is real and is the largest spelling divergence since D32's `ref T`: every C++ programmer will write `template<...>` first, once. **What this does not decide** is the bound vocabulary itself — what `Ord` is, and whether D31's requirement that a generic may mutate a bare parameter only when `T` is a `struct` is spelled as a bound like `is_trivially_copyable` or as something else. That is the next question, and §12 carries it. |
| D40 | **A bound is a promise about a type parameter, drawn from a fixed set, and written `where T : Copyable & Comparable`.** Several bounds on one parameter join with **`&`**; a second parameter takes a second clause, `where T : Comparable & Equatable, where K : Hashable`. Bounds are **nominal** — a named set, not a structural predicate. The set is **closed and compiler-provided**: `Copyable`, `Equatable`, `Comparable`, `Numeric`, `Integral`, `Floating`. They **imply** one another, so `Integral` carries `Numeric`, `Comparable` and `Equatable` without being written. A parameter may carry several. **`Copyable` is the one that is not about operations**: it means trivially copyable, which under D29 is exactly what a `struct` is, and D31 needs it to permit a body to copy or mutate a bare `T`. | **Bounds exist for one reason: D11.** Keel monomorphises (L9), so an instance always knows its `T`'s size, layout, destructor and move behaviour — a generic destroys an owning argument correctly today with no bound at all. Nothing about *emitting* code needs one. What needs one is checking the body **once, before any instance exists**, which is the whole of D11; drop that and bounds disappear, as they do in C++ before concepts and in Zig. That reframes the minimum set: not "what the compiler must know to emit", but "what a body needs to *do* to its `T`" — a short list, because storing, passing, returning and dropping need nothing. **Nominal follows from D11**: definition-checking needs the bound to *enumerate* what is available, and a structural predicate can only answer yes or no about a type already known, which is why C++20 still rechecks bodies at instantiation and Rust does not. **The set is fixed because the operator set is closed** (D33), so operator bounds are finite and small; user-defined bounds are only needed to constrain on a type's *own* methods, which is the trait system §6.6 defers, and the test for whether v0 needs it is whether `Vector<T>` does — it stores, moves and drops, so it does not. **`Copyable` rather than `where T : struct`**: everything else right of the `:` is an L15 property name, and mixing a keyword in reads as a different kind of thing, which it is not. Not `Trivially_copyable` either, because §6.6 leaves no other kind of copy for the qualifier to distinguish. **`Owning` is deliberately absent**: a body that writes `move` is correct for owning and non-owning alike — D31 makes `move` on a `struct` an assertion rather than a transfer — so the asymmetry is one-way, and only copying needs a promise. **This closes a live double free**: `T twice<T>( T a ) { T b = a; return b; }` compiles today and gives `twice<Counter>` one construction and two destructor calls, because `is_owning_type` on an unbound parameter answers *no*. That is why bounds land before generic aggregates rather than after. **`&` rather than `,` between bounds**, because all of them must hold and `&` says so. Keel's comma carries no such meaning — it separates parameters and fields, which are conjunctive, and enum variants and stacked `case` labels, which are not — so this adds information rather than overriding any, and the project has taken the explicit side of that trade before (D2's markers, `fallthrough`, `where` itself over an inline bound). Java spells the same case `<T extends Comparable & Serializable>`. `|` was rejected outright: it means bitwise OR and would be saying "or" while meaning "and", which is the §5.1 hazard at its sharpest. The comma before a second `where` is redundant to the parser and kept because it reads better. **The cost is that `&` now does a third job** — bitwise AND, address-of, and bound conjunction — which partly undoes D32's tidying of `&` out of type position; it is bounded by the clause being a list of *names*, never of types, so nothing there can be read as a type expression. **Implication is expanded once, where the `where` clause is read**, so every later query is one bit test rather than a lattice walk. **Satisfaction is simpler now than it will be**: D33's operators arrive at M8, so until then only a builtin can satisfy `Comparable`, and the check is a switch on `Type_kind` that grows one branch — "or the type declares that operator" — when D33 lands. |


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

**A comparison does not pay that cost (D41).** It has no result to protect — the
answer lands in `bool`, which holds every answer — so the containment clause does
not apply to it and the only requirement left is that the domain hold both
operands exactly. That is a second, wider table, and it is the one `<`, `<=`,
`>`, `>=`, `==` and `!=` are checked against:

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

`*` is **not an error**: it marks the eight pairs no type holds, where the
comparison is answered by cases instead. Five cells differ from the arithmetic
table in the other direction — `i8`, `i16` and `i32` against `u32`, and `i32`
and `u32` against `f32` — and each is a comparison C++ compiles and answers
wrongly, by converting both sides to a type that loses one of them.

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

*unsafe* marks a real conversion rather than nonsense, so it is **gated rather
than refused**: legal inside an `unsafe` block, an error outside one naming that
as the fix. It is the only cell `unsafe` opens, and as of M5.5 the only operation
in the language that needs one (D35).

Two asymmetries are deliberate. `wrap` accepts a widening integer conversion —
it simply never wraps — so it is total over integer-to-integer rather than
carrying a rule about which direction is allowed. And `cast` gives a **literal**
its type from context rather than converting it: `cast<u8>( 300 )` is the
ordinary out-of-range error and `cast<u8>( 200 )` is simply a `u8` literal, both
falling out of `check_literal`. `wrap` must not do this — accepting a value the
target cannot hold is the whole point of it — so `wrap<u8>( 300 )` infers `i32`
and then wraps to 44.

### 6.6 Not in v0

Optionals, traits beyond generic bounds, closures, `namespace`, copy
constructors, inheritance, virtual dispatch, `Shared<T>`, `Weak<T>`,
concurrency, reflection, coroutines, and any standard library.
**Operator overloading has moved off this list** — see D33; it arrives with
methods at M5.5, because an operator is a method and has nowhere to live before
then.
**Function overloading has moved off this list too** — §12 decided it and M6 shipped it
(2026-09-18, §15 slice 3), for free functions, methods and constructors alike.
**Unions are a decided non-goal, not a deferral**: a union is only safe when the
tag is maintained by hand and correct every time, and the one legitimate use of
one — a tagged variant — is what M5's payload-carrying `enum` (D7, D30) is, with
the tag maintained by the compiler and `switch` exhaustiveness checked.
Modules arrive at M8, and **`namespace` on the list above means "no second
mechanism", not "no namespacing"** — the module *is* the namespace. §3's style
line says "C with containers and namespaces" and the mangler has carried a module
slot since M0, `kl_<module>_<name>__<argtypes>`, empty until M8; a separate
`namespace` keyword would be a second way to do the one job, which is what C++
needs only because a header is not a unit. **Reflection** is listed here for v0 only — §12 records its direction: compile-time reflection, yes; runtime reflection, never. **String literals** lex and parse but have no type
(D20); they wait for `String`, which is M8 as well.

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

**What "reference parameter" means, now that it is implemented.** A parameter that
travels by address: `ref T`, `const ref T`, and a *bare* parameter of an owning type,
which D31 makes a read-only borrow. The last is a widening of the sentence above,
which was written before bare owning parameters became borrows — it is the consistent
reading, because the caller owns the object and outlives the call exactly as it does
for an explicit `ref`. A **local `ref` binding does not count**, even where its referent
is itself a parameter and the return would be safe: the rule traces the returned
*expression* to its root, and a binding is not a parameter. That conservatism is
deliberate and relaxing it is additive. **The receiver clause is real as of M5.5.** It was not, for as long as
`this` was a `T*` passed by value: it failed the by-address test this rule uses, and
nothing could reach the case anyway, because a constructor and a destructor were the
only methods and neither returns anything. Making the receiver a `ref T` binding fixed
it without a line written for it, which is the argument for that change in one sentence.

**Raw pointers are outside this rule, and that is the point.** `ref` is the spelling
that cannot dangle; `T*` is the one that can. `void escapes( out i32* p ) { i32 v = 7;
p = &v; }` compiles, and nothing here objects — D2 already says an address tells you
nothing about who owns what is at the other end, and §8 adds that it tells you nothing
about how long that thing lives either. A language with a checked reference and an
unchecked pointer has to say which is which somewhere, and this is where.

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

Revisit only after M8, with real programs as evidence.

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
| **M5** | `enum` (D30) payload-free first, then payloads (D7). `switch` with destructuring, `default`, stacked labels and exhaustiveness. Range labels (D34). | The `Shape`/`area` sample. Non-exhaustive `switch` is a compile error naming the missing variant. | Sum types, tagged variants |
| **M5.5** | ~~Methods on `struct` and `class`~~ **done**. ~~`unsafe` blocks (D35)~~ **done**. ~~`extern` (D36)~~ **done**. ~~`alloc<T>`/`free` and `kl_rt` (D37)~~ **done**. | A linked list whose nodes are allocated one at a time, walked, and freed — valgrind-clean. **Amended**: this was `Buffer` with `push`, which needs the many-item pointer `[*]T` that D27 leaves out of v0, so it was never reachable at M5.5. Option (b) keeps the question the acceptance was asked to answer — can the language express a heap data structure and release it — and defers only the growable-array half to M8. | Whether the language can express a real data structure |
| **M6** | Generics (D39's spelling, D11's checking), bounds (D40), the monomorphisation **worklist** and D42's termination rule — for functions *and* aggregates — name mangling with type args, and the §12 decisions M6 is the deadline for — **all now taken (2026-09-18)**, four of which carry implementation into this milestone: ~~**D41's mixed-signedness comparison**~~ (moved here from M6.5, because §12's `T` generalisation depends on it; **done 2026-09-18**, §15 slice 1g — and it is not only signedness, see the correction in D41), ~~**`cast`/`wrap` on a type parameter**~~ (moved here too — the shipped range diagnostic tells authors to reach for it, so it is not optional; **done 2026-09-18**, §15 slice 1f), ~~**overloading**~~ (constructors first, and the `ref`/pointer mangling collision with it; **done 2026-09-18**, §15 slice 3), and ~~**type-argument inference**~~ (**done 2026-09-18**, §15 slice 4 — which also pays slice 3's deferred generic tie-break). | `max<i32>` and `max<f64>` both work; a generic `Box<T>` with a destructor drops correctly; `u32 < i32` and `i64 < f64` both compile and answer correctly; `wrap<i32>( a )` works for an `Integral` `T` and `cast<f64>( a )` for a `Floating` one; `C( i32 )` and `C( f64 )` coexist; `i32 x = id( 5 );` needs no written type argument. | Instantiation, mangling |
| **M6.5** | The debts that are neither M6's feature nor M8's: ~~D41's mixed-signedness comparison~~ **moved into M6** — §12's generalisation of it to `T` depends on it, ~~`cast`/`wrap` on a type parameter~~ **moved into M6** — the shipped range diagnostic points at it, ~~§12's empty-aggregate rejection~~ **done (2026-09-19)** — and it kept the `kl_rt_alloc` guard that §12 expected it to delete, because pinning `malloc( 0 )` is worth a branch independently of what reaches it, ~~the two KIR passes carried from M5.5~~ — empty-block threading and constant-branch folding — **done (2026-09-18)** as one pass, `ir/simplify.cpp`, because folding forces pruning and pruning needs a finished graph; the warning that went with them was cut rather than written, — ~~the mangling rework (length-prefixing, the `ref`/pointer collision, `kl__id__T__i32`)~~ **done in M6's slices 2b and 3e** — 2b forced the length-prefixing, and ~~the `ref`/pointer collision is unreachable until overloading lands~~ **overloading landed in M6 and took the collision with it**, ~~constant checking inside a generic body~~ **done in M6's slice 1f** — literal adoption is what made it reachable, so it was paid where it broke rather than carried. **Two arrived with M6's decisions**: ~~§12's **conditional expression**, which is a decision rather than a debt and is here to stop it being an accident~~ **decided and built (2026-09-19)** — `?:` ships and `if`-as-expression is **rejected**, not deferred, and ~~**function pointers**, whose implementation is M6.5 or M8 depending on whether FFI asks first~~ **moved to M7 (2026-09-19)** — M8 is the library, and a language feature should not arrive inside a milestone that is otherwise about writing Keel in Keel. **Five more scoped here (2026-09-19)**, all of them debts this milestone exists for rather than features: ~~**splitting `sema/type_checker.cpp`**, which the §15 entry deferred until KIR landed and which has since gone from 2350 code lines to 7898 on one file-local class of 158 members — done *before* M7 rather than after, because M7 adds member-declaration rules to exactly this file and splitting is cheaper before the addition than after~~ **done (2026-09-19)**, and the class was kept: the audit measured the free-function shape this milestone assumed and found it reaches 7% of the file, so `Checker` moved to `sema/checker.h` and its 127 definitions to nine files, largest 1,373 code lines, with no assertion or golden changed — **the acceptance criterion was met and the code did not get easier to read**, which is recorded in §15 and in §3.2 rather than quietly dropped, and is why `Checker` was then dissolved into fourteen classes instead of kept — ~~that work is scoped, unscheduled, and not part of this milestone~~ **Stage B done and closed out (2026-09-24)**: seventeen classes, one per file, a machine-checked DAG at zero violations, and `checker.h` and all nine `check_*.cpp` deleted; **the mangling category tag**, the surviving half of that debt now that M6 length-prefixed the other half, owed because a static method is the first scheme with no receiver to tell it apart - ~~**and reachable today rather than at M7 (2026-09-24)**: a method and a free function of one name emit the same C symbol, in both the by-value and the `ref` spelling, and `cc` refuses the result~~ **done (2026-09-24)** — the tag leads a member's argtypes as `S<type>` and the receiver leaves them, see §15; ~~**`g().t = 1;`**, which assigns into a discarded temporary and which the conditional now inherits~~ **done (2026-09-24)** — and it was two bugs, not one: the same missing root let a write through a returned `const ref` take effect, which is a soundness hole rather than a useless write, and the struct-literal spelling aborted the lowerer rather than compiling to nothing, see §15; ~~**`enum Nothing { };`**, rejected on the empty-aggregate precedent so that an accidental spelling does not become the one uninhabited types have to live with~~ **done (2026-09-24)** — one pass beside `check_aggregate_has_fields` rather than inside it, because an enum was never reached by that rule rather than permitted by it, see §15; and ~~**the `Arena`'s decision**, whose two predicted callers have both been decided against — wire it to the `Interner` or delete it, but stop carrying it unowned~~ **decided and deleted (2026-09-24)** — the third candidate was measured rather than argued and bought about ten allocations per compile, and the arena an `Interner` would want is not the node arena that was built for the two falsified callers, see §15. **M6.5 is complete.** | `struct Empty { };` is refused naming a one-variant `enum`; `while( true ) { return 7; }` needs no `return` after it, `for( ; ; )` lowers to two blocks rather than three, `a > b ? a : b` compiles, runs only the arm it chose, and is refused when the arms disagree; `struct foo__ { };` beside `i32 foo()` no longer mangles to one name; `enum Nothing { };` is refused and `( c ? a : b ).t = 1;` is too; no source file in `keelc/src` exceeds 3000 code lines — **met (2026-09-19)**, the largest is now `parse/parser.cpp` at 2786; and the `Arena` either has a caller or is gone — **met (2026-09-24)**, it is gone. | Paying debts before they compound |
| **M7** | **Static methods** — a function that belongs to a type but takes no receiver, called `Type::name( args )`. The §15 debt, scheduled here because M8's library is the first thing that wants one: `Vector::with_capacity`, `String::from_bytes`. Scope is *methods only* — type-scoped **data** waits on M8's modules and globals, function-local static storage has no customer, and internal linkage is the access-control debt below it rather than this one. The **spelling is undecided** and §12 now carries it: every method has an implicit receiver, so something has to say "this one does not", and D30 already gives `Type::name` at the call site without saying how the declaration is marked. **Two more scoped here (2026-09-19). Access control**, the §15 debt sitting directly below this one, because M7's own customer argues for it: `Vector::with_capacity` is a named constructor, and a named constructor only earns its place if the ordinary one can be hidden — so the feature that motivates M7 is incomplete without it. It is a resolver feature, member lookup carrying visibility, and a slice of its own rather than a rider. **Function pointers**, moved from M6.5's fork: the alternative was M8, and M8 is the standard library — a milestone about writing Keel in Keel should not also be where a language feature first appears. It is the smallest of the three and goes last, because nothing else here depends on it. **Order matters within the milestone**: the spelling decision, then static methods, then access control, then function pointers. | A named constructor returns an aggregate, is called as `Type::make( args )`, and is refused as `value.make( args )` — both a golden and the mangling that tells it from a method; a field declared private is refused from outside its type and accepted from a method of it; and a function's address is taken, stored in a variable, and called through it. | **Whether a type is a namespace, and whether a function is a value** |
| **M8** | Modules (`import`), multi-file compilation, then begin `Vector` and `String` **in Keel**. | A two-module program. Then a `Vector<i32>` that grows and frees. | **Whether the design actually works** |

**M3 is where this stops being a toy** — it is the first thing C cannot do for
us. **M4 is where we learn whether the ownership model is real.** **M5.5 is where
it becomes usable**: methods and allocation together are what let a type own,
grow and free real storage, and neither is worth much without the other. A
`Vector` needs `push` to have somewhere to live *and* somewhere to allocate.

**M5.5 was a gap rather than a plan.** Methods appear in no milestone above and
on no exclusion list in §6.6 — they fell between M3, which delivered constructors
and destructors, and M8, whose acceptance is a `Vector<i32>` that grows and
frees, which cannot be written without them. The runtime is the other half of the
same missing floor; §12 has carried its open questions since M3 without a
milestone to answer them in. Both are deferred deliberately from here: **every
question about either is answered when M5.5 starts, not before.** What is settled
is only the order — after M5's payload-carrying `enum`, before M6's generics,
because a generic container is exactly the thing that needs both.

**M6.5 is a named place for debts, not a feature.** Three of the four items in it
were found while building M6 and belong to none of it: D41 is a rule about
concrete numeric types that generics only *surfaced*, the mangling rework was owed
before generics existed and generics made it uglier, and constant checking inside
a generic body is a hole that literal adoption opens rather than one it causes.
Each is small, each is the kind of thing that gets carried indefinitely because no
milestone owns it, and carrying them into M8 means paying them while also finding
out whether the language works. The milestone exists so they have somewhere to be
finished rather than somewhere to be remembered.

**M7 is a renumbering, and the old M7 is now M8** (2026-09-19). Static methods
had been the first entry in §15's debt list since methods landed, with no
milestone and a trigger condition — "worth designing when the first one wants to
exist" — rather than a date. The trigger is met: M8's acceptance is a `Vector<i32>`
that grows, and the first thing anyone writes to build one is `Vector::with_capacity`.
Folding it into M8 was the cheaper edit and was rejected, because M8's acceptance
program does not exercise a static method, and a milestone whose feature its own
acceptance never runs is exactly what M6.5 exists to prevent. So it gets a
milestone and an acceptance of its own, and lands *before* the library rather than
alongside it. Every "waits for M7" elsewhere in this document meant "waits for
modules and the library" and now reads M8; none of them were statements about the
past, so nothing here was rewritten to say something it did not say at the time.

Write no standard library before M6. Write no `Shared<T>` before M8.

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

Use unit tests for anything with an invariant expressible in C++: the
interner, span arithmetic, token boundaries, lattice joins. **With the caveat the
`Arena` earned (2026-09-24)**: a suite this style makes cheap to write is not evidence
that the thing under it is used, and the `Arena`'s twelve cases outlived its last caller
by the whole project. Test what the compiler calls.

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
    common/               # interner, span, diagnostics, small containers
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

**Superseded at M5.5.** One repository, three projects, three languages:

```
keel/
  keelc/                  # the compiler, C++
    src/                  # the layout above, unchanged
    test/                 # golden corpus + run_tests.sh
  keel_rt/                # the C floor every Keel program links against
    src/ test/
  keel_stl/               # the standard library, written in Keel
    src/ test/
  CMakePresets.json  vcpkg.json  .clang-format
```

Each project is buildable from the root and **keeps its own include root**, so keelc's
sources still say `#include "lex/lexer.h"` and never name the project they are in - the
restructure moved every file and changed no include. `keel_rt` is a separate project
rather than part of keelc because it is not part of the compiler: it is *data the
compiler ships*, in a different language, linked into programs keelc produces. `keel_stl`
is Keel compiled by keelc, and is empty until M8. What makes one repository the right
answer rather than three is that a change to the runtime and the change to the compiler
that needs it are one commit, and one build proves both.

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
writes its own interner and containers — those are the parts we are
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
| Do we keep `?` for error propagation, or find a spelling from the C family? It is the only construct in the language with no C++ heritage (D6). **Leaning: a prefix keyword, `try read_file( path )`.** A single `?` is easy to miss, and what it does — return from the enclosing function — is the loudest thing an expression can do; every other early exit in the language is a keyword at the start of a line. `try` is free here, and **L14 is what makes it free permanently**: no exceptions is a Law, not a deferral, so there will never be a `catch` for a reader to expect. `try expr` is also a *syntax error* in C++ rather than a reinterpretation, so §5.1 permits it outright. The worry that it implies `catch` is real on first reading and dies on the second — Zig is the precedent, using `try` for exactly this with no exceptions anywhere; Swift is the counter-example, where `try` genuinely does pair with `throws`. **The counter-evidence is real and worth recording**: Rust shipped `try!( expr )` first and replaced it with `?` precisely because the loud form buried the happy path once calls nested — `try parse( try read_file( path ) )` is worse than `parse( read_file( path )? )?`. Which suggests the answer may be loudness *plus* a rule against nesting them, rather than loudness alone. **A function that fails in more than one way still returns one `Result`**, because the error type is an `enum` — that is what payload-carrying variants are *for*, and it is why they land before error handling rather than after: `enum Config_error { Io( Io_error ), Parse( Parse_error ) }`. The question that leaves open is whether propagation **converts**: `try read_file( path )` inside a function returning `Result<Config, Config_error>` is handed a `Result<String, Io_error>`, and those error types differ. Rust converts via a `From` trait, which Keel cannot copy — traits are M6 at the earliest, and possibly never (§6.6). **Leaning: convert when there is exactly one way to.** If the target enum has precisely one variant whose payload is the source error type, `try` wraps it; if there are none or several, that is an error and the author writes the wrap. Unambiguous by construction, no trait required, and the same shape as D26's literal adopting a type from context and D30's scoping — Keel already prefers "the one reading that exists, or a diagnostic", never a silent pick among several. The alternative is no conversion at all, which is more explicit and considerably more verbose, and which can be relaxed into the above later without breaking a line of code. **The conversion sub-question is answered by the error-union row below** (2026-09-18): propagation widens into a superset rather than converting, so there is no conversion rule left to define — which is one of the arguments *for* the union, and the two entries are one decision. | M5 for the shape, but nothing binds until after M6 — `Result<T, E>` needs payloads *and* generics, so neither spelling can be written before then |
| **What happens on integer overflow?** D5 is often mistaken for an answer here, and is not: it governs conversions *between* types, not arithmetic *within* one. `u32 a = 0; a - 1;` involves no conversion, so D5 is silent and the result is 4294967295. §7.7's `-fwrapv` currently makes signed overflow wrap silently too — defined, which is better than C++'s UB, but still a wrong answer delivered quietly. Underflow is not a separate question: `u32 a = 0; a - 1;` is overflow off the bottom, and one decision covers both. Float underflow to denormals is IEEE's business and is not a trap candidate. The options are wrap (status quo — fast, silent, a wrong answer delivered quietly); trap always (a predicted branch per operation, and some lost vectorisation); trap in debug only, as Rust does (free in production, but tests and production then compute different answers); saturate (surprising, and wrong for a systems language); or trap by default with `+% -% *%` to opt out. **Decided: wrap.** A branch per operation genuinely inhibits auto-vectorisation, and Keel's performance claim makes that a real cost rather than a theoretical one — and wrapping is what every programmer was taught happens. Silence is answered two ways. **Compile-time rejection of constant overflow** — `u32 d = 1 - 2;` is an error rather than 4294967295 — is free, backend-independent, and can land now. **Opt-in runtime checking** is a frontend feature, not a `cc` flag: passing `-fsanitize=signed-integer-overflow` through to `$CC` would work today and evaporate with the backend, which is precisely the coupling §2.2 forbids. The frontend decides where a check belongs and each backend spells it — `__builtin_add_overflow` in C, `llvm.sadd.with.overflow` in LLVM. That makes it **KIR work at M3**: a checked add is an instruction, and building it into the AST-walking emitter first means building it twice. A sanitiser flag pass-through is a fine convenience until then, but it is not the design. With no trapping default there is nothing to opt out of, so `+%` is not needed. Keep `-fwrapv`, so the wrap is defined rather than UB. A manifesto that claims safety by default cannot leave this at "whatever `-fwrapv` does". D5 now depends on the answer: `T op T` yields `T` (§6.4), so `u8 + u8` can overflow where C++'s promotion to `int` could not — that is the one `†` divergence in the §6.3 audit, and whether it traps or wraps decides whether the divergence is loud or silent. | **Decided: wrap.** Constant-overflow rejection is **done**; runtime checking is KIR work at M3. |
| **What, if anything, does `->` come to mean?** Free, with nothing assigned. D22 freed the token and the three call-site markers it was a candidate for are keywords instead: `move x` (transfers, D2), `out x` (the callee assigns it, and it need not be initialised first) and `ref x` (initialised, and may be modified) — C#'s distinction, which earns both. `->` stays a hard error naming `.`, and the token stays lexed so that error can be given by name. Rejected along the way: `socket -> connection` as a move expression (competes with `=`); a state-machine DSL (a domain feature in a language about ownership, and M5's exhaustive `switch` already makes illegal transitions a compile error); and scope injection, `user -> { greet( name ) }` — Pascal's and JavaScript's `with`, which JS deprecated in strict mode because you cannot tell a field from a local and adding a field silently changes the meaning of code that already compiled. That is D19's action-at-a-distance with a larger blast radius. | No deadline — it costs nothing to leave free |
| ~~**What is a cast?**~~ **Answered — D28.** `cast<T>( x )` preserves the value, `wrap<T>( x )` keeps the low bits, both keywords, table in §6.5. What remains open is narrower: `cast` is *defined* to check the value at run time and nothing can do that yet, so narrowing `cast` is refused rather than silently truncating. See the debt in §15. | M1 — done |
| ~~`a < b > ( c )` — a call to a generic, or two comparisons?~~ **Answered: the generic call, and the comparison reading is not expressible.** C++ needs `template` disambiguators and Rust needs turbofish because in both, `( a < b ) > ( c )` is a valid expression. In Keel it is not, and the argument is three steps. **D33 makes an operator a method on the *left* operand's type**, so `X > Y` needs `operator>` declared in `X`. The two-comparison reading therefore needs `operator>` on the type of `a < b`. **If a comparison yields `bool`, there is no such method and no way to write one** — `bool` is a builtin, and D33 already refuses an overload declared for a type it does not belong to. That kills the reading for every combination of aggregates and plain values, including an aggregate `c`, because lookup is on the left operand and never reaches it. Today D5 is what enforces this (`a < b > c` reports *no operator `>` for `bool` and `i32`*, from the type checker rather than the parser — D16 does not cover relational mixing). **The dependency is the whole argument, and it is not free**: this holds only while a comparison must return `bool`. If `operator<` could return an arbitrary type, `( a < b ) > ( c )` types again and the ambiguity is real — which is exactly how C++ expression templates work. That makes "comparison returns `bool`" a rule D33 has to state rather than assume, and it is listed there as still open. A `<=>`-shaped answer is compatible either way: `a <=> b` does not produce the `< ... >` shape, and a `<` derived from it still yields `bool`. **Two further kills that do not depend on the return type**: `a < b, d > ( c )` has no expression reading at all, because Keel has no comma operator; and with the comparison reading untypable, the parser can commit to the generic one with nothing to fall back to, which is what keeps L17's no-symbol-table rule intact. | **Answered**, conditional on D33 fixing comparison to `bool` |
| Do we ever add lifetimes/borrow checking, or is the non-escaping rule permanent? | After M8, with real-program evidence |
| **Are a call's type arguments inferred?** Today they are written — `id<bool>( false )` — and the bare form is an error naming the explicit one. **This is a different question from D39's declaration list and has the opposite answer.** At a declaration, `T` is a name being *introduced*, and nothing distinguishes that from a misspelled type, which is why the list is mandatory. At a call site the name is being *used*: it resolves to a declaration that already says how many parameters it has, so the only question left is arithmetic — the parameter is `T`, the argument is a `bool`, so `T` is `bool`. Both C++ and Rust infer here, and in both the explicit form is rare enough that writing it reads as emphasis. **Three things make it more than unification.** *Some calls cannot be inferred*: where `T` appears only in the return type — `T make<T>()` — there is nothing to deduce from, so the explicit syntax stays and the rule becomes "inferred where possible, required where not", which needs a diagnostic that says which case it hit rather than just failing. *Literals are circular, and it is the same circularity function overloading has*: D26 and L5 give a literal its type from context, and under inference the context is the thing being inferred, so `id( 1 )` needs a rule — a literal contributes its default type, or contributes nothing and lets another argument decide. *Conflicting deductions need a message that names the disagreement*: `same( 1, true )` against `T same<T>( T a, T b )` should say which two arguments could not agree, not "no match". **Worth doing, and worth doing after bounds**, because a bound constrains what a deduced type may be and the two interact — inferring `T` and then finding it fails `where T : Comparable` is a diagnostic that has to name both halves. **Decided (2026-09-18): inferred where possible, written where not.** A call deduces its type arguments from two sources — the types of the arguments, and the **expected type at the call site**, which `check( Node_id, Type_id )` already carries for every node and which slice D/E built the composite half of. **A literal contributes nothing.** `func( 5 )` against `T func<T>( T a )` has only a literal to deduce from, and D26 gives a literal its type from context — here the context *is* what is being deduced, so there is nothing to read. **That is not an exception to D26 but a consequence of it**: `i32 x = func( 5 );` works, because the expectation supplies `i32` and the literal then checks against a concrete type in the ordinary way, while a bare `func( 5 )` in statement position is an error naming the explicit form. **A struct literal contributes nothing either, and for the same reason** — `f( Box { r } )` against `f<T>( Box<T> b )` asks the instance to come from the argument when slice E made it come from the expectation. One rule covers both. **Deduction is structural, not positional**: `T f<T>( Box<T> b )` deduces `T = i32` from a `Box<i32>`, so the rule is “`T` occurs somewhere in a parameter's type”, not “a parameter is `T`”. **The list is all-or-nothing**: if any parameter cannot be deduced, every one is written. A partial list is where C++'s left-to-right-up-to-the-last-deducible rule comes from, and nobody can recite it. **This is one story with struct literals rather than two** — context decides, and says so when there is none. **Done (2026-09-18), §15 slice 4 — with one rule this entry owed.** It named two sources and did not say which wins, and `i64 x = id( a );` on an `i32` argument needs an answer: **the arguments decide, and the expectation fills only what they left**. That makes a deduced call behave exactly as the written one does — instantiate at the argument's type, then widen the result under §6.4 — where the other reading would refuse a program the explicit form accepts. The expectation is also **taken and cleared** by the call it belongs to, so in `f( g() )` what `f`'s result is wanted for says nothing about `g`; the consequence is that a return-type-only generic cannot be an argument to another generic call without writing its type arguments, and the message says which parameter had nothing to read. A constructor deduces against the **aggregate's** open form rather than its own return type, which makes `Box<i32> b = Box( 7 );` one story with `Box { 7 }` too. | **Decided and done** — M6, §15 slice 4 |
| **Can a numeric literal adopt a type parameter's type, and where is its range checked?** `T abs<T>( T a ) where T : Numeric { if( a < 0 ) { return 0 - a; } return a; }` is the first generic anyone writes that needs one, and it does not compile: a literal takes its type from context (D26), and `T` is not a context `check_literal` knows. **Leaning: yes, and the rule is asymmetric by literal kind.** An integer literal may adopt a `Numeric` `T`, because every numeric type represents an integer — `f64 x = 1;` already works concretely and the generic case should not differ. A *float* literal needs `Floating` specifically, because a `Numeric` `T` may turn out to be an integer and `1.5` is not one. A `bool` literal never adopts, and neither does `null`: `Equatable` admits pointers *and* enums, so it cannot tell which. A `char` literal follows the integer rule under `Integral`. **The range check is the real question**, because it is the one thing about a literal that cannot be answered from the bound alone: `return a + 300;` is well-typed for every `Integral` `T` and out of range for `u8`. **Leaning: check it at the definition against every type the bound admits, and require an explicit `wrap<T>`/`cast<T>` past that.** For `Integral` that intersection is `0..127`, which is not as tight as it sounds — the literals that appear in generic numeric code are `0`, `1` and `2`. **The route not taken, and why it is worth writing down**: the instantiation set is *closed* — Keel has no separate compilation, so by monomorphisation every `T` a generic is used at is known, and the range could be checked against that list instead. It is a real mechanism and it is more permissive. It was rejected because **adding a call site elsewhere would then break a function that compiled yesterday**, which is precisely the failure mode D11 exists to abolish; and the signature would no longer be the contract, since nothing in `where T : Numeric` warns a reader that `300` is in the body. Paying that for a case whose realistic literals all fit is a bad trade. The mechanism stays available for an obligation that has *no* definition-time answer and no escape hatch; the literal has both. **What the restriction really exposes** is that `Integral` is too coarse: an author who writes `-1` means "signed", and there is no bound that says so. Watch for that pressure rather than pre-empting it — a signedness or width bound is a real extension to D40's fixed set, and one example does not justify it. **What does not work is letting the literal fall back to its default type**: `a + 1` would then be `T + i32`, which the definition would have to accept and which no instantiation does, since §6.4 refuses mixed signedness. That is D11 inverted — the definition passing what an expansion rejects. **Decided (2026-09-18): as shipped.** The definition-time check against the intersection of the bound is built, and so is the asymmetry by literal kind — `300` under `where T : Integral` is refused naming `i8`, and `1.5` under `Numeric` is refused telling the author to write `Floating`. **The widening bound is shelved, not rejected.** `Signed` is the case this row identified and one example is not evidence; it would also be **the first bound that is not a capability**, and D40's set is closed *because the operator set is closed*, so a range predicate is not an operator bound and adding one retires the argument for the set being fixed. Raise it again if real generic numeric code asks. **The escape hatch is still owed**: `wrap<T>( 300 )` and `cast<T>( x )` do not compile today — they answer *“`wrap` cannot convert `i32` to `T`”* — so the shipped diagnostic points at a remedy that does not exist. It moves into **M6** with the rest of §12's generic-numeric work (2026-09-18), rather than waiting for M6.5 as a debt: a shipped diagnostic naming a remedy that does not compile is not a debt, it is an unfinished feature. | **Decided: as shipped**; `cast`/`wrap` on a parameter lands with it at M6 |
| **Should a `T` be comparable against a concrete numeric type?** `i8 < i32` and `i8 + i32` are both legal — §6.4 mixes *widths* freely and refuses mixed *signedness* — so the parameter rule is stricter than the concrete one it sits above: `a < x` with `T : Numeric` and `x : i32` is refused for needing both operands to be the same type. **Leaning: keep it refused, and say so rather than calling it principled.** The blocker is signedness, not width: `Numeric` admits `u64`, `common( u64, i32 )` does not exist, so no answer holds for every admissible `T`. The closed-world check above would answer it per instantiation, and is a *worse* trade here than for a literal — an operand mismatch is a type error, so deferring it means the signature stops telling you whether the body compiles, which is the whole of D11. **Most of the pressure disappears with literal adoption**: the case that occurs in practice is `a < 0`, where `0` is a literal and adopts `T`. What remains is `a < x` against a typed variable, where `cast<T>( x )` is explicit and correct. ~~**Leaning: keep it refused.**~~ **Decided (2026-09-18): allowed — and the leaning above was stale when it was written.** It argues that `Numeric` admits `u64`, that `common( u64, i32 )` does not exist, and so that no answer holds for every admissible `T`. **D41 removed that premise**: comparison is not arithmetic and needs no common type, because the result lands in `bool`. This row was written against the pre-D41 rule and never updated, so this closes a contradiction rather than deciding something new. `<`, `>`, `<=`, `>=`, `==` and `!=` accept a `T` against a concrete numeric exactly as they accept two concrete numerics; `+`, `-`, `*` and `/` keep D5's common-type rule, because a result has to land somewhere and a `bool` always does. ~~**Gated on the numeric bounds only** — `Numeric`, `Integral`, `Floating` and `Comparable`, all four of which admit integers and floats and nothing else. **`Equatable` is excluded**, because it also admits `bool`, enums and pointers, and D41's closing line already refuses an enum against a number.~~ **Corrected when it was built (2026-09-18): gated on `Numeric`.** Including `Comparable` was reasoning from an accident rather than from a meaning. What this rule needs is that `T` *is a number*, and `Numeric` is the bound that says so; `Comparable` says `T` is orderable and `Equatable` says it is comparable for equality, and both are satisfied by exactly the numbers **only because D33's operators are not shipped** — D40 says as much in its own closing line. When they land, a type that orders itself satisfies `Comparable` and still does not compare against an `i32`, and a body that passed D11's check would fail at every instantiation. That is the failure mode D11 exists to abolish, so the gate is the meaning and not the coincidence. It also makes the help honest: an author who wrote `Equatable` wanted equality, and *"write `where T : Comparable`"* asks them to promise ordering for a reason that has nothing to do with ordering. The message now reads *"`T` must be a number to compare against `i32`; write `where T : Numeric`"*. **D41 is unimplemented even for concrete types** — `u32 < i32` is refused today, not only `u64 < i64` — so it moves from M6.5 into M6 and this generalisation lands with it rather than after it. **Built (2026-09-18), §15 slice 1g.** The gate is one test rather than four, because `closure()` puts `Numeric` in `Integral` and `Floating` already. No walk over the admissible types was needed either, unlike the conversion in slice 1f — every type `Numeric` admits compares against every number, so the answer is `bool` without asking which `T` turns out to be. **What was left out and is not decided here**: one `T` against *another*, which this row never considered. Every admissible pair would compare, so it is scope rather than soundness; it stays refused, with a test pinning it, until something asks. | **Decided: allowed** — D41 generalised to `T`; **done 2026-09-18** |
| **Is there a `Boolean` bound?** No, and the reason generalises: the bound would admit exactly one type. `Floating` admits two, `Integral` eight, `Equatable` every number, `bool`, every `enum` and every pointer — each names a *set* a body can be written against. A bound whose admissible set is a singleton is that type spelled longer, and the parameter should be `bool`. It stays a singleton because Keel has no conversion operators and D33's set is closed, so nothing can ever become bool-like; `&&` in particular short-circuits, which is the one operator overloading has no sound meaning for. | Answered; revisit only if D33 ever admits a conversion operator |
| D41 | **Comparison is not arithmetic, and only arithmetic needs a common type.** `<`, `>`, `<=`, `>=`, `==` and `!=` accept any two numeric operands, mixed signedness included: `u64 a; i64 b; a < b` is legal and gives the mathematically correct answer. Every other binary operator keeps D5's rule — both operands widen to the smallest type that losslessly holds both, and no such type means a hard error. The emitter is responsible for the answer being right, not the C compiler: where no wider type exists to compare in, it emits a sign guard rather than a bare C comparison. | **The result type is the whole difference.** `a + b` must land somewhere, and for `u64` and `i64` there is nowhere — no integer type holds both ranges, so D5 refuses it and is right to. `a < b` lands in `bool`, which holds every answer, and the mathematical question "is this value less than that one" always *has* an answer regardless of how either side is stored. Refusing it was D5 applied one operator too widely: the rule protects a *result*, and a comparison has no result to protect. **C is the cautionary tale, not the precedent.** `-1 < 1u` is *false* in C, because the usual arithmetic conversions force both sides to a common type before comparing and `-1` becomes `UINT_MAX`. That is the single most-cited integer footgun in the language, `-Wsign-compare` exists for it alone, and it is caused precisely by insisting on a common type where none is needed. Keel does not inherit it because Keel does not do the conversion: the comparison is evaluated in a domain wide enough for both operands, or by cases when no such domain exists. **§5.1 is satisfied by rejection-turned-acceptance in the safe direction.** Keel previously refused `u32 < i32`; it now accepts it and answers correctly, so no program changes meaning — there were no such programs. Against C++ the divergence is real and is an *improvement* that must be listed in §6.3: C++ compiles `a < b` for mixed signedness with a warning and the wrong answer, Keel compiles it with the right one. This is the one place the project deliberately diverges by being *more* permissive than the audit surface usually allows, and it earns it by removing a silent wrong answer rather than adding one. **The cost is in the emitter, and it is bounded but not as small as this entry first claimed** — the two sentences that follow replace a pair that were wrong, and the correction is the useful part. **Eight pairs have no domain, not two.** Four are mixed signedness against `u64`, and four are a 64-bit integer against a float: no `f64` holds `i64` or `u64` exactly, so `i64 < f64` is in the same position as `i64 < u64`. **Float against integer was said to be unaffected, and it is half the work.** `f64 < i32` did already widen into `f64`, but `f64 < i64` did not compile at all and now must. Once each operand widens as far as its own kind reaches the eight pairs become **three shapes** — `i64`/`u64`, `i64`/`f64`, `u64`/`f64` — and the backend writes one small function per shape and operator, emitted only where used. Only the first is the branch-free guard described above. **The float shapes cannot be one expression**: truncating a `double` to an integer is undefined for a NaN or an out-of-range value, so the NaN test and the two range tests are not an optimisation but the thing that makes the truncation legal, and past them the fraction is what breaks a tie the integer comparison cannot see. All three are exactly testable against every boundary value, which is what they were checked against. **What this does not open**: the mixed operands still have to be *numeric*. A pointer and an integer do not compare, an enum and a number do not compare, and `Comparable` as a bound is unchanged — this is about which pairs of concrete numeric types an operator accepts, not about what kinds of thing are ordered at all. |
| D42 | **A generic may not be instantiated with a type built out of its own parameter, around a cycle.** `f<T>` calling `f<T>` is ordinary recursion and is fine at any depth; `f<T>` calling `f<T*>` is an error, and so is any cycle through several generics carrying at least one edge that wraps a parameter in a type constructor. Detected at the definition, by the same walk `contains_itself` does over struct fields, and reported at the call that builds the type rather than at the one that closes the cycle. | **Monomorphisation emits one function per set of type arguments, so it works only if that set is finite**, and this is exactly the shape that makes it infinite: `f<i32>` needs `f<i32*>` needs `f<i32**>`, without end. Something has to reject it. **The alternative was a depth limit**, which is what C++ does and what `-ftemplate-depth` exists to raise. It was refused on three counts: the number is unjustifiable — any value is arbitrary and every real program sits far below it, so the limit only ever fires on a mistake; the diagnostic lands at whichever expansion happened to hit the wall, which is a place the author never wrote; and it makes a *program property* depend on a compiler setting, which L1's "no configuration that changes what a program means" already refuses elsewhere. **The shape is decidable without instantiating anything**, which is why it does not need one: the question is whether a cycle in the generic call graph carries an edge whose argument is a construction over a parameter, and both halves are visible in the source. That keeps it inside D11 — checked once, at the definition, before any instance exists — where a depth limit would have been the deferred check D11 exists to abolish. **Forwarding is the line, not recursion.** `f<T>` to `g<T>` to `f<T>` is finite however long the cycle, because the arguments never change and the second lap produces an instance already emitted; it is accepted, and the recursion it describes is the ordinary runtime kind. Only building is refused. **One level with no cycle is also fine** — `f<T>` calling `g<T*>` needs two instances and stops — so the rule is about cycles rather than about type construction, which matters because `Vector<T>` storing a `T*` is the first thing M8 writes. **The escape** is to take the built type as a second parameter, which turns an unbounded family into a bounded one: `f<T, U>` can be called at `<i32, i32*>` without `f` ever naming a type it did not receive. |
| ~~**What is the bound vocabulary, and is it structural or nominal?**~~ **Answered — D40.** Nominal, a fixed set, with implication; `Copyable` rather than `struct`; and the mechanism is a bitset consulted in front of the operator table. The open remainder is user-defined bounds, which arrive with traits and have no customer yet — the test being whether M8's `Vector` and `String` need one, and they appear not to. | Answered; user-defined bounds wait for traits |
| **Does Keel have function overloading?** **This is not currently a decision, and that is the finding.** No Law refuses it, no D-entry refuses it, and it is absent from §6.6's "not in v0" list; it does not exist only because the resolver reports a second declaration of a name as a redeclaration. Two places in the codebase already assume it is coming — the mangler encodes parameter types with the comment *"parameter types are what make overloads distinct, so they are part of the name even in v0 where no overloads exist"*, and §15 carries a note headed *"noted for whenever overloading arrives"* — and **D33 contradicts the implementation outright**, saying constructors *"do have arg types because they are overloadable"* while `C( i32 )` and `C( f64 )` together report *"`C` already has a constructor"*. An accident that three places already plan around should be settled deliberately. **The cost is far lower here than the C++ experience suggests, and the reason is D5.** C++'s overload resolution is enormous because of what surrounds it: integral promotions, standard conversion sequences, user-defined conversions, and a ranking system to order them all. Keel has none — an argument either has the parameter's type or it does not — so resolution collapses to "which candidate matches exactly", with no ranking table. There is no ADL to add, and D33 keeps operators out of it entirely by making them methods with one candidate. **D31 makes the call site *more* informative than C++'s**: `f( x )`, `f( ref x )` and `f( move x )` are distinguished by what the author wrote rather than by reference-binding rules, so the marker participates in selection for free. **Three wrinkles, and only the last is hard.** *Literals are circular*: D26 and L5 give a literal its type from context, and under overloading the context is the overload set, so `f( 1 )` against `f( i32 )` and `f( f64 )` has nothing to appeal to — C++ answers by ranking, which Keel deleted. The plausible rule is that a literal prefers the family it is written in, integer or float, and that is a rule to state rather than derive. *`ref i32` and `i32*` mangle identically*, both to `i32p` — §15 already records this as harmless only while a second `f` is caught by name, and it stops being harmless here. *Generics are where it genuinely gets hard*: overloading plus templates is where C++ reaches partial ordering, and Keel needs a rule for a generic and a non-generic both matching — probably that the non-generic wins, with two generics being ambiguous. **The customer that forces it is constructors.** A class that cannot have two is a real limitation on no deferral list, and it is the narrowest slice to land first; `print( i32 )` and `print( f64 )` want it again at M8. **Decide it with M6**, because generics are the only part that is hard and deciding before they exist would be guessing. **Decided (2026-09-18): yes, for functions and methods.** What settles it is not ergonomics but that **D33 already contradicts the compiler**, and choosing “no” would mean rewriting D33, the mangler's comment and §15's note — three places that already plan around it arriving. **Constructors first**, as the narrowest slice and the one with a real limitation behind it. **The literal rule is shared with type-argument inference and stated once**: a literal prefers the family it is written in, integer or float, and two candidates in the same family are ambiguous. **A non-generic candidate beats a generic one, and two generics are ambiguous** — committed to deliberately, with no specificity ranking now or later, because ranking generics by how well they match is rebuilding what D5 deleted. **The `ref i32` / `i32*` mangling collision stops being unreachable.** §15 records it as harmless only while a second `f` is caught by name and M6.5 defers it as unreachable until overloading lands; overloading landing is exactly what makes it reachable, so it is M6 work and ships with this rather than after it. **Done (2026-09-18), §15 slice 3 — with two corrections this entry owes.** *First, “an argument either has the parameter's type or it does not” was false as implemented.* `Type_table::holds` is §6.4's lossless widening, so a `u8` argument reaches an `i32`, an `i64` and an `f64` parameter alike, and two candidates can both accept — which needs ranking, the machinery this entry promises never to rebuild. The rule that keeps the promise is stated here rather than derived: **selection is by exact type, and widening happens after a candidate has been chosen, never to choose one.** One candidate therefore behaves exactly as it always did, and `u8` against `f( i32 )` beside `f( i64 )` is refused rather than ranked. *Second, signature identity is what a call site can write, not what a declaration says.* `f( i32 )` and `f( const ref i32 )` are different parameter types and the same call — a `const ref` takes no marker — so they are refused as a pair; `f( ref i32 )` and `f( i32* )` are the same lowered pointer and two different calls, so they coexist. Identity is therefore the pair **(declared type, marker the call must write)**, which the checker already computed for D31's argument rules, and that one definition settles the duplicate rule, the selection filter and the mangling alike. **The generic tie-break was not implemented and is deliberately deferred to inference**: a call with type arguments and a call without them select disjoint candidate sets today, so “the non-generic wins” has nothing to decide, and writing it now would be a rule no fixture could exercise. **Paid the same day, §15 slice 4**: deduction is what lets a call leave its type arguments unwritten, which is what puts a generic and a non-generic in one set, and the tie-break is three lines reading `is_generic` off the declarations. **Members are the one place the decision had no answer, and it is now taken.** `at( T )` and `at( i32 )` on a `Box<T>` are one signature at `Box<i32>`, and a method call writes no type argument that could say which was meant — unlike a free function, where `g<i32>( x )` against `g( x )` is a syntactic choice. Per-position specificity would be the ranking this entry forbids, so the pair is **refused at the declaration**: two members of one name that some substitution could make identical may not both be written. That keeps the failure where D11 wants it, at the contract rather than at an instantiation, and relaxing it later is safe while tightening would not be. The check is deliberately coarse — it treats any parameter mentioning a type parameter as able to collide, so `at( Box<T> )` beside `at( i32 )` is refused although no `T` makes them equal. | **Decided and done** — M6, §15 slice 3 |
| **Are interfaces/traits the only form of polymorphism, or is there virtual dispatch?** They answer different questions and only one is forced at M6. **Static** — "what operations does this type support?" — is resolved at compile time, monomorphised, no indirection, no metadata; **dynamic** — "a collection of different types behind one interface" — needs a vtable pointer per object and an indirect call. **M6 forces the static half whether or not the dynamic one is wanted**, because D11's definition-checking *is* a bound system: the body is checked once against the bound, so the bound has to name the operations. D31 adds a concrete requirement to that vocabulary — a generic that mutates a bare parameter is legal only when `T` is a `struct`, so something like `is_trivially_copyable` has to sit beside `is_numeric`, or the error lands at the instantiation site, which is the C++ behaviour D11 exists to avoid. **Leaning: Rust's shape** — one declaration, static by default, dynamic opt-in and visibly spelled in the type. The prior art splits: Rust does both from one trait (`T: Draw` static, `&dyn Draw` dynamic); Zig has comptime duck typing and no built-in dynamic dispatch at all; Go's interfaces are *only* dynamic and implicit; C++ keeps concepts and `virtual` entirely unrelated. Keel already leans static everywhere else — §4 refuses RTTI, D33 gives operators exactly one candidate — so the dynamic half should wait for a real customer rather than be designed alongside the static one. **The M6 half is answered — D40.** Bounds *are* the static system: a named, closed, nominal set checked once against the definition, which is what D11 requires. D31's concrete requirement is met by `Copyable`, which means trivially copyable and is exactly the promise a body needs to copy or mutate a bare `T`. Nothing is left here for M6. | **Static half: answered — D40.** Dynamic dispatch after M8, with real code |
| **What exactly is in an `unsafe` block, and what does it permit?** **Half answered — D35.** The block exists as of M5.5, the two rules around it are decided (an unused block and a nested block are both errors), and what stays open is only the *list*: today it holds one operation, converting between pointer types. Raw-pointer dereference is the next candidate and is deliberately still outside, because gating it breaks programs that compile now. The rest of this entry stands as written. The shape is settled: Rust's model — `unsafe { }` blocks and `unsafe fn` — with Zig's `[*]T` beside it. What remains is the enumerated list. The property that makes Rust's version work, and the one most often misunderstood: **`unsafe` permits operations, it does not disable checks.** Move checking, type checking and D5 all still apply inside one; an unsafe block is not a different language. The two mechanisms are orthogonal rather than overlapping — `p + 1000000` on a `[*]T` is type-correct and catastrophic, so the type says the operation is *meaningful* while `unsafe` says the author *checked the invariant*. D's `@trusted` is deliberately **not** taken: a safe function containing an unsafe block already *is* one, so Rust's two levels encode D's three, and a standalone `@trusted` without D's full `@safe`/`@system` lattice would be an optional marker whose absence means either "safe" or "forgot" — the same defect that keeps `move` out of signatures under D2. | The block: **M5.5 — done**. The full list: whenever `[*]T` and `alloc<T>` give it something beyond the pointer cast to gate |
| **Does an empty aggregate exist, and what is its size?** `struct Empty { };` type-checks today and emits a C struct with no members, which is a **constraint violation in standard C** — GCC and Clang accept it as an extension and give it size 0, which is §2.2's boundary leaking: the emitted C compiles only because two compilers agree on something the standard does not define, and `-Wpedantic` rejects it. It is also how `kl_rt_alloc( 0 )` becomes reachable from valid Keel, which collides with the one invariant the allocation stopgap rests on — `nullptr` means failure and nothing else — since `malloc( 0 )` may legitimately return `NULL`. The runtime currently guards it by bumping a zero request to one byte, which treats the symptom. **Three answers, and the question is which problem is real.** *Reject an aggregate with no fields*, naming an `enum` with one variant as the tag-only alternative: cheapest, and it removes the C extension and the zero allocation together — but it forecloses the empty-struct-as-marker pattern that generics will want at M6, where a `Unit`-like type is the natural thing to instantiate a container with when there is nothing to store. *Give it a size of one*, as C++ does for an empty class: emits `struct { char pad; }` and matches what every C++ programmer already believes, at the cost of a byte no Keel program asked for and a field that shows up in nothing else. *Keep it at zero and emit standard C for it*, which cannot be done — C has no spelling for a zero-size object. Note the question generalises past `struct`: a payload-free `enum` variant, and any future `Unit`, land in the same place. ~~**Decide with generics.**~~ **Decided: reject it**, with a diagnostic naming a one-variant `enum` as the tag-only alternative. That removes the C extension and the zero-size allocation together, and the marker-type use that argued for keeping it is served by an `enum` with a single variant — which is a better spelling for "a type with one value" than a struct with no fields, because it says so. The runtime's one-byte guard can go when this lands. **The check is “an aggregate with no fields”, not “a type with no data”** — a payload-free `enum` variant stays legal, since it is the very thing the diagnostic recommends, and conflating the two would reject the fix the message suggests. | **Done (2026-09-19)** — `Checker::check_aggregate_has_fields`, called from `check_aggregate_members`; golden `sema/errors_empty_aggregates.kl`. **Two corrections to the decision above.** *The predicate is "no `Field_decl`", not "no members"*: a method-only class emits the same empty C struct, and the entry's wording would have let it through. *The runtime guard stayed.* This entry said it could go once the case was unreachable, and it is — `alloc<T>()` is single-object, and a payload-free `enum` is `int32_t`, so no valid program reaches `kl_rt_alloc( 0 )` — but pinning `malloc( 0 )`'s implementation-defined answer is worth one branch on its own terms, so the guard is kept with its rationale rewritten rather than deleted. One test died with the construct: `"an empty struct takes an empty literal"` tested a program the language no longer has, and was removed rather than repaired |
| **How is a static method declared?** M7 implements them and D30 already settles the *call* — `Type::name( args )`, the same scoped spelling a variant uses — so what is open is the declaration. Every method today has an implicit receiver as parameter 0, so something has to mark the one that does not. **Three shapes.** *A `static` keyword on the member*, which every C++ and Java programmer reads instantly and which costs one keyword and one parser branch; the objection is that `static` in C means three unrelated things and importing the word imports the confusion. *Declaring it outside the aggregate under a scoped name*, `Vector<T> Vector<T>::with_capacity( u64 n )`, which needs no keyword at all and is how C++ defines out-of-line members — but D18 forbids a declaration split from its definition, so this would be the one place a member's body lives away from its type. *Inferring it from the absence of a receiver mention*, which is the shape that cannot work: a method that never touches `this` is extremely common and is still a method. **Leaning: the keyword**, because it is the only one of the three that says what it means at the point of declaration, and because the objection to it is about C's other uses of the word rather than about this one. | **M7**, with the implementation |
| **Can a value in place position be assigned to?** `g().t = 1;` type-checks today, where `g` returns a `P` by value: the call's result lands in a temporary, the field is written, and the temporary is discarded — so the assignment is accepted and does nothing. Found while giving the conditional the same place handling (2026-09-19), which is how a conditional inherits it: `( c ? a : b ).t = 1;` now behaves identically. **Reading through a temporary is right and should stay** — `g().t` and `( c ? a : b ).t` are ordinary field reads — so the question is only about *assignment*, and the shape of the answer is a place that is readable but not writable. **Leaning: refuse it**, on the ground that an assignment which provably does nothing is never what was meant, and the diagnostic is easy to write. Note the rule has to name what a place came from rather than its type, which the lowerer knows and the checker currently does not — so it is a checker question with a lowering answer, and that is the part to design rather than the diagnostic. ~~**Scheduled: M6.5 (2026-09-19)**, with the other debts rather than left loose — it is one rule, it is pre-existing, and the conditional has just widened the set of programs that reach it.~~ **Done (2026-09-24)** — and it was not one rule: the same line hid a soundness hole, recorded in the row below. The struct literal was not accepted-but-useless either, it **aborted** in `lower_place`, so this removed a crash. No value categories were needed — naming what the place came from was enough, which reverses the cost this row assumed and the unit test that encoded it. See §15. | **Done (2026-09-24)** |
| **Can a place rooted in a call be written to?** Found (2026-09-24) while arguing the walkthrough for the row above, and it is a **soundness hole rather than a useless one**. `Node_id Places::place_root` resolved a root only when it was a `Name_expr`, so a place rooted in a *call* came back as an invalid `Node_id` — and `check_writable`'s `if( !root.is_valid() ) { return true; }` read that as permission, skipping every rule below it: `const`, borrow, and pattern binding alike. §8 permits only `const ref` returns, so **every** reference a call hands back is read-only, and `by_ref( a ).t = 2;` is therefore a write through a read-only borrow. It is accepted today and it **mutates**: the probe emits `( *kl_t4 ).kl_t_1 = 99;` and the program exits 99 instead of 7. The same hole runs through a `Method_decl` — a `const` method returning `const ref` lets a caller write the object the method was forbidden to touch, also verified at 99. The same write spelled through a *named* `const ref` binding is refused with ``\`r\` is `const```, so one fact has two spellings and two answers, which is the tell that the rule was never reached rather than deliberately relaxed. **The discriminator is `returns_a_binding`, not `is_const_binding`** — `const P f()` parses, and `is_const_binding` accepts a `Function_decl` and reads child 0 as a return annotation, so keying on it would call a `const`-returning *by-value* function a read-only reference; `returns_a_binding` answers false for it, confirmed by `const ref P r = const_by_value();` being refused. **Filed separately from the temporary row above** rather than as a footnote to it: they share a cause and a fix site, but a discarded write and a silent mutation are different classes of bug and the second was not in scope when M6.5 was written. **Done (2026-09-24)** — `check_writable` asks `place_source` for the landing expression before it asks for a root, and a call whose callee `returns_a_binding` is refused there. The goldens do not distinguish the two discriminators, so the `is_const_binding` mistake is killed by a unit test and nothing else. See §15. | **Done (2026-09-24)** |
| **Does a variant-less `enum` exist?** `enum Nothing { };` type-checks silently today and lowers to `int32_t`, so it is not a zero-size type and the empty-aggregate rejection above does not reach it — the two look alike and are not. Found while testing that rejection (2026-09-19), and it is a genuine hole rather than a deliberate permission: nothing in D7 or D30 says whether an enum with no variants is meaningful, and it is a type with **no values at all**, which is a different thing from the one-variant enum that the empty-aggregate diagnostic recommends. Such a type is uninhabited — nothing can ever be constructed, so a function returning one can never return — which is a real and useful concept (Rust's `!`, C++'s `[[noreturn]]`) and certainly not one anybody has designed here. **Leaning: reject it**, on the same ground as the empty aggregate — an accidental spelling of a concept the language has not decided it wants is worse than no spelling — and raise uninhabited types properly if a customer appears. ~~**Scheduled: M6.5 (2026-09-19)**, on the empty-aggregate precedent: reject the accidental spelling now, as one check beside the one that already refuses a fieldless aggregate, and leave **uninhabited types** genuinely undecided rather than letting `enum Nothing { };` become their spelling by default.~~ **Done (2026-09-24)** — `Signatures::check_enum_has_variants`, a pass of its own, and the "one check beside" turned out to be literal: the empty-aggregate loop filters on `is_aggregate`, so an enum was never reached by that rule rather than permitted by it. The message is deliberately not the aggregate's — *no variants, so it has no values* against *no fields, so it has no size* — because reusing it would restate the very conflation this row found. **Uninhabited types stay open**, as intended. Rejecting is the safe direction — it can be relaxed the day the concept is designed, and a program that relied on it cannot be un-broken. | **Done (2026-09-24)** — the check only; uninhabited types stay open |
| **Is there a construct for "do several unconnected things to one value"?** This started as a way to spell fallthrough — let a `case` label appear more than once, run the matching arms in written order — and was rejected for `switch` on action at a distance (D38): knowing what `case A` does would mean scanning the whole `switch` for other `case A`s. **The idea survives the rejection, because what it actually describes is not a `switch`.** A `switch` selects *one* arm and the exhaustiveness that makes it worth having depends on that. What this wants is the opposite: a value, and a list of independent things to do to it, each guarded, each running if it matches, in order. Validation is the obvious customer — a field checked against several rules, accumulating findings — and so is dispatch that genuinely is one-to-many. Open questions if it is ever built: what the arms produce (nothing, or a collected value), whether the guards are patterns or predicates (D34 refused predicates in a `case` because they make exhaustiveness undecidable — a construct that *has* no exhaustiveness claim is free to allow them), and whether it needs a keyword of its own or is a library shape once generics land. **Not a v0 feature and possibly not a feature at all**; recorded because rejecting it from `switch` is not the same as deciding against it, and the reasoning is easy to lose. | After M8, with real code to judge it against |
| Optionals: `T?`, `Optional<T>`, or a nullable-reference type — and how does it interact with `&`? | M5 |
| ~~Does `class` exist at all, or is `struct` the only aggregate?~~ **Answered — D29.** Both exist, split at trivial copyability: `struct` is a transparent aggregate that cannot own, `class` is a type with invariants that can. Answered early, at M3 rather than M8, because the cost is asymmetric — one keyword now, versus a breaking change to every program that declared a `struct` that should have been a `class`. | M3 — decided |
| **Is there a conditional expression, and is it `?:` or an `if` that has a value?** **This is not currently a decision, and that is the finding** — the same shape as the overloading row below. It appears in no Law, no D-entry and no §6.6 exclusion, and `a > 0 ? 1 : 2` is simply a parse error. **D33 already assumes it exists**, listing `?:` among the operators that may *not* be overloaded because "short-circuiting and sequencing cannot survive becoming a call" — an entry planning around a feature the compiler does not have, which is exactly what made function overloading an accident rather than a decision. **The token is free.** `Question` is lexed and was reserved for D6's postfix `?`, and §12's error-handling row has since redirected propagation to a `try` prefix, so nothing else wants it. **Two shapes, and they are not the same decision.** C's `?:` is a pure addition under §5.1 and costs one right-associative Pratt rule. Making `if` an *expression*, as Rust and Kotlin do, is the larger and more interesting option: it removes a second form rather than adding one, it composes with `switch` — which would want the same treatment or the language has two inconsistent answers — and it interacts with D26, since `i64 x = c ? 1 : 2;` needs the expectation to reach both arms exactly as `check()` already pushes it through a binary operator's operands. **The real content is the type rule, not the syntax.** Both arms must agree, and "agree" is D5's question: exact equality, or `Type_table::common`, which is §6.4's widening and would make this the second place a type is chosen by ranking. The overloading row's correction applies unchanged — widening after a type is settled, never to settle one — so the leaning is that an expectation decides and, with none, the arms must match exactly. **Nothing before M8 needs it**, and `if` as a statement covers every case; what it should stop being is an accident. **Decided and built (2026-09-19): `?:`, with `if`-as-expression deferred to M8.** Shipping `?:` alone is what makes D33 true rather than aspirational, and it is the half that costs one Pratt rule; `if`-as-expression is not a smaller version of the same change, because the row's own argument drags `switch` in with it and a `switch` arm is a block with `case` labels. **The type rule is as the leaning said**: an expectation reaches both arms, and with none the arms must match exactly — `Type_table::common` is not consulted, so `u8` beside `i64` is refused although §6.4 would widen them. That keeps the overloading correction intact in a second place. **Three corrections the walkthrough for this owed.** *A conditional does need a case in `lower_place`*: `( c ? a : b ).t` projects from the result, and without one the compiler aborted on valid Keel — a value-returning call in the same position already gets its temporary's place, and this is the same answer. *An arm needs `converted`*, because an expectation reaches an arm without retyping it, so `i64 x = c ? a : b;` leaves both arms `i32` and the assignment would write an `i32` into an `i64` local. *An owning arm needs both halves of the ownership handling* — `moved_if_owning` on the arm and the result registered as a statement temporary — and the two are reached by different programs: `move a` already arrives as a move, so only an arm that *builds* its value exercises the first, and only a result nothing moves out of exercises the second. **`if`-as-expression is now rejected rather than deferred (2026-09-19).** The deferral assumed its value was the `switch` form, where exhaustiveness already proves an arm runs and so makes the expression total. Weighed against a written-out `switch` that assigns to the variable, that turns out to buy almost nothing: the ordinary form is the same shape, one line longer, and needs no answer to the question the feature actually costs — how a block says which expression is its value. Rust's tail expression makes `100` and `100;` different statements, which is the invisible distinction D2 and D31 exist to prevent, and a `yield` keyword is uglier than the one-liner it was meant to improve. `?:` already covers the case that motivated it. **Raise it again only if a block-with-value is wanted for some other reason** — it should not arrive as a side effect of wanting a conditional. | **Decided and done** — M6.5, `?:` only; `if`-as-expression **rejected** |
| **What does importing two modules that each declare `print` give you?** **Overloading created this question and M8 has to answer it** (2026-09-18). Before M6 a name meant one declaration, so an import either brought it or collided. Now a name denotes a *chain* — §15 slice 3's `next_overload` — and two modules each contributing to it is a merge rather than a clash: `print( i32 )` from one and `print( f64 )` from another could reasonably form one set, exactly as two declarations in one file do. **The argument for merging** is that a call site already tells them apart by what it writes, which is the whole basis of the feature, and refusing would make the module boundary mean something the type system does not. **The argument against** is that it makes an import able to change which function an existing call resolves to, which is the failure mode D11 exists to abolish — and the duplicate rule then has to run across modules, where the two declarations may not both be visible. **It also decides where the duplicate check lives**: it is a checker pass today (§15 slice 3) precisely because the resolver cannot ask about types, and across modules it needs every contributing declaration in hand at once. Note the mangler is already ready either way — a parameter list is part of the symbol and the module slot is separate — so this is a name-lookup decision, not a codegen one. | M8, with module granularity |
| Custom allocators / arenas — visible in the type system or not? | M8 |
| Module granularity: file, directory, or explicit declaration? | M8 |
| **A build tool.** A language people use needs one, and `cmake` is the argument for writing it rather than adopting it. The scope that stays small: find the sources, build a dependency graph from `import` (M8 gives the graph for free), invoke the C compiler, cache. The scope that eats the project: package management, versioning, cross-compilation, a configuration language. Note the second list is where every "not horrible" build tool became horrible — Cargo is the closest to good and is inseparable from crates.io. **Start with the smallest thing that builds a multi-module program**, and treat every addition as a decision with an entry rather than a feature request. | After M8 — the module graph is the input, so it cannot start earlier |
| **An editor extension, and the architecture it needs.** Exhaustiveness, `move` checking and definite assignment all know *exactly* what the author must fix, and today that only reaches them by running the compiler — which is the loop this is meant to delete. The parser is already the hard half: it recovers rather than bailing, so a half-typed file still produces a tree, which is what an editor needs on every keystroke. **The conflict to resolve first is §3's memory policy**: *"never free, the compiler is a batch process; it exits"* is true of `keelc` and false of a language server, which is long-lived and re-analyses continuously. Two honest answers — run `keelc` as a subprocess per analysis, which keeps the policy and is how several real servers work, or make the compiler re-entrant. **The second got no cheaper from the `Arena` sitting in the tree (2026-09-24)**, which is part of why it was deleted: it had no `reset`, so it never served this entry either, and re-entrancy is about the vectors and maps that hold the real state. The first is much cheaper and should be tried first. | After M8, alongside the build tool |
| **Does an import bring the types reachable from a signature with it?** If `load_config` returns `Result<Config, Config_error>`, then importing it means needing all three to handle the result, and requiring three imports is friction carrying no information. Errors are where this bites first — a combining enum is needed in the module that declares it, the one that handles it, and every layer that re-wraps it — but nothing about it is specific to errors. The alternative floated and rejected was a dedicated file kind for error enums (`.kle`): that is a naming convention enforced by the toolchain rather than a language feature, it still has to be imported, and it files declarations by *what kind of thing they are* instead of *what they belong to*, which scatters a module for no gain. | M8 |
| **Does the error type of a `Result` have to be a named enum?** Written out, multi-source error handling produces a combining enum per layer — `enum Config_error { Io( Io_error ), Parse( Parse_error ) }` — that carries no information and exists only to say "either of those". Every Rust project reaches for `thiserror` to generate exactly this, or `Box<dyn Error>` to erase it; both are libraries patching a language gap, and the layers it creates then force a matching cascade of one-level `switch`es to peel them back off. **Zig's answer is error sets**, which union structurally and can be inferred from a body, so no combining type is ever declared. The Keel shape would be an **anonymous error union** — `Result<Config, Io_error \| Parse_error>` — with `try` widening into any union containing the source type, which generalises the "exactly one way to convert" rule above rather than replacing it, and with `switch` matching leaf variants directly so the cascade collapses. **The cost is the largest type-system addition in this document**: flattening so `( A \| B ) \| C` is `A \| B \| C`, order-independence so `A \| B` and `B \| A` are one type, and exhaustiveness across a union of unions. The cheap half-measure is inference alone — keep the named enum, allow `Result<Config, _>` — which removes the declaration but not the type, and makes the signature less informative, which is the opposite of what every other entry here has chosen. **One argument for the named enum that brevity comparisons miss**: exhaustiveness means the compiler knows the complete failure set at every call site, so an editor can offer *fill in the missing arms* as a fix rather than the author compiling to discover them one at a time. A computed set can be enumerated too, but a named type has a declaration to jump to, somewhere to hang a doc comment, and a stable name in a diagnostic. Weigh that against the boilerplate rather than only the line count. **Do not decide this at M5.** Payload-free enums need none of it, and the evidence that decides it is real error-handling code, which cannot be written until after M6. **Considered and rejected: attaching the error type to the success type**, so that `Config` declares its own `Config_error` and `Result<Config>` needs no second parameter. It is discoverable and it reads well for a type with exactly one fallible constructor, and it fails on four counts. The same success type comes from operations that fail differently — `parse_int` and `divide` both yield an `i32`, and the failure belongs to parsing and to dividing, not to the number. Primitives have no declaration site at all, so `Result<i32>` has nowhere to hang one. It inverts the dependency: `Config` would have to name `Io_error`, a filesystem concept, and would then change when a *loading strategy* changed. And it is the wrong axis — a combining error set is per **layer**, not per type, since `load_config` unions IO with parse while a caller unions config with network. The instinct behind it is right and worth keeping: the objection is to naming the error type *at all*. Inference attaches it to the **operation** instead, which is the axis that actually varies, and composes — a caller's set is the union of everything it propagates. **Decided (2026-09-18): the anonymous error union, with widening confined to propagation.** `Result<Config, Io_error \| Parse_error>`, no combining enum declared, and `switch` matching leaf members so the cascade never forms. **The two costs quoted above are not equal, and the first was overstated.** Flattening and order-independence are one canonicalise-at-construction step — expand nested unions, sort members by `Type_id`, dedupe — and `Type_id`s are already interned, so sorting by numeric id gives a canonical key for free. That is a small function, not an architectural fork. What is real is that **a union has no declaration**, so `Type_table` gains a second interning path keyed on the member set rather than on declaration plus arguments; that is the one place this cuts against the grain of §4. **The tag is the part the comparison with Zig hides.** Zig's error sets are cheap because errors are globally numbered and payload-free, so a set is a compile-time subset and widening is a runtime no-op. Keel's errors carry payloads, so that does not transfer directly — but it transfers with one move: **give every error type a stable global tag**, which is available precisely because Keel has no separate compilation and the whole program's error types are known. Widening then keeps the tag and copies into a larger payload slot, and exhaustiveness is a subset test over a bitset. **Widening is confined to `try` and is not a subtype relation. This condition is what the decision rests on.** A general “an `Io_error` is acceptable wherever an `Io_error \| Parse_error` is expected” would fire at every point where types meet, and the collision is with overloading, decided above: `handle( Io_error \| Parse_error )` and `handle( Io_error \| Parse_error \| Net_error )` would both accept an `Io_error`, which needs most-specific-match, which over sets is subset ordering, which is *partial* and brings ambiguity rules with it — the ranking machinery D5 deleted and D33's overloading argument depends on being absent. Confined, the rule is **one subset check in one place**: every other position matches a union by identity, and D5's “exactly, or not at all” survives everywhere else. **It is an explicit conversion, not an implicit one**, because `try` is a keyword the author typed — the same family as `cast<T>`, `wrap<T>`, `move` and `ref`, a visible marker where something non-obvious happens (D2, D31). **The error set is declared, never inferred.** Zig infers it from the body; inferring here would stop the signature stating what can go wrong, which is the property every other entry in this document has chosen. Declaring it makes `try`'s check a subset test against something written down, and makes adding a `try` for a new error type a compile error **at the contract** rather than a silent change at every call site — D11's spirit. **What it gives up**: constructing a union value by any route other than `try` is explicit. That is a small loss in a rare position and the safe direction to be wrong in — relaxing a rule later is safe, tightening one is not. **This also answers the conversion sub-question in the `?`/`try` row above**: under a union there is no conversion to define, only widening into a superset. | **Direction decided: the union, widening confined to `try`** — implement with error handling, post-M6 |
| **Does a pattern nest?** `case Err( Io( e ) ):` is what anyone will write once errors are enums of enums, and peeling one layer per `switch` is the alternative — two small functions rather than one deep pattern. Nesting is more expressive and is where pattern matching starts to need a real compiler: exhaustiveness over a product of variants, and a reasonable diagnostic when a case is missing three levels down. One layer at a time is the conservative start and composes by hand. | M5, with destructuring |
| **Are there function pointers, and does anything still need them?** They appear nowhere above — the same gap methods had. Three uses, and generics take two of them: a comparator for `sort` becomes a type parameter at M6, and so does any strategy passed to a container. What survives is **FFI** — a C library that takes a callback has no other spelling — and that alone may be enough to need them. `T( * )( args )` is C's syntax and is widely disliked; a named form reads better. **Decided (2026-09-18): yes, and member pointers with them — but that is three features wearing one name, and only two of them are new.** M6 went as predicted: generics took the comparator and the strategy, and **FFI is the surviving customer**, which settles the first half on its own. *A member **function** pointer is not a second feature here, and L13 is why.* C++'s are fat and implementation-defined — a code address plus a `this` adjustment plus a vtable index — for reasons that are entirely inheritance and virtual dispatch, and Keel has neither. A method already lowers to a function whose first parameter is the receiver, `ref C` or `const ref C` by D32's `const`, so `&C::get` **is** an ordinary function pointer over that signature. Writing that down is the point of this clause: the machinery must not be built. *A member **data** pointer is the one genuinely new thing.* It is an **offset, not an address**, so it is applied to an object rather than dereferenced — which is D27's own instinct, since the thing that makes it safe is that it cannot be arithmetic. **Four sub-questions, and the last two are the ones that bite.** *The type spelling*, where this row's dislike of `T( * )( args )` now has a reason rather than a preference: §15 slice 3 made a parameter's identity the pair **(declared type, marker)**, so `fn( ref i32 )` and `fn( i32* )` are different types and a marker needs somewhere to sit — which C's declarator has nowhere for. Leaning `fn( i32, i32 ) -> i32`. *The application spelling*, which cannot be `obj->*p` because D22 deleted `->` outright, and where `C::v` also widens `::` from D30's *“reaches a variant, and only an `enum` has them”* to a general scope qualifier. *Overloading makes `&f` ambiguous*, and the only thing that can choose is the **expected type** — which is exactly the channel §15 slice 4 built, taken and cleared by the node that owns it. So `fn( i32, i32 ) -> i32 g = &add;` selects, and `auto g = &add;` on an overloaded name is an error; today the whole question is masked by `infer_name` reporting *“`add` is a function, not a value”*. *Taking a generic's address is a new **seed** for the monomorphisation worklist*, and this is the one that fails silently: `&id` is an error and `&id<i32>` is required, but `instantiations_` and `generic_calls_` are recorded in `infer_call`, and an address-of is not a call — miss it and the symbol is referenced and never emitted, which is a `cc` error in generated code rather than a diagnostic, the worst failure class this project has. **Two dependencies rather than one.** Member data pointers need **access control to exist first**, or `&C::v` on a private field is a hole in visibility the day visibility arrives; and they sit against §12's reflection direction — *compile-time yes, runtime never* — because a runtime-valued field selector is the weak form of exactly that, which is a boundary to state rather than to discover. **Timing: the FFI half with `extern`'s customers, the member half with access control**; neither is M6's, whose obligation was the decision and is discharged. **Scheduled: M7 (2026-09-19).** The fork this row left open was M6.5 or M8, and neither is right — M8 is the standard library, and a language feature arriving inside the milestone that writes Keel in Keel would make that milestone about two things. **The row's own timing rule picks M7 without being changed**: member pointers were always to land *with access control*, and access control is now M7, so the member half was already scheduled there the moment that moved. The FFI half goes with it rather than waiting separately, because splitting one feature across two milestones to satisfy a customer that has not appeared is worse than building it once. | **Decided** — M6; **scheduled M7** — FFI half and member half together, since access control landed there |
| **Pointer-to-member — `&Point::x`?** Its real uses are serialisation and generic field access, and §12 already promises **compile-time reflection**, which covers both and more. Likely subsumed rather than added: a feature whose only customers are served better by another feature is one to leave out. | Whenever reflection is designed; not before |
| **`for( var : collection )`.** Needs an iteration protocol, which needs something to iterate — so it waits for `Vector` and `String`, and the protocol should be designed against a real container rather than invented ahead of one. Note D34's `for( i : 0..n )` is the *same syntax*, which is an argument for settling both together. | M8 |
| Standard library naming. `MANIFESTO.md` §12 already refuses to mirror `std`, but the specific names are unsettled: one `Hash_map` rather than `map`/`unordered_map`, and a better name than `vector` for a dynamic array. Note the one real trap — `List` reads as a *linked* list to a C++ programmer (it is `List<T>` in C#/Java but `std::list` in C++), so a familiar name would carry the wrong semantics. Not a §6.3 divergence: those cover syntax and semantics the compiler enforces, and no library exists yet. | M8, when the first containers are written in Keel |
| **What does the runtime look like, and how does Keel call into it?** **Answered — D36 and D37**, at M5.5. `extern` declares what C defines, unmangled, and every call needs `unsafe` (so the sub-question below is settled *yes*, and §6.2's sample has been corrected as this entry predicted it would be). `alloc<T>()` and `free( p )` are keywords over `kl_rt_alloc`/`kl_rt_free`, and `keel_rt/src/kl_rt.{h,c}` exists — twenty lines, not two hundred. Two amendments to what this entry guessed: **allocation is single-object for now**, `alloc<T>()` rather than `alloc<f64>( count )`, because the count needs `[*]T` and that is still deferred; and **`free` is a keyword rather than an extern**, because Keel has no `void*` for one to take. `print` still waits on `String` at M8, which is the only part of §7's promise outstanding. §7 promises a `runtime/kl_rt.{h,c}` under 200 lines for allocation, abort/panic and `print`, and none of it exists - no Keel program can allocate anything. **Deferred past M3**, deliberately: M3's acceptance names freeing, but what makes drop placement hard is *where*, and that is proven by a fixture whose destructor increments a counter on every path out, early `return` included. The blocker underneath is that Keel cannot declare what it does not define - a body-less function is a parse error, D18's corollary - so an FFI declaration has no spelling. The likely answer is **`extern`**, which keeps that corollary intact (a bare prototype stays an error, with `extern` as the fix it names) and should mean what C++'s `extern "C"` means, **including suppressing mangling**: `kl_rt_alloc` has to emit under its own name. Four sub-questions, with leanings rather than answers. **Is an `extern` call `unsafe`?** Probably, as Rust's `unsafe extern` - the FFI boundary is where the assertion belongs, and being the safe wrapper over it is the point of `Buffer`. That contradicts §6.2's sample, which calls `kl_rt_alloc` bare, so the sample changes when this lands; to be reviewed rather than assumed. **What does allocation return?** Spelled `alloc<f64>( count )` - a keyword in D28's mould rather than a generic, so the `<` can only be a bracket, and the element type gives the compiler the size, so no `sizeof` is needed. Its type is `[*]f64`, which means this waits on the many-item pointer D27 leaves out of v0: the spelling is settled, the prerequisite is not. **What does failure do?** `nullptr` for now, which D26 already makes adopt from context, replaced by `Result` at M5. Nothing forces the check until then, and that is the stopgap's whole cost. **What can `print` print?** Nothing worth having - D20 leaves string literals with no type until `String` arrives - so it waits for M8 alongside them rather than shipping an integers-only version that has to be unbuilt. **And what language is it written in?** A thin C floor, with the runtime proper in Keel above it - the same instinct that already puts `Vector` and `String` in Keel at M8. That is what every comparable language converges on: Rust's `std` is Rust over libc, Zig's is Zig over a small `os` layer, D's druntime is D over a little C. Nobody writes a runtime in C by preference; they write it in their own language over the smallest floor they can manage. Raw syscalls, as Go and Zig do, buy static binaries free of libc versioning - genuinely appealing, and rejected here for two reasons: per-architecture assembly stubs, and that reaching them from generated C means `__asm__` blocks, which couples the C backend to one compiler's extensions and is exactly what §2.2 forbids. libc is the right floor for v0. Note also that with `alloc<T>` a keyword, the backend could simply spell it `malloc` and no runtime file would be needed at all - cheaper than `extern`, but it bakes an allocation policy into the compiler, sits badly beside D10's `Owned<T>` direction, and gets unbuilt at M6. `extern` costs more and is never thrown away, because it is also how Keel talks to any C library. | **M5.5**, with methods — see the milestone table. `print` still waits for M8 |
| **Compile-time evaluation: how much, and is there reflection?** **Direction decided: yes, and compile-time only.** The word usually evokes C#'s runtime reflection — `typeof( T ).GetProperties()` — and that is the one version Keel cannot afford: it requires type metadata for every type in every binary, which is the cost §4's *No RTTI* already refuses. Compile-time reflection has neither problem, and both motivating uses are compile-time by nature. **A testing framework in the standard library** needs to enumerate test functions and report their names, which is discovery over the program's own declarations: Zig builds this into the language (`test "name" { }`) with `@typeInfo` for the general case, D has `__traits`, and Rust reaches the same place from the other side with derive macros — compile-time code generation rather than reflection proper. **`enum` to string** is the canonical example, and the one C++ programmers have wanted for twenty years; C++26's `std::meta` finally delivers it. In Keel it lowers to a generated static table — the variant names are known at compile time, the enum is closed and scoped (D30), and the run-time cost is one array index. Because both uses are pure code generation, nothing need survive into the binary that the program does not use. What stays open is the surface — a `@` builtin as in Zig, a `__traits`-style call, or attributes as in Rust — and how much general compile-time evaluation sits underneath it. M5 must land first: payload-carrying variants change what printing a value even means. | Surface post-M5; the framework needs the standard library, so **M8** |
| ABI stability: is there one at all? | Post-M8 |

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
realistic somewhere after M8, once `Vector`, `String`, `Hash_map`, and a file
API exist in Keel, and it will be a full rewrite rather than a port.

---

## 15. Where the work is

### M0 — done

| | |
| --- | --- |
| `common/` | `Span`, `Source_manager`, `Diagnostics`, `Interner`, ~~`Arena`~~ — deleted 2026-09-24, see the slice below |
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
   records values into a `Literal_pool` that the token's unused `symbol` slot
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
and dereference is **not** gated — the `unsafe` gate arrived at M5.5 (D35) and
guards one operation, converting between pointer types, which is a separate
question from reading through one.


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

### M4, M5 and M5.5 — done

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

~~**Noted for whenever overloading arrives:**~~ **Paid, 2026-09-18 — §15 slice 3e.**
`void f( ref i32 n )` and `void f( i32* n )` mangled identically, both to `i32p` and
later to `P3i32`. Harmless while a second `f` was a redeclaration caught by name long
before mangling mattered; overloading is exactly what stopped it being caught. The
mangler now encodes each parameter as the pair a call site writes — declared type plus
marker — so the borrow is `R3i32` and the pointer `P3i32`. This note and the mangler's
own comment were two of the three places already planning for a feature nothing had
decided on; all three are now settled.

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

**`const ref` returns and the non-escaping rule, which finishes `ref`.** §8's argument
survived contact with the implementation intact: the check is **one syntactic test with
no dataflow** - trace the returned expression to its root and require a parameter that
travels by address. There is no lifetime, no region, no borrow graph. Two `const ref`
parameters and a `return` of either is clean, which is precisely the case Rust needs a
lifetime parameter for, and it costs nothing here because *all* of them outlive any
binding declared at the call site.

The binding model carried through unchanged for a third position. A function's recorded
type stays `T`; the address `T*` goes on the **return annotation**, which is
`children[decl][0]` for a `Function_decl` exactly as it is for a variable - so
`binding_type` and `is_borrowed_binding` answered for functions with no new code, and
the KIR return slot, the C prototype and the C definition all read the one fact.

At the caller the result is a pointer in KIR and a `T` in the language. That is resolved
**once**, in `lower_call`, by dereferencing immediately - after which both uses fall out
of machinery that already existed: a copy reads `(*_t)`, and a binding takes `&(*_t)`,
which is `_t` again. `is_assignable` gained one branch so a binding-returning call
counts as a place, which is what makes `const ref i32 r = pick( x, y );` legal. An
ordinary call still does not, so **§8's temporaries hole stays shut** - the result of a
plain call has no scope of its own, and binding to one is still refused.

Only `const ref` may be returned. A mutable one would hand a caller write access to
something it never asked to lend, and §8 gave up reseating in exchange for needing no
lifetimes - not in exchange for mutability through a returned binding.

Four things this cost that the walkthrough had wrong, all of the same shape - claiming a
predicate would answer for a new node kind without checking:

- **`is_const_binding` had a kind guard** listing `Var_decl` and `Param_decl`, so every
  `const ref` return reported *only a `const ref` may be returned*. A return type is a
  binding position, so `Function_decl` belongs in that list.
- **`parse_declaration`'s entry guard** admitted only an identifier or `const`, so
  `ref T f()` never reached the declaration path at all - four cascading parse errors in
  place of the one sema message. Fixing the *scanner* was not enough; the guard above it
  had to widen too.
- **The emitter did need changing.** The definition and the prototype both spelled the
  return type from the declaration's recorded type, so C was told `int32_t` and handed
  `int32_t*`. `Spelling::return_type` now owns that, next to `parameter_types`, where
  "a binding travels by address" already lived.
- `returns_a_binding` and `current_function_` simply did not exist.

A note on the KIR: `return a` from a binding parameter prints `_0 = &(*_1)` rather than
`copy _1`. `lower_place` derefs the binding and the address is taken straight back - the
same round trip forwarding a borrow already prints, and one any C compiler folds. Not
worth a peephole; worth knowing before reading a dump and thinking something is wrong.

**M4's reference work is complete**: `ref` and `const ref` parameters, `ref` and
`const ref` local bindings, `const` throughout, the read-only borrow, and now returns
and the non-escaping rule. `out` is all that remains.

**`out`, which finishes M4.** The callee assigns the caller's variable, both sides say
so, and *"the callee must assign it"* is a promise rather than advice - so a path that
returns without writing one is an error.

**It is not §8's analysis, and that is the finding worth keeping.** The move lattice has
`Uninitialised` as its **join identity**, chosen deliberately so a block can start empty
and be filled by its predecessors: `join( Live, Uninitialised )` is `Live`. That is a
*may* analysis. Definite assignment is a *must* analysis - assigned on **every** path -
and needs intersection, where the identity is "assigned" and merging can only lower it.
Bending §8's lattice to answer both would have broken move checking, so `out` gets
`check_assignment`, a second pass over the same CFG with the opposite lattice. The
duplication is the ~20-line worklist skeleton, not a rule.

Two bits per local. `always` is the answer; `ever` exists only so the diagnostic can
distinguish *never assigned* from *not assigned on every path*, which are different
mistakes and deserve different sentences. The subtle one is `reached`: a block the
worklist has not visited yet holds the identity - all assigned - and intersecting with
that would be a lie, so it is skipped until something reaches it.

**The state tracked is the referent, not the local.** An `out` parameter travels by
address, so its KIR local holds a perfectly valid pointer; what is empty is `(*_1)`.
Recording "has the referent been written" as the local's own state is a small abuse that
costs nothing, because `transfer_block` already treats a projected assign as
initialising the whole local - so `(*_1) = ...` sets it with no new code. KIR gained
`Function::out_parameters` because nothing in the graph says which locals start empty,
and the lowerer is the only thing that knows.

**Taking a place's address counts as assigning it.** Without this rule
`void forward( out i32 n ) { init( out n ); }` reports *never assigned* - the call
lowers to `&n`, and forwarding is the natural way to write an `out` wrapper. The
analysis cannot see through a pointer, which is the same shallowness `place_root`
already has, and it errs in the direction a *must* analysis must: it can miss a mistake,
never invent one. The cost is that a `const ref` argument counts too. The real fix is
for KIR to record which arguments are `out`, and it can wait for something that needs it.

**`out` is restricted to types that own nothing.** Assigning one destroys nothing, so an
owning `out` parameter would leak whatever the caller was already holding. Lifting it
needs the caller to emit a drop before the call, which drop flags could do - a separate
piece of work, and refusing with a diagnostic that says why is the honest interim.

Two things the tests pinned that are *deliberate*, so that changing either is a decision
rather than a bug fix:

- **Partial struct initialisation satisfies it.** Writing `p.x` and not `p.y` passes,
  because the analysis is per local. Same granularity problem that stops a field being
  moved on its own, and it wants the same answer whenever either is addressed.
- **A raw pointer may escape.** `void escapes( out i32* p ) { i32 v = 7; p = &v; }`
  compiles. §8 now says so outright: `ref` is the spelling that cannot dangle, `T*` is
  the one that can, and a language with both has to say which is which somewhere.

**The fixture had to be split by pass, for the second time.** `errors_out.kl` reported
five errors and silently lost four: type errors stop the driver before lowering, so
`check_assignment` never ran. The rule was already recorded for `errors_owning_copies.kl`
and was worth recording twice - one fixture per pass, wherever a pass can gate the one
after it.

**M5: sum types.** `enum` in four slices - payload-free enums, `switch` with
exhaustiveness, integer and float scrutinees with D34's range labels, and payloads
with destructuring. Each was a working feature on its own, which is what let the
hard question wait until there was code to answer it against.

**D30 was mostly enforced by what is absent.** `holds()` needed no case at all: an
enum reaches an integer through no branch that exists, so `i32 x = Colour::Red;` is
rejected by there being nothing to accept it. The same for arithmetic - `cpp_result`
has no answer for a non-numeric operand. Only `==` needed writing, and it joined
D27's pointer-equality path rather than getting its own: both compare by identity,
and **ordering is deliberately absent from both**. For an enum the reason is worth
keeping - variants are names rather than magnitudes, and the declaration order they
happen to have is not an ordering anyone wrote down.

**Two coverage strategies, split by decidability.** An enum has finitely many
variants, so coverage is a vector indexed by ordinal and a gap can be *named* -
`missing \`Green\` and \`Blue\`` is the message the whole feature exists to produce.
A number has 2^32 or more values, so coverage is a set of half-open intervals that
can be checked for overlap and never proved complete: `default` is required there
and optional here, and the dead-`default` rule applies only to the enum side.

**Stacked labels fall out of the grammar.** An arm holds its own labels, so the
parser takes labels until something that is not one appears.

**The paragraph that stood here was wrong, and is worth keeping as a correction.**
It claimed falling out of a non-empty arm was *unrepresentable* rather than rejected,
and called that "the answer to the destructuring problem C++ never had to face". Both
halves were false. Falling out was entirely representable - it simply left the
`switch` instead of running on, which is a silent divergence rather than an
impossibility - and the destructuring problem was live the whole time, because
stacking bound every label's payload with no tag test. D38 is where both became real
rules. The lesson is the general one: "unrepresentable" is a claim about the grammar
that has to be checked against the checker and the lowerer, not inferred from the
parser alone.

**A range is syntax and lives only where it is legal.** Parsing `a..b` in the
expression grammar would make `i32 x = 1..5;` parse and then need a diagnostic to
un-parse it. Lowering it needs no conjunction in KIR - `&&` short-circuits and the
lowerer already builds that out of blocks - so a range is one more link in the chain
a stacked label list already builds.

**Payloads: no union, and that is what made it tractable.** A tagged variant would
normally be a tag plus a union. Payload field names are already mangled with their
node id, so two variants cannot collide, and every payload field can sit as a
sibling in one struct. The cost is the space a union would have saved, which is
invisible - nothing guarantees an enum's layout and no FFI can see one. What it buys
is that a payload field is an **ordinary Field projection**: `Projection_kind` gained
one entry, `Tag`, rather than a variant-aware projection needing its own type walk
and a reverse lookup from field back to variant. Moving to a union later is a change
to `emit_enums` and nothing else.

**Two representations, and the one that must not change.** A payload-free enum stays
its underlying integer, which is what keeps every enum written before payloads
untouched; only one carrying payloads becomes a struct. The scrutinee's tag is read
*once*, alongside the scrutinee, so the arms are unchanged - they still compare one
operand against one constant and never learn payloads exist.

**A pattern is not an expression.** `case Shape::Circle( r )` binds `r`; parsed as an
expression the name would reach the resolver, which would report about a variable
that does not exist instead of about the pattern. The postfix loop folds the `(` into
a Call_expr before the label parser sees it, so the pattern is recovered from that
rather than parsed with a lower binding power - which would also stop `::`, since
both are 110.

**The arm is the scope, not its body.** A pattern's bindings are written *outside*
the block, so a scope on the block alone puts them out of reach of the code that
uses them.

Four things only the doing found. Enums were declared before structs, so a payload
could not name one. The resolver skipped variants entirely, so a payload field's
*type* never resolved - it now visits the annotations while still refusing to declare
the variant names, which is D30's scoping intact. A pattern binding was not a place,
and `visit_assign` stays silent for an unassignable *name*, so `r = 2.0;` compiled in
silence; it is now a place that `check_writable` refuses, exactly as `const` is. And
the **fallback arm never bound its pattern** - the one arm the binding loop skips,
which matters because with no `default` the *last arm* is the fallback and may well
destructure. Every earlier fixture happened to end on a payload-free variant, so
nothing caught it until M5's own acceptance sample did.

**On that sample.** M5's acceptance is the `Shape`/`area` listing in §6.2, and it did
not compile: it wrote `case Circle( r ):` unqualified. D30 scopes a variant in every
position and a `case` is not an exception, so the sample was corrected rather than the
language - a bare name legal only where the scrutinee's type happens to be known
would be a second spelling for one thing, which is the redundancy D25, D30 and D32
all refuse. It is now `tests/sema/sample_m5.kl`, so the acceptance is a checked claim
rather than a paragraph - and writing it is what found the fallback bug above.

**M5.5, first half: methods.** Two decisions, and the second fell out of the first.
A method says whether it mutates with a **trailing `const`**, which is C++'s spelling
and means what Keel's `const` already means. And the receiver becomes a **`ref T` /
`const ref T` binding** rather than the `T*` D22 had to reach through - because once a
method's constness is a property of its receiver, the receiver has to be the kind of
thing that can carry one.

**That second change was the whole of the work, and it was done first and alone.** M4
had already built everything it needed: `record_borrowed_parameters` marks the receiver
by mode, `place_for` derefs a borrowed binding, and `address_operand` hands one over at
a call. Turning `this` into a binding was a parser change and two lines in the lowerer,
with the existing suite as the proof that nothing moved. Methods were then additive.

**M5.5, second half, slice A: `unsafe`.** The keyword was already reserved, so the
lexer needed nothing; the whole feature is a parser rule and a checker rule. **An
unsafe block is a `Block` with `aux == 1`**, not a node kind of its own — the choice
that made the slice small. Both passes that dispatch on `Node_kind::Block` (the
resolver, for scoping, and the lowerer) already recurse into children, and the checker
had no `Block` case at all, so the marked form needed **zero** downstream edits: scoping,
lowering, drop placement, move checking and definite assignment all handle it without
being told. A `Node_kind::Unsafe_block` would have needed four edits, each of which
compiles cleanly when forgotten and makes a rule silently absent — the bug this project
has now hit often enough to design against rather than test for.

The gate has **one customer**, and it was waiting: the `Conversion::Unsafe` cell of the
table carried the comment *"a real conversion, but one that needs a gate Keel does not
have yet"*. It now reads *"converting between pointer types needs an `unsafe` block"*
outside one and compiles inside. Nothing that compiled before changed meaning — the
cell was an error either way — so the only golden that moved was the one pinning that
message. No backend work at all: `Rvalue_kind::Cast` emits `( T ) x`, which is already
right for pointers, and `codegen/unsafe_blocks.kl` round-trips `i32*` through `u8*` and
writes through the restored pointer to prove it at run time.

**The two rules either side of it are the part worth arguing about.** An unsafe block
that uses none of its permission is an **error**, on D15's precedent, and so is one
nested in another. The first is the rule that keeps the marker meaning something: an
unsafe region that may quietly outgrow the operation it was opened for stops telling a
reader where to look. It has a real cost, visible immediately in the test suite — four
sections testing *other* rules inside a block had to gain a pointer cast, because
otherwise they report two errors instead of one. That is the honest evidence for
revisiting it as a warning, and it is written down here rather than discovered later.

**What the slice deliberately left out**: `unsafe` on a declaration, because the only
thing that will need "unsafe to call" is `extern` in the next slice, and a marker with
no customer is D2's defect; and gating dereference, because that breaks programs that
compile today and is a decision of its own. See D35.

**M5.5, slice B: `extern`.** The slice above left "unsafe to call" to this one, and the
answer was that **it needs no marker**. `Function_decl`'s `aux` is the name, so an
`extern` flag had nowhere to live — and then the absent body turned out to be the flag.
D18's corollary already makes a bare prototype a parse error, so `extern` is the only
rule that can produce a body-less function, and `is_extern` is
`!ast.children( decl )[2].is_valid()`. Nothing to forget to set, and nothing to check
twice.

**Four passes needed no change at all**, which was checked rather than hoped: the
resolver registers the name in its file-scope sweep and its `visit` already guards an
invalid id before recursing into the body; the checker's `visit` guards identically;
`declare_signatures_function_decls` reads the return type and the parameter list and
never touches the body; and move checking, definite assignment and drop flags all run
on KIR, which an extern never enters. The whole implementation was a predicate, a
parser branch, `&& !is_extern` in `lower`, an early return in `Spelling::function`, and
one new emitter pass.

**That emitter pass is the only piece that did not fall out.** Prototypes come from
walking the *lowered KIR functions*, and an extern is not one — so externs need a
separate walk over the AST. Worth noting the failure mode if it were missed: C that
does not compile, rather than C that is quietly wrong. `Spelling::function` being the
single place a C name is produced is what guarantees the prototype and the call site
agree, and it is where mangling is suppressed — one early return covers both.

**`extern` alone implies unsafe-to-call**, with no `unsafe extern` spelling. §12 leaned
the other way, toward Rust's two words; Rust needs both because its 2024 edition lets
individual items be marked `safe` inside the block, and Keel has no such escape. With
every extern unsafe, the second word carries no information, which is what D25 and D30
refuse. This is D35's second gated operation, and the one that turns `unsafe_used_`
from a flag with a single writer into a real one.

**The binding modes work on an extern and are more useful than expected**: `ref i32`
and `out i32` both emit `int32_t*`, which is exactly the signature a C function taking
`int*` has, so D32's modes are how a real C library gets spelled. They are also where
the honesty is: `out` discharges definite assignment in a body that does not exist, and
`move` hands ownership to a destructor Keel will never run. Both are accepted on the
call site's `unsafe` and both are now in the debts.

**The `switch` slice, and how it was found.** A bug hunt after M5.5 turned up four
things, and the two most serious were in `switch`. Both were found the same way:
compiling the same source with `keelc` and with `c++ -std=c++20` and comparing exit
codes. `case A: t = 1; case B: t = t + 10;` gave **1** against C++'s **11**, and
`break` in a `switch` inside a loop gave **0** against **3**. Neither produced a
diagnostic in either compiler. §6.3 claims no divergence does that; these were
unrecorded exceptions, and nothing short of running both would have shown it.

**A third, worse one came out of a question rather than a test.** Asked how
`fallthrough` would interact with payload enums, the answer turned out to be that
Keel already had the bug — stacked labels *are* fallthrough, and the emitted C bound
every stacked label's payload unconditionally, with no tag test:

```c
kl_r_2 = kl_s_1.kl_r_1;   /* from the Circle label */
kl_w_3 = kl_s_1.kl_w_4;   /* from the Rect label   */
```

So `case Circle( r ): case Rect( w, h ): return r;` read a field a `Rect` has not
got. D7 had named that exact hazard and the M5 log had claimed it impossible; both
were wrong, and the claim is corrected above rather than deleted.

**`fallthrough` is load-bearing, not a convenience.** Without it the run-on rule has
no fix to name: `break` was already spoken for, and "this arm must say how it ends"
is unsatisfiable if there is nothing to write. Two designs were weighed. Go's
`fallthrough` won on contiguity — you read downward, as in C — against a proposal to
let a `case` label appear twice and run in written order, which composes better and
is *immune* to the payload hazard by construction (each label does its own match and
its own binding) but is action at a distance: knowing what `case A` does would mean
scanning the whole `switch` for other `case A`s, which is what D19 and the `->`
scope-injection rejection already refuse. It also costs the duplicate-label check and
forecloses ever emitting a real jump table. Recorded because it is a better idea than
its rejection suggests.

**The rules interlock.** `break` had to bind to the nearest `switch` *because* leaving
it bound to the loop is the divergence; the run-on rule needed `fallthrough` to have a
fix; `fallthrough` needed the no-binding rule or it recreated the payload bug one
mechanism over; and the no-binding rule needed bare `case Shape::Circle:` or stacking
payload variants would have become impossible rather than merely unbinding. Five
changes, and dropping any one leaves another unsatisfiable.

**What the tests caught that review did not.** Writing them found a crash in
`completes_normally` — `If_stmt` keeps its else *slot* when there is no else, so
testing `children.size()` rather than `children[2].is_valid()` read an invalid node,
the fourth instance of that exact assert this session. And a `fallthrough` outside a
`switch` segfaulted while one nested in a block was silently accepted, because the
arm walk only inspects an arm's top-level statements: both now go through a set of
the fallthroughs that walk has ruled on, so anything it never saw is reported once.

**Mutation testing earned its place here.** Breaking each rule in turn, three failed
9, 2 and 1 assertions — and the fourth, `continue` unwinding to the switch's scope
depth instead of the loop's, **passed everything**. The test was too weak: the shape
that distinguishes them needs an owning local declared in the loop body *outside* the
`switch`, and the buggy build drops 0 where the correct one drops 3. The check moved
into a golden that counts destructor calls through the exit code.

**One recovery decision, found by review rather than by a test.** `extern i32 f() { }`
had no check at all: the `;` simply never arrived, the braces were left unconsumed, and
the body's statements were read as top-level declarations — five errors from a two-line
file, which is the exact count the comment on the `struct`-inside-a-function guard
warns about. It now reports once and **recovers as an ordinary definition** rather than
dropping the declaration. That is the opposite of what the bare-prototype path does, and
deliberately: that path returns an error node because a real definition is coming and
letting the prototype through would make it look like a duplicate, whereas here the body
*is* the definition, and discarding it would turn every call into an unknown name.

The prototype error finally names its fix, as §12 promised it would — *"write the
definition here, or `extern` if it is defined in C"* — and `codegen/extern_calls.kl`
links against the real libc and checks its own answer, which is the first time a Keel
program has called anything it did not define.

**M5.5, slice C: `alloc<T>`, `free`, and the runtime.** The milestone's last piece, and
the one that makes the language able to build something. A linked list now allocates its
nodes one at a time, walks them, frees them, and comes back valgrind-clean — which is
M5.5's acceptance, amended as the milestone row now records.

**Three bugs, and all three were the same bug wearing different clothes: a dispatch that
did not cover a new kind.** `infer`'s switch was missing `Alloc_expr` and `Free_expr`, so
`infer_alloc` and `infer_free` were written and never called — and because that switch's
`default:` silently recurses into children, `alloc` was **completely ungated and
untyped**: `i32* n = alloc<i32>();` outside an `unsafe` block compiled clean. Nothing
warned; the node simply had no type. `parse_expression_stmt` listed only `Call_expr` as
effectful, so `free( p );` was rejected by D15 as a construct with no effect. And
`uses_runtime` scanned the *top-level declarations* for an `Alloc_expr`, which lives
inside a function body, so it could never return true and the emitted C called
`kl_rt_alloc` undeclared. That is the fourth, fifth and sixth time this session, and it
is the reason D35 argues for an `aux` flag over a new `Node_kind` wherever the choice
exists.

**`uses_runtime` is now asked of KIR rather than the tree**, and the reason is worth more
than the fix: the question is whether the C about to be written calls those symbols, and
KIR is what it is written from. An AST scan answers a *proxy* question, and the day
`Owned<T>` allocates without an `Alloc_expr` in the tree it would silently stop finding
one and the generated C would stop linking. Reverting it to the broken version fails
seven assertions, which is how that is known rather than hoped.

**The runtime is twenty lines, and one of them is a guard on a case that should not
exist.** `kl_rt_alloc( 0 )` is reachable from valid Keel, because `struct Empty { };`
type-checks and an empty C struct has size zero under a GCC/Clang extension — so the
runtime bumps a zero request to one byte, to keep `NULL` meaning failure and nothing
else. That guard treats a symptom; §12 now carries the question of whether Keel should
have empty aggregates at all, to be decided with generics, and §15 says to delete the
guard if the answer removes the case.

**The golden runner compiles `kl_rt.c` from source beside the emitted `.c`** rather than
linking a built library, which answers the question the monorepo restructure left open:
the suite needs to know nothing about build directories, presets or configurations, it
works whether or not CMake has run, and it uses the same `$CC` a real Keel program would.
The first attempt at that line pointed at §7's pre-monorepo path and, because a failed
`cd` inside a command substitution yields *empty* rather than stopping, left
`runtime_sources` as the literal `/kl_rt.c` — a plausible-looking absolute path that
broke 34 fixtures with a C compiler error instead of a clear one. The fix removes the
computation entirely: the script already `cd`s to its own directory, so a relative path
cannot fail. Same lesson as `find src` matching nothing in the pre-commit hook, and the
third time this shape has appeared.

**What the pair does not do is the entry worth reading.** `alloc` runs no constructor and
`free` runs no destructor — the memory is raw, which is precisely why both are gated, and
precisely the gap `Owned<T>` exists to close. Double free, use after free, leaking and
freeing a stack address all type-check, and all four are pinned in the tests as
*accepted* rather than correct, so that the day one becomes a compile error the change
shows up as a failing test rather than as a silent improvement nobody notices.

**A method is a function whose parameter 0 is the receiver**, which is why so little
was needed: the checker already skipped an implicit first parameter for constructors,
mangling already worked off the declaration, and the emitter needed nothing at all -
`kl__sum__Pointp` came out of machinery that had never heard of a method. Adding
`Method_decl` to `is_function_like` gave method bodies member scope in one edit,
because both the resolver's field scope and the checker's signature pass were already
gated on that predicate.

**The const rule enforces itself.** A method without a trailing `const` takes its
receiver as `ref T` and may write the object, so calling one needs a receiver that may
be written - which is `check_writable`'s question asked of the object rather than of an
assignment. A `const` local, a read-only borrow and a pattern binding are all refused
by the rules that already refuse them, each with its own message, and nothing new was
written for any of them.

**Siblings are callable by bare name.** Methods join the member scope alongside fields,
so `add( by )` means `this.add( by )`. That is not a nicety - a type whose methods have
to qualify each other is tiring to write long before it is large, and keelc itself is
the evidence. The constness question there has no object expression to ask about, so it
is asked of the *enclosing* method's receiver, which is what stops a `const` method
calling a mutating sibling. The member scope is the type's, so the name does not escape
it, which is what keeps the bare form from being a second way to spell a free function.

**Three of the bugs found were silent, and that is the pattern worth noting.** Writing
the tests found that a `const` method could write its own object, that method bodies
were never checked against their return type, and that `find_field` matched *any*
member by name - so `b.B( 2 )` reported that `B` was a field of itself. None of them
misbehaved visibly; each made a rule quietly *absent*. The cause was the same every
time: a new node kind that an existing predicate or dispatch did not list. `Method_decl`
had to be added to `is_function_like`, to the checker's visit dispatch, to the
resolver's barrier-scope case, to `is_const_binding`, and to `is_assignable` - and every
one of those omissions compiled.

**And a fourth chain, four layers deep.** A method returning `const ref` needed its
return annotation's address recorded (only free functions did), needed a method call to
count as a *place* (`is_assignable` knew only `Function_decl`), needed `is_const_binding`
to answer for a `Method_decl`, and needed the call typed as the pointer it returns
rather than the language type - which produced C assigning an `int*` to an `int`, caught
only by `-Werror`. Each layer was independently wrong and each was silent alone.


### M6 — in progress

**Slice 1a: the declaration parses.** `T id<T>( T a )` — D39's spelling, no bounds, no
instantiation, sema deferring it with one diagnostic. Deliberately the smallest thing
that forces the shape without forcing the bound vocabulary, which §12 leaves open until
a `T` has actually flowed through the checker.

**The staging decision behind it.** D11's definition-checking is both the point of Keel's
generics and the whole risk: the checker must type a body where `T` is not a concrete
type, so every rule in a 10,000-line file has to cope with meeting one. Monomorphisation
is the other half and is mechanical. **The mechanical half goes first** — they are
separable, because definition-checking is an extra pass over the generic body rather
than a change to how instantiation works, and building it first gives a working
`id<i32>( 1 )` to test the hard half against. The cost is that errors are C++-shaped,
reported at the expansion, until D11 lands; that is temporary and should not be allowed
to become the design.

**One bug, three times, all silent.** `Function_decl` grew a fourth child for the type
parameters, and each of the three other function-like kinds kept three — so the shared
walk in the resolver read past the end of a node. `Ast::children` returns a
`std::span`, whose `operator[]` is unchecked, so the first instance was a segfault in
the test binary rather than a diagnostic, and the second made a *non-generic* function
report as generic because the out-of-bounds read landed on a neighbouring node's child.

The fix was an `Ast::child( id, index )` that asserts, and it earned its place
immediately: it caught the third instance — the `extern` path, which returns early and
had been reviewed twice without anyone noticing. **All 176 indexed call sites across the
compiler were then converted**, which needed a balanced-paren rewriter rather than a
regex: `ast.children( ast.children( decl )[1] )` has to convert the inner call and leave
the outer one alone, since that one feeds a range-`for`.

The general rule, which is the reusable part: **a fixed-arity node kind means every kind
sharing a walk must have the same arity.** Giving the three other kinds an unused fourth
slot is cheaper than a guard that asks which kind it is looking at, and it is what
`Vector<T>`'s generic methods will need anyway.

**Two diagnostics worth the lookahead.** `<>` reports rather than building an empty list,
and `<Comparable T>` — C++'s terse-bound form, and what a reader of that language writes
first — is named once with the `where` clause as the fix, instead of derailing the
parameter list and then the signature after it. That is fourteen diagnostics for one
habit, reduced to one.

**Slice 1b: the call site parses.** `id<i32>( 1 )` read as `( id < i32 ) > 1` until the
postfix loop learned to scan for a type-argument list followed by `(`. Committing to the
generic reading is what §12's entry licenses: the comparison reading has no valid typing
to fall back to. **The scan counts bracket depth itself rather than calling
`match_generic_close`**, which splits `>>` by leaving `pending_greater_` set — parser
state that a scan rewinding `pos_` would leave dirty, handing the next real `>` to
whatever came after. `Vector<Box<i32>>` reaches that path directly.

**Slice 1c-i: generics type.** `Type_kind::Parameter`, interned per `Type_param_decl` so
one declaration's `T` is never another's, and `Type_table::substitute` over it. The call
site binds the parameters positionally and checks the arguments against the *substituted*
signature.

**The part that was not one line**: the parameter's declared type is read in **five**
places in `infer_call`'s argument loop — the check, the `ref` borrow comparison twice, its
message, and the ownership test. Missing any is silent, and two of them are worse than a
missed diagnostic: the borrow check would compare against `T` and fire on every `ref`
argument, and `is_owning_type` on a bare `Parameter` answers false, which waves every
`move` through unmarked.

**D11 arrived as a side effect rather than as a slice.** Once the signature types, the
body is walked with `T` standing for itself — so `a + a` reports *no operator `+` for `T`
and `T`* **at the definition**, with no instantiation needed. That is definition-checking,
and what remains of D11 is bounds: an unbounded `T` supports nothing, which is the correct
reading and is why `id` is close to the only generic writable today.

**Slice 1c-ii: generics run.** The problem is identity: `Rvalue::callee` is a `Node_id`,
and `id<i32>` and `id<bool>` share a declaration — so a `Node_id` no longer names a
function. Both `Function` and `Rvalue` carry the type arguments, and the mangled symbol is
the pair.

**No AST cloning.** §15 predicted from M0 that monomorphisation would earn the `Arena` its
first caller, and it did not: an instance needs no *re-checking* (the body is already
checked abstractly), so cloning's only remaining job was giving the lowerer different types
per instantiation — which a bindings map does without copying a tree, rewriting spans, or
growing the AST. `Lowering` takes the bindings and every type read goes through two
helpers; empty bindings make them the identity, so the non-generic path is unchanged, which
the untouched golden corpus confirms.

**Two bugs from the same misunderstanding.** `lower_call` asked for the callee's parameter
and return types using the *enclosing* function's bindings — but `main` calling `id<i32>`
has no `T` of its own. And `prototype()` read types from the declaration while the
definition read them from the KIR locals; for a generic the declaration still says `T`, so
the prototype tripped `Spelling::type`'s assert. Both now read the lowered function, which
makes the prototype and the definition agree **by construction** rather than by two paths
computing the same thing.

**The symbol is `kl__id__T__i32`** — the declared parameter type, then the type argument.
Unique and correct, and ugly: it reads better as `kl__id__i32__i32`. Left alone because
§15 already owes a mangling rework for length-prefixing and the `ref`/pointer collision,
and those three want doing together.

**Slice 1d: bounds are checked.** D40's set is read off the `where` clause into a bitset,
closed under implication once at that point rather than walked at each query. Two
predicates consume it, and they are the same question asked of the two things a type
parameter can be: `has_bound` asks what a parameter *promises*, `satisfies` asks what a
concrete type *delivers*.

**The split is the design, not an implementation detail.** `satisfies`' first line
forwards a parameter straight to `has_bound`, and that single line is the whole rule for
one generic calling another: `void has<T>( T a ) where T : Numeric { needs<T>( a ); }`
type-checks exactly when `needs`' bounds are a subset of `has`', because the type
argument is itself a parameter and the question routes to the promise. Nothing else in
the checker knows that case exists.

**Implication is expanded at record time and collapsed again at report time.** `where T :
Integral` is stored as `Integral | Numeric | Comparable | Equatable`, so a `Buffer` type
argument fails four bounds for one mistake. Reporting all four would be four diagnostics
naming things the author never wrote, so a failing bound that another failing bound
implies is suppressed — computed from the closure function rather than from the order of
the bound table, so a seventh bound needs nothing added. Independent failures are still
reported together: `Copyable & Integral` against a class is two things to fix, and one
per compile would be two compiles.

**A forwarded parameter gets different advice.** `` `f64` is not `Integral` `` wants "it
has to be an integer"; `` `T` does not promise `Integral` `` wants "add `Integral` to the
`where` clause on `has`". The fix is a clause away rather than a type away, and the same
help line would be advice about the wrong thing.

**One ordering is load-bearing and nothing marked it.** `out T` asks `is_owning_type`
about a parameter, which can only be answered by the `where` clause on the same
declaration — recorded ten lines earlier in the same loop body. A probe that fires when a
parameter is asked about before its own clause is read found no violation anywhere in the
corpus, and fires immediately when the recording is moved after the annotations; so the
order is right, and a test pins it behaviourally rather than leaving an assert in the hot
path. It will matter more when generic aggregates land, because a field of type `T` is a
second way in.

**The double free, closed, and the borrow it introduced.** Making `is_owning_type` honest
about a parameter is what closes D40's double free — `void take<T>( T a ) { T b = a; }`
now reports rather than emitting one construction and two destructor calls. It also makes
a bare `T a` D31's read-only borrow for every parameter that was not promised `Copyable`,
which is correct and which broke fifteen tests written before bounds existed; each now
says `where T : Copyable`, and that is the honest reading of what they were always doing.

**`Numeric` implies `Copyable`, and that implication is dated.** Every other implication
in the lattice is a fact about the bounds — `Integral` is a kind of `Numeric` whatever
satisfies either. This one is a fact about *satisfaction*: D33's operators are not
shipped, so nothing but a builtin can satisfy `Numeric`, and every builtin copies. A
class overloading arithmetic while owning a resource makes it false. It is written in
`closure()` rather than anywhere else **precisely so that the failure is safe**: putting
`Copyable` in the promised set means the call site checks it, so that class is *refused*
rather than copied twice. The alternative — deciding it where `is_owning_type` reads the
set — would let a body copy a `T` the call site never verified, which is the original bug
wearing a different hat. **Deliberately not extended to `Comparable` and `Equatable`**,
which is where an owning type will want to be first: a string orders and compares and owns
its bytes, and leaving those two borrowing is what leaves room for it.

**A constant reaching a by-address parameter.** The borrow above is the first thing in the
language that passes a *literal* by address: `void f<T>( T a ) where T : Equatable { }`
called as `f<i32>( 1 )` has an argument with no place to take the address of, which
`lower_argument` asserted on. It now materialises the constant into a temporary and
borrows that. The callee may only read through the borrow, so a temporary living to the
end of the statement is exactly long enough, and nothing that reaches there has a
destructor to run.

**Bounds change the mangled name, and should.** `kl__f__T__i32` and `kl__f__Tp__i32` are
the same generic at the same type argument with and without `Copyable` — different
signatures in C, so different symbols. That falls out of mangling the declared parameter
type, and is the one place the calling-convention consequence of a bound is visible.

**Slice 1g: D41, and it was never only about signedness — done (2026-09-18).** The entry
is titled "mixed-signedness comparison" and the acceptance criterion was `u32 < i32`, but
the rule it states is wider than either, and the wider reading is the correct one: a
comparison asks a question about two *values*, and how either is stored has no bearing on
the answer. Signedness is simply where the old rule refused most visibly.

**One clause is the whole change.** `arithmetic_result` is two questions — is there a type
holding both operands exactly (`common`), and would C++ pick that type (`cpp_result`) —
and the second is §5.1's containment clause, which exists to protect a *result*. A
comparison has no result to protect, so it asks `common` and nothing else. In the checker
that is one branch returning `bool` before the common type is ever computed; `common` is
not called there at all, because sema does not need to know where the comparison happens,
only that it can.

**Five pairs become legal for free**, needing no backend work, because a wider type does
hold both: `i8`, `i16` and `i32` against `u32` meet in `i64`, and `i32` and `u32` against
`f32` meet in `f64`. Those last two are not a signedness case at all — they are the
mantissa case, and C++ answers them wrongly with no warning of any kind.

**Eight pairs have no domain, and D41's own cost paragraph was wrong about them** — it
said two, and said float against integer was unaffected. It is four and four: `u64`
against each signed integer, and `i64` or `u64` against either float. The entry has been
corrected rather than quietly worked around, because a plan that under-budgets by half is
worse than one that says nothing. Once each operand widens as far as its own kind reaches
— `i64`, `u64` or `f64` — the eight become **three shapes**, which is the number that
matters: `i64`/`u64`, `i64`/`f64`, `u64`/`f64`.

**This is the one binary whose operands reach KIR with different types**, and the comment
on `Lowering::converted` that promised otherwise now says so. It is not a hole in the
invariant but the reason the invariant exists: both operands share a type so that a
backend need not re-derive §6.4 for an *add*, and a comparison has no result the mismatch
could corrupt. A backend that cannot call a C helper — LLVM — has to emit the guard
itself anyway, so handing it the pair as written is more useful than handing it a domain
that does not exist.

**Only the integer guard is the branch-free one D41 describes.** A negative signed side is
below every unsigned value, so the answer is settled without comparing, and otherwise the
reinterpretation is exact. The float guards cannot be one expression: truncating a
`double` to an integer is undefined for a NaN or an out-of-range value, so the NaN test
and the two range tests are what make the truncation legal rather than what makes it
fast. Past them the truncation is exact — a double of magnitude 2^53 or more is already a
whole number — so the fraction is what breaks a tie the integer comparison cannot see.
Eighteen functions at most, `static inline` in the emitted file, emitted only for the
shapes and operators a program uses, and one guard per shape rather than per operand
order: the operands are swapped and the operator mirrored, which holds for all six
including the unordered float cases.

**§12's generalisation to a `T` is gated on `Numeric`, which is not what the row said.** The
row lists four bounds — `Numeric`, `Integral`, `Floating` and `Comparable` — justified by all
four admitting integers and floats and nothing else. That is true today and is an accident:
D40's own closing line records that only a builtin can satisfy `Comparable` *until D33's
operators arrive at M8*, after which a type that orders itself satisfies it and still does
not compare against an `i32`. A body that passed D11's check would then fail at every
instantiation, which is the one failure D11 exists to prevent. So the gate is `Numeric`,
which is the bound that means "is a number" rather than the one whose satisfaction set
currently coincides. `closure()` puts it in `Integral` and `Floating`, so it is still one
test. There is no walk over the admissible types either, unlike slice 1f's conversion:
every type `Numeric` admits compares against every number.

**The diagnostic was the thing that exposed it.** Gating on `Comparable` made `a == x` with
`T : Equatable` report *"`==` needs `Comparable`"*, which reads as an overlap between the two
bounds and is really a wrong answer: `==` needs `Equatable`, and what this expression needs
is something neither bound promises. The message now names the missing promise instead —
*"`T` must be a number to compare against `i32`; write `where T : Numeric` on `equated`"* —
and asks for ordering nowhere.

**How the guards were checked, since an exit code cannot see a wrong answer that is still
a bool.** All eighteen were emitted from a Keel program, extracted, and run against exact
rational arithmetic: 1,133 boundary cases — every combination of ±2^63, 2^64, ±2^53 and
its neighbours, NaN, both infinities, both zeroes and the extremes of each integer type —
and 600,000 randomised triples, with UBSan on throughout. Zero disagreements. The
boundary cases that a fixture can carry are in `codegen/comparisons_mixed.kl`; `-2^63` is
the one worth naming, because it is the only double in either float shape that is a
*valid* value of the integer type rather than one past the range, so a guard that
compared its bound with the wrong strictness would answer it wrongly and nothing else
would notice.

**Three mutants are equivalent, and all three are the same shape: a test that pins a
decision the surrounding code happens to imply already.** Dropping `is_comparison( op )`
from the lowering test survives, because `operation_type` returns an invalid type only for
a comparison — every other path falls back to the left operand; dropping it from the
emitter's `guard_for` survives for the same reason one step later; and dropping
`!is_parameter( other )` from the checker survives because a parameter is neither an
integer nor a float, so the numeric test already excludes it. All three are kept. The
first two are defence against a lowering that breaks the premise, and the second would
trip an assert rather than emit nonsense; the third is the one that would matter, because
it is the only thing that keeps one `T` against another refused if a parameter ever counts
as numeric. Everything else was killed, including the strictness of both float bounds, the
fraction in each of the six tails, the NaN answer, the operand mirror and the operator
mirror.

**Two other survivors were not equivalent, and both were worth the run.** One was a weak
test: `u8 b; b = b + 300;` asserted *an* error, and a literal fallback that ignored the
comparison rule still produced one — the wrong one, about the assignment downstream rather
than about the literal — so the assertion now names the message. The other was a real
defect. The constant-comparison warning asked one operand and trusted the other, which is
right whenever exactly one side is constant and wrong when both are: `300 > cast<u8>( 200 )`
is as settled as `b < -1` and went unreported. Asking both sides is shorter than the guard
it replaced and catches more.

**A literal in a comparison no longer has to fit the other operand**, which closes an
inconsistency D41 created: `b < c` with `u64 b` and an `i64 c` holding `-1` compiled and
answered `false`, `b < (0 - 1)` compiled because a `Binary_expr` is not a literal and took
the default type, and only `b < -1` was refused — for not being a `u64`, which it never had
to be. The literal now takes a type of its own when the operand beside it cannot hold its
value: its default first, because that is D26's answer and the one an author predicts, and
whichever 64-bit type holds it otherwise. Which one is picked cannot change the answer, so
it decides only where the comparison happens. **Arithmetic keeps the adoption**, because
there the two really do have to meet, and `u8 b; b = b + 300;` is still an error.

**Every comparison this newly accepts is a tautology, and the compiler says so.** That is not
a coincidence to note in passing but a theorem: a numeric range is contiguous, so a constant
outside it is above all of it or below all of it, and the sign says which — a negative that
does not fit is below the minimum, a non-negative one above the maximum. The ordering is
therefore settled before the program runs, and all six operators have constant answers. So
the refusal was carrying real information, and dropping it without replacing it would have
been a net loss. It is replaced by **the first warning in the language** — *"this comparison
is always false"*, with *"`-1` is outside the range of `u64`"* underneath — reported from the
comparison rather than from the literal, which is what lets it catch `b < (0 - 1)` and
`b < -1` alike. An error would have been the other defensible choice; a warning is right
because the code is well defined and means something, it just does not mean what it looks
like. Float literals are deliberately outside this: an `f32` overflowing is a precision
question rather than a range one, and it keeps the error it has.

**The parameter half is not included and is not an oversight.** `a < 300` with `T : Integral`
is still refused by slice 1e's rule, which checks the literal against *every* type the bound
admits. A `T` has no range for a constant to be outside of, so the tautology argument does
not reach it, and relaxing it would be a change to §12's literal row rather than to D41.

Fixtures: `codegen/comparisons_mixed.kl` (all three shapes, all six operators, every
boundary a fixture can reach, and a generic instantiated at two type arguments whose
domains differ), `kir/comparisons_mixed.kl` (the unequal operand types, visible at the
level the invariant is about) and `sema/errors_mixed_comparison.kl` (what D41 does not
reach: arithmetic on the same pairs, a bool, a pointer, an enum, and an `Equatable` `T`
against a number). `type_table_common_is_the_comparison_domain` pins all one hundred cells
of the domain table and asserts that exactly sixteen are guarded — if that number moves,
the backend has a shape it does not implement.

**Slice 1f: `cast` and `wrap` on a `T` — done (2026-09-18).** Both were keyed on
`Type_kind` pairs and `Type_kind::Parameter` is in none of them, so `cast<i32>( a )` was
refused whatever the clause said. The rule is not "has a bound" but **"legal for every
type the bound admits"**, which is slice 1e's question asked of a conversion instead of a
range — and it reuses 1e's machinery unchanged.

`possible_types` is the whole of the new idea: a concrete type is itself, a parameter is
`admissible_numeric_types( bounds_for( it ) )`, and `conversion_for` is then run over the
product, at most ten by ten, taking the weakest answer. `Integral` need not be spelled as
a precondition anywhere — under `Numeric` alone `f64` is admissible, float-to-int is
absent from the pair table, and the product refuses it by itself. One rule, no per-target
bound table. The narrowing gate gets the same treatment through `narrowing_witness`,
separately, because narrowing is a different question and only `cast` asks it.

**The empty set is a refusal, not a free pass.** A parameter without `Numeric` is the one
way this could have gone silently wrong: `admissible_numeric_types` filters the ten
numeric types, so a `Copyable` `T` comes back as *all ten* — none of them are owning —
and the structs, bools, enums and pointers `Copyable` also admits are invisible to it. A
vacuous product answers `Both`, and `cast<f64>( v )` sails through on a struct.
`possible_types` returns the empty set for a non-`Numeric` parameter and
`conversion_between` turns an empty side into a refusal before the loop runs. Two
mutations confirm it: dropping either guard leaves the golden suite green on everything
except `sema/errors_generic_conversions.kl`.

**Every diagnostic names a witness**, for 1e's reason: `T` is not something an author can
act on and `f32` is. The message still names what was written — `` `cast` cannot convert
`T` to `i32` `` — and the help carries the witness in front of the existing explanation,
so the commonest case reads *"`T` may be `f32`, and rounding is not implied; this needs
an explicit rounding function"*. The witness comes from whichever side is the parameter,
which three separate mutations are needed to pin: the value side, the target side, and
the narrowing gate's own.

**`Integral` does not make `cast` usable, and that is correct.** `cast` has to clear the
narrowing gate too, and `Integral` admits `u64`, which fits no integer type at all —
`holds( u64, i64 )` is false because unsigned reaches signed only at a strictly greater
width. So **no concrete integer target survives `Integral`** while
`k_narrowing_cast_needs_a_run_time_check` stands, and `wrap` is the operator that works
there. `cast` on a parameter is usable under `Floating`, whose admissible set is
`{ f32, f64 }` and where every pair widens. That is not a gap in the rule, it is the same
run-time check concrete narrowing `cast` is already waiting on, and it lifts on its own
when that check lands.

**Lowering needed nothing.** `Lowering::type_of` substitutes through the instance's
bindings, so by the time a `Cast_expr` reaches `converted()` both ends are concrete and
the emitter writes an ordinary C cast. `lower.cpp`'s `// TODO: distinguish between cast
and wrap` is no more owed than it was: the checker still refuses every narrowing `cast`,
so the only conversions that reach lowering are the ones where the two operators agree.

Fixtures: `sema/errors_generic_conversions.kl` (ten refusals, and the two conversions
that are fine at every instantiation kept at the top, where a line in stderr would mean
the rule had started over-refusing) and `codegen/generics_conversions.kl` (each generic
instantiated at two type arguments that convert differently, because a body checked once
would pass a fixture that only ever used one).

**Slice 1e: a literal can be a `T`.** D26's adoption meeting a bound. The question a
bound answers is not "does the value fit `T`" — there is no such type yet — but "does
it fit *every* type `T` may turn out to be", and the admissible set is built by
filtering the ten numeric types through `satisfies`, so a seventh bound needs nothing
added. An integer literal adopts any `Numeric` parameter, including a floating one,
because every numeric type represents an integer and `f64 x = 1;` already works. A
fractional literal needs `Floating` specifically, since a `Numeric` `T` may be an
integer. `bool` and `null` never adopt.

**The range window is the price of D11, and it is worth paying.** `Integral` admits all
eight integers, so the intersection is `0..127`: `-1` is refused because `u8` is
admissible, `128` because `i8` is. The permissive alternative was live — the
instantiation set is closed, so the value could be checked against every `T` the
program actually uses — and it was rejected because **adding a call site elsewhere would
then break a function that compiled yesterday**, which is the failure mode D11 exists to
abolish. The signature has to stay the contract. The realistic literals in generic
numeric code are `0`, `1` and `2`, so the window costs almost nothing; what it does
expose is that `Integral` is too coarse, since an author who writes `-1` means *signed*
and no bound says so. Watch for that pressure rather than pre-empting it.

**Which pool holds the value is the literal's question, never the type's.** Inside a
generic a literal has adopted `T`, which is neither an integer nor a float, so the
recorded type cannot say which of the two literal pools the value is in. Dispatching on
the candidate type instead reads an unrelated entry out of the wrong vector — *silently*,
because the index is usually in range. The same mistake appeared three times in one
afternoon: measuring a range, folding a divisor, and the pre-existing note at the
emitter's own integer-literal-in-float-context branch. **The rule is one sentence** — ask
the node kind, not the type — and it is worth stating because nothing in the types makes
it obvious.

**A third `type_of` that did not substitute.** `operation_type` derives the type an
operation happens *at* from its operands, and read them raw. With both operands recorded
as `T` there is no arithmetic result, so it fell back to the left operand and carried an
unsubstituted `T` into the emitter, which has no C spelling for one. This is now the
**fourth** instance of reading a callee's or a node's type without the instance's
bindings; two more were found beside it, in compound assignment and increment. The fix
each time is the same one line, which is the argument for there being no second spelling
of `type_of` at all.

**What a literal reopened.** `check_constant` returned early for any type that is neither
integer nor float, so with `T` in that position **every constant check was skipped inside
a generic body** — division by zero, remainder by zero, shift width. Unreachable while a
literal could not be a `T`, and live the moment it could. The split is between the checks
that need to know `T` and the checks that do not: a zero divisor is answerable without
it, and a shift count is answerable against the *narrowest* width the bound admits —
which is the literal rule again, applied to a width instead of a value. Only the folded
value-range check genuinely cannot be answered, and it is unreachable anyway, because a
constant expression of two literals settles on the default type rather than on `T`.

**One route in was missed.** The operator pre-check returned through `record` rather than
`record_constant`, so even after the above, the parameter path went around the checks
entirely. The concrete tail four lines below had always used `record_constant`; the two
paths existing at all is what let them drift.

**Slice 1f: a bug hunt over generics, and what it found.** Four findings, two of them one
root cause and both fatal. The corpus was 64 programs across four output modes, plus a
systematic hunt for inputs that pass `--check` and then crash the rest of the pipeline —
which is the technique that found the serious one, and the reason it is worth naming: a
`--check`-only test proves the checker and nothing else.

**A generic calling a generic crashed the compiler, in every shape.** `f<T>` calling
`g<T>`, self-recursion, `void` and value forms, two levels deep, bound forwarding — six
variants, all of them `Assertion 'no C spelling for this type'`, and all of them clean
under `--check`. Two separate faults, and fixing either alone still crashed. The checker
records an *open* instantiation, `g<T>`, whose argument is a parameter rather than a type;
the emit loop treated it as an instance. And `bindings_for_call` built the callee's map
from the recorded arguments without substituting the *enclosing* instance's bindings
first, so a call inside `f<i32>` resolved to `g<T>` rather than to `g<i32>`.

**The comment predicted it and got the trigger wrong**, which is the part worth keeping.
It said the plain pass becomes a worklist "the day a generic body calls another generic
with a type built from its own parameters, which needs generic types". Neither clause
held: plain `T` is enough, and `T*` has been a type built from a parameter since
substitution learned the pointer case. An assumption with an escape condition attached is
still an assumption, and this one went false without anybody noticing because nothing
tested the shape.

**The list is a seed, and the answer is its closure.** The set of functions to emit is the
checker's instantiations closed under the generic call graph — a new edge list recorded
where each generic call is typed, since that is the only pass that turns a type argument
into a type. `f<i32>` reaches `g<i32>`, which no call site ever wrote. A fixture exercising
four chains emits nine instances, of which five are named nowhere in its source.

**Termination is D42's rule rather than a depth limit**, and the two fixes are one piece
of work: the worklist is only safe because the checker has already refused the cycles that
would not close.

**Nine parser spellings aborted the compiler on a file it had already diagnosed.**
`f<T,>`, `f<,T>`, `f<T,,U>`, `f<if>`, and five malformed `where` clauses, all reaching
`Interner::text` with an invalid `Symbol_id`. The same family as the thirty-one crashes
the M5.5 hunt found, and the same fix: a node with no name is not built. Both sites now
return an error node, which the kind-filtering walks drop for free.

**And a test-coverage finding worth recording as a rule.** Bound forwarding — one generic
passing its own `T` to another — was built, mutation-tested and shipped in slice 1d with
nine test sections, every one of them a `Typed`. It had never once lowered. **A generic
test that only constructs a `Typed` proves the checker and nothing further**; the feature
it was testing was one of the six shapes that crashed.

**Slice 2a, step 0: `Ast::members`.** Generic aggregates need a place in the aggregate's
child list for its type parameters, and an aggregate's children are its *members* —
variadic, with no fixed slot to put anything in front of. Nineteen walks across the
compiler read that list, so the slot goes in behind an accessor and the walks convert
first, while the two answers still coincide.

**The slot goes at the front, following `enum`.** An `Enum_decl` already carries its
underlying type at child 0 and every walk says `subspan( 1 )`; an aggregate carrying its
type parameters the same way is one rule rather than two. Appending instead would be
safer for code that exists — a non-generic aggregate grows no child, so nothing changes —
but it leaves the hazard live for exactly the new feature, which is the wrong trade: a
loud break on the day the slot appears beats a silent one later.

**It asserts on any other kind**, and that is what made the sweep verifiable rather than
hopeful. Nineteen sites were classified by hand from every `children()` call in the
compiler; a misclassified `enum` or parameter-list walk would have aborted the debug test
binary on the first fixture that reached it, and none did.

**Proving a refactor did something is the other half.** A no-op rename is indistinguishable
from a rename that missed its targets, so `members()` was temporarily made to drop the
first child — what slice 2a will make it do for real — and the suite produced **62 unit
failures**. That is the evidence the sites route through it. The five test-block
assertions that count an aggregate's children were converted too: they are correct today
and would have failed then for a reason that has nothing to do with what they test.

**Slice 2a: `class Box<T>` declares, and `Box<i32>` is a type.** The type side only —
nothing emits one yet, so a *closed* instantiation reports `cannot be used yet` and the
slice is honest about where it stops. Interning is by declaration **and** arguments, so
`Box<i32>` and `Box<f64>` are two types from one declaration, and a field written `T` is
whichever of them asked.

**`Type::arguments` is a view, not a vector.** A span into a deque the table owns, which
is the arrangement `name()` already uses for the same reason — the views handed out have
to survive every later insertion, and `Type` stays cheap to copy. The mutation that
matters is the one that borrows the *caller's* span instead of copying: it passes every
test written with a temporary argument list, because reading a dead temporary usually
still finds the right bytes. The test that catches it builds the list in a local, writes a
different type over it, and lets it die.

**Four accessors replaced four conventions.** `Ast::members` skips the type-parameter
slot, `Ast::type_param_list` knows it sits at the front for an aggregate and at the fixed
last slot for anything function-like, `Checker::field_type` substitutes a field's declared
type through its receiver's arguments, and `Checker::resolve_type_arguments` is the arity
and bounds check shared by a generic call and a generic annotation. Each exists because
the alternative is the same rule written twice and drifting — the shape this milestone has
now hit four times.

**`wraps_a_parameter` got smaller rather than bigger.** D42's rule was written for `T*` and
walked pointers by hand; a generic aggregate would have meant a second walk beside it. It
is now "mentions a parameter without *being* one", which is one line over
`mentions_parameter` and answers `T*`, `Box<T>` and `Box<Box<T>>` uniformly. `f<T>` calling
`f<Box<T>>` is refused with nothing added for it.

**The rules for ownership and self-containment needed no generic case at all**, which is
the strongest evidence they were written at the right level. D2 counts only by-value
members, so `class Box<T> { T v; }` is owning exactly when `T` is and `struct Ref<T> { T* a; }`
never is. `contains_itself` walks declarations, so `Odd<T> inner` and `Odd<Box<T>> inner`
are both caught as infinite size, and `Node<T>* next` is finite. The one edit was the
struct rule reading `is_owning_type` rather than the `owning_` set directly — a field of
type `T` has no declaration to look up, and an unbounded one may own something.

**The gate is on closed types only, and finding that out took a wrong version first.**
Gating every generic annotation made `class Node<T> { Node<T>* next; }` unwritable, and
skipping generic aggregates in `order_structs` skipped their *cycle check* along with their
layout. Both are one rule seen from two sides: a generic aggregate has no layout and
belongs in no order, but it still has a shape and still has to be walked for one.

**Before 2b: `>>` silently misparsed a nested generic.** `Bad<Box<T>>*` read as
`Bad<Box<T>*>` — two well-formed types, and the compiler picked the wrong one with nothing
to report. `match_generic_close` splits `>>` into two closes and holds the second in a
counter, but the *inner* `parse_type`'s postfix loop ran before the outer close was taken,
so it read the `*` as its own. The guard is one test at the top of that loop: a pending
close means every token after it belongs to whoever opened the enclosing list. The spaced
form `Bad<Box<T> >*` needs no splitting and was always right, which is what the fix is
tested against.

It had a visible symptom nobody had traced: `class Bad<T> { Bad<Box<T>>* next; }` reported
*"contains itself, so it has no size"*, because the field's type came out as the struct
rather than a pointer to it. The identical spaced form was accepted.

**The mangling rework, done once rather than twice.** §15 owed three things here and 2b
forces the first: `Box<i32>` is not a plain word, and the old scheme rewrote `*` to `p`
and said in its own comment that it was safe only while every type name was one. Every
name is now length-prefixed — `i32*` is `P3i32` and a struct genuinely called `i32p` is
`4i32p` — which is injective by construction. No separator is needed, because a length is
always followed by a non-digit; one is written anyway, because `3i32_3i32` can be taken
apart by a reader and `3i323i32` cannot.

**A generic aggregate is `kl__Box__I3i32E`**, nesting as `kl__Box__I3BoxI3i32EE`, and an
aggregate with no type arguments keeps the name it already had — so nothing a v0 program
can write changed.

**And the third debt fell out for free.** `kl__id__T__i32` was the declared parameter type
followed by the type argument, which reads as though `T` were a type someone could write.
An instantiation is now named by its **type arguments alone**: its value parameters are
written in the declaration's own parameters, two instantiations of one generic differ only
by type arguments, and two generics cannot share a name. `kl__id__3i32`.

**What that gave up, and why it stopped being free.** An instance's symbol no longer
recorded its calling convention: `void f<T>( T a )` and `void f<T>( ref T a )` at `i32` both
became `kl__f__3i32`. They could not coexist — same name, and there was no overloading — so it
was not a collision. It was the same reason the `ref`/pointer ambiguity the PLAN also owed
here was unreachable, and **both became real together on the day overloading landed**, which
is what this predicted. Slice 3e pays both: an instantiation now carries its type arguments
*and* its substituted parameters, `kl__id__I3i32E__3i32`, and the `T` wart 2b was avoiding
does not come back, because substitution happens before the parameters reach the mangler.

**Slice 2b: one C struct per instantiation.** `Box<i32>` and `Box<f64>` emit as two, the
open `Box<T>` as none, and the whole set is decided nowhere in the source: an
instantiation reached only through a field of another is discovered by the walk that
orders them.

**The set is the table, not a record anyone kept.** By the time emission runs, every
struct type any code mentions is already interned — the checker interned what annotations
wrote, and `Type_table::substitute` interned what lowering created. So there is no
discovery pass, only an enumeration and a filter: `mentions_parameter` separates the
templates from the instances. The cost is that a closed type interned by dead code is
still emitted, which is a redundant definition rather than a symbol, and harmless.

**`Type_table::struct_types` has to be sorted, and the reason is not tidiness.** It walks
an unordered map, and the emitter writes structs out in the order it is given — so without
a sort the generated C differs between runs of the same compiler on the same source.
Type_id is interning order, which is deterministic for a given program. A mutation that
reverses it passes the whole unit suite and fails two goldens, which is the only reason
that property is pinned at all.

**The order is a post-order over *types*, computed at emission rather than in the
checker**, because an instantiation can be created during lowering and the checker has
gone by then. It interns as it goes, which is the point: `Holder<i32>` names `Box<i32>`
only through a field, and nothing else in the program writes that type down. It terminates
on the checker's own guarantee — `contains_itself` rejected every by-value cycle before
this runs, and a pointer field is skipped here exactly as it is there.

**Ownership is where 2b deliberately stops.** `Types::is_owning` answers from the
*declaration*, so `Box<i32>` and `Box<Buffer>` get one answer between them and one of them
is wrong. Emission does not care — a C struct holding an owning member is still a C struct
— but a drop elaborated against the wrong answer is a double free found under valgrind
several slices later rather than a diagnostic now. So an instantiation that owns something
is refused, by a check that computes the per-instance answer purely in order to reject it.
The rest of 2b works at every argument that does not own.

**What is still missing is construction.** A generic aggregate can be declared, instantiated,
emitted, assigned to field by field and read back — but not built from a literal, which is the next
thing worth having and is what the runnable fixture works around. *(Closed by slice E below. The
diagnosis here is wrong and worth keeping for what it cost: `Box<i32> { 7 }` does not parse, but
`Box { 7 }` always did, and the context names the instance in every position that can hold one.)*


**A bug hunt after 2b, and it reordered what comes next.** Five findings over the generic
aggregate surface, against a tree where the whole suite passes — so all five are gaps rather
than breakage, and none of them is the struct literal.

**Methods, constructors and destructors are all refused on a generic aggregate, by one line.**
`synthesise_receiver` builds the receiver as the enclosing aggregate's *name*, which for a
generic is an open reference and is rejected — so `Box<T>` may have no method, no constructor
and no destructor, and the error points at the method's return type and names a type the author
never wrote. The receiver needs the aggregate's own parameters as its arguments: inside
`Box<T>`, `this` is a `Box<T>`. **This is M6's acceptance blocker**, not the literal: the
milestone is defined by a generic `Box<T>` with a destructor dropping correctly, and a
destructor on a generic cannot currently be declared. The literal is still worth having and is
still next after it, but it was never the thing standing in the way.

**The refusal of an owning instantiation is reported against the wrong cause, and misses the
case that matters.** *(Fixed — the refusal itself went in 2c; the case it missed is closed under
"A field typed `T` at a drop site" below, which also corrects the diagnosis here.)* A generic class with its *own* destructor is told that its type argument
owns something, when the argument is an `i32`. And the guard fires only on a closed annotation,
so `Box<T>` written inside `f<T>` passes — the instance `Box<Buf>` is first created by
substitution when `f<Buf>` is monomorphised, by which point nothing re-asks, and the emitted
function declares the struct with no drop at scope exit. That is a silent leak past a check
whose entire purpose is to refuse it, and it is the same family as 1f's finding: **the
checker's instantiation list is a seed, not the answer.** The ownership rework 2b deferred has
to answer at the instance, wherever the instance is created, rather than at the annotation.

**A field that grows its own type argument hangs the compiler.** *(Fixed — see "D42 for
aggregates" below.)* `Box<T> { Box<Box<T>>* next; }`
checks clean and never finishes emitting. `contains_itself` does not catch it because the cycle
is not by-value, and D42's rule is enforced for functions — `f<Box<T>>` is diagnosed — with no
equivalent for aggregates. The loop that does not terminate is the *enumeration* in
`emitted_struct_order`, not its recursion: it is indexed over a table that visiting deliberately
grows, which is what discovers a `Box<i32>` reached only through a field, and here the growth
never stops. D42 belongs on the aggregate side too, and the diagnostic already exists to copy.

**A closed instantiation named before its generic is declared crashes the compiler.** *(Fixed —
see "The forward reference, and which fix actually took" below.)* Source
order alone decides it: the same file with the generic moved above its use is clean. Two layers,
and both are worth fixing — `is_owning_type` asks `is_parameter` before its own validity guard,
and `instantiation_owns` is reached from field declaration for a type whose fields are not yet
recorded, so it is handed an invalid `Type_id`. A forward reference to a *non*-generic struct is
fine, which is what makes this specific rather than a missing pass.

**No golden covers a generic-aggregate diagnostic at all.** *(Fixed — see "A golden for the
generic-aggregate diagnostics" below.)* `errors_generics.kl` declares no
generic aggregate, and the owning-instantiation refusal exists only as a unit test — so every
finding above needs a fixture as well as a fix, and the absence is why four of the five survived
2b's own testing.

**Slice 2c, first half: the receiver.** `synthesise_receiver` now builds `Box<T>` rather than
`Box` — a `Generic_type` over the aggregate's parameters *by name*, as fresh `Named_type` nodes
rather than the `Type_param_decl`s themselves, so the receiver resolves through the same path a
written annotation does. The three member parsers thread the aggregate's `Type_param_list` down to
it, and each member now stores that list in its own type-parameter slot.

That last part is what makes the rest work, and it is not merely convenient. A member pushes a
**barrier** scope, so the aggregate's `T` is not visible inside a method at all — the member has to
declare the parameters itself, and sharing the aggregate's list is what lets it, pointing at the
same `Type_param_decl` nodes the checker has already recorded types for. It also makes `is_generic`
true for a method of a generic, which is what keeps `lower` from emitting one directly and trying
to give `T` a C spelling.

The fix to `is_owning_type`'s assert was taken first and was wrong in a way the suite caught
immediately: putting the validity check in front of the existing condition also put `!is_struct`
in front of the parameter branch, and a `T` is not a struct — so an unbounded `T` stopped being
owning, three unit tests and `generics_owning.kl` failed. Validity first, then the parameter
question, then the struct one.

Mangling followed the same rule generics already use everywhere else: an instantiation is told
apart by its **type arguments**, not by a composed name. `mangle_destructor` takes the type's name
and its arguments rather than a `Type_id`, which keeps `Spelling::function` at the shape its five
call sites already pass. `mangle_constructor` was given the same treatment and remains what it was
before — **dead code**: `Spelling::function` never branches to it, so a constructor is mangled as
an ordinary function by its own name, and the `__ctor` scheme this file documents is implemented
nowhere.

What this opened up, with the suite green at 577 cases and 149 goldens:

- **A method's return type is not substituted at the call site.** `Pair<i32>`'s `T first()` reports
  `expected i32, but got T`, with no ownership or destructor anywhere near it. The seventh instance
  of reading a type without the instance's bindings, and the smallest one yet.
- **The owning refusal now fires on the acceptance program itself**, and blames the argument: a
  `Box<T>` with its own destructor is told that `i32` owns something. Both halves are 2c's second
  half — answer ownership at the instance, and say what is actually wrong.

**Slice 2c, second half: ownership per instance, and methods.** Two changes, and the first one is
smaller than it looked. `owning_` and `Types::is_owning` stay exactly as they are — they answer
*"does this declaration own?"*, which is the open form's answer and the right one for the checker's
move rules inside a generic body, where no instance exists yet. What was missing is the other
question, and it is now `instance_owns( ast, table, instance, recorded )`, a free function beside
`aggregate_bindings` and `field_type` for the same reason they are free: the emitter needs the same
answer and has no checker. Drop elaboration asks it through a one-line `Lowering::owns`, and
`moved_if_owning` drops `const` because answering interns as it substitutes.

The recursion carries a visiting set. Not as a cycle check — `order_structs` already refuses a
by-value cycle between declarations — but because instances are interned as they are asked about,
and the `Bad<Box<T>>` hang is the standing proof that instances have no equivalent guarantee.

A method of a generic was wrong in two places, and the second was hiding behind the first.
`check_method_arguments` now takes the receiver's type and substitutes both the parameter types and
the result through it; without that, `Pair<i32>`'s `T first()` returned `T` to a caller expecting
`i32`. Fixing the checker then exposed the same fault one stage down: `lower_method_call` read the
method's parameters under the *enclosing function's* bindings, which for a call in `main` is an
empty map, and substitution asserted on an unbound `T`. That is the eighth instance of the family,
and the assertion's own comment — "every parameter in scope is bound by construction at the one
call site that makes a map" — was true right up until there were two.

A method call writes no type arguments, so the explicit path that records an instantiation never
ran for one. `record_method_instantiation` records it from the receiver's arguments and pushes the
same `Generic_call` edge a written call would, which is what puts the method in the worklist at
all. `type_arguments_for_call` was lifted out of `lower_call` so both call shapes mangle the same
way. `Pair<i32>`'s `first` now emits as `kl__first__3i32( struct kl__Pair__I3i32E* )`, links, and
returns the right answer.

Constructors came out of 2.4 already working: `Box<i32>( 7 )` writes its type arguments on the
callee, and the member carrying the aggregate's parameter list is what makes `resolve_type_arguments`
and `record_instantiation` apply to a `Constructor_decl` unchanged. Giving `Spelling::function` a
constructor branch made `mangle_constructor` live for the first time — it had been dead code
documenting a scheme implemented nowhere — and renamed every constructor in seven goldens, with
every `.run` and `.exit` file unchanged.

Still open, and all that stands between here and M6's acceptance: nothing seeds a destructor into
the worklist, because a destructor is never called by name.

**Slice 2c, third half: the destructor reaches the worklist, and M6's acceptance runs.** A
destructor is never called by name, so nothing ever seeded one. `lower` now walks
`Type_table::struct_types()` and queues an `Instantiation` whose declaration is the
`Destructor_decl` and whose arguments are the instance's — guarded to generic declarations, because
a non-generic class's destructor is already emitted by the direct loop and seeding it again is a C
redefinition. Nothing else in the worklist changed: `ast.type_param_list` of a `Destructor_decl` is
the *aggregate's* list after 2.4, so the parameters bind with no special case.

Proving it took three more readings of a type without the instance's bindings, found one at a time
by running the acceptance program and reading the backtrace. `lower_construction` had it twice — the
receiver's type and each parameter's — and was also mangling the constructor with no type arguments
at all, so two instantiations wanted one symbol. `drop_place` had the tenth and worst instance: it
walks the members of the *declaration* and dropped each field at its declared type, so the field of
a `Box<Buffer>` was dropped as a `T`. `field_type` is the fix and already existed.

The pattern is now unambiguous enough to state as a rule. **Any code that reaches from an instance
into its declaration must come back through the instance.** Ten sites, one shape, and each was
found by a crash rather than by a test — which is what `--check`-only fixtures cost.

With the refusal at `type_of_annotation` temporarily lifted, **M6's acceptance passes**: a
`Box<T>` with a destructor at `i32` and `f64`, a live-object counter back to zero, exit 0, valgrind
clean. Both destructors are emitted, both are called on every path out, and in reverse declaration
order. What remains is to delete the refusal and write that program down as a fixture.

**M6's acceptance is met.** The refusal in `type_of_annotation` is gone, and with it
`Checker::instantiation_owns`, which had no other caller — it existed only to reject. What replaced
it is not a diagnostic but an answer: `instance_owns`, asked at every drop site.

`codegen/generics_destructors.kl` is the acceptance written down. A `Box<T>` with a constructor, a
destructor and a method, at `i32` and `f64`, with a live-object counter; two instances alive at
once, dropped at the end of their block rather than the end of the function; and an early `return`
out of a second function so the drop flag is exercised too. It passes, and passes under valgrind.
Two C structs, two constructors, two destructors and two `get`s come out of one declaration.

The unit test that used to assert the refusal now asserts the thing the refusal was standing in
for: `Box<Buf>` owns, `Box<i32>` does not, `Ref<Buf>` does not because D2 counts only by-value
containment, and a generic with a destructor of its own owns at every argument. It also pins the
half that cannot tell them apart — `Types::is_owning( Box<i32> )` and `Box<Buf>` give one answer —
which is the reason both questions exist rather than an oversight.

**A mangling defect this uncovered, not yet fixed.** A constructor's symbol encodes its parameter
types from the *declaration*: `Box<i32>`'s is `kl__Box__I3i32E__P3BoxI1TE_1T__ctor`, with the
template's own `T` in the name of a concrete instance. It is not a collision today — the type
arguments in the prefix separate the instances — but a symbol naming a type parameter is wrong on
its face, and it is the constructor branch alone that has it, because an instantiation is otherwise
named by its type arguments and nothing else. `Spelling::function` reads
`parameter_types_vector` against the declaration; it wants the call's bindings, the same ones
`lower_construction` now uses.

**The constructor symbol no longer names a type parameter.** `Spelling::function` substitutes the
declaration's parameter types through the instance's bindings before mangling, so `Box<i32>`'s
constructor is `kl__Box__I3i32E__P3BoxI3i32E_3i32__ctor` rather than `..._P3BoxI1TE_1T__ctor`.
`Spelling` holds a mutable `Types&` for it, because substituting interns. It is the eleventh site of
the same shape and the first found by reading a symbol rather than by a crash.

A constructor is the one symbol that needs both halves: the type arguments say which instance, and
the parameter types tell two constructors of that instance apart. Every other instantiation is
named by its type arguments alone, which is why nothing else had the fault.

**Two things changed under this that were not aimed at.** A closed instantiation named *before* its
generic is declared now compiles — both causes are gone, `is_owning_type`'s guard order and
`instantiation_owns` being reached with an unrecorded type, the latter because the function no
longer exists. `codegen/generics_forward_reference.kl` is its regression test: every generic in it
is named before it is declared, `Box<i32>` among them, and it is run rather than only compiled so a
struct that emits without its destructor call is caught too (2026-09-18).

### D42 for aggregates — done

The `Bad<Box<T>>` family is closed. The hang was never in a recursion: `emitted_struct_order`'s
driver loop re-reads `struct_types()` on every step *because* visiting a type can intern another,
which is what discovers a `Box<i32>` reached only through a field — and `Bad<T>` holding a
`Bad<Box<T>>*` makes that growth unbounded. A gdb interrupt lands in the `std::sort` inside
`struct_types()`, which is a symptom of an unbounded set rather than a bad comparator.

**The rule needed no analysis written, only edges recorded.** D42's graph is over *declarations*
with arguments spelled in the caller's own parameters; it was never about calls in particular. A
field naming another generic is the same edge, so `record_generic_uses` walks a recorded field type
— through pointers, into struct instances, and into their type arguments — and pushes a
`Generic_call` for each generic instance it finds. `wraps_a_parameter`, `expands_forever` and the
diagnostic are untouched. Recorded into the *one* graph rather than a second, so a cycle running
through both an aggregate and a function is found the way a cycle through two functions is.

`Bad<Box<T>>` is two facts — `Bad` names `Bad` at `Box<T>`, and `Bad` names `Box` at `T` — so the
arguments are walked as well as the type. Only the inner mention closes the cycle in
`Box<A<Box<T>>>`, which is the section that pins it.

**One mistake, one message.** The by-value form is infinite twice over — no size, and no finite
instance set — and both rules fired, so `order_structs` now seeds `expanding_` with the cycle it
reported. Having no size is the more basic half and the one the author fixes first.

**The guard on the field pass is not observable, and stays anyway.** A non-generic aggregate cannot
write a type argument that mentions a parameter, so skipping it changes no diagnostic; deleting it
kills no test. It keeps `generic_calls_`'s stated invariant — arguments in the caller's parameters
— true by construction, which is what the lowering worklist's `is_closed` filter leans on. The
test section is named for what it actually pins.

**Still open in the same family:** the shape in a *method signature* rather than a field
(`Bad<Box<T>>* grow()`) is recorded by nobody. *(Fixed — see "D42 sees a method signature" below.)*
It compiles today only because `emitted_struct_order` walks fields and not signatures, so it is
latent rather than broken. The second call site for `record_generic_uses` is
`declare_signatures_member_functions`, whose loop has the same two ends in hand.

### A golden for the generic-aggregate diagnostics

`sema/errors_generic_aggregates.kl`, thirteen messages over one file. Its own fixture rather than
more declarations in `errors_generics.kl`, which scopes itself to a generic's signature and to what
a call site names: these mistakes are made in a type *annotation*, in a field, or in the shape of
the declaration, and a call reaches none of the three.

Two of the messages have call-site twins that were already covered and are nonetheless separate
emit sites — a bare `Box` in an annotation, and `Plain<i32>` on an aggregate that takes no type
arguments, whose note (`write `Plain` on its own`) the callee form has no way to give. The enum
form, `Suit<i32>`, is a third branch with no note at all. The rest are the annotation span on the
shared `resolve_type_arguments`, an unresolved name in either half, the rendering of an
instantiated name in an ordinary type error, the two layout cycles, and D42's growing argument.

**The clean declarations are the point, not padding.** `Node<T>* next` and a `Holder<T>` holding a
`Box<i32>` sit in the same file and produce nothing; because the runner diffs stderr *whole*, they
assert that the rules stop without anyone writing an assertion. The same mechanism is what pins
one-mistake-one-message on the by-value `Deep<Box<T>>`: in the unit test that is a `== npos` someone
has to think to write, and here a second message simply changes the file.

Four mutations, each backed out of `type_checker.cpp` alone and each caught by this fixture with the
unit tests set aside: dropping the field-pass edge, dropping the `expanding_` seed, dropping the
pointer recursion, and dropping the argument recursion. The last survived the first draft — nothing
in it closed a cycle through a type *argument* — which is what the `Ring<T>` declaration was added
for. Written by hand rather than with `--update`, since a new fixture has nothing to snapshot and
`--update` would have rewritten all 150 others to add three files.

### The forward reference, and which fix actually took

The crash is gone, and not by the fix aimed at it. Two layers were recorded and they had opposite
fates.

`is_owning_type`'s guard order was corrected deliberately — `ace2fdf` asks `is_parameter` before
the validity check, `81e8789` does not. It is also **inert**: putting the old order back changes
nothing, and the whole suite passes with it. `Type_table::is_parameter` asserts on an invalid id,
so that survival is proof that nothing reaches `is_owning_type` with one. The comment there used to
give the forward reference as its reason; it now says only that the guard is insurance, which is
what it is. It stays, because the cost is one comparison and the failure it prevents is an abort.

`instantiation_owns` was never fixed — its *call site* was deleted. It appears in no commit at all,
having lived only inside 81e8789's working tree, and the per-instance ownership answer is now
`instance_owns`, whose only non-test caller is the lowerer. The checker's field pass asks no
ownership question at all any more. That is what removed the crash, and it was a side effect of
moving ownership to lowering rather than anything aimed here.

The successor is also written the way the original was not: `owns_through_fields` guards validity
before `get`, so putting the call back into the field pass does **not** crash. The original bug needs
both layers restored — the call site, and a question asked ahead of the validity check — and with
both back the assert reproduces exactly.

**What that leaves at risk is not the old assert.** Nothing stops a later pass from asking a
fields-dependent question during field declaration again, which is what the old call did; and note
that before it crashed it was also getting a *wrong* answer, because the fields it needed were not
recorded yet. So the test had to be a program rather than an assertion about a guard.

`codegen/generics_forward_reference.kl` declares every generic at the bottom and names them all
above: a field, a two-level field (`Box<Box<i32>>`), a local, a parameter, a return type, and an
instance whose destructor has to be found. It counts its own drops, because a forward reference
that emits a struct with no destructor call still compiles, links and exits 0.

No fixture forward-referenced a generic before it — the apparent hits in `generics_aggregates.kl`
and `generics_destructors.kl` are all in comments. Under the faithful two-layer reproduction the
only goldens that fail are this one and `errors_generic_aggregates.kl`, both written the same day.

**And a driver bug fell out of writing it.** §11's emitter entry has always said `keelc` writes the
`.c` beside the executable; it wrote it beside the *input*, so building a fixture in place left a
stray `.kl.c` in the source tree — which the golden suite's own stray-file check then reported as a
failure. It now goes where `-o` points, keeping the input's name so `foo.kl.c` still says what it
came from. With no `-o` the executable is already the input's stem in the working directory, so both
artifacts land there together.

### A field typed `T` at a drop site — done

`class Box<T> { T v; }` at `Box<Buf>` type-checked, lowered to correct KIR — `drop _1.v`, the field
rather than the whole local — and then aborted in `Ast::members`. `Kir_emitter::type_of` walks a
place's projections and read a field's type straight off the declaration, so `_1.v` answered `T`;
`Spelling::destructor_of` asked that type for its declaration and got a `Type_param_decl`, which is
not an aggregate. The assert it would have hit one line later is the accurate one: a drop naming a
type with no destructor of its own.

**The twelfth site of the same shape**, and the first in the emitter rather than the checker or the
lowerer: a type read without the instance's bindings. The fix is that the projection walk goes
through `field_type`, whose own comment already claimed *every* read of a field's type went through
it — the emitter was the one that did not. It composes down a chain, so `Box<Box<Buf>>` drops as
`kl__Buf__dtor( &kl_outer_1.kl_v_31.kl_v_31 )` rather than needing a second rule for nesting.

**The non-generic twin always worked**, which is what made this specific rather than a missing
feature: `class Holder { Buf v; }` decomposes to `kl__Buf__dtor( &kl_h_1.kl_v_29 )` and runs.
Member-wise drop existed; only the substitution was missing.

**This is what 2b's refusal was standing in front of.** The annotation-time guard rejected an owning
instantiation outright, so no such drop ever reached emission; deleting it in 2c exposed the hole
rather than opening it. That also closes the second half of 1f's finding by contradicting its
diagnosis: the leak was described as needing an instance created by *substitution* inside `f<Buf>`,
and a plain `Box<Buf> b;` reached it, so the fault was never about where the instance came from.

`codegen/generics_owning_field.kl` pins it — an owning instance and a non-owning one from the same
declaration, a two-level nesting, and an early return so the drop flag is exercised. Reverting the
one expression fails that golden and no other, which is also the measure of how little covered it:
the crash survived 152 passing fixtures.

### D42 sees a method signature — done

A return type and a parameter are the same edge a field is, and `declare_signatures_member_functions`
now records both through `record_generic_uses`. The rule, the walk and the diagnostic are untouched
for the third time running — D42 keeps turning out to need edges rather than analysis.

**The `from` is the aggregate, not the method**, and that is load-bearing twice over. The cycle to
close is `Bad` to `Bad`; an edge from the *method* would need a `Bad` to `grow` edge coming back, and
nothing records one, so the self-loop would be invisible. And `generic_calls_` is not only D42's
graph — the monomorphisation worklist follows it, matching `edge.from` against an instance's
declaration and handing `edge.to` to `Lowering` as a function. An aggregate declaration is never an
instance's declaration there, because an aggregate instance is entered under its *destructor* node;
so an edge from an aggregate is invisible to the worklist, which is exactly right for a type named in
a signature that nobody calls. An edge from a `Function_decl` would not be, which is why the free
function pass does **not** get the same call — and does not need it: a function cannot be named as a
type, so a signature edge out of one can never close a cycle.

**Latent, and with a date on it.** The growth that hangs is `emitted_struct_order`'s driver loop, and
it walks fields. A signature-only mention interns the open form once and nothing re-substitutes it
per instance, so nothing grew. That ends the moment the monomorphiser substitutes a method's
signature types per instance — which is what `T* data()` needs, and M8's first container writes it.

**Three cases, because two would have passed for the wrong reason.** The cycle through a return type
and the cycle through a parameter are separate declarations, since one message is reported per
generic however many ways round it goes — a single method with the cycle in both positions leaves
either recorder unpinned. The third declaration must stay *silent*: naming its own generic at the
argument it was given is forwarding, not building, and it is what every method of a container looks
like. Stderr is diffed whole, so it costs nothing to assert.

That mattered here. The parameter recorder went in guarded by `is_generic` asked of the
`Param_decl`, which `Ast::type_param_list` answers with an invalid id for anything neither aggregate
nor function-like — so the guard was always false and the recording was dead. It compiled, and with
only a both-positions fixture the suite would have gone green over half a fix.

### A generic `enum` — done

`enum Opt<T> { None, Some( T v ) };` used to abort in `Interner::text` on an invalid `Symbol_id`.
The parser did **not** accept the type-parameter list — there was no list to accept: `parse_enum_decl`
expected `:` or `{` after the name, so `<` failed `expect( Token_kind::L_brace )`, recovery made a
`Variant_decl` out of each of `<`, `T`, `>`, and `declare_signatures_enum_decls` then read a name off
one that had none. Two faults, a missing feature and an unguarded read, and the feature removes the
input that reaches the read.

**It was neither promised nor refused** — D39 names functions and aggregates and says nothing about
an `enum`, and §6.6 does not exclude one. Settled now in favour of implementing it, because the
customer is dated: `Result<T, E>` is an `enum` over two parameters, §12's error-handling question is
due with M6, and it cannot be answered while the spelling crashes.

Three things make an `enum` cost more than an aggregate. **Child 0 was taken** by the underlying
type, so twelve places hand-wrote `children( decl ).subspan( 1 )`. **`Type_table::enumeration` was
keyed by declaration alone**, one type per declaration, where `structure` keys by declaration *and*
arguments. And **`Type_kind::Enum` falls through every structural walk** — `substitute` and
`mentions_parameter` both end in `default: return type`, so `Opt<T>` would substitute to itself and
the worklist would call the open template a closed instance.

**Slice A — it parses.** Done. `parse_enum_decl` now reads a type-parameter list and `where` clauses
in the same shape `parse_aggregate_decl` does, and an enum's children are `{ type_params, underlying,
variants… }`. Child 0 matches an aggregate's, so `Ast::type_param_list` answers for both with one
predicate and `is_generic` starts telling the truth for an enum; `is_aggregate` was deliberately left
alone, because `Ast::members` asserts on it and an enum joining that set would be walked as a struct
whose variants are fields. `Ast::variants` replaced all twelve hand-written spans, and its assert is
what made that sweep verifiable. The duplicate-variant check now skips a nameless variant, which is
the crash itself: `main` runs sema on a broken parse, so error recovery still reaches it.
`sema/errors_generic_enums.kl` pins both halves.

**Slice B — it types.** Done, with one gap it cannot close on its own. `structure` and `enumeration`
now forward to one private `composite` taking a `Type_kind` and an `element`, which deleted `enums_`
and one copy of the `<…>` spelling loop; `declare_signatures_enum_decls` declares its type parameters
before resolving the underlying type and passes them as the instance arguments, so `Opt<T>` is the
open form D11 checks payloads against. `substitute` and `mentions_parameter` grew an `Enum` case
sharing the `Struct` one's body, the resolver scopes the parameters behind a `Barrier`,
`type_of_annotation` admits an `Enum_decl` in both its `Generic_type` and `Named_type` branches, and
`aggregate_bindings` no longer gates on `is_struct`, so `field_type` answers for a payload.

What works and is pinned by `sema/errors_generic_enums.kl`: instantiation at one and two parameters,
arity in both directions, a bare `Opt` naming the open form, a bound violation, two arguments being
two types, and a D42 cycle closing through a payload. **An unbounded `T` payload is refused**, and
that is D30 and D11 agreeing rather than a gap — `is_owning_type` answers a parameter from its
bounds, so `Some( T v )` needs `where T : Copyable` exactly as a struct field does.

**Slice D — which instance a variant path names.** Done. `Opt::Some( 7 )` parses and resolves
already; what it produced was `types_[decl]`, the *declaration's* type, which for a generic enum is
the open form `Opt<T>` — a template rather than anything a value can hold. Every route to a variant
runs through the tail of `infer_path`, so one change there serves construction, patterns and bare
`case` labels alike. The instance now arrives through `expected_enum_`, the sibling of the existing
`naming_variant_` flag: the pattern and the bare label pass the scrutinee, and `check` passes its
expectation around the trailing `infer`. `infer` is not memoised, so a flag set around a call is read.

**The declaration comparison is the whole guard.** `infer_path` takes the expectation only when it
belongs to the same declaration; a mismatch falls through to the open form and is refused by the
ordinary message, rather than being quietly retyped to whatever was wanted. Inverting that one test
silently accepts `i32 x = Colour::Red;` and stops two different enums comparing unequal — which is
what it did when first written, and what three unit tests caught.

**A generic path with no expectation is now an error**, because `auto` has none to disagree with:
`auto x = Opt::Some( 7 );` used to give `x` the type `Opt<T>` with no complaint at all. Deliberately
not inferred from the payload argument — that would still leave `Opt::None` untypeable, so an
expectation is the only rule that covers both.

**Deferred: the explicit spelling.** `Opt<i32>::Some( 7 )` still does not parse —
`Parser::scan_type_arguments` commits to the generic reading only when `(` follows the closing `>`,
and the postfix branch after it builds a `Call_expr` unconditionally, so a path would need a
`Path_expr` carrying a `Type_arg_list`, the same shape `Box<i32> { 7 }` needs. Nothing requires it:
every position that can hold a variant can also state the type.

`sema/errors_generic_variants.kl` pins it, and the correct code at the top of that fixture is as much
of the test as the errors — a sema fixture stops before codegen, so a positive case can live in one
before slice C exists. Reading `v.v` off a pattern binding is the assertion that the binding is a
`Red` and not a `T`. The types are two structs rather than two numbers: `i32` widens to `f64`, so a
payload checked against the wrong one of those is still accepted and proves nothing.

**Slice B's three unverified guards are now verified.** All three needed a substitution that reaches
*through* an enum rather than replacing a bare `T`, which the corpus had nowhere: a payload of type
`Opt<T>` inside `Nest<T>` kills dropping the `Enum` case from `substitute`, a D42 cycle whose growing
argument is `Opt<T>` rather than `Box<T>` kills dropping it from `mentions_parameter`, and `name` for
`base_name` needed an instance created *only* by substitution — `enumeration` interns by declaration
and arguments, never by name, so a wrong name is discarded unless that call is the first to intern
that pair. All nine mutations over slices B and D now die.

**Slice C — it emits.** Done. Enum instances join the one ordered composite list rather than
getting a second: an enum with a by-value struct payload and a struct with a by-value enum field are
both writable, and only one post-order over all of them puts every definition after everything it
contains. Two asserts have to be guarded first — the destructor-seeding loop in `lower.cpp` calls
`ast.members` on whatever `is_generic` admits, and `emitted_struct_order`'s walk gates on
`is_struct`. Then `emit_enums` is deleted rather than rewritten, folded into `emit_structs`, which
also fixes a **latent bug that predates generics**: `run()` emits all structs before all enums, so a
struct holding a payload enum by value emits before the enum it contains. Mangling and spelling need
nothing — `mangle_struct` already reads `base_name` and `arguments` off the `Type`.

What it actually took, in the order the fixture forced. **Lowering came first, and it aborted rather
than misspelled**: `lower_variant_construction` and `bind_variant_pattern` both read a payload
field's *declared* type, so `Lowering::type_of` substituted a bare `T` through `main`'s empty
bindings and tripped the "type parameter is not bound" assert on the first `Opt::Some( 7 )` in any
program. Both now go through `field_type` against the instance — the same call `emit_structs` makes,
and reachable only because slice D records the instance on the path.

`struct_types` became `composite_types` and stopped filtering, and the two walks that consume it
each needed a guard, because `Ast::members` asserts on an `Enum_decl`: lowering's destructor seeding
skips an enum outright — D30 refuses an owning payload, so there is no destructor to seed — and
`emitted_struct_order` reads a new `contained_fields` that answers for both kinds, a struct's
`Field_decl` members and an enum's payload fields flattened across its variants. That flattening is
the same list `emit_composites` writes, which is why it is one function and not two.

**A payload-free enum must stay out of the order.** It has no C struct at all — `Spelling::type`
writes it as its underlying integer — so admitting one forward-declares a struct that is an `int`.
The kind test has to come before `enum_has_payload`, which asserts on anything but an `Enum_decl`.

`emit_enums` is deleted. It walked the *AST*, so it wrote one C struct per declaration — the open
`Opt<T>` for a generic — and it ran after every struct, which is the latent ordering bug. Driving
both kinds from the one ordered list fixes both at once, and gives payload enums the forward
declarations they never had: `codegen/payloads.kl.expected` gains three lines and nothing else moves.

`codegen/generics_enums.kl` pins it, and two of its lines exist only because a mutation survived
without them. `Wrap<f64>` is named before anything names `Box<f64>`, so that containment edge has to
come from the enum's own payload rather than from interning order — with the walk reverted to struct
members the rest of the fixture still passed, because every other container happened to be interned
after what it holds. And `Opt<Colour>` carries a payload-free enum as a type argument, which is what
notices if the filter goes.

**Slice E — it is built.** Done. `Box<i32> { 7 }` was recorded here as the thing that does not
parse, and that framing was wrong in the same way slice D's was: the unqualified `Box { 7 }` already
parses and resolves, and `infer_struct_literal` simply took `types_[decl.v]` — the open form, at the
same line and for the same reason `infer_path` did. The parser was never in the way.

So the slice is two edits. `infer_struct_literal` takes `expected_composite_` when that type's
declaration is this one, which is slice D's guard reused verbatim — the member is renamed from
`expected_enum_` because a literal is not a variant, and nothing else about the channel changes:
`check()` already carried the expectation into it. `field_type` then substitutes each field through
the instance without being asked, so the second of the two errors every generic literal used to
produce disappears with no edit of its own. And `lower_struct_literal` converted each value to
`Lowering::type_of( field )` — **C6 a third time**, after `lower_variant_construction` and
`bind_variant_pattern`, and failing the same way: `T` has no binding in `main`, so it aborts rather
than misspells.

**The explicit `Box<i32> { 7 }` spelling is dropped, not deferred.** The argument is the one already
made for `Opt<i32>::Some`, and it is now checked rather than assumed: type arguments are never
deduced from call arguments — **true when this was written and deliberately changed by §12's
inference decision, which leaves the conclusion standing**: a struct literal, like a numeric one,
contributes nothing to deduction, so its instance still comes from the expectation — so every
argument position is a written type, and a field initialiser's
expectation arrives already substituted. Every position that can hold a literal states its type.
`auto` is the exception, and it is a diagnostic.

**One message was wrong, and it was wrong in slice D too.** Both sites reached "nothing here says
which `Box` this is" by way of *the expectation did not apply*, which conflates nothing being
expected with something else being expected: `Plain p = Box { r };` was told nothing said which
`Box`, and offered a help line that would not fix the program. The two causes have two different
fixes, so they need two messages — `no_instance_named` decides which and both sites call it. The
wrong-declaration branch states the mismatch itself rather than falling through, because falling
through puts the open form on the `got` side of a message about a type the author never wrote.

`sema/errors_generic_literals.kl` and `codegen/generics_construction.kl` pin it, and all eight
mutations die, including slice D's guard inverted. The codegen fixture runs because the failure that
survives emission is quieter than the one that does not: reading the declared type aborts, but a
conversion made against the wrong *instance* is valid C, and `Box<f64> { 2.5 }` arrives as 2.0 with
nothing to say so.

### Slice 3: overloading — done (2026-09-18)

§12's row decided it; this is what building it cost and what it corrected. Free functions,
methods and constructors, generic and not, in five slices.

**The whole feature is one question, asked twice.** Could a call site say which of these it
meant? Asked of two declarations, it is the duplicate rule. Asked of a call, it is selection.
Both are answered from the same pair — each parameter's **declared type** and the **marker the
call must write** — and the checker already computed that pair for D31's argument rules, so
the rule is stated once and read three times: by the duplicate check, by selection, and by the
mangler.

**§12's central claim was wrong, and the fix is the entry's own principle.** It says "an
argument either has the parameter's type or it does not", from which resolution collapses to
exact matching with no ranking. But `check` accepts on `Type_table::holds`, which is §6.4's
lossless widening: a `u8` argument reaches `i32`, `i64` and `f64` alike, so two candidates can
both accept and something has to order them. That something is ranking, which D5 deleted and
which §12 promises never to rebuild. **Selection is therefore by exact type, and widening
happens only after a candidate has been chosen.** One candidate behaves exactly as before —
every one of the 164 goldens that existed proves it — and `f( b )` with a `u8` against
`f( i32 )` beside `f( i64 )` is refused with *no `f` matches these arguments* rather than
resolved by a table nobody can recite.

**The candidate set is a chain on declarations, not a set per use.** `Resolution` gained one
vector the size of the node count: `next_overload`, the next declaration of a name in the same
scope. `declaration_of` keeps its meaning — the first candidate — so every existing caller is
untouched, and the cost is one vector rather than a list per use. It is built where `declare`
already detected the collision, and only two callables of one kind chain: a function beside a
variable is still a redeclaration, and a local still shadows the whole set.

**The duplicate check moved passes, because the resolver cannot ask it.** Whether two
signatures differ is a question about types, and the resolver has none. So the resolver stops
reporting a second `Function_decl` or `Method_decl`, and a new checker pass after
`declare_signatures` reports the pairs nothing can tell apart. That is one message per reason:
identical parameters, a difference only in the return type, a difference only in a trailing
`const` — which binds the receiver, a thing a call does not write, and is the C++ habit most
likely to be reached for. Two more kinds cannot be overloaded at all: an `extern` keeps the C
name someone else defined it under, and a program has one `main`.

**`const ref` is why identity is the pair and not the type.** `f( i32 )` and
`f( const ref i32 )` have different parameter types — `i32` against a pointer — and are called
identically, because a `const ref` takes no marker. They are refused. `f( ref i32 )` and
`f( i32* )` lower to the same pointer and are called differently, `f( ref x )` against
`f( p )`. They coexist. Reading identity off the lowered type would have got both backwards.

**A literal narrows to a family and no further**, which is D26 read in the one position where
its context is the thing being chosen. `f( 1 )` against `f( i32 )` and `f( f64 )` picks the
integer one; against `f( i32 )` and `f( i64 )` it is ambiguous, and the message says so rather
than inventing a preference. `nullptr` and a struct literal say nothing at all, by the same
argument — a struct literal takes its instance from the expectation, which is what is being
chosen. `true` is not in that group: it has exactly one type, so it is as informative as a
variable.

**Selecting has to type the arguments, and typing them twice would report twice.** The
argument loop's job was three things at once — give literals a type from context, check the
type, and check the marker — and selection needs the type before the parameter is known. So
arguments that type themselves are inferred once up front, and the ones that need context are
checked afterwards against the chosen parameter. The split is recorded per argument rather
than inferred later, because "has this been typed" is not a question the AST can answer. Only
a set of two or more takes this path.

**Lowering stops choosing and starts reading.** The map that already carried
`Call_expr -> Method_decl` now carries the callable for *every* call, and lowering reads it —
so `lower_construction` no longer rescans for the first constructor, and `lower_call` no longer
reaches for the name. One map, one recorded decision, three readers. Its own comment already
gave the reason before this slice needed it: finding a callable by name is a rule, and a pass
repeating it is how two passes drift.

**Slice 3e: both mangling collisions became reachable on the same day, as predicted.** §15's
note and slice 2b's note each said the same thing — harmless while a second declaration of a
name is caught by name — and overloading is precisely what stops it being caught. A parameter
now mangles as the pair a call site writes, so a borrow is `R3i32` and a pointer `P3i32`, and
a transfer is `M3i32` where a copy is `3i32`. An instantiation carries its type arguments
**and** its substituted parameters, `kl__id__I3i32E__3i32`; 2b dropped the parameters to avoid
`kl__id__T__i32`, and substituting before mangling — which the constructor path already did —
gets both. The one pair still sharing an encoding is `const ref T` and bare `T`, which is
sound precisely because they may not be declared together. 277 golden lines changed and every
one of them is a symbol: with `kl__*` masked, the removed and added sets are identical.

**Members are where §12 had no answer, and the rule is new.** `at( T )` and `at( i32 )` on a
`Box<T>` are one signature at `Box<i32>`, and a method call writes no type argument that could
say which was meant. Ranking by how specific a parameter is would be exactly what D5 deleted.
So the pair is **refused at the declaration**: two members of one name that some substitution
could make identical may not both be written. D11's promise is that a generic which checks
instantiates, and the alternative — reporting ambiguity per instantiation — breaks it. The
check is deliberately coarse: any parameter mentioning a type parameter is treated as able to
collide, so `at( Box<T> )` beside `at( i32 )` is refused although no `T` makes them equal.
Refusing a legal pair is recoverable; accepting an ambiguous one is not.

**Not built, deliberately: the generic tie-break.** §12 decided a non-generic candidate beats a
generic one. It is unreachable today — a call with type arguments and a call without them
select disjoint sets, because the count filters before anything is typed — so it has no fixture
that is not synthetic. It belongs with type-argument inference, which is the next item and the
thing that makes the two sets overlap. **Paid there, slice 4.**

**What the tests found.** Two stale tests inverted, as usual, and both said something: the
resolver no longer reports two functions of a name, and two constructors are no longer refused
for existing. One **real defect**, caught by a unit test and not by any golden: the call's
result type was read off `decl` — the *first* candidate — rather than off the callable
selected, so every call in a set had the first one's return type. It survived the codegen
fixture because every overload there returned `i32`; the fixture now has a pair returning
`bool` and `i32`, which widen into each other in neither direction. The unit tests were
rewritten to read answers off return types that cannot widen, rather than off node indices,
after two of them turned out to be asserting against the wrong `Call_expr`.

**27 mutations, 27 killed.** Two survived the first pass and both were gaps rather than
defects: a `move` argument meeting a bare non-owning parameter with two candidates in scope,
and an argument typed twice — which is only visible when a *nested* error would then be
reported twice, since an argument that fails to type cannot be selected against. Both now have
tests. `codegen/overloads.kl` runs every shape and returns 42; `sema/errors_overloads.kl`
holds the thirteen refusals.

### Slice 4: type-argument inference — done (2026-09-18)

§12's row decided it; this is what building it cost, what it corrected, and the one rule the
row did not state.

**The row named two sources and did not say which wins.** `i64 x = id( a );` on an `i32`
argument has the argument saying `T = i32` and the expectation saying `T = i64`, and they
cannot both be right. **The arguments decide, and the expectation fills only what they left.**
That makes the deduced call behave exactly as the written `id<i32>( a )` does — instantiate at
the argument's type, then widen the result under §6.4 — where the other reading would refuse a
program the explicit form accepts. An expectation is where a value is going, not a claim about
how it was made.

**Deduction is the inverse of substitution and lives beside it.** `Type_table::deduce` walks
the same four cases `substitute` and `mentions_parameter` walk: a parameter binds, a pointer
recurses into its element, an aggregate recurses pairwise over its arguments when the
*declarations* match, and anything else contributes nothing. Two properties keep it small. It
**never reports**: a `Box<T>` parameter given an `i32` deduces nothing, and the ordinary
argument check reports that once the other arguments have settled what `Box<T>` is — so there
is one place that says a type is wrong. And it needs **no occurs check**: type parameters are
interned per declaration, so an argument's type can never mention the callee's own parameters
and `T := Box<T>` is unreachable. The growth D42 guards is across calls, which `generic_calls_`
already catches.

**A literal contributes nothing, and that is not a branch.** A shape whose kind is not `Typed`
carries no type at all, so deduction reads nothing from it — the rule lives in `Argument_shape`
rather than in a guard at each reader. Two guards written for it were deleted after mutation
showed them unkillable, which is the same finding stated twice: `deduce` refusing an invalid
type *is* D26 here.

**Selection probes, and the probe throws its answer away.** A candidate set can hold a generic
whose parameters are unwritten, so choosing needs to know whether they can be deduced at all —
but the probe is silent, because a candidate that cannot be deduced is simply not a candidate
and what to say when nothing matches is selection's to decide. That is the split
`candidate_accepts` and `check_call_arguments` already have. **The probe deliberately does not
read the expectation**: choosing a callable by what its result is wanted for is selection by
return type, which §12 refuses outright when it forbids two declarations differing only in
what they return. Two things mutation proved about the probe: its conflict check is redundant,
because `candidate_accepts` then requires exact equality on every argument and rejects a
disagreeing one whichever binding was kept; and its merge policy is for the same reason a
no-op. Both were deleted.

**Deduction happens once, after the callable is settled, and finding that took a crash.**
Selection reaches an answer by arity alone as readily as by type — `one( T )` beside
`one( T, T )` needs no argument typed — and a candidate chosen that way still has its
parameters to deduce. Putting the deduction inside either branch left the other substituting an
unbound `T`, which asserts. One place deduces, whichever way the callable was arrived at, and
the parallel vector of per-candidate answers that selection used to hand back was deleted once
mutation showed it could not be observed.

**The expectation belongs to the node it was written for.** `check()` sets it, and a call
**takes it and clears it**, so in `f( g() )` the type wanted of `f` says nothing about what `g`
should produce — only ever right by accident. The visible consequence is stated rather than
hidden: a generic whose parameter appears only in its result cannot be an argument to another
generic call without writing its type arguments, and the message says which parameter had
nothing to read.

**A constructor deduces against the aggregate, not against itself.** It returns nothing and
writes through `this`, so what the expectation is matched against is the open `Box<T>` — which
makes `Box<i32> b = Box( 7 );` read its instance from the expectation exactly as slice E made
`Box { 7 }` do. One rule, two spellings.

**A bound is a promise however the type argument was arrived at**, so `check_bounds` now takes
a span rather than a written annotation: a deduced argument has nothing written to underline,
and the argument that *chose* the type stands in its place.

**Three messages, one per way it can fail.** Nothing said what a parameter is — which names the
parameter and the explicit form. Two arguments disagreed — which names both types, and reports
the *first* parameter as declared when two disagree at once, so the message does not depend on
how a map hashed. And more than one generic matched, where the help names the type-argument
form, because when both fit exactly no argument can say which was meant.

**The list is all-or-nothing.** One parameter left open sends the whole call to the explicit
form, because a partial list is where C++'s left-to-right-up-to-the-last-deducible rule comes
from and nobody can recite it.

**What the tests found.** One **real defect**, and it was in slice 3's code rather than this
one: `check_call_arguments` skipped an argument that selection had already typed, on the
grounds that selection had checked it — true of selection, false of deduction, which *reads* an
argument without judging it. Every deduced call was therefore skipping its argument type check
entirely, and `unbox( n )` on an `i32` compiled silently. The skip now means "do not type it
again", and an argument that was typed elsewhere is still checked. A second, latent: a
candidate list in a diagnostic could hold a generic nothing had instantiated, and substituting
a `T` with no binding asserts — reachable before this slice and reachable more often after it.

**24 mutations, 23 killed.** The survivor is the probe's merge policy, which is provably a
no-op for the reason above. Four earlier survivors were not gaps but dead code and were
deleted, which is the better outcome. `codegen/inference.kl` runs seventeen shapes and returns
42; `sema/errors_inference.kl` holds thirteen refusals. **No existing golden changed** except
the one case that had to move — `return id( 1 );`, which the row's own worked example says
should now compile — and that is the proof the slice needed: a deduced call reaches
instantiation, mangling and emission as the same call a written list produces.

## M6.5 — in progress

### The two KIR passes — done (2026-09-18)

`ir/simplify.{h,cpp}`, one exported free function — `void simplify( Function&, const Literal_pool& )` —
run per function in the driver immediately after `lower()` and before anything reads the graph.
Three steps in one sweep: fold a `Branch` whose condition is a constant into a `Goto`, thread every
edge through blocks that carry no statements and end in a `Goto`, then delete what is no longer
reachable and rebuild `blocks` and `statements` together.

**Why the two halves are one pass.** The debt entries treat them as separate, and they are not.
Folding in the lowerer cannot work, because `break_target()` allocates a loop's exit block *before*
the branch is terminated — so a folded `while( true )` still leaves that block behind with nothing
reaching it, and `verify` rejects an unreachable block. Folding therefore forces pruning, pruning
renumbers blocks, and renumbering is only expressible over a finished graph. One pass falls out of
that rather than being a preference.

**One sweep is the fixpoint.** Folding is what turns a loop header into an empty `Goto`, so it runs
first; threading cannot turn a `Goto` back into a `Branch`, and pruning creates neither. A test
runs the pass twice and requires the second to change nothing, which is the only way that claim
stays true as the pass grows.

**The empty-goto cycle has no special case.** `for( ; ; ) { }` is a ring of blocks that all thread
to one another. `final_target` walks with a visited set and stops when it comes back round, which
resolves the ring to a deliberate `bb1: goto bb1` — an infinite loop the verifier accepts, the
assignment check walks, and C spells directly. The PLAN entry asked for a guard against infinite
substitution; the guard is the visited set, and the self-goto it produces is the right answer
rather than the thing being avoided.

**Statements are rebuilt, not left with holes.** Two things scan `Function::statements` end to end
— whether a function assigns its return slot anywhere, and which locals need drop flags — so a
pruned block's statements sitting in the middle of the vector would answer both with code that
cannot run. The rebuild is the shape drop elaboration's `rewrite_statements` already uses.

**What broke, and it was in the backend.** C declares every local at the top of the function, so a
temporary that only the pruned block mentioned would be declared and never used — which `-Wall`
reports and the golden runner builds with `-Werror`. **A valid Keel program would have failed to
compile.** `mentioned_locals` in `emit_kir.cpp` is the fix: the declaration loop skips a local no
surviving statement, operand or terminator names, beside the parameter and void skips it already
had. Locals are deliberately **not** renumbered in KIR — that would mean rewriting every `Place`,
and the dump showing a local nothing uses is honest rather than wrong.

**The consequence to know about.** The pass runs before the flow-sensitive checks, so a body under
a constant-false condition is no longer move-checked or definite-assignment-checked. It is still
type-checked — that happens on the AST. The alternative was to teach `check_assignment`'s
`successors()` to skip a constant branch's untaken edge and leave the graph alone, which was
rejected for giving the compiler two different answers to "what is reachable".

**What the tests found.** The pinned false positive did its job: `assign_check_reports_a_constantly_
true_loop` was written *"so that the day constant branches are folded this test says so rather than
the behaviour changing quietly"*, and it failed on the first run after the pass was wired into that
harness. It is now `assign_check_accepts_a_constantly_true_loop`. `never_leaves_the_loop` moved out
of `sema/errors_returns.kl` into `codegen/loops.kl`, where it is run rather than merely accepted —
the same move `return id( 1 );` made in slice 4.

**13 mutations, 12 killed.** The survivor is `mentioned_locals` visiting terminator conditions,
which is unkillable because the temporary a condition reads is assigned in the same block — kept
anyway, because the helper's contract is every local the emitter *can* name. One earlier survivor
was a forced `mentioned[k_return_slot]`, deleted rather than kept: a non-void function that never
assigns its return slot is refused before emission, and a void one is skipped as void.
**49 goldens changed and no `.run` or `.exit` file did** — the emitted C lost 910 `goto`s and kept
436, and every codegen fixture still returns its own answer, which is the proof the output is
different and still correct.

### The empty-aggregate rejection — done (2026-09-19)

`Checker::check_aggregate_has_fields( Node_id )`, called from `check_aggregate_members` before its
`k_member_kinds` loop. **Not a pass of its own**, unlike `check_enum_payloads` next door: that one
is separate because `is_owning_type` cannot answer before `compute_owning` has run, and this one
reads node kinds and nothing else — no types, no `struct_order_`, no `owning_` — so nothing forced a
third walk over the root's children. Diagnostic at `ast_.span( decl )`, which the renderer collapses
to the declaration's first line, the same shape `report_containment_cycle` already uses.

**The predicate is fields, not members, and §12 had it wrong.** The entry said "an aggregate with no
fields" and read as though the member list being empty were the same question. It is not: a
method-only class emits `struct kl__Marker { };`, byte for byte the same empty C struct, and so does
one carrying only a constructor and destructor. Both are refused. A generic is caught once at its
declaration rather than per instantiation, because the loop is over the root's children and
`struct Box<T> { };` is empty for every `T`.

**The runtime guard was kept, against this document's own instruction.** §12 and the debt entry both
said `kl_rt_alloc`'s zero-size guard could go once the case was unreachable, and the case *is*
unreachable — verified rather than assumed: `alloc<T>()` emits `sizeof` of one element with no count
operand, and a payload-free `enum` lowers to `int32_t`, so neither reaches zero. The guard stayed
anyway, on a ground the entry had not considered: `malloc( 0 )` is implementation-defined, `NULL` is
how failure is reported, and pinning that to a one-byte allocation costs a single branch while
leaving it open costs a case no test can reach. Its comment no longer cites `struct Empty { };`,
which the compiler now rejects.

**One test died with the construct.** `"an empty struct takes an empty literal"` asserted that
`auto q = Empty { };` type-checked cleanly. The program is no longer legal, so the test was dead
rather than failing, and it was deleted — the same call the surviving-mutant rule makes. Worth
noting what it had been hiding: before the rejection, `Empty e = Empty { };` reported *"this value
is used before it is initialised"*, because an empty struct literal never registered as an
initialiser. A real bug, made unreachable by this slice rather than fixed by it.

**4 mutations, 4 killed**, and the counts pin which test does the work: restricting the check to
`Struct_decl` kills 3 sections — exactly the three class cases; testing any member rather than a
`Field_decl` kills 2 — the method-only and lifecycle-only pair; dropping the call kills all 5
positives; requiring two fields kills 47 unit cases and 49 goldens, which is blunt but proves the
single-field negative is load-bearing. The golden carries three negatives that must stay silent — a
one-variant `enum`, a payload-free variant beside a payload-carrying one, and a one-field struct —
because the first of them is what the diagnostic recommends, and a fixture that does not prove the
recommendation legal does not test the message.

**A hole found while testing it**, now a §12 row: `enum Nothing { };` type-checks silently and
lowers to `int32_t`. Not zero-size, so not this slice's business, and nobody has decided it.


### The conditional expression — done (2026-09-19)

`?:`, decided and built in one slice. The decision half was the point of putting it in M6.5 at all:
it appeared in no Law, no D-entry and no exclusion list, while D33 already named it among the
operators that may not be overloaded — a feature the compiler did not have, planned around in
writing.

**What shipped is `?:` and not `if`-as-expression.** The second is the more interesting option and
it is not a larger version of the first: §12's own argument for it drags `switch` along, and a
`switch` arm is a block with `case` labels rather than an expression, so it is a two-construct
change. It is recorded as M8's, not dropped.

**The type rule is the content, and it is two rules.** `Checker::check` gained a `Conditional_expr`
case beside `Binary_expr`'s, which pushes the expectation into both arms; `Checker::infer` gained
`infer_conditional`, which requires the arms to match exactly. `Type_table::common` is not
consulted from either, so `u8` beside `i64` is refused although §6.4 would widen them — the same
promise the overloading slice made, kept in a second place: widening happens after a type is
settled, never to settle one.

**Lowering is `lower_short_circuit` with the asymmetry removed** — one result local, two arms, one
join — and the three things that template does not cover are where the work actually was.

- **An arm needs converting.** An expectation reaches an arm without retyping it, so `i64 x = c ? a
  : b;` leaves both arms `i32` while the conditional is `i64`, and the assignment would write an
  `i32` into an `i64` local. The same `converted` call an owning struct literal already makes for
  its fields, and for the same reason.
- **An owning arm needs two separate things, reached by two different programs.** `moved_if_owning`
  only converts a *copy*, and `move a` already arrives as a move — so only an arm that **builds**
  its value, `c ? Owner( 1 ) : Owner( 2 )`, exercises it. Registering the result as a statement
  temporary only matters when nothing moves out of it, which means `( c ? move a : move b ).t`.
  Mutation testing is what separated these: copying instead of moving survived the first suite,
  because every owning fixture written by then used an explicit `move`.
- **A conditional is a place after all.** `( c ? a : b ).t` routes its base through `lower_place`,
  which had no case and aborted the compiler on valid Keel. A value-returning call in the same
  position already gets its temporary's place; the conditional gets the same answer, which is four
  lines. The walkthrough for this slice said no case was needed and was wrong.

**Parsing is one binding power and one branch.** `Question` returns 5, below `||`'s 10, so no
existing power moved; the else arm parses at `power` rather than `power + 1`, which is the whole of
right-associativity. The middle arm is delimited by `?` and `:` and so parses from zero. **D16
needed no edit**: `classify` returns `Operator_class::Other` for `?`, which pairs with nothing, so
`a & b ? c : d` is not one of the mixes that demands parentheses — pinned as a negative test rather
than assumed.

**Eight mutations, eight killed** — after the survivor above was paid with the missing fixture.
Each was killed by both suites rather than one.

**A hole found while testing it**, now a §12 row: `g().t = 1;` type-checks, and assigns into a
temporary that is then discarded. Pre-existing — the conditional inherits it by being given the
same place handling — and out of this slice's scope.

## M6.5 slice: splitting `sema/type_checker.cpp` (2026-09-19)

**16,678 lines became nine files, and the class stayed.** `Checker` is declared in
`sema/checker.h` and its 127 member definitions sit in `type_checker.cpp` (the spine) and eight
`check_*.cpp`, one per construct. The largest is `check_expressions.cpp` at 1,373 code lines,
under the 1,500 where Go's `types2` holds and well under M6.5's stated 3,000. 7,072 assertions in
602 test cases and 172 goldens pass unchanged, before and after, in debug and release.

**The plan this was scheduled under was wrong, and measuring is what showed it.** §3.2 asked for
free functions over `Ast` and `Type_table`. Propagating state use through `Checker`'s internal
call graph says that reaches **22 functions and 432 lines — 7%**. Another 64% touches transient
walk state, and removing any single member of that state from the class saves **zero** lines;
the best three-way combination saves 10%. There is no free-function escape from a body walk. The
full measurement, and what Go's `types2` was read for, was in `docs/TYPE_CHECKER_SPLIT.md`,
which is deleted now that the work has landed.

**The one hazard was linkage, and it cost a redesign of the first step.** Everything leaving the
anonymous namespace acquires external linkage, so it went into `keel::sema` rather than bare
`keel`. The step then failed on something I had predicted would work: `using namespace
keel::sema;` makes the names findable but **an out-of-class member definition must appear
lexically in a namespace enclosing the class**. So each file closes its anonymous namespace
before the first `Checker::` definition and opens `namespace sema`, which is now written into
`checker.h` so it is not rediscovered nine times.

**Two things the audit had not predicted, both found by the compiler.** The bound table:
`k_bounds` is read by five queries whose callers landed in three different files, so unlike every
other rule table it could not stay file-local — the five are declared in `checker.h` and defined
in `check_generics.cpp`. And the test fixture: all 162 `TEST_CASE`s shared one `Typed` class,
which became `sema/checker_test_support.h`, still in an anonymous namespace so each translation
unit gets its own.

**What made this safe was that it is a pure extraction.** The region between the first
`Checker::` definition and the namespace close contained nothing but definitions and blank lines
— checked before cutting, not after. Every line of the original file was then accounted for
against `HEAD` by content, so nothing could be silently dropped. `CMakeLists.txt` needed no edit:
`keelc/src/CMakeLists.txt:9` globs `*.cpp` with `CONFIGURE_DEPENDS`.

**And then it did not help, which is the part worth keeping.** Stage A met the acceptance
criterion and left the code no easier to read: the same "and this needs checking too" in every
function, now spread over nine files instead of one. The cut was along the grammar; the
entanglement is in the state. Re-measured by ownership rather than by construct, 22 of
`Checker`'s 26 fields are touched by twelve members or fewer and most by two or three — so the
class was never one entangled thing, it was fourteen small ones sharing a `this`. Splitting the
file could not fix that, because nothing stopped `check_literal` from reading `loop_depth_`.

The split plan had asserted "the walk state is irreducible" and that stands
corrected: the only experiment run was threading a field as a *parameter*, which measures
whether the walk can become free functions and not whether the state has an owner. Stage B is
rescoped to the fourteen classes (§3.2), with the eight `check_*.cpp` kept as the starting
neighbourhoods rather than reverted — eleven of the fourteen draw 75%+ of their lines from one
of them.

**Two corrections this forced elsewhere.** §2.3's "No inheritance. No virtual functions." was
being read as a statement about Keel and used to argue that a cycle between checker classes
could never be broken by an interface. It is a rule about the C++ subset this compiler is
written in; **Keel will have inheritance and dynamic dispatch**, and §2.3 now says so. The DAG
survives on its own merits — an interface that breaks a cycle hides it rather than removing it
— but it is a choice and not a constraint, and the difference was worth getting right.

### Stage B's close-out (2026-09-24)

**The fourteen classes landed, and §5.1's two claims came out one for two.** `Checker` is 33 lines
of `run()` plus a constructor; every rule lives in one of fourteen classes, one per file, and the
DAG is now checked mechanically rather than argued — **zero violations across all seventeen
classes**, including the three level-0 collaborators the design forced out. All nine `check_*.cpp`
and `checker.h` are deleted. That claim is met.

**`Expressions` did not come down — it went up.** §5 booked row 10 at 1,288 lines; it moved 1,564
and stands at 1,698 code lines, still the largest thing in `sema/`. The row was not wrong about what
belongs together; it was wrong that what belongs together would fit. The two *files* it briefly had
were a defect and are gone.

**It does not owe a split, and this entry said it did (2026-09-24).** The ruling is the author's and
it is the one this document argues for everywhere else: the measure is whether a class serves one
purpose, not how many lines serving it takes. `Expressions` type-checks expressions and does nothing
else, so the booked row is a miss in the estimate rather than a debt in the code. A line count is
evidence to go looking with, never a reason on its own — the type-checker split happened because
`Checker` held fourteen unrelated responsibilities, and the count was how that was noticed, not what
made it true.

### The close-out pass (2026-09-24)

Four things after B14, each verified and each its own commit.

**One class in one file is a self-hosting constraint, not a preference, and it cost nothing to
obey.** `expressions_calls.cpp` merged back into `expressions.cpp`: 4,013 lines, 1,698 of code.
The proof was a sorted-line diff of the two originals against the merge — the only lines lost were
the second file's own scaffolding, three braces and its includes and namespace markers, and not one
line of code or test. 7,263/653 and 172 goldens, unchanged either side. The rule is now real
everywhere in `sema/`: every class could be written in the language this compiler compiles.

**The one defect Stage B recorded was real, was in a different place than recorded, and had a worse
one underneath it.** B9 filed "a method candidate list names `T` where the author wrote `Box<i32>`"
and argued a mutant equivalent on the strength of it. Reproduced at close-out, the *method* half
renders correctly — the receiver's bindings reach it. The half that does not is the *constructor*
list, which `Overloads::viable_overloads` and `select_overload` ask for with no bindings at all,
because at selection time nothing has deduced any. That is not an oversight: `deduce_for_candidate`
refuses the expectation on purpose, since choosing a callable by what its result is wanted for is
selection by return type, which §12 forbids. **A construction is not that case.** A constructor's
type parameters are the *aggregate's*, and `Box<i32> b = Box( 7, true );` names the instance in the
same breath as the call — which is the receiver's role in a method call, not a return type. So the
instance is now threaded into both, and the three candidate-list sites read it.

**Fixing the message exposed the defect it was hiding.** With the list spelled correctly, the
diagnostic contradicted itself: `no `Box` matches these arguments / the ones declared take
`( i32, u8 )` and `( i32, bool )`` — for the call `Box( 7, true )`, which the second one takes
exactly. Selection could not match *any* constructor of a generic aggregate whose parameters
mention `T`, because a literal cannot deduce `T` and nothing else was allowed to. **A valid program
was being rejected, and only the wrong wording of the rejection had kept it hidden.** The same
instance fixes it: selection binds the aggregate's parameters from the instance and lets what the
call wrote or the arguments deduced win over it.

**Nine mutations, eight killed.** Three of the nine were gaps rather than kills on the first run —
two candidate-list sites with no test between them, and one that was worse than a gap: removing the
guard that keeps a *free function's* candidates out of the expectation's bindings makes the
compiler **abort** in `Type_table::substitute`, and 655 cases and 172 goldens had nothing to say
about it. `make`'s `T` is not `Box`'s, and binding one by the other leaves a parameter that a later
substitution asserts on. Four `TEST_CASE`s close all three. The survivor — the instance overwriting
what the call wrote instead of filling behind it — is observable only in programs the "a type
argument could make the two identical" rule already rejects; probed, not argued, after B11's lesson
that arguing is how a real gap gets filed as a non-gap.

**A placement and naming pass over `sema/`, and it found little, which is the point.** Four changes.
`Bounds::bound_set_contains` was a `const` member that read no member state — a bit test on a `u8` —
and is now a free `contains` beside the set's `operator|=`; `Aggregates::has_destructor` only hid
the free `keel::has_destructor`, and both are the same lesson B7 recorded about `parameter_mode`.
`Signatures`' six `declare_*_decls` lost the suffix that made them read "declare declarations", and
`Operators::result_of` became `result_of_binary` beside `result_of_unary` and `result_of_conditional`.
Nothing else moved: the two members that look like forwarders, `bindings_of` and `field_type`,
bundle `Types_builder`'s state into the question and earn their place.

**Eight members that only shortened a call are gone.** `Expressions`, `Statements`, `Signatures`
and `Coverage` each carried a private `record` and `error_at` forwarding one line to `Types_builder`
and `Reporter`. All eight are deleted and their 188 call sites now name the collaborator they
reach — 97 `reporter_.error_at` and 91 `types_.record` — which is what the DAG wanted said out loud.
The same lesson as `bound_set_contains` and `has_destructor`, at a larger scale: a member that adds
no behaviour hides an edge. Two comments carried real design information that would have died with
the declarations and were moved to where they are still true: that every branch of `infer` and
`check` ends in a recorded type, including the error branches, and that a pattern's bindings are the
one thing `Coverage` writes a type for.

**The comment pass found the density already right and the references already wrong.** 1,455 comment
lines against 7,927 of code, 18%, with no file out of line and only two comments in the whole of
`sema/` restating the code beneath them. What it did find: **eleven comments citing
`TYPE_CHECKER_SPLIT` by section number, in a document that was about to be deleted.** All
eleven now state the rule instead — chiefly §4.3's, that no class below the expression walk may
re-enter it — so nothing in `keelc/src/` names that file any more and deleting it breaks nothing.
Two more cited a Stage B step number, which is process rather than rule, and say the rule now.

**A rule with no D-entry behind it.** Linking implementations to the plan turned up one that cannot
be linked: by-value containment cycles between aggregates — `struct A { B b; }` and
`struct B { A a; }` — are computed and reported by `Aggregates::order_structs` and no D-entry
covers them. D42 is the *generic* instantiation cycle, a different rule. Recorded rather than
cited, because a wrong citation is worse than none.

**The tests got simpler for a third of the classes and not at all for the rest.** *Corrected at
the close-out: the count booked at B14 said "35 narrow, and 30 of those predate Stage B", which was
a miscount — the 30 `Resolved` cases are a separate pre-existing group, not part of the 35. Stage B
produced 35 narrow cases, not five. The measurement below is the recount, and it is kinder to the
split than the one it replaces.* Of **268 `TEST_CASE`s in `sema/`, 181 still construct a whole
front end through `Typed`** to ask about one rule. The other 87: 30 are `resolver.cpp`'s `Resolved`
and 22 are `type.cpp`'s, both of which predate Stage B; **35 are Stage B's own** — five on
`Aggregates`' `Shapes`, and thirty that build their subject by hand and name no fixture at all
(`Annotations` eleven, `Bounds` ten, `Operators` three, `Reporter` three, `Types_builder` three).
Those thirty are the strongest form of what the split was for: a class constructed directly and
asked a question, with no source string anywhere.

**But the pattern in which classes got them is the finding.** Every one of the five is a class that
takes types and spans rather than nodes. Not one of `Expressions`, `Statements`, `Signatures`,
`Coverage`, `Overloads`, `Places`, `Literals` or `Constant_folder` has a single narrow case — the
walk and everything that reports through it still needs the whole front end to say anything. **So
the DAG predicts testability exactly**: a class is narrowly testable when, and only when, it sits
where a service sits. Splitting the file was a necessary condition for that and not a sufficient
one, and the two-thirds that still reach for `Typed` are the two-thirds that were never going to
stop.

What the split also bought: mutation testing became possible per class rather than per
compiler, and it paid immediately — every step from B10 on found real gaps, and B13 found three in
one class. That was not a stated goal and is the better half of the outcome.

`parse/parser.cpp` is now the largest file in the tree at 2,786 code lines and has had no
equivalent audit; `Parser` is not `Checker`, a recursive-descent parser's state really is a
cursor and a token, and the measurement has to be redone before anything is assumed.

### M6.5's four remaining items, re-measured (2026-09-24)

All four were probed against the current tree rather than carried forward on the plan's word, and
**one of them is worse than this document says it is.**

**The mangling category tag is not latent — it is a miscompile shipping today.** The entry above
schedules it *"because a static method is the first scheme with no receiver to tell it apart"*,
which reads as a hole that opens at M7. It is already open. `Spelling::function` mangles a method
through `mangle_function` with the receiver as parameter 0, so a method `at` on a `Box` and a free
function `i32 at( Box b, i32 i )` both encode as `kl__at__3Box_3i32`. Nothing in `sema/` can catch
it: they are different signatures by every rule the checker applies, and the collision exists only
in the emitted name. The program compiles, emits two prototypes of one symbol with different
parameter types, and **`cc` rejects it** — *"conflicting types for `kl__at__3Box_3i32`"* — which
surfaces to the author as a C error against generated source, exactly what §2.2 says must never
happen. **Both spellings collide, and the second is the worse of the two.** The first probe
suggested the `ref` form escaped, because a borrow carries `R` — but the receiver of a *plain*
method is a `ref T`, so it carries `R` too, and `void bump( ref Box, i32 )` beside a method
`bump( i32 )` is the same symbol again. That pair is worse: their C prototypes are *identical*, so
it is a
plain redefinition rather than a conflict, and identical prototypes in two translation units are
what a linker resolves silently by picking one. The two halves are the trailing `const`: a const
method's receiver is a `const ref T`, which takes no marker and collides with the by-value free
function. A category tag fixes both at once, and it is the only one of the four that is a wrong
answer rather than a missing rule.

**The other three are as recorded.** `enum Nothing { };` type-checks silently and lowers to
`int32_t`. `g().t = 1;` and `( c ? a : b ).t = 1;` both type-check and both assign into a discarded
temporary — probed together, since the conditional inherits the hole rather than adding one.
The `Arena` has **zero call sites** outside `common/arena.{h,cpp}`, still 393 lines and a suite.

**The shape the fix takes.** A marker on the receiver would not do: a static method has no receiver,
so M7's case would stay open, and the marker is defined as what a call site *writes* for a
parameter. The tag belongs to the name instead. The receiver leaves the parameter list — the checker
already proves it cannot distinguish two members, since `Overloads::parameters_collide` compares a
member's parameters from index 1 and refuses `area()` beside `area() const` — and the enclosing type
takes its place as a leading `S<type>` in the argtypes, where every token is already length-prefixed
and injective and no encoded type begins with a letter. A member `at( i32 ) const` on `Box` becomes
`kl__at__S3Box_3i32`, the free `at( Box, i32 )` stays `kl__at__3Box_3i32`, and `at( ref Box, i32 )`
stays `kl__at__R3Box_3i32`. No free function changes name, a generic member carries its instance
through the substituted receiver type, and M7's static method is told apart by the same `S` with no
receiver to borrow it from. Qualifying the *name* instead — `kl__Box__at__3i32`, beside `__ctor` and
`__dtor` — reads better and was rejected: a free function legitimately called `Box__at` would then
collide, which is a new reachable hole rather than the theoretical one those two already carry.

**The red test is in the tree first**: `keelc/test/codegen/member_and_function_one_name.kl`, a valid
program holding both pairs, each answering a different number so that a wrong pick is an exit code
rather than a link-time coincidence. Today it fails with `cc` reporting the conflict and the
redefinition; its `.expected` is deliberately absent until the fix lands, because a golden recorded
from broken behaviour is worse than no golden.

**Order, and why.** ~~The tag goes first~~ **the tag is done (2026-09-24)**, see the slice below: it
is the live defect, and M7 adds the declaration form that makes it unavoidable, so paying it before
M7 is cheaper than paying it inside M7 — the same argument that moved the type-checker split ahead
of M7. ~~Then `enum Nothing { };`, which is one predicate beside
`Signatures::check_aggregate_has_fields`~~ **done (2026-09-24)**, and it is one predicate *beside*
that function rather than inside it — see the slice below. Then the temporary-assignment rule, which
is a question for `Places::is_assignable`. ~~The `Arena` last, being a decision with no code depending
on the answer.~~ **Done (2026-09-24)**, and last it was — see the slice below.

### M6.5 slice: the mangling category tag (2026-09-24)

**Done.** A member's symbol now leads its argtypes with `S` and the encoded enclosing type, and the
receiver is no longer among them. `mangle_function` gained a trailing `Type_id enclosing`, invalid
for a free function; the file-local `construct_arg_string` seeds its result with the tag before the
parameter loop, so the existing `_` join places it for free. `Spelling::function` grew one branch on
`Node_kind::Method_decl` that reads `params.front().type` — the receiver, already substituted by the
loop above it, so the tag is `Box<i32>` and not `Box<T>` — and passes the parameters from index 1.
Constructors were left exactly as they were: `__ctor` already keeps them out of a free function's
way, and moving them would have put a second topic in the commit.

**173 goldens pass, none fail**, and four fixtures' expectations moved. Every changed line carries
an `S`: no free function, no struct, no constructor and no destructor changed name, which was the
property the placement was chosen for. The new fixture emits four distinct symbols where it emitted
two.

**Five mutations, five killed.** Removing the `S` character, spelling the tag with `base_name`
instead of `encode_type`, leaving the receiver in the parameters, never taking the `Method_decl`
branch, and reading the receiver's declared type rather than the substituted one. Worth recording
that only the first two are killed by a unit test — the other three live in `Spelling::function`,
which has no unit test of its own and is covered by the goldens alone.

**Two wrong turns on the way, both caught before the commit.** The enclosing type was first read
from child 0 of the method declaration, which is the return type rather than the parameter list:
that compiled, indexed an empty span, left the id invalid and changed no emitted symbol at all — the
suite stayed exactly as red as it had been. And the tag first sat between two `__` fences after the
name, as `kl__at__SBox__3i32`, which is precisely what a free function legitimately named `at__SBox`
mangles to. Inside the argtypes there is no fence to imitate, which is the whole reason it goes
there.

### M6.5 slice: `enum Nothing { };` (2026-09-24)

**Done.** `Signatures::check_enum_has_variants` reports an `enum` whose variant list is empty, and
`enum Nothing { };` now exits 1 where it used to compile and lower to `int32_t`. The §12 row that
found the hole is closed; **uninhabited types remain undecided**, which was the point of rejecting
rather than assigning a meaning.

**Why it was a hole and not a permission.** `check_aggregate_members` walks the root and `continue`s
on anything `is_aggregate` refuses, and that predicate is `Struct_decl || Class_decl`. An enum was
never *permitted* by the empty-aggregate rule; it was never visited by it. So the fix is a pass of
its own beside `check_aggregate_has_fields` rather than a widening of that function, and it sits
next to `check_enum_payloads`, which the file already establishes as the shape an enum rule takes
here. Diagnostic order did not constrain the placement: `Diagnostics` sorts by span, so where in
`declare()` the call goes cannot move a golden line.

**The predicate is `ast_.variants( decl ).empty()` and nothing more.** An enum can hold nothing but
variants — `parse_enum_decl` pushes only `parse_variant_decl()` results — so no loop-and-flag is
needed, unlike the aggregate rule, which has to sift `Field_decl` out of methods and lifecycle
members. The two fixed slots `Ast::variants` skips are what makes it exact: child 0 is the type
parameter list and child 1 the underlying type, both always present and both irrelevant to whether
the type has any values. A malformed body leaves an `Error` node among the variants, so the rule
stays quiet on a program that already failed to parse, which is the behaviour wanted rather than a
gap.

**The message says something the aggregate one does not.** *`Nothing` has no variants, so it has no
values*, against *has no fields, so it has no size*. Reusing the second would have been exactly the
conflation this rule exists to undo — the two look alike and are not. The remedy is *add a variant,
or delete the type*, and it names both ways out because there is no third to recommend: an enum with
one variant is what the *aggregate* diagnostic points at, so pointing there from here would be
circular.

**`keelc/test/sema/errors_empty_enums.kl`**, its own fixture rather than four declarations added to
`errors_empty_aggregates.kl`, on the same ground the message is different. Four declarations: the
bare `Nothing`, a `Tagged : u8` whose underlying type is written, a generic `Empty<T>`, and a
one-variant `Unit` that must stay legal. 174 goldens, 0 failed, and the `--update` produced exactly
one new file — **no existing fixture moved**, which is the evidence that nothing in the suite had a
variant-less enum quietly compiling.

**Four mutations, four killed** — all by the golden, because nothing in `keelc/src/sema/` unit-tests
`Signatures` and this rule inherits that. Deleting the `check_enum_has_variants()` call from
`declare()` reproduces the original hole and fails the new fixture alone, which is what makes the
fixture a genuine red test rather than a golden recorded after the fact. Inverting the predicate
fails 21 fixtures, every one holding a real enum. Dropping the `Enum_decl` guard fails 133, since
`Ast::variants` asserts on anything else.

**One walkthrough claim was wrong and the mutation caught it.** The prediction was that swapping
`variants` for `children` would keep `Nothing` reporting and silence `Tagged` only. It silences all
three: `children` always holds the two fixed slots, so it is never empty and the diagnostic vanishes
entirely. The correction matters because it means **`Tagged` kills no mutation `Nothing` does not**.
It stays in the fixture as documentation — an underlying type does not make an enum inhabited — and
that is what it is worth, not extra coverage.

### M6.5 slice: writes through a call's result (2026-09-24)

Scheduled as one debt — `g().t = 1;` assigns into a discarded temporary — and it turned out to be
two bugs sharing one line. Arguing the walkthrough is what found the second, and the second is the
worse one.

**The shared cause.** `Places::place_root` resolved a root only when the walk landed on a
`Name_expr`, so a place rooted in a *call* came back as an invalid `Node_id`. `check_writable` opens
with `if( !root.is_valid() ) { return true; }`, meant for a pointer projection — what a pointer
points at was never part of the borrowed object — and that guard read the call's invalid id as
permission too. Everything below it was skipped: `const`, borrow, and pattern binding alike.

**The debt that was scheduled.** A by-value call, a conditional and a struct literal all produce a
value with no storage, so a write into one is discarded. The conditional inherited it when `?:`
shipped, since a conditional's result gets a temporary's place even when both arms are places. The
struct literal was not a silent acceptance at all: `P { 1 }.t = 2;` passed sema and then **aborted**
in `lower_place`, which had no place to project from. Refusing it in sema removes a crash.

**The bug that was not.** §8 permits only `const ref` returns, so every reference a call hands back
is read-only — and writing through one was accepted *and took effect*. `by_ref( a ).t = 99;` emits
`( *kl_t4 ).kl_t_1 = 99;` and the program exits 99 instead of 7, verified by compiling and running
it. The same hole runs through a `Method_decl`: a `const` method returning `const ref` let a caller
write the object the method itself was forbidden to touch, also verified at 99. The same write
spelled through a *named* `const ref` binding was already refused, so one fact had two spellings and
two answers — the tell that the rule was never reached rather than deliberately relaxed. This is a
soundness hole, not a useless one, and it is recorded in §12 as its own row rather than as a
footnote to the temporary.

**The discriminator is `returns_a_binding`, not `is_const_binding`.** `const P f()` parses, and
`is_const_binding` accepts a `Function_decl` and reads child 0 as a return annotation — so keying on
constness would call a `const`-returning *by-value* function a read-only reference.
`returns_a_binding` answers false for it. **The goldens do not catch this**: swapping the two leaves
the suite at 176/0 and is killed only by a unit test, which is why one exists for it.

**Shape.** `place_source` was extracted out of `place_root` — the `Field_expr` walk and its pointer
bail, returning the expression it lands on — and `check_writable` now asks it first and classifies
the landing before falling through to the root question. One walk, two callers, so the pointer bail
cannot get out of sync. The extraction step was not behaviour-neutral as committed: `place_source`
returns an invalid id at the bail and `place_root` fed it straight to `ast_.kind`, which asserts.
`p.t = 5;` — the bare reach-through spelling, which is the one D22 makes idiomatic — aborted the
compiler, and the suite caught it at 171/3. A validity guard fixed it.

**The stale test.** `keelc/src/sema/expressions.cpp` held a section named *a field of a temporary is
still assignable*, whose comment held that refusing it "would need value categories, which v0 does
not have". That was the decision this slice reverses, and it was wrong about the cost: naming what
the place came from was enough, and no value categories were added. It was deleted rather than
amended, and replaced by two cases in `places.cpp`, which is where the rule emits.

**Results.** 176/0 and 7300 assertions in 659 test cases. `--update` wrote exactly two new `.stderr`
files and **moved no existing fixture**, which is the evidence that nothing in the suite depended on
either hole. Six mutations, six killed: dropping the pointer bail 173/3; `is_const_binding` for
`returns_a_binding` 176/0 but one unit failure; dropping the `Conditional_expr` landing 175/1;
dropping the `Struct_literal` landing 175/1; making a call never a temporary 175/1; deleting the
block outright 174/2 and six unit failures, which reproduces the original compiler.

**Left open.** The `ref i32 r = by_value().t;` case is refused with the temporary's message, and its
real objection is lifetime rather than uselessness — `Statements::visit_var_decl` already has a
separate message for `ref A r = g();` one level up. Worth one message rather than two if the
distinction proves to matter.


### M6.5 slice: the `Arena`'s decision (2026-09-24)

**Deleted.** `common/arena.h` and `common/arena.cpp` are gone, and with them the last item on M6.5's
second list. Nothing else in `keelc/src` changed, because nothing else ever named them.

**The 393 lines were not 393 lines of asset.** `arena.cpp` is 344 lines of which only 1-84 are the
implementation; 85-344 are twelve `TEST_CASE`s behind `ENABLE_UNIT_TESTS`. With the 49-line header
that is ~133 lines of code carrying 260 lines of tests. A third of the public surface was
`bytes_used()`, which with zero call sites could only ever have had the suite as its reader.

**Three predictions, three falsified.** §15 has predicted a caller three times. Monomorphised AST
clones - M6 used a bindings map instead. KIR payloads - they went to `std::vector` on `Function`.
The type-checker split - the 2026-09-19 note in this section had already checked and recorded that
an allocator changes none of what blocked it. That is not a caller running late.

**The third candidate was measured, not argued.** The live proposal was the `Interner` holding its
strings in the arena instead of the map's keys, deleting `Sv_hash` and the `std::equal_to<>`. The
claim it rested on was "one heap allocation per symbol", and that is not what happens: most
identifiers fit the SSO buffer and never allocate, and `unordered_map` allocates its node either
way. Across the whole golden corpus plus `examples/` there are 2,546 distinct identifiers, of which
**130 exceed fifteen characters - 5%**. One compile interns far fewer: the largest fixture,
`codegen/comparisons_mixed.kl`, has 238 distinct identifiers, so **about ten of them allocate at
all**. The entry's own words were "it is not a bottleneck"; the measurement says that was generous.

**The decisive argument is shape, not size.** Even granting the wiring, the arena an `Interner`
wants is not this one. This is a node arena: `allocate( size, align )` with power-of-two alignment
assertions, `align_up`, the `+ align` slack for `new[]`'s 16-byte guarantee, and `allocate_n<T>`
behind a trivially-destructible `static_assert`. All of that exists for the two callers that were
decided against, both of which allocate *typed* objects. An `Interner` wants one function returning
a `std::string_view` into stable storage, alignment 1, no template. Wiring this class to that caller
would leave half its machinery permanently unexercised by its only user - which is how it got here.

**The stability argument is thinner than it looks.** `interner.h` documents that `texts_` holds
views into the map's keys and is correct because `unordered_map` is node-based. That is a real
dependency on a container guarantee, but it is one the standard makes, it is written down where it
matters, and nothing has come near breaking it. An arena would make it unconditional; it would not
make it correct, because it already is.

**The compiler confirmed the zero call sites rather than grep.** After deleting both files the build
**only relinked** - not one translation unit recompiled, so nothing had included the header. Goldens
stayed at **176 passed, 0 failed**, and no fixture moved. `keel_tests` went from 7300 assertions in
659 cases to **6234 in 647**: exactly the twelve arena cases, and nothing else.

**Two locked decisions were reopened, with the reason written down rather than edited away.** §4's
Memory row and §5's L3 both name the arena as policy. The half that was load-bearing survives
untouched - *never free*, and `u32` handles for IR-ish data - and the half that was deleted was
never what the compiler did: the AST is `std::vector<Node>` plus a flat `children_` array, types are
a vector, KIR payloads are vectors. The policy has been amended to say that, in both places.

**Three stale claims fell out of it.** `parse/parser.cpp` said an unreferenced node is "what an
arena-allocated tree costs" - the cost is real and the attribution was wrong, so it now reads
*append-only*. §3's pipeline diagram called the AST arena-allocated. And the `src/ir/` layout listed
a `kir.cpp` holding "accessors, arena"; there is no such file, the accessors are inline in `kir.h`,
and that line is now gone rather than left describing a file nobody wrote.

**The caveat §10 earned.** In-source Catch2 makes a suite cheap to write, and a suite is easy to
read as evidence that the thing under it is used. The `Arena`'s twelve cases outlived its last
caller by the entire project and were the main reason it kept looking like an asset. §10 now says to
test what the compiler calls.

**If the `Interner` change is ever wanted on its own merits** - one fewer allocation per long
identifier, one fewer template parameter on the map - it is thirty lines of purpose-built string
storage written against a caller that exists. That is cheaper than the review attention of carrying
a general allocator that has now outlived three predictions.


### Debts to pay along the way

- ~~**Static methods have no spelling.** Every method takes a receiver, so a function that belongs to
  a type but needs no object - a named constructor, a `from_bytes` - has nowhere to live. It is a
  separate shape rather than a missing keyword, and worth designing when the first one wants to
  exist rather than before.~~ **Scheduled as M7 (2026-09-19)**, and the old M7 became M8 to make
  room - see §9. The trigger this entry named has been met rather than guessed at: M8's acceptance
  is a `Vector<i32>` that grows, and `Vector::with_capacity` is the first line anyone writes toward
  one. The entry was right that it is a shape rather than a keyword, and the shape is now §12's
  question: D30 already spells the *call*, so what is open is how the declaration says "no
  receiver". Scope is methods only - type-scoped data waits on M8's modules, and internal linkage is
  the entry below rather than this one.

- ~~**Access control is still absent**, and methods make it visible: the first thing anyone writes is
  a getter, which only earns its place if the field is private. D29 says a class hides its
  representation and the compiler does not enforce it. It is a resolver feature - member lookup
  carrying visibility - and orthogonal to everything methods needed, which is why it was left.~~
  **Scheduled as M7 (2026-09-19)**, beside static methods. The entry called it orthogonal and that
  stopped being true when M7 got its customer: `Vector::with_capacity` is a named constructor, and
  a named constructor is only worth having if the ordinary one can be hidden — so the feature that
  justifies M7 is incomplete without this one. Two other things were already waiting on it and both
  now have a date: §12's member **data** pointers, which would otherwise be a hole in visibility the
  day visibility arrives, and internal linkage, which the static-methods entry above deferred here
  rather than into itself.


- **An enum cannot carry an owning payload.** D30 records the design - a synthesised destructor
  that switches on the tag, one generated function per owning enum, with every drop site unchanged
  - and the first cut refuses one instead. It is what `Result<Buffer, E>` needs, and it needs
  allocation to be worth having, so it is naturally M5.5's work rather than a loose end of M5's.

- **A payload enum is a struct of every variant's fields, not a union.** Correct and wasteful: a
  four-variant enum is as large as all four payloads together. Invisible today, because nothing
  guarantees an enum's layout and no FFI can see one; the fix is confined to `emit_enums` and the
  moment it matters is when one is put in a container.


- **`out` cannot take a type that owns a resource.** Assigning one destroys nothing, so it would
  leak whatever the caller already held. The fix is a conditional drop of the argument's place
  before the call, which drop flags already know how to emit - the machinery exists, nothing has
  wired it. Until then the diagnostic says why rather than pretending the mode does not exist.

- **Definite assignment is per local, so a struct `out` parameter may be left half-written.**
  `p.x = 1` satisfies it while `p.y` stays untouched. Per-field state fixes this and the
  neighbouring restriction that a field cannot be moved on its own; neither is worth it alone, and
  together they are one change.


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

- ~~**Three files have grown past what one file should hold.**~~ **The checker's half is paid
  (2026-09-19); `parse/parser.cpp` and `ir/lower.cpp` are not.** Code lines, excluding the
  in-source tests that roughly double each: `sema/type_checker.cpp` 2350, `parse/parser.cpp` 1675,
  `lex/lexer.cpp` 824. `ir/lower.cpp` at 768 is the one to watch.

  **Re-measured (2026-09-19), and the numbers are no longer in the same range**:
  `sema/type_checker.cpp` **7898**, `parse/parser.cpp` **2786**, `ir/lower.cpp` **2165**,
  `codegen_c/emit_kir.cpp` 1129, `lex/lexer.cpp` 823, `sema/resolver.cpp` 538. The checker has
  more than tripled and is now three times the next file; `lower.cpp`, flagged above as the one to
  watch, has also tripled. Two entries in this list were written as *"to be revisited after KIR
  lands"* — **KIR has landed**, so the condition is met and the deferral has expired. Note the
  shape of the growth matches the diagnosis rather than contradicting it: `lexer.cpp` and
  `resolver.cpp`, the two files that are not built around a file-local class holding every new
  responsibility, have barely moved.

  **Scheduled in M6.5 (2026-09-19), and before M7 rather than after.** M7 adds member-declaration
  rules to `type_checker.cpp` specifically, and splitting a file is cheaper before an addition than
  after it. M7 is also small enough to be a fair first test of whatever replaces the layout, which
  M8's library will not be.

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
  in before retrofitting it. ~~**To be revisited after KIR lands**, when there is evidence rather
  than preference. The likely shapes: split `type_checker.cpp` along its seams — annotation
  resolution, the operator tables, the constant folder — into free functions over `Ast` and
  `Type_table`; and split the parser by grammar section.~~ **Revisited, and the checker is split
  (2026-09-19).** The predicted shape was wrong twice over, which is the useful part of the entry.
  The obstacle was never that a class cannot span translation units — only a *file-local* one
  cannot — so the fix was a private header, not free functions. And the free-function shape it
  predicted was measured against the call graph and reaches **7% of the file**: 64% of it touches
  transient walk state that no combination of extractions reduces by more than 10%. What shipped
  keeps the class and splits its 127 definitions across nine files by construct, largest 1,373 code
  lines. `parse/parser.cpp` is unsplit and now the largest file in the tree; the same audit would
  have to be redone for it, because `Parser` is not `Checker` and the answer is not assumable.

- **Narrowing `cast` is blocked, not implemented.** D28 defines `cast` as checking the value at
  run time and trapping when it does not fit, and nothing in the pipeline can emit that check yet.
  Rather than let a narrowing `cast` silently truncate — which is `wrap`'s behaviour wearing
  `cast`'s name, exactly the silent wrong answer D5 exists to remove — the checker refuses it. The
  block is one named constant, `k_narrowing_cast_needs_a_run_time_check` in `sema/operators.cpp`,
  and the single branch that reads it; deleting both is the whole change once the KIR can trap.
  Nothing that compiles today changes meaning when it goes, because the cell is currently empty.
  The three tests under `type_checker_holds_back_a_narrowing_cast` go at the same time.

- `mangle_function` is not injective. Argument types are spelled with `Type_table::name()`, the
  plain Keel spelling, so `i32*` becomes `i32p` and a struct genuinely named `i32p` collides with
  it. Two separate improvements are wanted, and only the second is complete:

  **Re-checked (2026-09-19).** The second bullet is **done** — M6's slice 2b length-prefixes, so
  `add( i32, i32 )` is `kl__add__3i32_3i32` and a type's own encoded name is what appears. The
  first is **still open and still benign**: `struct foo__ { };` beside `i32 foo()` really does give
  `kl__foo__` twice, and the emitted C compiles and runs correctly anyway, because a struct tag and
  an ordinary identifier are different namespaces in C. So this is a latent hazard rather than a
  bug, and it stays latent only while every scheme that shares the ordinary-identifier namespace
  is distinguished by something else. **Methods are distinguished today and it is worth recording
  why**, since it is not this entry: a receiver encodes as `R1C` and a pointer parameter as `P1C`,
  so a method `C::get()` and a free `get( C* )` differ — the marker work from M6's slice 3, not the
  mangling scheme. A static method has *no* receiver to distinguish it, which is why M7 is the
  milestone that decides whether the category tag is finally owed. **Scheduled in M6.5
  (2026-09-19)**, one milestone earlier than that reasoning suggests, for the same reason as the
  file split: it is cheaper to fix a naming scheme before a third caller is written against it.
  - **A category tag** — `kl_struct_`, `kl_func_`, `kl_local_` — closes collisions *between* the
    three schemes, which exist today: `mangle_struct( "", "foo__" )` and
    `mangle_function( "", "foo", {} )` both give `kl__foo__`, and at M8 a module named `foo` makes
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
- ~~`Interner` should hold its strings in an `Arena` rather than in the map's keys.
  That deletes `Sv_hash` and `std::equal_to<>` — the lookup type becomes the key
  type again — and drops one heap allocation per symbol. Deferred: it is not a
  bottleneck and the API does not change.~~ **Measured and dropped (2026-09-24)**, and
  it took the `Arena` with it — see the slice below. "One heap allocation per symbol"
  was the overstatement: most identifiers fit the SSO buffer and never allocate, and the
  `unordered_map` node is allocated either way. Across the whole golden corpus 130 of
  2,546 distinct identifiers exceed fifteen characters, so a compile saves roughly ten
  `malloc`s. The two readability halves are still available without an allocator if they
  are ever wanted on their own: thirty lines of purpose-built string storage buys the
  same `Sv_hash` deletion.
- ~~The `Arena` still has no caller. It earns its place at M6 (monomorphised
  instances) or in KIR payloads, whichever arrives first.~~ **Both predicted callers have now been
  decided against, and it still has none (2026-09-19).** M6 did not clone the AST — §15's M6 entry
  records why: an instance needs no re-checking, so a bindings map does what a cloned tree would
  have, without copying, rewriting spans or growing the AST. KIR payloads went to `std::vector` on
  `Function`. So 344 lines and a test suite sit behind zero call sites, and the prediction that
  justified keeping them has been falsified twice rather than postponed. **Scheduled as a decision
  in M6.5**: either give it the one real caller still on the list — the `Interner` holding its
  strings in an arena, two entries above — or delete it. Note §3's *"arena-allocate, never free"*
  is already not what the compiler does, so deleting it costs a policy line that was aspirational
  rather than a mechanism anything depends on. **Deleted (2026-09-24)** — the third candidate was
  measured and did not pay either, and §3's Memory row and L3 were amended rather than left
  claiming a mechanism that is gone. See the slice below.

  **It would not help the file split above, and that was worth checking rather than assuming
  (2026-09-19).** What blocks splitting `type_checker.cpp` is that `Checker` is a *file-local*
  class, so the state has to become something a header can name before free functions in other
  translation units can take it — and that state is vectors, a hash set and a hash map, none of
  which an allocator changes. **The AST already has the half of L3 that was worth having**:
  `Ast` is `std::vector<Node>` plus one flat `children_` array read through `std::span`, which is
  arena layout with a vector as the store. Moving it onto the `Arena` would make a node hold a
  pointer plus count where it now holds an offset plus count, and would stop `add` appending at
  all, since arena memory cannot grow in place. **The one place it would read better argues
  against itself**: overload resolution threads `std::vector<Type_id>& resolved` and
  `std::vector<Node_id>& bound_by` as out-parameters — `deduce_type_arguments` takes ten
  parameters — and an arena would let those be returned as spans. But they are out-parameters
  *because they are reused*, once per candidate per call site, and an allocator that never frees
  cannot reuse; the nicest-reading case is the one where the policy costs most. **If those
  signatures are the actual complaint, the fix is a named scratch struct** holding both vectors,
  passed as one parameter — same readability, no allocator, and it survives the split because a
  header can declare it.
- The `unsafe` gate guards **two** operations — the pointer-to-pointer conversion
  and an `extern` call (D36). `alloc<T>` is not on the list, because §12 never
  said whether allocation itself needs one, and dereference is a decision still
  to take.
- **Two guarantees now rest on a promise rather than a proof, both through
  `extern` (D36).** `extern void init( out i32 v );` discharges definite
  assignment in a body that does not exist, so the caller's variable is believed
  initialised on the C function's word; and `move` on an extern parameter hands
  ownership to a function whose destructor Keel will never run. Both are what the
  `unsafe` at the call site is asserting, so neither needs a rule — but they are
  the first places where a Keel guarantee is an assumption, and they should be
  named when the runtime is reviewed.
- ~~**D9 is not implemented.**~~ **Done.** It is `check_assignment` generalised,
  not a condition added to `check_moves` — the note that used to stand here said
  the latter and was wrong. `check_moves`' `Uninitialised` is a *lattice bottom*
  meaning "no information yet", so `join( Uninitialised, Live )` is `Live`, which
  is exactly the wrong answer for a value assigned on one branch and not the
  other. D9 needs a **must** analysis, which is what `check_assignment` already
  is, and its transfer rules were already the right ones: `Address_of` counts as
  initialising (without it every constructor reports, since `C c = C( 3 )` writes
  through a pointer) and a projected write counts (without it every struct
  literal reports, since one is assembled entirely through projections). The pass
  now answers both halves of one question — whether a value exists by the time
  something reads it, and whether it exists by the time the function leaves.
  Reading an unwritten `out` parameter falls out for free and needed no extra
  rule.
- **Two known shallownesses in D9, both erring safe.** Taking a local's address
  counts as initialising it, so `i32 x; i32* q = &x; return x;` is accepted even
  though nothing wrote through `q` — the same shallowness `place_root` and
  drop-flag placement already have, and the thing that makes `out` forwarding
  work. And writing one field counts as initialising the whole struct, so
  `P p; p.x = 1; return p.y;` is accepted; closing that needs per-field state,
  which is a much larger lattice. Both are pinned in the tests as *accepted*, so
  the day either is closed a test says so.
- **Two guards in `check_assignment` are defensive and unexercised.** The
  reporting walk skips unreached blocks, and `Storage_live` clears a local's
  answer. Neither can change a verdict today: the lowerer discards statements
  after a terminator, so no unreached block is ever built, and `always` is an
  intersection reached by the entry path before any back edge, so a stale answer
  at a `Storage_live` cannot survive. Both are correct and cost nothing, and the
  first becomes load-bearing the moment constant-branch folding leaves a block
  behind. Recorded because a mutation test shows them surviving, and that should
  read as "known" rather than as a gap. **The prediction was wrong, and in the
  useful direction (2026-09-18):** `simplify` deletes an unreachable block rather
  than leaving one, so the unreached-block skip is *still* unexercised — folding
  removed the false positive by removing the block, not by teaching the pass to
  ignore it.
- ~~**Nothing checks that every path returns a value.**~~ **Fixed.** It was
  `check_assignment` applied to local 0, as predicted: the return slot *is* an
  `out` parameter of the function, so `Function::returns_a_value` seeds it in
  `entry_flow` and the reporting loop owes it at every exit. **A constantly-true
  loop is a known false positive**: `while( true ) { return 1; }` leaves a
  loop-exit block the graph can reach and the program cannot, so it is reported.
  `for( ; ; )` has no condition and therefore no exit block, which makes the
  workaround the better code — one fewer test per iteration — and is what the
  follow-up below should recommend.
- ~~**D7's "falling out of a non-empty arm is an error" is not enforced.**~~
  **Fixed — D38**, along with two things found while fixing it: `break` inside a
  `switch` left the enclosing loop rather than the switch, and stacked labels that
  destructure read payload fields of a variant that was not there. `fallthrough;`
  is what gave the rule a fix to name.
- ~~**Thread empty blocks.**~~ **Done (2026-09-18), M6.5.** `for( ; ; ) { return 1; }` lowered to `bb0: goto bb1`,
  `bb1: goto bb2`, `bb2: ...` — two blocks carrying no statements, existing only
  because the lowerer gives each construct its own entry. Every predecessor of a
  block whose statement list is empty and whose terminator is a `Goto` can jump
  straight to that target instead, and the block then has no predecessors and can
  go. **A pass over finished KIR rather than a change to any one construct's
  lowering**, which is the whole point: the shape comes from `for`, from `while`,
  from a `switch` arm and from nested blocks, and fixing it per-construct would be
  four fixes that each have to be got right again next time. It also shrinks the
  emitted C, where each of those blocks is a label and a `goto`. Watch the one
  interaction: a self-`Goto` (`bb1: goto bb1`) must not be threaded into an
  infinite substitution, and a block that is a branch target twice must keep both
  edges pointing somewhere valid. Cheap to verify — `verify.cpp` already checks
  that every terminator target is in range, and the golden KIR corpus is the
  before/after.
- ~~**Fold constant branches in the lowerer, and warn on `while( true )`.**~~ **Done
  (2026-09-18), M6.5 — and neither half landed where this entry expected.** Folding is **not in
  the lowerer**: `break_target()` allocates a loop's exit block before the branch is terminated, so
  a lowerer that folded would still leave that block behind, unreachable, which `verify` rejects.
  Folding therefore *forces* pruning, and pruning is only expressible over a finished graph — which
  is what makes this one pass with the threading above rather than two. **The `with constexpr`
  argument does not apply to what was built**: `simplify` reads a literal the lowerer already put
  in the terminator and propagates nothing, so it is not the machinery a constant evaluator needs
  and does not pre-empt one. **The warning was cut**, and not for the reason this entry gives —
  D41's *"this comparison is always true"* had already made `Diagnostics::warning` live during M6,
  so the exit-code question was settled before this arrived. It was cut because folding removes the
  thing it was there to explain: `while( true ) { return 7; }` now compiles, so a warning steering
  the author to `for( ; ; )` would be advice to rewrite working code.

- **A `switch` arm ending in `while( true )` is still reported as falling out of a non-empty arm.**
  The same false positive folding removed from the return check, in the one place folding cannot
  reach it: `completes_normally` in `sema/type_checker.cpp` answers over the **AST**, before
  anything is lowered, and returns `true` for a `While_stmt` unconditionally. So
  `case E::A: while( true ) { }` is refused although nothing can leave that arm. Fixing it needs
  the helper to ask whether the condition is the literal `true` **and** whether any `break` binds
  to that loop — a scan the checker already knows how to do, since it binds `break` to the nearest
  enclosing loop or switch for D38. Pinned as *accepted* in
  `type_checker_reports_an_arm_that_falls_out` ("a loop is assumed to finish"), so the day it is
  fixed a test says so. **Worth doing when the conditional expression lands**, which is the next
  thing to touch that file's view of what a statement is worth.

- **A block with one predecessor is not merged into it.** `simplify` threads edges and deletes
  blocks; it does not merge a block into a sole predecessor whose terminator is a `Goto`, so
  `if( false ) { ... }` before a `return` still emits `goto bb1; bb1:` where straight-line code
  would do. Every case the M6.5 acceptance names is already paid without it, it is one more rewrite
  of the same two vectors, and the emitted C it removes is a label and a jump that no C compiler
  keeps. Worth doing the day the generated C is read by a person rather than by `cc`.
- **An over-wide shift count is undefined behaviour in the emitted C.** A
  *constant* one is rejected — `i32 a = 1; a << 40;` says "the shift count is out
  of range" — but a variable one is not, and `a << b` with `b == 40` emits a plain
  C shift, which UBSan flags as *"shift exponent 40 is too large"*. §7.7's
  `-fwrapv` makes signed overflow defined and there is no equivalent flag for
  shifts, so this is the one place C's undefined behaviour reaches a Keel program
  that compiled cleanly. It is the same constant-versus-runtime split §12 settled
  for overflow, and that entry does not mention shifts — it should, because the
  answer may differ: overflow was decided as *wrap*, and the natural answers here
  are masking the count (what x86 does anyway, and what Java and C# define) or the
  checked-operation KIR work §12 already defers. **Whichever it is, it cannot stay
  undefined.** The corpus is otherwise clean: all 142 goldens pass under
  `-fsanitize=undefined -fno-sanitize-recover=all`.
- **The golden runner should have a UBSan mode.** `KEEL_VALGRIND=1` exists and
  earns its place; UBSan catches a different class — the one above — and today
  it only runs when someone sets `KEEL_CFLAGS` by hand. Note ASan cannot simply be
  added alongside it: its startup cost pushes several fixtures past the ten-second
  run timeout, and the set that times out varies between runs.
- **A generic body may copy an owning `T`, which is a double free.** D31 says a
  generic may mutate or copy a bare parameter only when `T` is a `struct`, and
  nothing enforces it: `T twice<T>( T a ) { T b = a; return b; }` compiles, and
  `twice<Counter>` gives one construction two destructor calls. The cause is that
  `is_owning_type` on an unbound `Parameter` answers *no*, so the checker believes
  every `T` is a non-owning struct. **This is what the `Copyable` bound closes**,
  and it is the reason bounds come before generic aggregates rather than after: a
  `Vector<T>` whose `push` copies an element has the same hole at scale, so
  building on top of it means auditing everything written meanwhile. Until then a
  generic is only safe over non-owning types, and nothing says so. **Closed at M6
  (D40 shipped, verified 2026-09-18), and it closes from both ends**: without a
  bound, `T b = a;` is refused with *an owning value is transferred, not copied*,
  because an unbound `T` no longer answers "not owning" to that question; with
  `where T : Copyable`, the body compiles and `twice<Counter>` is refused at the
  call — *`Counter` is not `Copyable`* — because a type with a destructor owns
  something. The definition and the instantiation each report the half that is
  theirs, which is D11's shape.
- **A raw pointer to a local may escape.** `i32* f() { i32 x = 1; return &x; }`
  compiles, and so does returning `&p.x`. §8's non-escaping rule is about
  *bindings* and correctly refuses `const ref i32 f() { i32 x = 1; return x; }`,
  but `&` produces a `T*`, which the rule does not cover. **Decide it with
  `[*]T`**, which is when pointers stop being rare. **The leaning is an error
  rather than a warning, and narrow**: where the compiler can see that the
  storage an escaping address names is about to be destroyed — a local going out
  of scope at the `return` that carries its address — there is no program that
  wants it, so there is nothing to warn about. That is a much smaller claim than
  tracking pointer lifetimes in general, and it is the half that is decidable
  from what §8 already knows.
- **A node that carries a name must not be built without one.** `parse_function_decl`
  has said so since M0 — *"a declaration with no name is not one"* — and the rule
  turned out to apply one level down, to expressions: `p.this`, `E::this` and any
  other keyword after `.` or `::` built a `Field_expr`/`Path_expr` holding an
  *invalid* `Symbol_id`, and `Interner::text` aborts on one. Seven spellings
  crashed the compiler. Both sites now produce an `Error` node instead, and both
  use `expect_name()` rather than a bare `expect( Identifier )` so the keyword is
  consumed — leaving it in place turned three mistakes into sixteen diagnostics.
  **The general rule, for any node kind added later: if `aux` is a `Symbol_id`,
  either the parser guarantees it is valid or every reader needs a guard.** The
  first is one edit and the second is unbounded.
- ~~**`kl_rt_alloc` guards a zero-size request, and that guard treats a symptom.**
  `struct Empty { };` type-checks, `sizeof` it is 0 under the GCC/Clang extension
  that lets an empty C struct exist at all, and `alloc<Empty>()` therefore asks
  the runtime for nothing. Since `malloc( 0 )` may return `NULL`, and `NULL` is
  how allocation failure is reported, the two become indistinguishable — so the
  runtime bumps a zero request to one byte. The real fix is upstream, in whether
  Keel has empty aggregates at all; §12 now carries the question, to be decided
  with generics. Delete the guard if that answer removes the case.~~ **Closed
  (2026-09-19), and not the way this entry said.** The upstream fix landed — §12's
  empty-aggregate rejection — and the case is now unreachable from any valid
  program, which was the stated condition for deleting the guard. The guard was
  **kept** regardless: the instruction above treats the guard as owing its
  existence to `struct Empty { };`, and it does not. `malloc( 0 )` is
  implementation-defined whatever reaches it, `NULL` still means failure and
  nothing else, and pinning that answer costs one branch while leaving it open
  costs a case no test can construct. What was a symptom is now a decision, and
  the comment in `kl_rt.c` says so instead of naming a construct the compiler
  rejects.
- **A wrong `extern` declaration is an ABI bug at run time, not a compile error.**
  The emitted C declares its externs itself and never includes the C header, so
  nothing cross-checks `u64` against a `size_t` that may not be the same width.
  Deliberate — including the header would risk a conflicting-declaration error
  instead — and the reason the runtime floor should stay thin and ours.
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
