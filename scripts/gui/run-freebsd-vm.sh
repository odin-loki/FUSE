#!/usr/bin/env bash
# Boot a FreeBSD 14.4 cloud image in qemu, copy the GUI sources in and run test-freebsd.sh inside it.
# Linux host; needs qemu-system-x86_64, qemu-img, xorriso (or genisoimage), ssh, curl, xz (self-installed via apt when possible).
# Env: FREEBSD_VERSION (14.4), VM_DIR (~/.cache/fuse-gui/freebsd), VM_ACCEL (kvm|tcg; default kvm if /dev/kvm is writable —
#      set tcg where nested KVM hangs), VM_CPUS (4), VM_MEM (3072), VM_PORT (2244), KEEP_VM=1 leaves the VM running.
source "$(dirname "$0")/lib.sh"
V=${FREEBSD_VERSION:-14.4}; D=${VM_DIR:-$HOME/.cache/fuse-gui/freebsd}; PORT=${VM_PORT:-2244}
ACCEL=${VM_ACCEL:-$([ -w /dev/kvm ] && echo kvm || echo tcg)}
IMG=FreeBSD-$V-RELEASE-amd64-BASIC-CLOUDINIT-ufs.qcow2; mkdir -p "$D"; cd "$D"
if want_deps && ! command -v qemu-system-x86_64 >/dev/null && command -v apt-get >/dev/null; then
    step "deps (apt qemu)" as_root apt-get install -y -q --no-install-recommends qemu-system-x86 qemu-utils xorriso >/dev/null
fi
[ -f "$IMG" ] || step "download $IMG" bash -c "curl -sfL https://download.freebsd.org/releases/VM-IMAGES/$V-RELEASE/amd64/Latest/$IMG.xz | xz -dT0 > $IMG.part && mv $IMG.part $IMG"
[ -f vmkey ] || ssh-keygen -q -t ed25519 -N '' -f vmkey
SSH=(ssh -i "$D/vmkey" -p "$PORT" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 -o LogLevel=ERROR fuse@127.0.0.1)
if ! "${SSH[@]}" true 2>/dev/null; then
    mkdir -p seed; printf 'instance-id: fuse-gui\nlocal-hostname: fuse-gui\n' > seed/meta-data
    printf '#cloud-config\nusers:\n  - name: fuse\n    groups: wheel\n    shell: /bin/sh\n    sudo: ALL=(ALL) NOPASSWD:ALL\n    ssh_authorized_keys: ["%s"]\n' "$(cat vmkey.pub)" > seed/user-data
    if command -v xorriso >/dev/null; then xorriso -as mkisofs -quiet -o seed.iso -V cidata -J -r seed; else genisoimage -quiet -o seed.iso -V cidata -J -r seed; fi
    [ -f vm.qcow2 ] || { qemu-img create -q -f qcow2 -F qcow2 -b "$IMG" vm.qcow2 40G; }
    acc=(-accel kvm -cpu host); [ "$ACCEL" = tcg ] && acc=(-accel tcg,thread=multi,tb-size=512 -cpu Nehalem)
    step "boot VM ($ACCEL)" qemu-system-x86_64 "${acc[@]}" -smp "${VM_CPUS:-4}" -m "${VM_MEM:-3072}" \
        -drive file=vm.qcow2,if=virtio,cache=unsafe -drive file=seed.iso,media=cdrom \
        -nic user,model=virtio-net-pci,hostfwd=tcp:127.0.0.1:$PORT-:22 -display none -serial file:serial.log -daemonize -pidfile vm.pid
    wait_ssh() { for _ in $(seq 180); do "${SSH[@]}" true 2>/dev/null && return 0; sleep 10; done; return 1; }
    step "wait for ssh" wait_ssh
fi
copy() { cd "$ROOT" && git ls-files -z --cached --others --exclude-standard Source/FUSE/UI Source/FUSE/Core Source/FUSE/Relight cmake scripts/gui vendor/forge vendor/fonts vendor/stb vendor/sqlite vendor/lua vendor/xiph 'vendor/xiph-*' \
         | tar --null -T - -czf - | "${SSH[@]}" 'rm -rf fuse && mkdir fuse && tar -xzf - -C fuse'; }
step "copy sources" copy
# The BASIC-CLOUDINIT image has no sudo and its pkg repo uses SRV mirrors (fails behind many proxies). root has no
# password in the cloud image, so bootstrap pkg over plain HTTPS + sudo/bash via su once.
prep() { "${SSH[@]}" "cat > /tmp/fuse-prep.sh; echo | su -m root -c 'sh /tmp/fuse-prep.sh'" <<'PREP'
mkdir -p /usr/local/etc/pkg/repos
printf 'FreeBSD: { url: "https://pkg.FreeBSD.org/${ABI}/quarterly", mirror_type: "none" }\nFreeBSD-kmods: { enabled: no }\n' > /usr/local/etc/pkg/repos/FreeBSD.conf
export ASSUME_ALWAYS_YES=yes
pkg bootstrap -f >/dev/null 2>&1 || pkg -N >/dev/null || exit 1
pkg install -y sudo bash >/dev/null || exit 1
echo "%wheel ALL=(ALL) NOPASSWD: ALL" > /usr/local/etc/sudoers.d/wheel
PREP
}
"${SSH[@]}" 'command -v sudo >/dev/null && command -v bash >/dev/null' || step "VM prep (pkg, sudo, bash)" prep
step "test-freebsd.sh (in VM)" "${SSH[@]}" "cd fuse && bash scripts/gui/test-freebsd.sh"
[ "${KEEP_VM:-0}" = 1 ] || { "${SSH[@]}" 'sudo shutdown -p now' >/dev/null 2>&1; }
summary freebsd-vm
