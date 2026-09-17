# Handoff — session state and the next walkthrough

Written 2026-09-17, at the end of a session. Untracked scratch: delete it, or `.gitignore` it,
once the work below is done. `docs/PLAN.md` is the real record — nothing here supersedes it.

---

## 1. Where the tree is

Working tree **clean** at `0c5c97a`. Recent commits, newest first:

| commit | what |
| --- | --- |
| `0c5c97a` | fix D42 method-signature hole |
| `7af5b16` | fix a generic field that owns from crashing the emitter |
| `ea75d6a` | implement some fixes for generics |
| `81e8789` | implement generic aggregates |

**Verification as of that commit:** 578 unit cases / 6411 assertions, 153 goldens, 0 failures,
and 153 under `KEEL_VALGRIND=1`.

Commands:

- build: `cmake --build build/debug -j`
- unit: `./build/debug/bin/keel_tests`
- goldens: `./keelc/test/run_tests.sh ./build/debug/bin/keelc` (`--update` to re-snapshot;
  `KEEL_VALGRIND=1` for the leak pass)

---

## 2. Standing constraints (these are in Claude's memory; restated so the file stands alone)

- **Walkthroughs are pointers, not blocks.** Every step names the function, gives its signature,
  says where it is called from, and gives a `file.cpp:line` pointer. Never paste finished code.
  Every step must be a *change*, not a justification for one — when an argument or node is the
  substance of a step, name the exact identifier to pass and put the reasoning after it. When the
  user says a step is done, read the diff before testing it: a wrong identifier that still compiles
  is dead code, not an error.
- **The user does all git operations.** Read-only git is fine (`status`, `diff`, `log`, `show`).
  Never `commit`, `push`, `add`, `checkout`, `stash`, `reset`.
- **Terse code comments** — one line where possible. Rationale belongs in `docs/PLAN.md`.
- **Diagnostics never cite the plan** — self-contained, no D-entries, no naming other languages.
- **Claude maintains `docs/PLAN.md`** without being asked.
- **Compact after every major step.** The user keeps one long session and does not use subagents.
- Test-first; mutation-test every fix; one topic per fixture; codegen fixtures return **42** with
  numbered early returns.

---

## 3. What closed this session

**A field typed `T` at a drop site.** `class Box<T> { T v; }` at `Box<Buf>` type-checked, lowered
to correct KIR, then aborted in `Ast::members`. `Kir_emitter::type_of` read a field's type straight
off the declaration, so a projection answered `T`. It now goes through `field_type`, which composes
down a chain. Fixture `codegen/generics_owning_field.kl`. This also closed the second half of bug-hunt
finding 2 and contradicted its diagnosis — the fault was never about where the instance came from.

**D42 sees a method signature.** `declare_signatures_member_functions` now records a return type and
each parameter through `record_generic_uses`, with the **aggregate** as the `from`. Three cases went
into `sema/errors_generic_aggregates.kl`: a cycle through a return type, a cycle through a parameter,
and a forwarding shape that must stay silent. Three mutations, all killed.

Both are written up in `docs/PLAN.md` under §15's M6 section.

---

## 4. What is left before M6 closes

1. **A generic `enum` crashes** — the walkthrough below. The user asked for it to be *fully
   implemented* in M6, since M6 is the generics milestone.
2. **`Box<i32> { 7 }` does not parse.** A struct literal carries its type name in `aux` with no room
   for type arguments, so `<` reads as a comparison. A generic aggregate can be declared,
   instantiated, emitted, methodded, dropped and read back — but not built from a literal.
3. **§12's M6-deadline questions**, all undecided: type-argument inference (`PLAN.md:1003`),
   function overloading (`PLAN.md:1010`), traits' static half (`PLAN.md:1011`), `Result`'s error
   type (`PLAN.md:1022`), function pointers (`PLAN.md:1024`). Plus the already-**decided**
   empty-aggregate rejection (`PLAN.md:1013`) which is not implemented — `type_checker.cpp:7526`
   still accepts one.

Item 1 partly gates item 3's `Result` question: `Result<T, E>` is an `enum` over two parameters.

