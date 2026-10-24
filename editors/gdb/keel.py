# Copyright 2026 Laurence Bradshaw
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Shows a Keel program's values and frames in Keel in gdb. Load it with
# `source editors/gdb/keel.py`; VS Code's cppdbg also needs `-enable-frame-filters`.

import os
import re
import sys

import gdb
from gdb.FrameDecorator import FrameDecorator

# A list or string read before its declaration runs holds garbage, so reads stop here.
LIMIT = 10000

# keelc embeds the prelude and names it `<prelude>`; this is the file it embeds. gdb sets no
# `__file__` for a sourced script.
PRELUDE = os.path.join(
    os.path.dirname(os.path.abspath(sys._getframe().f_code.co_filename)), "../../keelc/src/prelude/prelude.kl"
)

MODES = {"R": "ref ", "M": "move ", "O": "out ", "K": "const ref "}


class _Reader:
    """Reads mangle.cpp's encodings back into Keel spellings."""

    def __init__(self, text):
        self.text = text
        self.at = 0

    def peek(self, what=""):
        return self.text.startswith(what, self.at) if what else self.at < len(self.text)

    def take(self, what):
        if not self.text.startswith(what, self.at):
            raise ValueError(self.text)
        self.at += len(what)

    def number(self):
        start = self.at
        while self.at < len(self.text) and self.text[self.at].isdigit():
            self.at += 1
        if start == self.at:
            raise ValueError(self.text)
        return int(self.text[start : self.at])

    def word(self):
        size = self.number()
        self.at += size
        return self.text[self.at - size : self.at]

    def mode(self):
        if self.peek() and self.text[self.at] in MODES:
            self.at += 1
            return MODES[self.text[self.at - 1]]
        return ""

    def types(self, separator, end):
        spelled = []
        while self.peek() and not (end and self.peek(end)):
            if spelled and separator:
                self.take(separator)
            spelled.append(self.mode() + self.type())
        self.take(end)
        return spelled

    def type(self):
        if self.peek("P") or self.peek("N"):
            many = self.peek("N")
            self.at += 1
            const = "const " if self.peek("K") else ""
            self.at += len(const) > 0
            return const + self.type() + ("[*]" if many else "*")
        if self.peek("A"):
            self.at += 1
            count = self.number()
            self.take("_")
            return "{}[{}]".format(self.type(), count)
        if self.peek("L"):
            self.at += 1
            return self.type() + "[]"
        if self.peek("F"):
            self.at += 1
            params = self.types("", "E")
            returned = self.mode() + self.type()
            return "fn( {} ) -> {}".format(", ".join(params), returned)
        if self.peek("D"):
            self.at += 1
            aggregate = self.type()
            return "field( {} ) -> {}".format(aggregate, self.type())
        if self.peek("U"):
            self.at += 1
            return " | ".join(self.types("", "E"))
        package = ""
        if self.peek("Q"):
            self.at += 1
            package = self.word()
        return _qualified(package, self.word() + self.arguments())

    def arguments(self):
        if not self.peek("I"):
            return ""
        self.at += 1
        return "<{}>".format(", ".join(self.types("_", "E")))

    def owner(self):
        """`kl_<package><module>_`: the package qualifies, the module never does."""
        self.take("kl_")
        parts = []
        while self.peek() and self.text[self.at].isdigit():
            parts.append(self.word())
        self.take("_")
        return parts[0] if parts else ""

    def name(self):
        """Up to the `__` that ends it; a name ending in `_` leaves a longer run of them."""
        end = self.text.find("__", self.at)
        if end < 0:
            end = len(self.text)
        while self.text.startswith("___", end):
            end += 1
        name = self.text[self.at : end]
        self.at = end
        return name


def _qualified(package, name):
    return "{}::{}".format(package, name) if package and package != "prelude" else name


def demangle_function(symbol, signature=True):
    """`kl_2kl7console_println__3i64` is `kl::println(i64)`, or `kl::println` without the
    signature; anything else is None."""
    try:
        reader = _Reader(symbol)
        package = reader.owner()
        name = reader.name()
        arguments = ""
        if reader.peek("__I"):
            reader.take("__")
            arguments = reader.arguments()
        reader.take("__")
        if reader.peek("dtor"):
            return "~" + _qualified(package, name + arguments)
        constructor = reader.text.endswith("__ctor")
        if constructor:
            reader.text = reader.text[: -len("__ctor")]
        receiver = ""
        if reader.peek("S"):
            reader.at += 1
            receiver = reader.type()
        conformed = ""
        if reader.peek("C"):
            reader.at += 1
            conformed = " [{}]".format(reader.type())
        if receiver and reader.peek():
            reader.take("_")
        params = reader.types("_", "") if reader.peek() else []
        if not signature:
            params, conformed = None, ""
        if constructor:
            return _signed(_qualified(package, name + arguments), params)
        if receiver:
            name = "{}::{}".format(receiver, name)
        else:
            name = _qualified(package, name)
        return _signed(name + arguments, params) + conformed
    except (ValueError, IndexError):
        return None


