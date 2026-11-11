# Keel for VS Code

Highlighting and editing rules for `.kl` files, keelc's errors underlined in the editor, names
coloured by what they refer to, hovers, go-to-definition, and a header view of a file's
declarations.

Install by linking this folder into VS Code's extensions directory, then reloading the window:

    ln -s "$PWD" ~/.vscode/extensions/keel.keel-0.0.1          # VS Code running locally
    ln -s "$PWD" ~/.vscode-server/extensions/keel.keel-0.0.1   # VS Code attached to WSL or SSH

On Windows, from PowerShell in this folder (a junction needs no administrator rights):

    New-Item -ItemType Junction -Path "$env:USERPROFILE\.vscode\extensions\keel.keel-0.0.1" -Target $PWD

Edits to the grammar or to `extension.js` take effect on the next reload.

## Building and debugging a project

Debugging needs Microsoft's C/C++ extension (`ms-vscode.cpptools`), and on Linux gdb. For a project
whose program is `src/main.kl`, as `keelb` lays it out, `.vscode/tasks.json` builds it with debug
information:

    {
        "version": "2.0.0",
        "tasks": [
            {
                "label": "keelb build --debug",
                "type": "process",
                "command": "${userHome}/.local/bin/keelb",
                "windows": { "command": "${env:LOCALAPPDATA}/Programs/keel/bin/keelb.exe" },
                "args": [ "build", "--debug" ],
                "options": { "cwd": "${workspaceFolder}" },
                "problemMatcher": []
            }
        ]
    }

and `.vscode/launch.json` runs it under the debugger, with breakpoints in the `.kl` files:

    {
        "version": "0.2.0",
        "configurations": [
            {
                "name": "Debug (Linux)",
                "type": "cppdbg",
                "request": "launch",
                "preLaunchTask": "keelb build --debug",
                "program": "${workspaceFolder}/build/debug/${workspaceFolderBasename}",
                "cwd": "${workspaceFolder}",
                "MIMode": "gdb"
            },
            {
                "name": "Debug (Windows)",
                "type": "cppvsdbg",
                "request": "launch",
                "preLaunchTask": "keelb build --debug",
                "program": "${workspaceFolder}/build/debug/${workspaceFolderBasename}.exe",
                "cwd": "${workspaceFolder}",
                "console": "integratedTerminal"
            }
        ]
    }

The paths are the installed ones, so a VS Code started before the install needs no restart. The
program is named after the project folder.

## Errors

When a `.kl` file is opened or saved, the extension runs `keelc --check --diagnostics=json --names` on it
from the workspace folder, and underlines what keelc reports in every file the program loaded.
It has no dependencies and needs no build step. Two settings:

- `keel.compilerPath`: the keelc to run, relative to the workspace folder. Empty, the default, runs
  the one `scripts/install.sh` or `install.ps1` installed: `~/.local/bin/keelc`, or
  `%LOCALAPPDATA%\Programs\keel\bin\keelc.exe`. Working on keelc, install with `--link` so this is
  the debug build.
- `keel.packages`: the packages to pass, each as `name=<dir>`, the same as keelc's `--package`.

The saved file is checked alone, with what it imports but not the file importing it, so it reports
only what it can be sure of: a file with no `main()` cannot see the program's `@functions_of`, so
none of its private functions is reported unused. *Keel: Check Program* checks the whole program
from the file beside it that declares `main()`; what that finds stays shown until it runs again,
except what a file's own check also reports, which shows once.
The *Keel* output channel has a line for each check: what it found and how long it took, or why
keelc could not run.

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

`keelc/src/prelude/prelude.kl` itself is checked with `--as-prelude`, so it stands in for keelc's
own copy rather than declaring every prelude name a second time.

## Header view

*Keel: Show Header*, or the list button in a `.kl` file's title bar, opens `name (header).kl`
beside the file: every declaration the file holds, as hover writes it, without bodies or docs.
Each type's members sit inside its braces, private ones too, and each banner stays where it is.
Ctrl+click or F12 on a name goes to its definition. The view comes from
`keelc --check --diagnostics=json --declarations` and is listed again each time the file is saved.

Alt+O (*Keel: Switch Header/Source*) swaps a file and its header in the same group instead, as
C++'s header and source swap.

A banner is a `///` line of dashes around a name. It opens a section that runs to the next banner
or to the end of the body it is in, and keeldoc's pages divide at it too:

    /// --- Look-ups ---