---

# 5. Walkthrough — fully implement a generic `enum`

## Background: why it crashes, and what "generic" costs an `enum`

`Parser::parse_enum_decl()` at `parser.cpp:974` reads the name at :988 and then expects `:` or `{`.
There is no type-parameter list, so `enum Opt<T>` fails `expect( Token_kind::L_brace )` at :993,
recovers into the variant loop, and makes a `Variant_decl` out of each of `<`, `T`, `>`, `where`,
`T`, `:`, `Copyable` — most with an invalid `Symbol_id` in `aux`. Sema then runs on that wreckage
and `declare_signatures_enum_decls` calls `interner_.text( variant_name )` at
`type_checker.cpp:1229`, which asserts.

So the crash is a missing feature plus an unguarded read, and fixing the feature removes the input
that reaches the read.

**The three things that make an `enum` different from an aggregate**, each of which costs a step:

1. **Child 0 is taken.** An aggregate puts its `Type_param_list` at child 0 and its members after;
   an enum already uses child 0 for the underlying type, with variants from 1. Twelve places
   hand-write `children( decl ).subspan( 1 )`.
2. **`Type_table::enumeration` is keyed by declaration alone.** `type.cpp:86` looks up
   `enums_.find( declaration.v )` and returns one type per declaration. `structure` at `type.cpp:102`
   keys by declaration *and* arguments.
3. **`Type_kind::Enum` falls through every structural walk.** `substitute` at `type.cpp:188` handles
   `Parameter`, `Pointer`, `Struct`, then `default: return type;` — so `Opt<T>` would substitute to
   itself, silently. `mentions_parameter` at `type.cpp:159` has the same shape, and it is what
   `is_closed` in the worklist and `emitted_struct_order` decide instance-versus-template with.

Item 3 is the one that produces wrong programs rather than errors, so it comes early.

---

## Slice 3a — it parses, and nothing indexes by hand

### Step 1: `Parser::parse_enum_decl()` learns the list

`Node_id Parser::parse_enum_decl()` at `parser.cpp:974`, called from the declaration dispatch at
`parser.cpp:607`.

Between `expect_name()` at :988 and the underlying-type parse at :991, do what
`Parser::parse_aggregate_decl()` already does at `parser.cpp:879-908`: capture `generic_start`, set
`generic` from `check( Token_kind::Less )`, call `parse_type_params()`, run the
`while( check_keyword( Keyword::Where ) )` loop pushing each `parse_where_clause()` into the same
vector, and build one `Node_kind::Type_param_list` node over `generic_span`.

Order matters: the list goes **before** the `:` underlying type, so `enum Opt<T> : u8` reads left to
right.

### Step 2: the child order becomes `{ type_params, underlying, variants… }`

Same function, at `parser.cpp:998`: `members` is seeded `{ underlying }`. Seed it
`{ type_params, underlying }` instead, and update the comment at :995.

Put the list at **child 0**, matching an aggregate, so step 3 needs one predicate rather than two.
Variants then start at index **2**.

The unit test at `parser.cpp:6782` asserts `children( decl ).size() == 3` for a two-variant enum;
that becomes 4.

### Step 3: `Ast::type_param_list` answers for an enum

`Node_id Ast::type_param_list( Node_id id ) const` at `ast.cpp:53`. Its first branch is
`if( is_aggregate( kind( id ) ) ) return child( id, 0 );`. Widen that condition to
`is_aggregate( kind( id ) ) || kind( id ) == Node_kind::Enum_decl`.

**Do not widen `is_aggregate` itself** at `node.h:100`. `Ast::members` asserts on it at `ast.cpp:65`,
and `declare_signatures_field_decls` at `type_checker.cpp:866`,
`declare_signatures_member_functions` at `type_checker.cpp:1095`, `check_struct_ownership` and
`emit_structs` all filter on it. An enum joining that set would be walked as a struct whose variants
are fields.

This one change is what makes `is_generic( ast_, enum_decl )` at `type_checker.cpp:6162` start
telling the truth — it is just `type_param_list( decl ).is_valid()`.

