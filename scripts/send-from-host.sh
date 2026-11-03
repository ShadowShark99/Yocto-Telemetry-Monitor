#!/bin/sh
# Builds the simulator for the host (if needed) and sends telemetry into the
# QEMU guest through the forwarded UDP port (see QB_SLIRP_OPT in
# kas/qemux86-64.yml). The sensors show up on the target as host-01, host-02.
#
#   ./scripts/send-from-host.sh [extra telemetry-sim options]
#   ./scripts/send-from-host.sh --spike-every 20
set -eu
cd "$(dirname "$0")/.."

if [ ! -x build-host/telemetry-sim ]; then
    cmake -S app -B build-host -DCMAKE_BUILD_TYPE=Release
    cmake --build build-host --target telemetry-sim
fi

exec build-host/telemetry-sim --host 127.0.0.1 --port 5500 --sensors 2 --id-prefix host --rate 1 "$@"
