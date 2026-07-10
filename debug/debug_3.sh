#!/usr/bin/env bash
# XeWe LED — discovery diagnostics, PHASE 3.
# GOAL: prove whether HA loads the custom integration at startup and therefore
#       registers its zeroconf matcher (_xewe-led-os._tcp.local.).
# RUN ON THE HA "Terminal & SSH" ADD-ON. Restarts HA (the SSH session may drop;
# the script keeps running server-side and writes the log file - just reconnect
# and read it). Keep the device ADVERTISING throughout.
set -u

if [ -d /homeassistant ]; then CFG=/homeassistant; else CFG=/config; fi
OUT="$CFG/xewe_debug3_$(date +%Y%m%d_%H%M%S).log"
exec > >(tee "$OUT") 2>&1

echo "###################################################################"
echo "# XeWe LED discovery — PHASE 3 (startup registration)"
echo "# date:   $(date)"
echo "# output: $OUT"
echo "###################################################################"

echo
echo "================ 1. Restart HA (capture a clean boot) ================"
ha core restart
echo "Restart issued. Waiting 55s for boot..."
sleep 55

echo
echo "================ 2. Is the folder loaded as a CUSTOM integration? ================"
echo "Expect a WARNING like: 'We found a custom integration xewe_led_os ...'"
ha core logs 2>/dev/null | grep -iE "custom integration" | tail -n 40 || echo "(no 'custom integration' lines in buffer)"
echo
echo "--- any mention of xewe_led_os at all ---"
ha core logs 2>/dev/null | grep -iE "xewe_led_os" | tail -n 40 || echo "(none)"

echo
echo "================ 3. Manifest / load / setup errors ================"
ha core logs 2>/dev/null | grep -iE "error|traceback|exception|failed|invalid|manifest" \
  | grep -iE "xewe|zeroconf|custom_component|config_flow" | tail -n 40 || echo "(no matching errors)"

echo
echo "================ 4. Zeroconf startup ================"
ha core logs 2>/dev/null | grep -iE "zeroconf" | grep -iE "start|setup|browser|listen|registers|number of" | tail -n 20 || echo "(no zeroconf startup lines)"

echo
echo "================ 5. Live capture: does HA now browse _xewe-led-os? ================"
echo ">>> Keep the device ADVERTISING. Capturing 90s..."
for i in 1 2 3; do
  sleep 30
  echo "----- _xewe-led-os matches @ t=$((i*30))s -----"
  ha core logs 2>/dev/null | grep -iE "_xewe-led-os" | tail -n 20 || true
done
echo
echo "--- final check: any _xewe-led-os in the whole current buffer ---"
if ha core logs 2>/dev/null | grep -qiE "_xewe-led-os"; then
  echo ">>> SEEN: HA IS browsing the type. Showing lines:"
  ha core logs 2>/dev/null | grep -iE "_xewe-led-os" | tail -n 30
else
  echo ">>> NOT SEEN: HA is NOT browsing _xewe-led-os._tcp (matcher not registered)."
fi

echo
echo "###################################################################"
echo "# DONE. Full log saved to:"
echo "#   $OUT"
echo "# Send that file back to Claude."
echo "###################################################################"
