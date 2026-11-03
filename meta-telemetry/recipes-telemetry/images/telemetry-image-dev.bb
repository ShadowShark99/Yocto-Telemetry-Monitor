require telemetry-image.bb

SUMMARY = "Development image for the telemetry monitor"
DESCRIPTION = "telemetry-image plus the sensor simulator (started at boot so \
there is live data to look at), the on-target test suite, debugging tools \
and passwordless root login. Never ship this image."

IMAGE_INSTALL += " \
    telemetryd-sim \
    ${@bb.utils.contains('DISTRO_FEATURES', 'ptest', 'telemetryd-ptest ptest-runner', '', d)} \
    strace \
"

# Development conveniences only: root can log in on the console and over
# SSH with an empty password. These are the features the old "debug-tweaks"
# bundled together.
IMAGE_FEATURES += "allow-empty-password empty-root-password allow-root-login post-install-logging"
