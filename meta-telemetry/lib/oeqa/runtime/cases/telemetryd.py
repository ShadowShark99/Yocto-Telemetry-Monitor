#
# SPDX-License-Identifier: MIT
#
# Runtime tests for telemetryd, executed inside the booted image by OEQA:
#
#   bitbake telemetry-image-dev -c testimage     (with TEST_SUITES containing "telemetryd")
#
# Each test runs commands on the target over SSH.

import json
import time

from oeqa.core.decorator.depends import OETestDepends
from oeqa.runtime.case import OERuntimeTestCase
from oeqa.runtime.decorator.package import OEHasPackage

STATUS_FILE = "/run/telemetryd/status.json"


class TelemetrydTest(OERuntimeTestCase):

    def read_status(self):
        status, output = self.target.run("cat %s" % STATUS_FILE)
        if status != 0:
            return None
        try:
            return json.loads(output)
        except ValueError:
            return None

    @OETestDepends(["ssh.SSHTest.test_ssh"])
    @OEHasPackage(["telemetryd"])
    def test_service_active(self):
        status, output = self.target.run("systemctl is-active telemetryd.service")
        if status != 0:
            _, journal = self.target.run("journalctl -u telemetryd --no-pager -n 50")
            self.fail("telemetryd is not active (%s)\n%s" % (output, journal))

    @OETestDepends(["telemetryd.TelemetrydTest.test_service_active"])
    def test_runs_as_unprivileged_user(self):
        status, pid = self.target.run("systemctl show --property MainPID --value telemetryd.service")
        self.assertEqual(status, 0, msg=pid)
        status, uid = self.target.run("awk '/^Uid:/ { print $2 }' /proc/%s/status" % pid.strip())
        self.assertEqual(status, 0, msg=uid)
        status, expected = self.target.run("id -u telemetry")
        self.assertEqual(status, 0, msg=expected)
        self.assertEqual(uid.strip(), expected.strip())
        self.assertNotEqual(uid.strip(), "0", msg="telemetryd must not run as root")

    @OETestDepends(["telemetryd.TelemetrydTest.test_service_active"])
    def test_watchdog_enabled(self):
        status, output = self.target.run("systemctl show --property WatchdogUSec --value telemetryd.service")
        self.assertEqual(status, 0, msg=output)
        self.assertNotIn(output.strip(), ("0", "infinity", ""), msg="WatchdogSec is not set")

    @OETestDepends(["telemetryd.TelemetrydTest.test_service_active"])
    @OEHasPackage(["telemetryd-sim"])
    def test_receives_simulated_telemetry(self):
        # telemetry-sim.service starts at boot and sends 3 sensors at 2 Hz.
        deadline = time.time() + 60
        doc = None
        while time.time() < deadline:
            doc = self.read_status()
            if doc and doc.get("packets_total", 0) >= 20 and len(doc.get("sensors", [])) == 3:
                break
            time.sleep(2)
        self.assertIsNotNone(doc, msg="%s missing or not valid JSON" % STATUS_FILE)
        self.assertGreaterEqual(doc["packets_total"], 20, msg=json.dumps(doc, indent=2))
        self.assertEqual(sorted(s["id"] for s in doc["sensors"]), ["env-01", "env-02", "env-03"])

    @OETestDepends(["telemetryd.TelemetrydTest.test_service_active"])
    def test_reload_keeps_service_running(self):
        status, output = self.target.run("systemctl reload telemetryd.service")
        self.assertEqual(status, 0, msg=output)
        status, output = self.target.run("systemctl is-active telemetryd.service")
        self.assertEqual(status, 0, msg=output)
        status, output = self.target.run(
            "journalctl -u telemetryd --no-pager | grep -c 'configuration reloaded'")
        self.assertEqual(status, 0, msg="no reload message in the journal")

    @OETestDepends(["ssh.SSHTest.test_ssh"])
    @OEHasPackage(["telemetryd-ptest"])
    def test_ptest_suite(self):
        status, output = self.target.run("ptest-runner telemetryd", timeout=300)
        self.assertEqual(status, 0, msg=output)
        self.assertNotIn("FAIL:", output, msg=output)
        self.assertIn("PASS: integration", output, msg=output)
