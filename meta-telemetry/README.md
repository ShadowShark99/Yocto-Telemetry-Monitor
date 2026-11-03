# meta-telemetry

Yocto Project layer that builds and integrates the telemetry monitor
(`../app`) into a systemd-based Linux image.

| Item | |
|---|---|
| Compatible releases | `wrynose` (Yocto Project 6.0 LTS) |
| Depends on | `openembedded-core` (`meta`), `meta-openembedded` (`meta-oe`) |
| Tested distro | `poky` with `INIT_MANAGER = "systemd"` |

## Contents

```
conf/layer.conf                         layer registration, dependencies, release compatibility
recipes-telemetry/telemetryd/
  telemetryd_1.0.bb                     cmake + systemd + useradd + ptest recipe
  files/telemetryd.service              Type=notify unit with watchdog and sandboxing
  files/telemetry-sim.service           demo traffic generator unit
  files/telemetryd.conf                 default /etc/telemetryd/telemetryd.conf
  files/rpi/telemetryd.conf             Raspberry Pi override (USB serial input)
  files/run-ptest                       on-target test runner
recipes-telemetry/images/
  telemetry-image.bb                    minimal production-style image
  telemetry-image-dev.bb                + simulator, ptests, strace, root login
lib/oeqa/runtime/cases/telemetryd.py    OEQA runtime tests for `-c testimage`
```

## Packages

| Package | Contents |
|---|---|
| `telemetryd` | `/usr/bin/telemetryd`, unit file, `/etc/telemetryd/telemetryd.conf`, `telemetry` user |
| `telemetryd-sim` | `/usr/bin/telemetry-sim`, demo unit file |
| `telemetryd-ptest` | GoogleTest binary, integration test, `run-ptest` |

## Using the layer without kas

```sh
bitbake-layers add-layer /path/to/meta-openembedded/meta-oe
bitbake-layers add-layer /path/to/yocto-telemetry-monitor/meta-telemetry
echo 'INIT_MANAGER = "systemd"' >> conf/local.conf
bitbake telemetry-image-dev
```

## Customising per product

Override the configuration from your own layer with a bbappend:

```
# meta-myproduct/recipes-telemetry/telemetryd/telemetryd_%.bbappend
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
```

and put your `telemetryd.conf` in `meta-myproduct/recipes-telemetry/telemetryd/files/`.
