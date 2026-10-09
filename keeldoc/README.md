# keeldoc

Writes a Keel package's documentation as HTML pages, from the `///` comments on its declarations and
the `//!` comments of its `packageinfo.kl`.

    build/debug/bin/keeldoc -o keel_stl/docs kl=keel_stl/src

It runs keelc with `--declarations` on a program importing every module of the package. If keelc
reports an error, keeldoc prints it and writes nothing. `--keelc <path>` names the keelc to run;
by default it is the one beside keeldoc.

The pages are `index.html`, with the package's doc and a summary line for each module's
declarations; one page per module, `a::b` at `a.b.html`; and `style.css`. Only public declarations
appear, and a function's or method's overloads share one entry. A type's members are grouped as
variants, fields, constructors and methods, each group in source order.

Every page ends with the package's notice, taken from the `// Copyright ...` and
`// SPDX-License-Identifier: ...` lines that open its `packageinfo.kl`; a package without them gets
no footer.

Docs are Markdown, of which keeldoc renders this much: paragraphs, headings (any level renders as
one section heading, as `### Panics` is meant), `-`, `*`, `+` and `1.` lists, fenced code blocks,
inline code, `*emphasis*`, `**strong**`, `[links](url)` and backslash escapes. Anything else shows as
text.

Pre-commit regenerates `keel_stl/docs` into a scratch directory and fails if the committed pages
differ, so a doc change needs the command above run before committing. keeldoc's own tests are
`keeldoc_tests` and `test/run_tests.sh <keeldoc> <keelc>`, which compares the pages for
`test/geo/` with `test/expected/`.
