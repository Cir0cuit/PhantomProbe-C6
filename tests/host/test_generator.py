"""Checks that scripts/generate_config.py turns any manifest text into a C string literal that
compiles under the firmware's warning flags and reproduces the manifest byte for byte.
Run through: python tests/host/run.py
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GENERATOR = os.path.join(HERE, "..", "..", "scripts", "generate_config.py")

# Trigraphs, quotes, backslashes, a tab, non-ASCII text, a byte order mark and CRLF line ends
MANIFEST = (
    "﻿[networks]\r\n"
    "What??!\r\n"
    "# right??) and ??/ in a comment\r\n"
    'Say "hi" \\ there\r\n'
    "Tab\there\r\n"
    "Café שלום 9\r\n"
    "Last line without newline"
)
EXPECTED = MANIFEST.lstrip("﻿").replace("\r\n", "\n") + "\n"

CHECKER = r"""
#include <stdio.h>
#include "embedded_config.h"
int main(void) { fwrite(EMBEDDED_SSID_LIST_CONF, 1, sizeof(EMBEDDED_SSID_LIST_CONF) - 1, stdout); return 0; }
"""


def main(compiler):
    with tempfile.TemporaryDirectory() as tmp:
        # The generator works on <root>/ssid_list.conf and <root>/main, relative to its own location
        os.makedirs(os.path.join(tmp, "scripts"))
        os.makedirs(os.path.join(tmp, "main"))
        shutil.copy(GENERATOR, os.path.join(tmp, "scripts", "generate_config.py"))
        with open(os.path.join(tmp, "ssid_list.conf"), "w", encoding="utf-8", newline="") as f:
            f.write(MANIFEST)
        subprocess.check_call([sys.executable, os.path.join(tmp, "scripts", "generate_config.py")], stdout=subprocess.DEVNULL)

        with open(os.path.join(tmp, "check.c"), "w", encoding="utf-8") as f:
            f.write(CHECKER)
        exe = os.path.join(tmp, "check.exe" if os.name == "nt" else "check")
        subprocess.check_call([compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wtrigraphs",
                               "-I", os.path.join(tmp, "main"), os.path.join(tmp, "check.c"), "-o", exe])
        got = subprocess.check_output([exe]).replace(b"\r\n", b"\n")
        if got != EXPECTED.encode("utf-8"):
            print("generator test FAILED: embedded text differs from the manifest")
            print("  expected:", EXPECTED.encode("utf-8"))
            print("  got:     ", got)
            return 1

        # A local override file wins over the tracked manifest
        with open(os.path.join(tmp, "ssid_list.local.conf"), "w", encoding="utf-8", newline="") as f:
            f.write("[networks]\nLocalOnly\n")
        subprocess.check_call([sys.executable, os.path.join(tmp, "scripts", "generate_config.py")], stdout=subprocess.DEVNULL)
        with open(os.path.join(tmp, "main", "embedded_config.h"), encoding="utf-8") as f:
            header = f.read()
        if "LocalOnly" not in header or "What" in header:
            print("generator test FAILED: ssid_list.local.conf was not used instead of ssid_list.conf")
            return 1
        os.remove(os.path.join(tmp, "ssid_list.local.conf"))

        os.remove(os.path.join(tmp, "ssid_list.conf"))
        missing = subprocess.run([sys.executable, os.path.join(tmp, "scripts", "generate_config.py")],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if missing.returncode == 0:
            print("generator test FAILED: a missing ssid_list.conf must be an error")
            return 1
    print("generator test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
