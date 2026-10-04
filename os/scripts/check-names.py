#!/usr/bin/env python3
"""scripts/check-names.py [--list]

Pre-flash check 7 (docs/os/architecture/upstream-hooks.md, "Checking the hooks"): nothing a user
or the network can see names upstream's brand ("Solana" as the name of the OS, "SKYRIZZ").

What is searched is the text that can reach a screen, a log, a radio frame or a reader:

  * string literals of the compiled sources (os.ino and src/, with every comment removed first,
    both // and /* */; identifiers are not text and are not searched). The embedded web page is
    a raw string literal in src/net/push_server.cpp, so it is covered;
  * string literals of the Lua apps and the Lua library (apps/, lib/), comments removed;
  * string literals of the tools (tools/*.py, scripts/*.py): help texts and messages;
  * README.md, whole.

A literal may name the Solana blockchain, which is the payment network and not the OS, and two
upstream protocol texts that no user sees. Those are the ALLOWED patterns below; the same list is
in upstream-hooks.md under "Names that stay". Everything else that matches is printed as
`file:line: text` and the exit status is 1. With --list every match is printed with its verdict
(kept or FAIL), which is how the allow-list is reviewed.

Standard library only. Works from any directory.
"""

import io
import os
import re
import sys
import tokenize

FW = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

BRAND = re.compile(r"solana|skyrizz", re.IGNORECASE)

# Names that stay (upstream-hooks.md). Each is removed from a literal before the brand search.
ALLOWED = [
    # --- the Solana blockchain: the network the badge pays on ---
    r"^solana$",                          # the signing domain and the rail name, as a whole literal
    r"solana_pay",                        # the feature
    r"begin_solana",                      # wallet.begin_solana
    r"Solana JSON-RPC",                   # help text of config key rpc_url
    r"[a-z0-9.-]*\.solana\.com",          # public RPC hosts (api.devnet.solana.com)
    r"solana_(wallet|ata)",               # record fields: the payee's wallet and token account
    r"/feed/solana",                      # the listener's feed route for this rail
    r"[Ss]olana (blockchain|domain|rail|devnet)",   # the network, the signing domain, the rail, in messages
    r"solana-keygen",                     # the blockchain's key tool (vkdev.py help)
    # --- upstream protocol texts no user sees ---
    r"solana-badge-register:",            # the store registration line (upstream's broker protocol)
    r"solana-badge identity self test",   # signed and verified locally, never shown or sent
]
ALLOWED_RE = [re.compile(p) for p in ALLOWED]

# Whole-file allowances for README.md: the credit to upstream, and the blockchain by name.
README_ALLOWED = [
    re.compile(r"Solana OS by spacemandev"),
    re.compile(r"Solana (blockchain|devnet|payments?|network|rail)"),
    re.compile(r"solana-payments\.md"),
]

C_SUFFIXES = (".ino", ".cpp", ".c", ".h", ".hpp")


def c_literals(text):
    """Yield (line, literal) for every string literal of C or C++ source, skipping comments."""
    i, n, line = 0, len(text), 1
    while i < n:
        ch = text[i]
        if ch == "\n":
            line += 1
            i += 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            line += text.count("\n", i, j)
            i = j
        elif ch == "R" and text.startswith('R"', i) and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
            open_paren = text.find("(", i + 2)
            if open_paren < 0 or open_paren - (i + 2) > 16:
                i += 1
                continue
            closing = ")" + text[i + 2:open_paren] + '"'
            j = text.find(closing, open_paren + 1)
            j = n if j < 0 else j
            body = text[open_paren + 1:j]
            for k, part in enumerate(body.split("\n")):
                yield line + k, part
            line += body.count("\n")
            i = min(n, j + len(closing))
        elif ch == '"':
            j = i + 1
            while j < n and text[j] != '"' and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            yield line, text[i + 1:j]
            i = j + 1
        elif ch == "'":
            j = i + 1
            while j < n and text[j] != "'" and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            i = j + 1
        else:
            i += 1


