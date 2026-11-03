#!/bin/sh
# Prepares an Ubuntu (22.04 / 24.04 / 26.04) machine to build the image:
# Yocto's host packages, the en_US.UTF-8 locale, a user-namespace setting
# BitBake needs, and kas.
#
# Usage: ./scripts/setup-host-ubuntu.sh
set -eu

echo "==> Installing Yocto Project host packages"
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential chrpath cpio debianutils diffstat file gawk gcc git \
    iputils-ping libacl1 locales lz4 pipx python3 python3-git python3-jinja2 \
    python3-pexpect python3-pip python3-subunit python3-websockets socat \
    texinfo unzip wget xz-utils zstd

echo "==> Generating the en_US.UTF-8 locale (BitBake requires it)"
sudo locale-gen en_US.UTF-8

# BitBake runs tasks in a user namespace with networking removed, so only
# do_fetch can reach the internet. Ubuntu 24.04 and later restrict
# unprivileged user namespaces through AppArmor, which makes BitBake stop with
# "User namespaces are not usable by BitBake, possibly due to AppArmor".
# This lifts the restriction system-wide. On a shared or security-sensitive
# machine, prefer an AppArmor profile for BitBake instead.
if [ -e /proc/sys/kernel/apparmor_restrict_unprivileged_userns ]; then
    echo "==> Allowing unprivileged user namespaces (needed by BitBake)"
    echo "kernel.apparmor_restrict_unprivileged_userns = 0" |
        sudo tee /etc/sysctl.d/60-bitbake-userns.conf >/dev/null
    sudo sysctl --quiet -p /etc/sysctl.d/60-bitbake-userns.conf
fi

echo "==> Installing kas"
if command -v kas >/dev/null 2>&1; then
    pipx upgrade kas || true
else
    pipx install kas
    pipx ensurepath
fi

echo
echo "Done. Open a new shell if 'kas' is not on your PATH yet, then run:"
echo "  kas build kas/qemux86-64.yml"
