#!/usr/bin/env bash
# XeWe LED — discovery deep diagnostics, PHASE 1 (HA Core side).
# RUN ON THE HA "Terminal & SSH" ADD-ON.
#
# WHAT IT DOES:
#   A. Prints HA Core's network interfaces (which one zeroconf binds).
#   B. Turns ON zeroconf + xewe debug logging (temporary; backed up).
#   C. Restarts HA Core  <-- yes, this restarts HA (takes ~1 min).
#   D. Captures HA Core logs for ~2 min while the device advertises.
#
# BEFORE RUNNING: make sure the device is (or can be put) IN DISCOVERY MODE.
# You'll re-trigger it during the capture window when prompted.
#
# Output is saved to a log file; send that file back to Claude.
set -u

if [ -d /homeassistant ]; then CFG=/homeassistant; else CFG=/config; fi
YAML="$CFG/configuration.yaml"
OUT="$CFG/xewe_debug1_$(date +%Y%m%d_%H%M%S).log"
exec > >(tee "$OUT") 2>&1

echo "###################################################################"
echo "# XeWe LED discovery — PHASE 1 (HA Core zeroconf)"
echo "# date:   $(date)"
echo "# output: $OUT"
echo "###################################################################"

echo
echo "================ A. HA Core network interfaces ================"
echo "Look for a leftover NAT interface (10.0.2.x) alongside your LAN (192.168.x)."
ha network info 2>/dev/null || echo "(ha network info failed)"

echo
echo "================ B. Enable zeroconf + xewe debug logging ================"
if grep -qE '^logger:' "$YAML"; then
  echo "A 'logger:' block already exists. Current contents:"
  sed -n '/^logger:/,/^[^[:space:]#]/p' "$YAML"
  echo
  echo ">> Make sure it contains these two lines under logs:, else add them manually:"
  echo "     homeassistant.components.zeroconf: debug"
  echo "     custom_components.xewe_led_os: debug"
else
  BAK="$YAML.bak.$(date +%s)"
  cp "$YAML" "$BAK"
  echo "Backed up configuration.yaml -> $BAK"
  cat >> "$YAML" <<'YML'

# --- XeWe LED debug (temporary) ---
logger:
  default: warning
  logs:
    homeassistant.components.zeroconf: debug
    custom_components.xewe_led_os: debug
# --- end XeWe LED debug ---
YML
  echo "Appended temporary logger block."
fi

echo
echo "================ C. Validate config + restart HA Core ================"
echo "--- ha core check ---"
ha core check
echo "--- restarting HA Core (this takes a minute) ---"
ha core restart
echo "Restart command returned. Waiting 45s for HA to finish booting..."
sleep 45

echo
echo "================ D. Capture logs while device advertises ================"
echo ">>> ACTION REQUIRED: make sure the device is IN DISCOVERY MODE right now."
echo ">>> (Re-trigger it if the 5-min window may have expired.)"
echo ">>> Capturing for ~2 minutes; keep the device advertising the whole time."
for i in 1 2 3 4 5 6; do
  sleep 20
  echo "----- xewe matches @ t=$((i*20))s -----"
  ha core logs 2>/dev/null | grep -iE "xewe|_xewe-led-os" | tail -n 20 || true
done

echo
echo "================ E. Full zeroconf + flow context ================"
echo "--- last 80 zeroconf lines ---"
ha core logs 2>/dev/null | grep -iE "zeroconf" | tail -n 80 || echo "(none)"
echo
echo "--- any config-flow / discovery lines ---"
ha core logs 2>/dev/null | grep -iE "config_flow|flow_impl|discovery|already_configured|no_mac" | tail -n 40 || echo "(none)"

echo
echo "###################################################################"
echo "# DONE. Full log saved to:"
echo "#   $OUT"
echo "# Send that file back to Claude."
echo "#"
echo "# To turn debug logging back OFF later, tell Claude (or delete the"
echo "# '# --- XeWe LED debug (temporary) ---' block in configuration.yaml"
echo "# and restart HA)."
echo "###################################################################"