def lua_literals(text):
    """Yield (line, literal) for every string literal of Lua source, skipping comments."""
    i, n, line = 0, len(text), 1

    def long_bracket(at):
        m = re.match(r"\[(=*)\[", text[at:at + 40])
        return m.group(1) if m else None

    while i < n:
        ch = text[i]
        if ch == "\n":
            line += 1
            i += 1
        elif text.startswith("--", i):
            level = long_bracket(i + 2)
            if level is not None:
                j = text.find("]" + level + "]", i + 2)
                j = n if j < 0 else j + len(level) + 2
                line += text.count("\n", i, j)
                i = j
            else:
                j = text.find("\n", i)
                i = n if j < 0 else j
        elif ch == "[" and long_bracket(i) is not None:
            level = long_bracket(i)
            start = i + len(level) + 2
            j = text.find("]" + level + "]", start)
            j = n if j < 0 else j
            body = text[start:j]
            for k, part in enumerate(body.split("\n")):
                yield line + k, part
            line += body.count("\n")
            i = min(n, j + len(level) + 2)
        elif ch in "\"'":
            j = i + 1
            while j < n and text[j] != ch and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            yield line, text[i + 1:j]
            i = j + 1
        else:
            i += 1


def py_literals(text):
    """Yield (line, literal) for every string token of Python source (docstrings included)."""
    try:
        for tok in tokenize.generate_tokens(io.StringIO(text).readline):
            if tok.type == tokenize.STRING or tok.type == getattr(tokenize, "FSTRING_MIDDLE", -1):
                for k, part in enumerate(tok.string.split("\n")):
                    yield tok.start[0] + k, part
    except (tokenize.TokenError, SyntaxError, IndentationError):
        for k, part in enumerate(text.split("\n")):
            yield k + 1, part


def files():
    """(relative path, kind) of every file searched."""
    out = [("os.ino", "c")]
    for root, _dirs, names in os.walk(os.path.join(FW, "src")):
        for name in names:
            if name.endswith(C_SUFFIXES):
                out.append((os.path.relpath(os.path.join(root, name), FW), "c"))
    for top in ("apps", "lib"):
        for root, _dirs, names in os.walk(os.path.join(FW, top)):
            for name in names:
                rel = os.path.relpath(os.path.join(root, name), FW)
                if name.endswith(".lua"):
                    out.append((rel, "lua"))
                elif name.endswith(".ini"):
                    out.append((rel, "text"))
    for top in ("tools", "scripts"):
        folder = os.path.join(FW, top)
        if os.path.isdir(folder):
            for name in os.listdir(folder):
                if name.endswith(".py") and name != os.path.basename(__file__):
                    out.append((os.path.join(top, name), "py"))
    out.append(("README.md", "readme"))
    return sorted(out)


def verdict(literal, kind):
    """True when the literal still names the brand after the allowed names are removed."""
    rest = literal
    for pattern in (README_ALLOWED if kind == "readme" else []) + ALLOWED_RE:
        rest = pattern.sub("", rest)
    return BRAND.search(rest) is not None


def main():
    list_all = "--list" in sys.argv[1:]
    failures = 0
    for rel, kind in files():
        path = os.path.join(FW, rel)
        try:
            with open(path, encoding="utf-8", errors="replace") as handle:
                text = handle.read()
        except OSError:
            continue
        if not BRAND.search(text):
            continue
        if kind == "c":
            literals = c_literals(text)
        elif kind == "lua":
            literals = lua_literals(text)
        elif kind == "py":
            literals = py_literals(text)
        else:
            literals = ((k + 1, part) for k, part in enumerate(text.split("\n")))
        for line, literal in literals:
            if not BRAND.search(literal):
                continue
            bad = verdict(literal, kind)
            if bad:
                failures += 1
            if bad or list_all:
                shown = literal.strip()
                if len(shown) > 110:
                    shown = shown[:107] + "..."
                prefix = ("FAIL " if bad else "kept ") if list_all else ""
                print("%s%s:%d: %s" % (prefix, rel, line, shown))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
