#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 5 ]]; then
  echo "usage: $0 <dxvk.so> <runtime.so> <readelf> <nm> <expected-soname>" >&2
  exit 2
fi

DXVK_SO=$1
RUNTIME_SO=$2
READELF=$3
NM=$4
EXPECTED_SONAME=$5

for file in "$DXVK_SO" "$RUNTIME_SO"; do
  [[ -s "$file" ]] || { echo "::error::Missing or empty ELF: $file"; exit 1; }
  "$READELF" -h "$file" | grep -q 'AArch64' || {
    echo "::error::Not an AArch64 ELF: $file"; exit 1;
  }
done

"$READELF" -d "$DXVK_SO" | grep -Fq "SONAME.*[$EXPECTED_SONAME]" || {
  echo "::error::DXVK SONAME mismatch; expected $EXPECTED_SONAME"; exit 1;
}

DXVK_EXPORTS=$("$NM" -D --defined-only "$DXVK_SO")
grep -Eq '[[:space:]]Direct3DCreate9$' <<<"$DXVK_EXPORTS" || {
  echo "::error::Direct3DCreate9 export missing"; exit 1;
}
grep -Eq '[[:space:]]Direct3DCreate9Ex$' <<<"$DXVK_EXPORTS" || {
  echo "::error::Direct3DCreate9Ex export missing"; exit 1;
}

"$READELF" -d "$RUNTIME_SO" | grep -Fq "Shared library: [$EXPECTED_SONAME]" || {
  echo "::error::Runtime does not depend on $EXPECTED_SONAME"; exit 1;
}

echo "Verified ARM64 DXVK/runtime pair: $DXVK_SO + $RUNTIME_SO -> $EXPECTED_SONAME"
