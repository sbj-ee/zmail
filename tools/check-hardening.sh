#!/usr/bin/env bash
# Fail unless the given binary was built with the exploit mitigations
# CMakeLists.txt asks for (ZMAIL_HARDEN): PIE, full RELRO (GNU_RELRO + BIND_NOW),
# a non-executable stack, and stack-protector calls. Usage: check-hardening.sh build/zmail
set -euo pipefail

bin="${1:?usage: $0 <binary>}"
header=$(readelf -hW "$bin")
segments=$(readelf -lW "$bin")
dynamic=$(readelf -dW "$bin")
symbols=$(nm -D "$bin")

status=0
check() { # description, command...
  local what="$1"; shift
  if "$@"; then echo "ok    $what"; else echo "FAIL  $what" >&2; status=1; fi
}

check "PIE (position-independent executable)" grep -q 'Type:.*DYN' <<<"$header"
check "RELRO segment" grep -q 'GNU_RELRO' <<<"$segments"
check "BIND_NOW (full RELRO)" grep -Eq 'BIND_NOW|FLAGS_1.*Flags:.*NOW' <<<"$dynamic"
check "non-executable stack" bash -c '! grep -E "GNU_STACK" <<<"$1" | grep -q "RWE"' _ "$segments"
check "stack protector (__stack_chk_fail)" grep -q '__stack_chk_fail' <<<"$symbols"
exit "$status"
