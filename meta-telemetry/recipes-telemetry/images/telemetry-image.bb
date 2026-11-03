SUMMARY = "Minimal systemd image running the telemetry monitor"
DESCRIPTION = "Production-style image: boot essentials, an SSH server and \
telemetryd started by systemd. Root login is not enabled; see \
telemetry-image-dev for a development image."
LICENSE = "MIT"

IMAGE_INSTALL = " \
    packagegroup-core-boot \
    telemetryd \
    ${CORE_IMAGE_EXTRA_INSTALL} \
"

IMAGE_FEATURES += "ssh-server-dropbear"

# No locale data: the daemon only ever uses the C locale.
IMAGE_LINGUAS = ""

inherit core-image features_check

# telemetryd is a Type=notify systemd service; refuse to build without systemd
# instead of producing an image where it never starts.
REQUIRED_DISTRO_FEATURES = "systemd"

# Same sizing approach as core-image-minimal, plus headroom for the journal.
IMAGE_ROOTFS_SIZE ?= "8192"
IMAGE_ROOTFS_EXTRA_SPACE:append = "${@bb.utils.contains('DISTRO_FEATURES', 'systemd', ' + 16384', '', d)}"
