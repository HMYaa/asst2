#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

echo "[1/3] build part_a"
bear --output compile_commands.json -- make -C part_a clean
bear --append --output compile_commands.json -- make -C part_a

echo "[2/3] build part_b"
bear --append --output compile_commands.json -- make -C part_b clean
bear --append --output compile_commands.json -- make -C part_b

echo "[3/3] build tutorial"
bear --append --output compile_commands.json -- make -C tutorial clean
bear --append --output compile_commands.json -- make -C tutorial

echo "done: $ROOT_DIR/compile_commands.json"
