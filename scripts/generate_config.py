"""Compiles ssid_list.conf (project root) into main/embedded_config.h.

Runs as a PlatformIO pre-build script, from main/CMakeLists.txt for plain idf.py builds,
and standalone: python scripts/generate_config.py
"""
import os
import sys

MAX_LINE_BYTES = 127  # config_parser.c reads lines into a 128-byte buffer


def fail(message):
    sys.stderr.write(f"[CONFIG] ERROR: {message}\n")
    sys.exit(1)


def generate():
    root_dir = None
    try:
        Import("env")
        root_dir = env["PROJECT_DIR"]
    except Exception:
        pass

    if not root_dir:
        root_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    conf_path = os.path.join(root_dir, "ssid_list.conf")
    out_path = os.path.join(root_dir, "main", "embedded_config.h")

    try:
        # utf-8-sig drops the byte order mark some Windows editors add
        with open(conf_path, "r", encoding="utf-8-sig") as f:
            content = f.read()
    except FileNotFoundError:
        fail(f"{conf_path} not found")
    except (OSError, UnicodeDecodeError) as e:
        fail(f"cannot read {conf_path} (must be UTF-8): {e}")

    lines = content.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    if lines and lines[-1] == "":
        lines.pop()

    escaped_lines = []
    for number, line in enumerate(lines, start=1):
        if len(line.encode("utf-8")) > MAX_LINE_BYTES and not line.lstrip().startswith(("#", ";")):
            print(f"[CONFIG] Warning: ssid_list.conf line {number} is longer than {MAX_LINE_BYTES} bytes; "
                  "the firmware ignores the rest of the line")
        # Three-digit octal escapes are safe before any following character
        esc = "".join(chr(b) if 0x20 <= b <= 0x7E and b not in (0x22, 0x5C, 0x3F) else "\\%03o" % b
                      for b in line.encode("utf-8"))
        escaped_lines.append(f'    "{esc}\\n"')
    if not escaped_lines:
        escaped_lines.append('    ""')

    c_content = "\n".join(escaped_lines)
    header_code = f"""// AUTO-GENERATED FROM ssid_list.conf - DO NOT EDIT DIRECTLY
#pragma once

static const char EMBEDDED_SSID_LIST_CONF[] =
{c_content};
"""

    if os.path.exists(out_path):
        with open(out_path, "r", encoding="utf-8") as f:
            if f.read() == header_code:
                return

    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(header_code)
    print(f"[CONFIG] Generated main/embedded_config.h from {conf_path}")


generate()
