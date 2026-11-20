#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 zenmastercat and Magia contributors
"""hmp.py SOCKET COMMAND...: send QEMU human-monitor commands over a unix
socket (QEMU -monitor unix:SOCKET,server,nowait), one per argument.
"type:TEXT" expands to one `sendkey` per character of TEXT (PS/2
keyboard input for the monitor test); "\\n" in TEXT becomes Return."""
import socket
import sys
import time

KEYS = {" ": "spc", "\n": "ret", "-": "minus", ".": "dot", "/": "slash", "=": "equal"}


def main() -> None:
    path, cmds = sys.argv[1], sys.argv[2:]
    s = None
    for _ in range(100):
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(path)
            break
        except OSError:
            time.sleep(0.1)
    if s is None:
        sys.exit("hmp.py: cannot connect to " + path)
    s.settimeout(0.5)
    out = []
    for c in cmds:
        lines = []
        if c.startswith("type:"):
            for ch in c[5:].replace("\\n", "\n"):
                k = KEYS.get(ch, ch)
                lines.append("sendkey shift-" + k.lower() if ch.isupper() else "sendkey " + k)
        else:
            lines.append(c)
        for line in lines:
            s.sendall((line + "\n").encode())
            time.sleep(0.05)
            try:
                out.append(s.recv(65536).decode(errors="replace"))
            except socket.timeout:
                pass
    s.close()
    sys.stdout.write("".join(out))


if __name__ == "__main__":
    main()
