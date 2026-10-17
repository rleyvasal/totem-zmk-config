#!/usr/bin/env bash
set -euo pipefail

# The caller checks out the pinned custom fork, never a clean upstream tree.
git -C "$1" merge --no-ff --no-commit "$2"
