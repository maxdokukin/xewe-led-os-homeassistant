#!/usr/bin/env bash
# XeWe LED — one-shot mDNS discovery diagnostics.
# RUN ON THE HA "Terminal & SSH" ADD-ON, with the device IN DISCOVERY MODE.
# Safe / read-only. Writes everything to a log file and prints its path at the end.
# Send that log file back to Claude.
set -u

# ---- setup ----------------------------------------------------------------
if [ -d /homeassistant ]; then CFG=/homeassistant; else CFG=/config; fi
CC="$CFG/custom_components/xewe_led_os"
SERVICE="_xewe-led-os._tcp.local."
HALOG="$CFG/home-assistant.log"
OUT="$CFG/xewe_debug_$(date +%Y%m%d_%H%M%S).log"

# Send all stdout+stderr from here on to both the screen and the log file.
exec > >(tee "$OUT") 2>&1

echo "###################################################################"
echo "# XeWe LED discovery diagnostics"
echo "# date:   $(date)"
echo "# config: $CFG"
echo "# output: $OUT"
echo "###################################################################"

echo
echo "================ 0. Environment ================"
echo "--- HA / OS versions ---"
ha core info 2>/dev/null | grep -iE 'version|channel' || echo "(ha core info unavailable)"
echo
echo "--- Host network interfaces / addresses ---"
if command -v ip >/dev/null 2>&1; then
  ip -brief addr 2>/dev/null || ip addr
else
  ifconfig 2>/dev/null || echo "(no ip/ifconfig)"
fi

echo
echo "================ 1. Integration files ================"
if [ -d "$CC" ]; then
  echo "OK: $CC exists"
  echo "--- manifest.json ---"
  cat "$CC/manifest.json"
  echo
  echo "--- zeroconf key (must contain exactly $SERVICE) ---"
  grep -i zeroconf "$CC/manifest.json" || echo "!! NO zeroconf key found in manifest"
  echo
  echo "--- files present ---"
  ls -la "$CC"
else
  echo "!! MISSING: $CC  (integration not installed on this HA)"
fi

echo
echo "================ 2. Existing config entry ================"
# If the device is already configured, HA suppresses the 'Discovered' card.
ENTRIES="$CFG/.storage/core.config_entries"
if grep -qi xewe_led_os "$ENTRIES" 2>/dev/null; then
  echo "!! A xewe_led_os config entry EXISTS -> HA will NOT show it under Discovered."
  echo "   To test discovery: Settings > Devices & Services > XeWe LED > Delete, then retry."
  echo "--- matching lines ---"
  grep -i xewe_led_os "$ENTRIES"
else
  echo "OK: no existing xewe_led_os config entry (discovery is not suppressed)."
fi

echo
echo "================ 3. mDNS browse from HA ($SERVICE) ================"
echo "NOTE: runs in the add-on's network namespace. Listening 20s for the advert..."
if command -v python3 >/dev/null 2>&1; then
  python3 - <<'PY'
import time
try:
    from zeroconf import Zeroconf, ServiceBrowser
except Exception as e:
    print("!! python3 has no 'zeroconf' module:", e)
    print("   (skip step 3; rely on the debug-log method in step 5)")
    raise SystemExit(0)
SERVICE = "_xewe-led-os._tcp.local."
found = []
class L:
    def add_service(self, z, t, n):
        i = z.get_service_info(t, n)
        props = {k.decode(): (v.decode() if v else "")
                 for k, v in (i.properties.items() if i else [])}
        addrs = i.parsed_addresses() if i else []
        found.append(n)
        print("FOUND", n, "addrs=", addrs, "txt=", props)
    def update_service(self, *a): pass
    def remove_service(self, *a): pass
z = Zeroconf(); ServiceBrowser(z, SERVICE, L())
time.sleep(20); z.close()
print(">>> RESULT:", "device SEEN by HA" if found else "NOTHING seen")
PY
elif command -v avahi-browse >/dev/null 2>&1; then
  timeout 20 avahi-browse -rpt _xewe-led-os._tcp || true
else
  echo "!! Neither python3+zeroconf nor avahi-browse is available in this shell."
fi

echo
echo "================ 4. Generic mDNS browse (ALL services) ================"
echo "Confirms multicast reaches HA at all. Listening 8s; looking for any _tcp services..."
if command -v python3 >/dev/null 2>&1; then
  python3 - <<'PY'
import time
try:
    from zeroconf import Zeroconf, ServiceBrowser
except Exception as e:
    print("!! no zeroconf module:", e); raise SystemExit(0)
seen = set()
class L:
    def add_service(self, z, t, n): seen.add(n)
    def update_service(self, *a): pass
    def remove_service(self, *a): pass
z = Zeroconf()
# Browse the meta-service that lists all service types being advertised.
ServiceBrowser(z, "_services._dns-sd._udp.local.", L())
time.sleep(8); z.close()
print("service types/instances seen (%d):" % len(seen))
for n in sorted(seen):
    print("  ", n)
print(">>> multicast reaching HA:", "YES" if seen else "NO (no mDNS at all!)")
PY
else
  echo "(python3 unavailable; skipped)"
fi

echo
echo "================ 5. Recent log activity ================"
if [ -f "$HALOG" ]; then
  echo "--- zeroconf/xewe lines in current home-assistant.log (last 60) ---"
  grep -iE "xewe|_xewe-led-os|zeroconf" "$HALOG" | tail -n 60 || echo "(no matches)"
  echo
  echo "--- any recent config_flow / discovery errors (last 30) ---"
  grep -iE "config_flow|discovery|async_step" "$HALOG" | tail -n 30 || echo "(no matches)"
else
  echo "!! no home-assistant.log at $HALOG"
fi

echo
echo "================ 6. Is debug logging on? ================"
if grep -qE '^logger:' "$CFG/configuration.yaml" 2>/dev/null; then
  echo "logger: block present in configuration.yaml:"
  sed -n '/^logger:/,/^[^[:space:]]/p' "$CFG/configuration.yaml"
else
  echo "No 'logger:' block. zeroconf debug logging is OFF."
  echo "If step 3 shows FOUND but no Discovered card, tell Claude - he'll give"
  echo "the next (log-capturing) step."
fi

echo
echo "###################################################################"
echo "# DONE. Full log saved to:"
echo "#   $OUT"
echo "# Send that file back to Claude."
echo "###################################################################"
