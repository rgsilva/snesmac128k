#!/usr/bin/env bash
# Fetch the SingleStepTests/m68000 vectors (~113 MB, 127 mnemonic files) into
# vectors/. Mirrors snes86's core-c/tests/vectors/fetch.sh. The .json.bin files
# are the binary-packed form; run_vectors.py decodes them in-memory via
# decode.py, so no on-disk .json expansion is needed.
set -euo pipefail
cd "$(dirname "$0")/vectors"
BASE="https://raw.githubusercontent.com/SingleStepTests/m68000/main/v1"
NAMES=$(python3 -c "import json;print(' '.join(sorted(json.load(open('_urls.json')))))")
echo "fetching $(echo "$NAMES" | wc -w | tr -d ' ') files..."
printf '%s\n' $NAMES | xargs -P8 -I{} curl -sfL -o "{}" "$BASE/{}"
echo "done: $(ls -1 *.json.bin | wc -l | tr -d ' ') files, $(du -sh . | cut -f1) total"