### Step 4: add `Ast::variants`, and convert every hand-written walk

New accessor beside `Ast::members`, declared at `ast.h:38` and defined at `ast.cpp:62`:

`std::span<const Node_id> Ast::variants( Node_id id ) const` — asserts
`kind( id ) == Node_kind::Enum_decl`, returns `children( id ).subspan( 2 )`.

The assert is what makes the sweep verifiable rather than hopeful, exactly as `Ast::members` was in
slice 2a step 0. The twelve callers to convert:

| file | lines |
| --- | --- |
| `sema/type_checker.cpp` | 1222, 1240, 1749, 2426, 2461, 4099, 4236, 6106 |
| `sema/resolver.cpp` | 260 |
| `ir/lower.cpp` | 778, 836 |
| `codegen_c/emit_kir.cpp` | 176 |

The other `subspan( 1 )` hits in that grep are switch arms, patterns and method parameter lists —
leave them.

### Step 5: the two readers of the underlying type move to child 1

`Checker::declare_signatures_enum_decls()` at `type_checker.cpp:1197` reads `ast_.child( child, 0 )`,
and `Resolver::visit`'s `Enum_decl` case at `resolver.cpp:255` reads `children[0]`. Both become
child 1, and both keep their existing "invalid when unwritten" guard.

### Step 6: the hardening line that was the actual crash

Still in `declare_signatures_enum_decls`, the duplicate-name loop at `type_checker.cpp:1222-1232`
calls `interner_.text( variant_name )` unguarded. After this slice no valid program reaches it with
an invalid id — but error recovery can still hand sema a nameless `Variant_decl`, and this is a
`--check` crash rather than a diagnostic. Skip a variant whose `Symbol_id` is invalid, and say so in
one line.

**End of 3a:** `enum Opt<T> { … };` parses, `is_generic` is true for it, and the compiler does not
crash. It does not type yet.

---

## Slice 3b — it types

### Step 7: `Type_table::enumeration` is keyed by its arguments

`Type_id Type_table::enumeration( Node_id declaration, std::string_view name, Type_id underlying )`
at `type.cpp:86`, declared at `type.h:66`. One caller: `type_checker.cpp:1213`.

Give it a `std::span<const Type_id> arguments` parameter and rebuild its body on `structure`'s at
`type.cpp:102`: `enums_` becomes `declaration → std::vector<Instance>`, the linear scan compares
argument lists, `arguments_.emplace_back` owns the copy, and the composed spelling appends `<…>`
from each argument's own `name()`.

`Type` already has room — `element` carries the underlying type and `arguments` the instantiation,
and its comment at `type.h:45-47` wants widening from "generic aggregate" to include an enum.

### Step 8: the caller passes the enum's own parameters

`Checker::declare_signatures_enum_decls()` at `type_checker.cpp:1181`. Copy the two things
`declare_signatures_struct_decls` does at `type_checker.cpp:851-858`, in that order:

- `declare_type_parameters( child, ast_.type_param_list( child ) )` **before** the underlying type is
  resolved — a `where` clause bound has to exist before anything names `T`.
- build `arguments` by pushing `types_[type_param.v]` for each
  `type_parameters( ast_, ast_.type_param_list( child ) )`, and pass it to `enumeration`.

That makes the open form `Opt<T>` the type recorded for the declaration, which is what D11 checks
variant payloads against.

### Step 9: `substitute` and `mentions_parameter` learn `Enum`

Both in `type.cpp`. In `Type_table::substitute` at `type.cpp:188`, add a `case Type_kind::Enum:` that
mirrors the `Struct` case at :215-234 — early-return `type` when `arguments` is empty, otherwise
substitute each argument and re-intern through
`enumeration( described.declaration, substituted, base_name( type ) )`, with `element` carried
through as the underlying type.

**`base_name`, not `name`** — the comment at `type.cpp:232` says why: handing back the rendered
`Opt<T>` interns `Opt<T><i32>`.

