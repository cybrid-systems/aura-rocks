#!/usr/bin/env python3
"""Report top-level forms whose parens don't balance (strings/comments aware)."""
import sys
for path in sys.argv[1:]:
    s = open(path).read(); depth = 0; line = 1; start = None; i = 0; instr = False
    while i < len(s):
        c = s[i]
        if instr:
            if c == '\\': i += 1
            elif c == '"': instr = False
        elif c == '"': instr = True
        elif c == ';':
            while i < len(s) and s[i] != '\n': i += 1
            continue
        elif c == '(':
            if depth == 0: start = line
            depth += 1
        elif c == ')':
            depth -= 1
            if depth < 0: print(f"{path}:{line}: extra ')' (form from {start})"); depth = 0
        if c == '\n': line += 1
        i += 1
    if depth: print(f"{path}: unclosed form starting line {start} (depth {depth})")
