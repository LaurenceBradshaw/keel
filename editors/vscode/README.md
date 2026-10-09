# Keel for VS Code

Highlighting and editing rules for `.kl` files, keelc's errors underlined in the editor, names
coloured by what they refer to, hovers, and go-to-definition.

Install by linking this folder into VS Code's extensions directory, then reloading the window:

    ln -s "$PWD" ~/.vscode/extensions/keel.keel-0.0.1          # VS Code running locally
    ln -s "$PWD" ~/.vscode-server/extensions/keel.keel-0.0.1   # VS Code attached to WSL or SSH

Edits to the grammar or to `extension.js` take effect on the next reload.

## Errors

When a `.kl` file is opened or saved, the extension runs `keelc --check --diagnostics=json --names` on it
from the workspace folder, and underlines what keelc reports in every file the program loaded.
It has no dependencies and needs no build step. Two settings:

- `keel.compilerPath`: the keelc to run, relative to the workspace folder. It defaults to
  `build/debug/bin/keelc`, so build keelc first.
- `keel.packages`: the packages to pass, each as `name=<dir>`, the same as keelc's `--package`.

The saved file is checked as the program's root, which is its `main.kl` when it has one. Saving a
module that another file imports checks the module and what it imports, but not the file importing it.
Errors that come from how the two files use each other only show once that file is saved as well.
If keelc cannot run at all, the reason is written to the *Keel* output channel.

`sample/` is for checking the highlighting by eye: two modules and a package, using every kind of
Keel syntax. From this folder, it passes `keelc --check --package extra=sample/extra sample/main.kl`.

## Names

The grammar guesses what a name is from where it stands, so a few cases come out wrong. The sample
includes them:

- `meter( 1 )` constructs a lowercase class, but reads as a call.
- `extra::scale` is a global, but reads as a variant.
- `Shape::Rect( 2.0, 3.0 )` is a variant, but reads as a call.
- `T` in `larger<T>` is declared as a type, but reads as a variable.

The same run also reports what each name resolved to, and the extension paints that over the
grammar: after a save, each of these takes the right colour. While a file has unsaved edits the
colours from its last save stay, moved along with the text, until it is saved again.

## Hover

Hovering a name shows its declaration on one line as the source writes it, without its body: a
`public void push( move T value )`. A member of a generic type shows its type parameters bound,
so `v.push` on a `list<i32>` reads `public void push( move i32 value )`. Below that comes the
declaration's `///` doc comment, as Markdown. A declaration's own name hovers as its uses do, and a
type parameter shows the declaration it parameterises, `where` clauses included. Like
go-to-definition, it answers from the last save. A package name has no hover.

## Go to definition

F12, or Ctrl+click on a name, goes to where it is declared, in whichever file or package that is.
It answers from the last save: after unsaved edits above a name it can find nothing or the wrong
place until the file is saved again. A package name such as `extra` has no one declaration. A field
or method after a `.` goes to the member of the object's type, through a pointer too, and a call
to the overload the compiler chose.

A name declared in the prelude, such as `str`, opens `prelude.kl` read-only. It is not a file on
disk: its text comes from `keelc --print-prelude`, run with `keel.compilerPath`, and is asked again
when that compiler is rebuilt.
