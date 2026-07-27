#!/usr/bin/env python3
"""Run the build-matched broker contract state-machine tests."""

import os
import subprocess

from deskpal_client import DESKPAL


def main():
    helper = os.path.join(os.path.dirname(DESKPAL), "broker-contract-test")
    result = subprocess.run([helper], check=True, capture_output=True, text=True)
    output = result.stdout.strip()
    assert output == (
        "PASS: broker identity, capabilities, errors, and operation states"
    ), output
    print(output)


if __name__ == "__main__":
    main()
