SUMMARY = "Telemetry monitor daemon for embedded sensor nodes"
DESCRIPTION = "telemetryd receives sensor packets over UDP or a serial port, \
validates them, tracks per-sensor health (threshold alarms, stale sensors, \
packet loss) and reports through the journal, a JSON status file and the \
systemd watchdog. telemetry-sim generates realistic test traffic."
HOMEPAGE = "https://github.com/your-user/yocto-telemetry-monitor"
SECTION = "console/utils"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=0a4d76bd15a5868f306a5b7877e6e790"

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
# The application lives in the same repository as this layer, three levels
# up from this file, so FILESEXTRAPATHS lets file:// find "app/". `:=`
# expands THISDIR now, while this recipe is being parsed.
#
# A product layer would normally fetch a pinned release instead:
#   SRC_URI = "git://github.com/your-user/yocto-telemetry-monitor.git;protocol=https;branch=main"
#   SRCREV = "<full commit hash>"
#   S = "${UNPACKDIR}/${BP}/app"
FILESEXTRAPATHS:prepend := "${THISDIR}/../../..:"

SRC_URI = " \
    file://app \
    file://telemetryd.service \
    file://telemetry-sim.service \
    file://telemetryd.conf \
    file://run-ptest \
"

S = "${UNPACKDIR}/app"

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
# cmake:   configure/compile/install with the cross toolchain file
# systemd: package and enable the unit files below
# useradd: create the unprivileged "telemetry" account at rootfs time
# ptest:   package the unit + integration tests as telemetryd-ptest
inherit cmake systemd useradd ptest

# GoogleTest is only needed when the distro builds ptest packages.
DEPENDS:append = "${@bb.utils.contains('PTEST_ENABLED', '1', ' googletest', '', d)}"

EXTRA_OECMAKE = " \
    -DTELEMETRY_BUILD_TESTS=${@bb.utils.contains('PTEST_ENABLED', '1', 'ON', 'OFF', d)} \
    -DTELEMETRY_FETCH_GTEST=OFF \
"

do_install:append() {
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/telemetryd.service ${D}${systemd_system_unitdir}/
    install -m 0644 ${UNPACKDIR}/telemetry-sim.service ${D}${systemd_system_unitdir}/

    install -d ${D}${sysconfdir}/telemetryd
    install -m 0644 ${UNPACKDIR}/telemetryd.conf ${D}${sysconfdir}/telemetryd/telemetryd.conf
}

# run-ptest itself is installed by ptest.bbclass from ${UNPACKDIR}.
do_install_ptest() {
    install -m 0755 ${B}/tests/telemetry_tests ${D}${PTEST_PATH}/
    install -m 0755 ${S}/tests/integration_test.sh ${D}${PTEST_PATH}/
}

# ---------------------------------------------------------------------------
# Packaging
# ---------------------------------------------------------------------------
# telemetryd        the daemon, its unit file and configuration
# telemetryd-sim    the traffic generator and its (demo) unit file
# telemetryd-ptest  on-target tests, run with `ptest-runner telemetryd`
PACKAGES =+ "${PN}-sim"
FILES:${PN}-sim = " \
    ${bindir}/telemetry-sim \
    ${systemd_system_unitdir}/telemetry-sim.service \
"

SYSTEMD_PACKAGES = "${PN} ${PN}-sim"
SYSTEMD_SERVICE:${PN} = "telemetryd.service"
SYSTEMD_SERVICE:${PN}-sim = "telemetry-sim.service"
SYSTEMD_AUTO_ENABLE = "enable"

# Package managers keep local edits to this file across upgrades.
CONFFILES:${PN} = "${sysconfdir}/telemetryd/telemetryd.conf"

RDEPENDS:${PN}-ptest += "${PN}-sim"

# ---------------------------------------------------------------------------
# Users
# ---------------------------------------------------------------------------
# The daemon runs as "telemetry", a system account in the dialout group so it
# can open serial devices without root.
USERADD_PACKAGES = "${PN}"
USERADD_PARAM:${PN} = "--system --no-create-home --home-dir /nonexistent \
                       --shell /bin/false --user-group --groups dialout telemetry"