In `mentions_parameter` at `type.cpp:159`, `case Type_kind::Enum:` joins `case Type_kind::Struct:` —
same loop over `arguments`. Miss this and `is_closed` in the worklist at `lower.cpp:1956` calls
`Opt<T>` a closed instance and tries to emit the template.

### Step 10: an annotation may name a generic enum

`Checker::type_of_annotation`'s `Node_kind::Generic_type` case at `type_checker.cpp:4950`. Two edits:

- The rejection at :4973, `if( !is_aggregate( ast_.kind( decl ) ) )`, must also admit
  `Node_kind::Enum_decl` — otherwise `Opt<i32>` reports *"`Opt` is not a generic"*.
- The interning at :5006, `table_.structure( decl, arguments, name )`, branches: `enumeration` for an
  `Enum_decl`, `structure` otherwise.

`resolve_type_arguments` at `type_checker.cpp:4789` needs nothing — it reads
`ast_.type_param_list( declaration )`, which step 3 taught about enums, so the arity message and
`check_bounds` work as they stand.

The mirror case is the `Named_type` branch at `type_checker.cpp:4858`, where a bare `Opt` with no
arguments should say *"`Opt` is generic, so its type arguments must be written"*. Its condition is
`is_aggregate( ast_.kind( decl ) ) && is_generic( ast_, decl )` — widen the first half the same way.

### Step 11: the resolver scopes the parameters

`Resolver::visit`'s `Enum_decl` case at `resolver.cpp:241`. Before visiting the underlying type or
any payload, do what the `Struct_decl`/`Class_decl` case does at `resolver.cpp:286-287`:
`push_scope( Scope_kind::Barrier )` then `visit( ast_.type_param_list( id ) )`, and pop at the end of
the case.

**A barrier, not a plain scope**, for the reason stated there: `T` belongs to this declaration, and a
plain scope would let a top-level `T` be found from a payload annotation. The existing comment in
this case about variant names deliberately not entering scope stays true and is unrelated.

### Step 12: a payload's type is read through the instance, never off the declaration

This is the family that has bitten eleven times, and an enum adds two more sites. Both read
`types_[payload[i].v]` — the declared type, `T`:

- `Checker::infer_variant_construction`, the argument check at `type_checker.cpp:4141` —
  `Opt<i32>::Some( 7 )` would otherwise report *expected `T`, got `i32`*.
- the destructuring binding at `type_checker.cpp:2452` — `case Some( v ):` would otherwise bind `v`
  as a `T`.

The helper already exists:
`Type_id field_type( const Ast&, Type_table&, Type_id aggregate, Node_id field, std::span<const Type_id> recorded )`
at `type_checker.cpp:6234`, and the checker's own wrapper is at `type_checker.cpp:4027`. Pass the
**enum instance type** as `aggregate` and the payload `Field_decl` as `field`.

For that to work, `aggregate_bindings` at `type_checker.cpp:6208` must stop gating on
`table.is_struct( aggregate )` — its guard becomes struct **or** enum. Everything after it already
reads `arguments` and `declaration` off the `Type` and calls
`type_parameters( ast, ast.type_param_list( … ) )`, all of which now answer for an enum.

### Step 13: D42 sees a variant payload

`Checker::declare_signatures_enum_decls`'s payload loop at `type_checker.cpp:1240`, whose comment
already says it is "the same recording `declare_signatures_field_decls` does for a struct". Add the
call that methods just got, with the same two decisions:

`record_generic_uses( child, <the payload field's recorded type>, ast_.span( field ) )`, guarded by
`is_generic( ast_, child )`. **`child` — the enum declaration — is the `from`**, never the variant
and never the field, for two reasons: the cycle to close is `Opt` to `Opt`, and an edge whose `from`
is a declaration the worklist can match would be followed at `lower.cpp:2023` and handed to
`Lowering` as a function.

Then `record_generic_uses` at `type_checker.cpp:5797` needs a `case Type_kind::Enum:` beside its
`Struct` one at :5814 — same body, since an enum instance is a generic use exactly as a struct
instance is.

