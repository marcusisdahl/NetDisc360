"""Runs the Linux tests in CI's order and, for any that fail, puts the
failure in a GitHub annotation, which shows on the run's page and through
the API without the log.

    python tests/run_ci.py
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = ["test_server", "test_discs", "test_compress", "test_sync", "test_generator"]


def annotate(title, text):
    # Workflow commands are one line: %, CR and LF are escaped.
    text = text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
    print(f"::error title={title}::{text}", flush=True)


def main():
    failed = 0
    for name in TESTS:
        result = subprocess.run([sys.executable, os.path.join(HERE, name + ".py")],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        print(result.stdout, flush=True)
        if result.returncode == 0:
            continue
        failed += 1
        # unittest's failure reports start at a line of = signs.
        reports = re.split(r"^=+$", result.stdout, flags=re.M)[1:]
        for report in reports or [result.stdout[-4000:]]:
            report = re.split(r"^-+\nRan \d+ test", report, flags=re.M)[0]
            annotate(f"{name} failed", report.strip()[-4000:])
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
