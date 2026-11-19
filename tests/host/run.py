"""Builds and runs the host unit tests: python tests/host/run.py

Needs a C compiler (gcc, clang or cc) on PATH. No ESP-IDF required: the parser and the
frame builder are compiled against the small stubs in tests/host/stubs.
"""
import os
import shutil
import subprocess
import sys
import tempfile

here = os.path.dirname(os.path.abspath(__file__))
main_dir = os.path.join(here, "..", "..", "main")

compiler = os.environ.get("CC") or next((c for c in ("gcc", "clang", "cc") if shutil.which(c)), None)
if not compiler:
    sys.exit("No C compiler found (tried gcc, clang, cc). Set CC to override.")

subprocess.check_call([sys.executable, os.path.join(here, "..", "..", "scripts", "generate_config.py")])

with tempfile.TemporaryDirectory() as tmp:
    exe = os.path.join(tmp, "host_tests.exe" if os.name == "nt" else "host_tests")
    subprocess.check_call([
        compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-g",
        "-I", os.path.join(here, "stubs"), "-I", main_dir,
        os.path.join(here, "test_main.c"),
        os.path.join(main_dir, "config_parser.c"),
        os.path.join(main_dir, "beacon_generator.c"),
        "-o", exe,
    ])
    result = subprocess.call([exe] + sys.argv[1:])

result |= subprocess.call([sys.executable, os.path.join(here, "test_generator.py"), compiler])
sys.exit(result)