**End of 3b:** `Opt<i32>` types, `Opt<f64>` is a different type, bounds are checked,
`enum Bad<T> { V( Bad<Box<T>> ) }` is refused by D42, and nothing is emitted.

---

## Slice 3c — it emits and runs

### Step 14: `emit_enums` walks instantiations, not declarations

`Kir_emitter::emit_enums()` at `emit_kir.cpp:161` iterates `ast_.children( ast_.root() )` and filters
`Enum_decl` — one C struct per declaration. It has to become one per *instantiation*, which is what
`emit_structs` at `emit_kir.cpp:190` already does by walking `struct_order_`.

The payload field write at `emit_kir.cpp:180` currently spells `types_.type_of( field )` directly; it
needs the substitution `emit_structs` does at `emit_kir.cpp:227-229` through `aggregate_bindings` —
which step 12 has already taught to answer for an enum.

`Type_table::struct_types()` at `type.cpp:262` walks `structs_` only. Decide here whether enum
instances join `struct_order_` or get their own ordered list. **Joining is the better answer** and
the reason is ordering, not tidiness: an enum with a by-value struct payload and a struct with a
by-value enum field are both writable, and only one post-order over all of them puts every definition
after everything it contains. `order_structs` and `emitted_struct_order` are then the single place
that decides.

### Step 15: mangling and spelling need almost nothing

`mangle_struct` at `mangle.cpp:97` already reads `base_name` and `arguments` off the `Type`, so
`Opt<i32>` mangles as `kl__Opt__I3i32E` the moment step 7 gives the type its arguments.
`Spelling::structure` at `spelling.cpp:82` calls it unchanged.

The one line to re-read is `Spelling::type`'s enum case at `spelling.cpp:73`: a payload-free enum
spells as its underlying integer. That stays right for a generic one — `Opt<T>` with no payloads
carries no `T` and every instance is the same integer — but check whether you want all instances
collapsing to one C name there.

### Step 16: lowering reads the variant through the instance

`lower.cpp` at 778 and 836 index the variant by ordinal — those are the `Ast::variants` conversions
from step 4 and need nothing more. What does need checking is every place lowering reads a payload
field's *type*; `Lowering` already carries the instance bindings and routes type reads through two
helpers, so the work is confirming both variant paths use them rather than the declaration.

---

## Testing

Per slice, and the fixtures are cheap because both suites already have the shapes:

- **3a:** a parser unit test in `parser.cpp` beside the `enum` section at `parser.cpp:6593`,
  asserting child 0 is the `Type_param_list` and `variants()` returns two. Plus a `sema/` golden for
  the error-recovery case from step 6 — a malformed generic enum that must diagnose rather than
  abort.
- **3b:** extend `sema/errors_generic_aggregates.kl`, or start `sema/errors_generic_enums.kl` if it
  grows past a handful — bad arity, a bare `Opt` with no arguments, a bound violation, and a D42
  cycle through a payload. One declaration per message, and at least one *clean* generic enum in the
  file, since stderr is diffed whole.
- **3c:** a `codegen/` fixture with `RUN`, returning 42: one enum at two arguments, a payload read
  back through `switch`, and a payload-free generic enum to pin that it stays an integer.

**Mutations that must kill something:** drop the `Enum` case from `substitute` (step 9) — an instance
silently keeps `T`; drop it from `mentions_parameter` — the worklist tries to emit the template; pass
`name` instead of `base_name` in step 9 — a doubled spelling; and pass the variant rather than
`child` as the `from` in step 13.

---

## Scope, honestly

Three slices, and 3b is the one with the real risk — steps 9 and 12 are the silent-wrong-answer ones,
which is why they come before anything is emitted. Steps 1–6 are mechanical and verifiable by an
assert.

Two things not to fold in unless wanted: **an owning payload** stays refused by D30's existing rule,
so `Opt<Buf>` should report rather than work, and generics do not change that; and **methods on a
generic enum**, which is slice 2c's whole story again and has no customer yet.

What this unblocks is `Result<T, E>` — §12's error-handling question at `PLAN.md:1022` is due with M6
and cannot be answered while the spelling crashes.