def _signed(name, params):
    return name if params is None else "{}({})".format(name, ", ".join(params))


def demangle_struct(tag):
    """`kl_2kl4list_list__I3i32E` is `kl::list<i32>`."""
    try:
        reader = _Reader(tag)
        package = reader.owner()
        if not package and (reader.peek("U") or reader.peek("A")):
            start = reader.at
            try:
                spelled = reader.type()
                if not reader.peek():
                    return spelled
            except (ValueError, IndexError):
                pass
            reader.at = start
        if reader.peek("run__"):
            reader.take("run__")
            return reader.type()
        name = reader.name()
        if reader.peek("__I"):
            reader.take("__")
            name += reader.arguments()
        return _qualified(package, name)
    except (ValueError, IndexError):
        return None


def _hidden(symbol):
    """A temporary, or a run parameter's halves, which its local holds."""
    name = symbol if isinstance(symbol, str) else symbol.print_name
    return re.fullmatch(r"kl_t\d+|kl_.+_\d+_(data|size)", name) is not None


def demangle_local(name):
    """`kl_p_1` is `p`, and anything else is itself."""
    match = re.fullmatch(r"kl_(.+)_\d+", name)
    return match.group(1) if match else name


def _field(value, name):
    for field in value.type.strip_typedefs().fields():
        if demangle_local(field.name).rstrip("_") == name:
            return value[field]
    raise gdb.GdbError("no field {} in {}".format(name, value.type))


def _text(data, count):
    count = int(count)
    if count == 0:
        return ""
    try:
        read = gdb.selected_inferior().read_memory(int(data), min(count, LIMIT))
    except gdb.MemoryError:
        return "<unreadable>"
    text = bytes(read).decode("utf-8", errors="replace")
    return text + ("..." if count > LIMIT else "")


class _StringPrinter:
    def __init__(self, data, count):
        self.data, self.count = data, count

    def to_string(self):
        return _text(self.data, self.count)

    def display_hint(self):
        return "string"


class _ListPrinter:
    def __init__(self, value):
        self.name = demangle_struct(value.type.strip_typedefs().tag)
        self.data = _field(value, "data")
        self.count = int(_field(value, "count"))

    def to_string(self):
        return "{} (count {})".format(self.name, self.count)

    def children(self):
        for i in range(min(self.count, LIMIT)):
            yield "[{}]".format(i), self.data[i]

    def display_hint(self):
        return "array"


class _StructPrinter:
    def __init__(self, value):
        self.value = value

    def to_string(self):
        return None

    def children(self):
        for field in self.value.type.strip_typedefs().fields():
            yield demangle_local(field.name), self.value[field]


def _lookup(value):
    kind = value.type.strip_typedefs()
    if kind.code != gdb.TYPE_CODE_STRUCT or not (kind.tag or "").startswith("kl_"):
        return None
    if kind.tag == "kl_2kl6string_string":
        text = _field(value, "bytes")
        return _StringPrinter(_field(text, "data"), _field(text, "count"))
    if kind.tag == "kl_7prelude_str":
        return _StringPrinter(_field(value, "data"), _field(value, "size"))
    if kind.tag.startswith("kl_2kl4list_list__I"):
        return _ListPrinter(value)
    return _StructPrinter(value)


class _KeelFrame(FrameDecorator):
    def function(self):
        name = super().function()
        if isinstance(name, str):
            return demangle_function(name, signature=False) or name
        return name

    # Through a filter, MI reports only this as the frame's file, so it must be the full path.
    def filename(self):
        symtab = self.inferior_frame().find_sal().symtab
        if symtab is None:
            return super().filename()
        if os.path.basename(symtab.filename) == "<prelude>" and os.path.exists(PRELUDE):
            return os.path.normpath(PRELUDE)
        return symtab.fullname()

    # Locals keep their C names: cppdbg evaluates each one by the name it is listed under.
    def _kept(self, symbols):
        if symbols is None:
            return None
        return [symbol for symbol in symbols if not _hidden(symbol.symbol())]

    def frame_args(self):
        return self._kept(super().frame_args())

    def frame_locals(self):
        return self._kept(super().frame_locals())


class _KeelFrames:
    def __init__(self):
        self.name = "keel"
        self.priority = 100
        self.enabled = True
        gdb.frame_filters[self.name] = self

    def filter(self, frames):
        return map(_KeelFrame, frames)


gdb.pretty_printers.append(_lookup)
_KeelFrames()

# A function with no Keel source, such as an interface's forwarding function, has only its C file's
# lines: step over it. This also steps over the Keel method it forwards to.
gdb.execute("skip -gfile *.c", to_string=True)
