# Keel for VS Code

Highlighting and editing rules for `.kl` files.

Install by linking this folder into VS Code's extensions directory, then reloading the window:

    ln -s "$PWD" ~/.vscode/extensions/keel.keel-0.0.1          # VS Code running locally
    ln -s "$PWD" ~/.vscode-server/extensions/keel.keel-0.0.1   # VS Code attached to WSL or SSH

Edits to the grammar take effect on the next reload.

`sample/` is for checking the highlighting by eye: two modules and a package, using every kind of
Keel syntax except string literals, which do not type-check yet. From this folder, it passes
`keelc --check --package extra=sample/extra sample/main.kl`.

The grammar guesses what a name is from where it stands, so a few cases come out wrong. The sample
includes them:

- `meter( 1 )` constructs a lowercase class, but reads as a call.
- `extra::scale` is a global, but reads as a variant.
- `Shape::Rect( 2.0, 3.0 )` is a variant, but reads as a call.
- `T` in `larger<T>` is declared as a type, but reads as a variable.
