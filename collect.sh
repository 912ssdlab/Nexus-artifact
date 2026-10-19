#!/usr/bin/env bash
set -euo pipefail
artifact_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
collection_python="${PYTHON:-python3}"
preview=false
for arg in "$@"; do
    if [[ "$arg" == "--dry-run" ]]; then preview=true; fi
done
if [[ "$preview" == false ]]; then
    "$collection_python" "$artifact_root/trace_tools/build.py" --jobs 2
fi
exec "$collection_python" "$artifact_root/trace_tools/collect.py" "$@"
