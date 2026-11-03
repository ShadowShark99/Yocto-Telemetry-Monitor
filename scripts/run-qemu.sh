#!/bin/sh
# Boots the most recent qemux86-64 build in the terminal.
#
#   ./scripts/run-qemu.sh          # with KVM if /dev/kvm is usable
#   ./scripts/run-qemu.sh nokvm    # pure emulation (slow, works anywhere)
#
# Log in as root (no password). Quit QEMU with Ctrl-a x.
# While it runs, from another terminal on the host:
#   ssh -p 2222 root@127.0.0.1
#   ./scripts/send-from-host.sh     # extra sensors over the forwarded UDP port
set -eu
cd "$(dirname "$0")/.."

ACCEL=""
if [ "${1:-}" != "nokvm" ] && [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    ACCEL="kvm"
fi

exec kas shell kas/qemux86-64.yml -c "runqemu qemux86-64 telemetry-image-dev nographic slirp $ACCEL"
