#!/usr/bin/env bash
# Run every GUI platform script that can run on this host and print one table.
# Usage: test-all.sh [platform...]   platforms: linux wine android freebsd-cross freebsd-vm macos ios-sim  (default: all applicable to host)
# Windows hosts: run test-windows.ps1 directly.
cd "$(dirname "$0")"; HERE=$PWD
case $(uname) in
  Linux) DEF="linux wine android freebsd-cross freebsd-vm" ;; Darwin) DEF="macos ios-sim" ;; FreeBSD) DEF="freebsd" ;; *) DEF="" ;;
esac
P=(${@:-$DEF}); R=(); rc=0
for p in "${P[@]}"; do
    case $p in freebsd-vm) s=run-freebsd-vm.sh ;; wine) s=test-wine.sh ;; *) s=test-$p.sh ;; esac
    echo "######## $p ($s)"; mkdir -p "$HERE/../../build-gui-logs"
    bash "$HERE/$s" 2>&1 | tee "$HERE/../../build-gui-logs/$p.log"; r=${PIPESTATUS[0]}
    if grep -q "^RESULT: .* PASS" "$HERE/../../build-gui-logs/$p.log" && [ $r = 0 ]; then R+=("$p PASS")
    elif grep -q "^  SKIP" "$HERE/../../build-gui-logs/$p.log" && ! grep -q "^  FAIL" "$HERE/../../build-gui-logs/$p.log"; then R+=("$p SKIP")
    else R+=("$p FAIL"); rc=1; fi
done
echo; echo "======== FUSE GUI test-all ========"; for r in "${R[@]}"; do printf "  %-12s %s\n" $r; done
echo "logs: build-gui-logs/"; exit $rc
