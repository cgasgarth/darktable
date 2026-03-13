#!/usr/bin/env bash
set -euo pipefail

script_path=$(readlink -f "$0")
script_dir=$(dirname "$script_path")
repo_root=$(dirname "$script_dir")

validation_timeout_seconds=${DARKTABLE_LIVE_BRIDGE_TIMEOUT_SECONDS:-15}
helper_timeout_seconds=${DARKTABLE_LIVE_BRIDGE_HELPER_TIMEOUT_SECONDS:-5}
ready_attempts=${DARKTABLE_LIVE_BRIDGE_READY_ATTEMPTS:-8}
post_set_attempts=${DARKTABLE_LIVE_BRIDGE_POST_SET_ATTEMPTS:-4}

if [[ ${1:-} != "--inner" ]]; then
  exec timeout --signal=KILL "${validation_timeout_seconds}s" \
    dbus-run-session -- "$script_path" --inner "$@"
fi
shift

source_asset_path=${DARKTABLE_LIVE_BRIDGE_ASSET:-/home/cgasgarth/Documents/projects/aiPhotoEditing/darktableAI/assets/_DSC8809.ARW}
requested_exposure=${DARKTABLE_LIVE_BRIDGE_EXPOSURE:-1.25}
requested_control_exposure=${DARKTABLE_LIVE_BRIDGE_CONTROL_EXPOSURE:-0.5}
requested_blend_opacity=${DARKTABLE_LIVE_BRIDGE_BLEND_OPACITY:-73}
darktable_bin=${DARKTABLE_LIVE_BRIDGE_DARKTABLE:-$repo_root/build/bin/darktable}
bridge_bin=${DARKTABLE_LIVE_BRIDGE_HELPER:-$repo_root/build/bin/darktable-live-bridge}
tmux_session=${DARKTABLE_LIVE_BRIDGE_TMUX_SESSION:-darktable-live-validate-$$}
tmux_socket=${DARKTABLE_LIVE_BRIDGE_TMUX_SOCKET:-darktable-live-validate-$$}

if [[ ! -x "$darktable_bin" ]]; then
  echo "missing darktable binary: $darktable_bin" >&2
  exit 1
fi

if [[ ! -x "$bridge_bin" ]]; then
  echo "missing darktable-live-bridge binary: $bridge_bin" >&2
  exit 1
fi

if [[ ! -f "$source_asset_path" ]]; then
  echo "missing asset: $source_asset_path" >&2
  exit 1
fi

run_root=$(mktemp -d)
config_dir="$run_root/config"
cache_dir="$run_root/cache"
tmp_dir="$run_root/tmp"
runtime_dir="$run_root/runtime"
asset_dir="$run_root/asset"
library_path="$run_root/library.db"
darktable_log="$run_root/darktable.log"
mkdir -p "$config_dir" "$cache_dir" "$tmp_dir" "$runtime_dir" "$asset_dir"
chmod 700 "$runtime_dir"
printf 'ui/show_welcome_screen=false\n' >"$config_dir/darktablerc"

asset_path="$asset_dir/$(basename "$source_asset_path")"
cp -- "$source_asset_path" "$asset_path"

cleanup() {
  tmux -L "$tmux_socket" kill-session -t "$tmux_session" 2>/dev/null || true
  tmux -L "$tmux_socket" kill-server 2>/dev/null || true
  rm -rf "$run_root"
}
trap cleanup EXIT

capture_tmux_log() {
  tmux -L "$tmux_socket" capture-pane -p -S -200 -t "$tmux_session" 2>/dev/null || true
}

fail() {
  echo "$1" >&2
  if [[ -f "$darktable_log" ]]; then
    echo "--- darktable.log tail ---" >&2
    tail -n 80 "$darktable_log" >&2 || true
  fi
  local pane_log
  pane_log=$(capture_tmux_log)
  if [[ -n "$pane_log" ]]; then
    echo "--- tmux pane tail ---" >&2
    printf '%s\n' "$pane_log" >&2
  fi
  exit 1
}

start_darktable_host() {
  local command
  command=$(cat <<HOST
export NO_AT_BRIDGE=1
export GDK_BACKEND=x11
export GIO_USE_VFS=local
export GVFS_DISABLE_FUSE=1
export XDG_RUNTIME_DIR='$runtime_dir'
exec xvfb-run -a --server-args='-screen 0 1280x800x24' \
  '$darktable_bin' \
  --configdir '$config_dir' \
  --cachedir '$cache_dir' \
  --tmpdir '$tmp_dir' \
  --library '$library_path' \
  --disable-opencl \
  '$asset_path' >'$darktable_log' 2>&1
HOST
)
  tmux -L "$tmux_socket" new-session -d -s "$tmux_session" bash -lc "$command"
}

ensure_tmux_session_alive() {
  if ! tmux -L "$tmux_socket" has-session -t "$tmux_session" 2>/dev/null; then
    fail "darktable tmux session exited unexpectedly"
  fi
}

wait_for_remote_lua() {
  local attempt
  for attempt in $(seq 1 "$ready_attempts"); do
    ensure_tmux_session_alive
    if gdbus call \
      --session \
      --timeout 1 \
      --dest org.darktable.service \
      --object-path /darktable \
      --method org.darktable.service.Remote.Lua \
      "return 'ready'" >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  fail "timed out waiting for Remote.Lua readiness"
}

run_bridge() {
  timeout --signal=KILL "${helper_timeout_seconds}s" "$bridge_bin" "$@"
}

run_remote_lua() {
  gdbus call \
    --session \
    --timeout 1 \
    --dest org.darktable.service \
    --object-path /darktable \
    --method org.darktable.service.Remote.Lua \
    "$1"
}

switch_to_lighttable() {
  run_remote_lua "local dt = require 'darktable'; dt.gui.current_view(dt.gui.views.lighttable); return tostring(dt.gui.current_view())" >/dev/null
}

switch_to_darkroom() {
  run_remote_lua "local dt = require 'darktable'; dt.gui.current_view(dt.gui.views.darkroom); return tostring(dt.gui.current_view())" >/dev/null
}

wait_for_session_payload() {
  local attempts=$1
  local expected_exposure=${2:-}
  local attempt json
  for attempt in $(seq 1 "$attempts"); do
    ensure_tmux_session_alive
    if json=$(run_bridge get-session 2>/dev/null); then
      if python3 - "$json" "$asset_path" "$expected_exposure" <<'PY'
import json, math, os, sys
payload = json.loads(sys.argv[1])
asset = os.path.realpath(sys.argv[2])
expected_exposure = sys.argv[3]
active = payload.get('activeImage') or {}
source = active.get('sourceAssetPath')
if payload.get('status') != 'ok' or not source or os.path.realpath(source) != asset:
    raise SystemExit(1)
if expected_exposure:
    requested = float(expected_exposure)
    exposure = (payload.get('exposure') or {}).get('current')
    if not isinstance(exposure, (int, float)) or math.isnan(exposure) or abs(exposure - requested) > 1e-6:
        raise SystemExit(1)
raise SystemExit(0)
PY
      then
        printf '%s\n' "$json"
        return 0
      fi
    fi
    sleep 1
  done
  if [[ -n "$expected_exposure" ]]; then
    fail "timed out waiting for post-set exposure readback"
  fi
  fail "timed out waiting for active darkroom session"
}

start_darktable_host
wait_for_remote_lua

initial_json=$(wait_for_session_payload "$ready_attempts")
snapshot_json=$(run_bridge get-snapshot)
blend_target_json=$(python3 - "$snapshot_json" "$requested_blend_opacity" <<'PY'
import json, sys

snapshot = json.loads(sys.argv[1])
requested = float(sys.argv[2])
module_stack = ((snapshot.get('snapshot') or {}).get('moduleStack') or [])

for item in module_stack:
    if not isinstance(item, dict):
        continue
    blend = item.get('blend') or {}
    if blend.get('supported') is not True:
        continue
    instance_key = item.get('instanceKey')
    opacity = blend.get('opacity')
    blend_mode = blend.get('blendMode')
    reverse_order = blend.get('reverseOrder')
    blend_colorspace = blend.get('blendColorspace')
    if not isinstance(instance_key, str) or not instance_key:
        continue
    if not isinstance(opacity, (int, float)):
        continue
    if not isinstance(blend_mode, str) or not blend_mode:
        continue
    if not isinstance(reverse_order, bool):
        continue
    if not isinstance(blend_colorspace, str) or not blend_colorspace:
        continue
    target = requested
    if abs(target - opacity) <= 1e-6:
        target = 27.0 if abs(opacity - 27.0) > 1e-6 else 61.0
    requested_blend_mode = 'multiply' if blend_mode != 'multiply' else 'normal'
    invalid_blend_mode = {
        'lab': 'divide',
        'rgb-display': 'divide',
        'rgb-scene': 'screen',
        'raw': 'divide',
    }.get(blend_colorspace)
    print(json.dumps({
         'instanceKey': instance_key,
         'moduleOp': item.get('moduleOp'),
         'blendColorspace': blend_colorspace,
         'previousOpacity': opacity,
         'requestedOpacity': target,
         'previousBlendMode': blend_mode,
         'requestedBlendMode': requested_blend_mode,
         'invalidBlendMode': invalid_blend_mode,
         'previousReverseOrder': reverse_order,
         'requestedReverseOrder': (not reverse_order),
      }, separators=(',', ':')))
    break
else:
    raise SystemExit('no blend-capable visible module available')
PY
)
blend_target_key=$(python3 - "$blend_target_json" <<'PY'
import json, sys
print(json.loads(sys.argv[1])['instanceKey'])
PY
)
blend_requested_opacity=$(python3 - "$blend_target_json" <<'PY'
import json, sys
print(json.loads(sys.argv[1])['requestedOpacity'])
PY
)
blend_previous_opacity=$(python3 - "$blend_target_json" <<'PY'
import json, sys
print(json.loads(sys.argv[1])['previousOpacity'])
PY
)
blend_requested_mode=$(python3 - "$blend_target_json" <<'PY'
import json, sys
print(json.loads(sys.argv[1])['requestedBlendMode'])
PY
)
blend_previous_mode=$(python3 - "$blend_target_json" <<'PY'
import json, sys
print(json.loads(sys.argv[1])['previousBlendMode'])
PY
)
blend_invalid_mode=$(python3 - "$blend_target_json" <<'PY'
import json, sys
value = json.loads(sys.argv[1]).get('invalidBlendMode')
print(value if isinstance(value, str) else '')
PY
)
blend_requested_reverse=$(python3 - "$blend_target_json" <<'PY'
import json, sys
print('true' if json.loads(sys.argv[1])['requestedReverseOrder'] else 'false')
PY
)
blend_previous_reverse=$(python3 - "$blend_target_json" <<'PY'
import json, sys
print('true' if json.loads(sys.argv[1])['previousReverseOrder'] else 'false')
PY
)
blend_apply_json=$(run_bridge apply-module-instance-blend "$blend_target_key" "{\"opacity\":$blend_requested_opacity,\"blendMode\":\"$blend_requested_mode\",\"reverseOrder\":$blend_requested_reverse}")
blend_followup_snapshot_json=$(run_bridge get-snapshot)
blend_revert_json=$(run_bridge apply-module-instance-blend "$blend_target_key" "{\"opacity\":$blend_previous_opacity,\"blendMode\":\"$blend_previous_mode\",\"reverseOrder\":$blend_previous_reverse}")
if [[ -n "$blend_invalid_mode" ]]; then
  invalid_blend_mode_json=$(run_bridge apply-module-instance-blend "$blend_target_key" "{\"blendMode\":\"$blend_invalid_mode\"}")
else
  invalid_blend_mode_json='{"status":"skipped","reason":"no-invalid-blend-mode-for-target"}'
fi
unsupported_blend_target_json=$(python3 - "$snapshot_json" <<'PY'
import json, sys
snapshot = json.loads(sys.argv[1])
module_stack = ((snapshot.get('snapshot') or {}).get('moduleStack') or [])
for item in module_stack:
    if not isinstance(item, dict):
        continue
    blend = item.get('blend') or {}
    if blend.get('supported') is False and isinstance(item.get('instanceKey'), str) and item.get('instanceKey'):
        print(json.dumps({
            'status': 'found',
            'instanceKey': item['instanceKey'],
            'moduleOp': item.get('moduleOp'),
        }, separators=(',', ':')))
        break
else:
    print(json.dumps({'status': 'skipped', 'reason': 'no-visible-unsupported-blend-module'}, separators=(',', ':')))
PY
)
unsupported_blend_json=$(python3 - "$unsupported_blend_target_json" "$bridge_bin" <<'PY'
import json, subprocess, sys
target = json.loads(sys.argv[1])
bridge = sys.argv[2]
if target.get('status') != 'found':
    print(json.dumps(target, separators=(',', ':')))
else:
    payload = subprocess.check_output(
        [bridge, 'apply-module-instance-blend', target['instanceKey'], '{"opacity":55}'],
        text=True,
    )
    print(payload.strip())
PY
)
mask_target_json=$(python3 - "$snapshot_json" <<'PY'
import json, sys
snapshot = json.loads(sys.argv[1])
module_stack = ((snapshot.get('snapshot') or {}).get('moduleStack') or [])
for item in module_stack:
    if not isinstance(item, dict):
        continue
    blend = item.get('blend') or {}
    if blend.get('supported') is True and blend.get('masksSupported') is True and isinstance(item.get('instanceKey'), str) and item.get('instanceKey'):
        print(json.dumps({
            'instanceKey': item['instanceKey'],
            'moduleOp': item.get('moduleOp'),
        }, separators=(',', ':')))
        break
else:
    raise SystemExit('no masks-supported target available')
PY
)
mask_target_key=$(python3 - "$mask_target_json" <<'PY'
import json, sys
print(json.loads(sys.argv[1])['instanceKey'])
PY
)
mask_reuse_target_json=$(python3 - "$snapshot_json" "$mask_target_key" <<'PY'
import json, sys
snapshot = json.loads(sys.argv[1])
source_key = sys.argv[2]
module_stack = ((snapshot.get('snapshot') or {}).get('moduleStack') or [])
for item in module_stack:
    if not isinstance(item, dict):
        continue
    blend = item.get('blend') or {}
    if blend.get('supported') is True and blend.get('masksSupported') is True and item.get('instanceKey') != source_key:
        print(json.dumps({
            'status': 'found',
            'instanceKey': item.get('instanceKey'),
            'moduleOp': item.get('moduleOp'),
        }, separators=(',', ':')))
        break
else:
    print(json.dumps({'status': 'skipped', 'reason': 'no-secondary-mask-target'}, separators=(',', ':')))
PY
)
mask_reuse_json=$(python3 - "$mask_reuse_target_json" "$bridge_bin" "$mask_target_key" <<'PY'
import json, subprocess, sys
target = json.loads(sys.argv[1])
bridge = sys.argv[2]
source = sys.argv[3]
if target.get('status') != 'found':
    print(json.dumps(target, separators=(',', ':')))
else:
    payload = subprocess.check_output(
        [bridge, 'apply-module-instance-mask', target['instanceKey'], json.dumps({'action': 'reuse-same-shapes', 'sourceInstanceKey': source}, separators=(',', ':'))],
        text=True,
    )
    print(payload.strip())
PY
)
mask_clear_json=$(run_bridge apply-module-instance-mask "$mask_target_key" '{"action":"clear-mask"}')
mask_unknown_source_json=$(run_bridge apply-module-instance-mask "$mask_target_key" '{"action":"reuse-same-shapes","sourceInstanceKey":"missing#0#0#"}')
module_instance_target_json=$(python3 - "$snapshot_json" <<'PY'
import json, sys
snapshot = json.loads(sys.argv[1])
module_stack = ((snapshot.get('snapshot') or {}).get('moduleStack') or [])
target = None
for item in module_stack:
    if not isinstance(item, dict):
        continue
    if not isinstance(item.get('instanceKey'), str) or not isinstance(item.get('enabled'), bool):
        continue
    if item.get('moduleOp') == 'exposure':
        target = item
        break
    if target is None:
        target = item
if target is None:
    raise SystemExit('no module stack target available')
action = 'disable' if target['enabled'] else 'enable'
revert = 'enable' if action == 'disable' else 'disable'
print(json.dumps({
    'instanceKey': target['instanceKey'],
    'moduleOp': target.get('moduleOp'),
    'multiPriority': target.get('multiPriority'),
    'previousEnabled': target['enabled'],
    'action': action,
    'revertAction': revert,
}, separators=(',', ':')))
PY
)
module_instance_key=$(python3 - "$module_instance_target_json" <<'PY'
import json, sys
print(json.loads(sys.argv[1])['instanceKey'])
PY
)
module_instance_action=$(python3 - "$module_instance_target_json" <<'PY'
import json, sys
print(json.loads(sys.argv[1])['action'])
PY
)
module_instance_revert_action=$(python3 - "$module_instance_target_json" <<'PY'
import json, sys
print(json.loads(sys.argv[1])['revertAction'])
PY
)
list_json=$(run_bridge list-controls)
get_control_json=$(run_bridge get-control exposure.exposure)
set_json=$(run_bridge set-exposure "$requested_exposure")
post_set_exposure_json=$(wait_for_session_payload "$post_set_attempts" "$requested_exposure")
set_control_json=$(run_bridge set-control exposure.exposure "$requested_control_exposure")
post_set_control_json=$(wait_for_session_payload "$post_set_attempts" "$requested_control_exposure")
unsupported_control_json=$(run_bridge get-control unsupported.control)
module_instance_action_json=$(run_bridge apply-module-instance-action "$module_instance_key" "$module_instance_action")
module_instance_revert_json=$(run_bridge apply-module-instance-action "$module_instance_key" "$module_instance_revert_action")
module_instance_create_json=$(run_bridge apply-module-instance-action "$module_instance_key" create)
module_instance_duplicate_json=$(run_bridge apply-module-instance-action "$module_instance_key" duplicate)
duplicate_result_target_json=$(python3 - "$module_instance_duplicate_json" <<'PY'
import json, sys
payload = json.loads(sys.argv[1])
action = payload.get('moduleAction') or {}
snapshot = payload.get('snapshot') or {}
result_key = action.get('resultInstanceKey')
if not isinstance(result_key, str) or not result_key:
    raise SystemExit('duplicate resultInstanceKey missing')
module_stack = snapshot.get('moduleStack') or []
result_item = None
for item in module_stack:
    if isinstance(item, dict) and item.get('instanceKey') == result_key:
        result_item = item
        break
if result_item is None:
    raise SystemExit('duplicate result snapshot item missing')
enabled = result_item.get('enabled')
if not isinstance(enabled, bool):
    raise SystemExit('duplicate result enabled state missing')
print(json.dumps({
    'instanceKey': result_key,
    'action': 'disable' if enabled else 'enable',
    'previousEnabled': enabled,
}, separators=(',', ':')))
PY
)
duplicate_result_key=$(python3 - "$duplicate_result_target_json" <<'PY'
import json, sys
print(json.loads(sys.argv[1])['instanceKey'])
PY
)
duplicate_result_action=$(python3 - "$duplicate_result_target_json" <<'PY'
import json, sys
print(json.loads(sys.argv[1])['action'])
PY
)
duplicate_result_previous_enabled=$(python3 - "$duplicate_result_target_json" <<'PY'
import json, sys
print(json.loads(sys.argv[1])['previousEnabled'])
PY
)
duplicate_result_toggle_json=$(run_bridge apply-module-instance-action "$duplicate_result_key" "$duplicate_result_action")
module_reorder_target_json=$(python3 - "$module_instance_duplicate_json" "$module_instance_target_json" <<'PY'
import json, sys
duplicate_payload = json.loads(sys.argv[1])
source_payload = json.loads(sys.argv[2])
action = duplicate_payload.get('moduleAction') or {}
source_key = source_payload.get('instanceKey')
result_key = action.get('resultInstanceKey')
if not isinstance(source_key, str) or not source_key:
    raise SystemExit('missing source instance key for reorder test')
if not isinstance(result_key, str) or not result_key:
    raise SystemExit('missing duplicate instance key for reorder test')
print(json.dumps({
    'targetInstanceKey': result_key,
    'anchorInstanceKey': source_key,
}, separators=(',', ':')))
PY
)
module_reorder_target_key=$(python3 - "$module_reorder_target_json" <<'PY'
import json, sys
print(json.loads(sys.argv[1])['targetInstanceKey'])
PY
)
module_reorder_anchor_key=$(python3 - "$module_reorder_target_json" <<'PY'
import json, sys
print(json.loads(sys.argv[1])['anchorInstanceKey'])
PY
)
module_reorder_move_before_json=$(run_bridge apply-module-instance-action "$module_reorder_target_key" move-before "$module_reorder_anchor_key")
module_reorder_move_before_noop_json=$(run_bridge apply-module-instance-action "$module_reorder_target_key" move-before "$module_reorder_anchor_key")
module_reorder_move_after_json=$(run_bridge apply-module-instance-action "$module_reorder_target_key" move-after "$module_reorder_anchor_key")
post_reorder_snapshot_json=$(run_bridge get-snapshot)
unknown_anchor_module_reorder_json=$(run_bridge apply-module-instance-action "$module_reorder_target_key" move-before unknown#-1#-1#missing-anchor)
unsupported_module_action_json=$(run_bridge apply-module-instance-action "$module_instance_key" unsupported-action)
unknown_module_instance_json=$(run_bridge apply-module-instance-action unknown#-1#-1#missing enable)
module_delete_nonbase_target_json=$(python3 - "$post_reorder_snapshot_json" "$module_instance_target_json" <<'PY'
import json, sys

duplicate_payload = json.loads(sys.argv[1])
source_payload = json.loads(sys.argv[2])
snapshot = (duplicate_payload.get('snapshot') or {})
module_stack = snapshot.get('moduleStack') or []
source_key = source_payload.get('instanceKey')
source_op = source_payload.get('moduleOp')

if not isinstance(source_key, str) or not source_key:
    raise SystemExit('missing source instance key for delete test')
if not isinstance(source_op, str) or not source_op:
    raise SystemExit('missing source module op for delete test')

def family_instance(key):
    parts = key.split('#', 3)
    if len(parts) != 4:
        raise SystemExit(f'invalid instance key: {key}')
    return parts[1]

source_instance = family_instance(source_key)
siblings = []
for item in module_stack:
    if not isinstance(item, dict):
        continue
    key = item.get('instanceKey')
    if not isinstance(key, str) or not key:
        continue
    if item.get('moduleOp') != source_op:
        continue
    if family_instance(key) != source_instance:
        continue
    siblings.append(item)

if len(siblings) < 2:
    raise SystemExit('delete test requires at least two visible family instances')

target = None
replacement_hint = None
for item in siblings:
    if item.get('multiPriority') != 0:
        target = item
        break
if target is None:
    raise SystemExit('delete test could not find non-base instance to delete')

print(json.dumps({
    'instanceKey': target['instanceKey'],
    'moduleOp': target.get('moduleOp'),
    'iopOrder': target.get('iopOrder'),
    'multiPriority': target.get('multiPriority'),
    'multiName': target.get('multiName'),
    'familyInstance': source_instance,
    'familyVisibleCount': len(siblings),
}, separators=(',', ':')))
PY
)
module_delete_nonbase_target_key=$(python3 - "$module_delete_nonbase_target_json" <<'PY'
import json, sys
print(json.loads(sys.argv[1])['instanceKey'])
PY
)
module_delete_nonbase_json=$(run_bridge apply-module-instance-action "$module_delete_nonbase_target_key" delete)
post_delete_nonbase_snapshot_json=$(run_bridge get-snapshot)
module_delete_target_json=$(python3 - "$post_delete_nonbase_snapshot_json" "$module_instance_target_json" <<'PY'
import json, sys

snapshot = json.loads(sys.argv[1])
source_payload = json.loads(sys.argv[2])
module_stack = ((snapshot.get('snapshot') or {}).get('moduleStack') or [])
source_key = source_payload.get('instanceKey')
source_op = source_payload.get('moduleOp')

if not isinstance(source_key, str) or not source_key:
    raise SystemExit('missing source instance key for base delete test')
if not isinstance(source_op, str) or not source_op:
    raise SystemExit('missing source module op for base delete test')

def family_instance(key):
    parts = key.split('#', 3)
    if len(parts) != 4:
        raise SystemExit(f'invalid instance key: {key}')
    return parts[1]

source_instance = family_instance(source_key)
siblings = []
for item in module_stack:
    if not isinstance(item, dict):
        continue
    key = item.get('instanceKey')
    if not isinstance(key, str) or not key:
        continue
    if item.get('moduleOp') != source_op:
        continue
    if family_instance(key) != source_instance:
        continue
    siblings.append(item)

if len(siblings) < 2:
    raise SystemExit('base delete test requires at least two visible family instances after non-base delete')

target = None
for item in siblings:
    if item.get('multiPriority') == 0:
        target = item
        break
if target is None:
    raise SystemExit('base delete test could not find base instance to delete')

replacement_hint = None
for item in siblings:
    if item.get('instanceKey') != target.get('instanceKey'):
        replacement_hint = item
        break

if replacement_hint is None:
    raise SystemExit('base delete test could not find replacement hint instance')

print(json.dumps({
    'instanceKey': target['instanceKey'],
    'moduleOp': target.get('moduleOp'),
    'iopOrder': target.get('iopOrder'),
    'multiPriority': target.get('multiPriority'),
    'multiName': target.get('multiName'),
    'familyInstance': source_instance,
    'familyVisibleCount': len(siblings),
    'replacementHintKey': replacement_hint['instanceKey'],
}, separators=(',', ':')))
PY
)
module_delete_target_key=$(python3 - "$module_delete_target_json" <<'PY'
import json, sys
print(json.loads(sys.argv[1])['instanceKey'])
PY
)
module_delete_json=$(run_bridge apply-module-instance-action "$module_delete_target_key" delete)
post_delete_snapshot_json=$(run_bridge get-snapshot)
module_delete_blocked_target_json=$(python3 - "$post_delete_snapshot_json" "$module_delete_json" <<'PY'
import json, sys

snapshot = json.loads(sys.argv[1])
delete_payload = json.loads(sys.argv[2])
module_stack = ((snapshot.get('snapshot') or {}).get('moduleStack') or [])
replacement_key = ((delete_payload.get('moduleAction') or {}).get('replacementInstanceKey'))

if not isinstance(replacement_key, str) or not replacement_key:
    raise SystemExit('missing replacementInstanceKey for blocked delete test')

for item in module_stack:
    if isinstance(item, dict) and item.get('instanceKey') == replacement_key:
        print(json.dumps({
            'instanceKey': item['instanceKey'],
            'moduleOp': item.get('moduleOp'),
            'iopOrder': item.get('iopOrder'),
            'multiPriority': item.get('multiPriority'),
            'multiName': item.get('multiName'),
        }, separators=(',', ':')))
        break
else:
    raise SystemExit('replacement instance missing from post-delete snapshot for blocked delete test')
PY
)
module_delete_blocked_target_key=$(python3 - "$module_delete_blocked_target_json" <<'PY'
import json, sys
print(json.loads(sys.argv[1])['instanceKey'])
PY
)
module_delete_blocked_json=$(run_bridge apply-module-instance-action "$module_delete_blocked_target_key" delete)
post_delete_blocked_snapshot_json=$(run_bridge get-snapshot)
fence_blocked_module_reorder_json=$(python3 - "$snapshot_json" "$bridge_bin" <<'PY'
import json, subprocess, sys

snapshot = json.loads(sys.argv[1])
bridge = sys.argv[2]
module_stack = ((snapshot.get('snapshot') or {}).get('moduleStack') or [])

fence_item = None
anchor_item = None
for item in module_stack:
    if not isinstance(item, dict):
        continue
    if fence_item is None and item.get('moduleOp') == 'demosaic':
        fence_item = item
    if anchor_item is None and item.get('instanceKey') != (fence_item or {}).get('instanceKey'):
        anchor_item = item

if fence_item is not None and anchor_item is not None:
    payload = subprocess.check_output(
        [bridge, 'apply-module-instance-action', fence_item['instanceKey'], 'move-after', anchor_item['instanceKey']],
        text=True,
    )
    print(payload.strip())
else:
    print(json.dumps({'status': 'skipped', 'reason': 'no-visible-fence-module'}))
PY
)
rule_blocked_module_reorder_json=$(python3 - "$snapshot_json" "$bridge_bin" <<'PY'
import json, subprocess, sys

snapshot = json.loads(sys.argv[1])
bridge = sys.argv[2]
rules = [
    ('rawprepare', 'invert'),
    ('invert', 'temperature'),
    ('temperature', 'highlights'),
    ('highlights', 'cacorrect'),
    ('cacorrect', 'hotpixels'),
    ('hotpixels', 'rawdenoise'),
    ('rawdenoise', 'demosaic'),
    ('demosaic', 'colorin'),
    ('colorin', 'colorout'),
    ('flip', 'crop'),
    ('flip', 'clipping'),
    ('ashift', 'clipping'),
    ('colorin', 'channelmixerrgb'),
]
module_stack = ((snapshot.get('snapshot') or {}).get('moduleStack') or [])
by_op = {}
for item in module_stack:
    if isinstance(item, dict) and isinstance(item.get('moduleOp'), str):
        by_op.setdefault(item['moduleOp'], []).append(item)

for op_prev, op_next in rules:
    prev_items = by_op.get(op_prev) or []
    next_items = by_op.get(op_next) or []
    if not prev_items or not next_items:
        continue
    payload = subprocess.check_output(
        [bridge, 'apply-module-instance-action', next_items[0]['instanceKey'], 'move-before', prev_items[0]['instanceKey']],
        text=True,
    )
    print(payload.strip())
    break
else:
    print(json.dumps({'status': 'skipped', 'reason': 'no-visible-rule-pair'}))
PY
)

switch_to_lighttable
unsupported_view_snapshot_json=$(run_bridge get-snapshot)
unsupported_view_get_control_json=$(run_bridge get-control exposure.exposure)
unsupported_view_set_control_json=$(run_bridge set-control exposure.exposure "$requested_control_exposure")
unsupported_view_module_instance_action_json=$(run_bridge apply-module-instance-action "$module_instance_key" "$module_instance_action")
unsupported_view_module_mask_json=$(run_bridge apply-module-instance-mask "$mask_target_key" '{"action":"clear-mask"}')
switch_to_darkroom
wait_for_session_payload "$ready_attempts" >/dev/null

if run_bridge set-control exposure.exposure '{"invalid":true}' >/dev/null 2>&1; then
  fail "set-control accepted non-numeric JSON"
fi

if run_bridge set-control exposure.exposure 4.5 >/dev/null 2>&1; then
  fail "set-control accepted out-of-range exposure"
fi

if run_bridge set-exposure 4.5 >/dev/null 2>&1; then
  fail "set-exposure accepted out-of-range exposure"
fi

if run_bridge apply-module-instance-blend "$blend_target_key" '{"opacity":"bad"}' >/dev/null 2>&1; then
  fail "apply-module-instance-blend accepted non-numeric opacity"
fi

if run_bridge apply-module-instance-blend "$blend_target_key" '{}' >/dev/null 2>&1; then
  fail "apply-module-instance-blend accepted missing mutation fields"
fi

if run_bridge apply-module-instance-blend "$blend_target_key" '{"opacity":50,"extra":true}' >/dev/null 2>&1; then
  fail "apply-module-instance-blend accepted extra keys"
fi

if run_bridge apply-module-instance-blend "$blend_target_key" '{"opacity":101}' >/dev/null 2>&1; then
  fail "apply-module-instance-blend accepted out-of-range opacity"
fi

if run_bridge apply-module-instance-blend "$blend_target_key" '{"blendMode":7}' >/dev/null 2>&1; then
  fail "apply-module-instance-blend accepted non-string blendMode"
fi

if run_bridge apply-module-instance-blend "$blend_target_key" '{"blendMode":"not-a-real-mode"}' >/dev/null 2>&1; then
  fail "apply-module-instance-blend accepted unknown blendMode"
fi

if run_bridge apply-module-instance-blend "$blend_target_key" '{"reverseOrder":"bad"}' >/dev/null 2>&1; then
  fail "apply-module-instance-blend accepted non-boolean reverseOrder"
fi

if run_bridge apply-module-instance-mask "$mask_target_key" '{}' >/dev/null 2>&1; then
  fail "apply-module-instance-mask accepted missing action"
fi

if run_bridge apply-module-instance-mask "$mask_target_key" '{"action":"not-real"}' >/dev/null 2>&1; then
  fail "apply-module-instance-mask accepted unknown action"
fi

if run_bridge apply-module-instance-mask "$mask_target_key" '{"action":"clear-mask","sourceInstanceKey":"exposure#0#0#"}' >/dev/null 2>&1; then
  fail "apply-module-instance-mask accepted sourceInstanceKey for clear-mask"
fi

python3 - "$initial_json" "$snapshot_json" "$module_instance_target_json" "$list_json" "$get_control_json" "$set_json" "$post_set_exposure_json" "$set_control_json" "$post_set_control_json" "$unsupported_control_json" "$module_instance_action_json" "$module_instance_revert_json" "$module_instance_create_json" "$module_instance_duplicate_json" "$duplicate_result_target_json" "$duplicate_result_toggle_json" "$module_reorder_target_json" "$module_reorder_move_before_json" "$module_reorder_move_before_noop_json" "$module_reorder_move_after_json" "$unknown_anchor_module_reorder_json" "$module_delete_nonbase_target_json" "$module_delete_nonbase_json" "$post_delete_nonbase_snapshot_json" "$module_delete_target_json" "$module_delete_json" "$post_delete_snapshot_json" "$module_delete_blocked_target_json" "$module_delete_blocked_json" "$post_delete_blocked_snapshot_json" "$fence_blocked_module_reorder_json" "$rule_blocked_module_reorder_json" "$unsupported_module_action_json" "$unknown_module_instance_json" "$unsupported_view_snapshot_json" "$unsupported_view_get_control_json" "$unsupported_view_set_control_json" "$unsupported_view_module_instance_action_json" "$requested_exposure" "$requested_control_exposure" "$asset_path" <<'PY'
import json, math, os, sys
initial = json.loads(sys.argv[1])
snapshot = json.loads(sys.argv[2])
module_instance_target = json.loads(sys.argv[3])
listed = json.loads(sys.argv[4])
get_control = json.loads(sys.argv[5])
set_payload = json.loads(sys.argv[6])
post_set_exposure = json.loads(sys.argv[7])
set_control = json.loads(sys.argv[8])
post_set_control = json.loads(sys.argv[9])
unsupported = json.loads(sys.argv[10])
module_instance_action = json.loads(sys.argv[11])
module_instance_revert = json.loads(sys.argv[12])
module_instance_create = json.loads(sys.argv[13])
module_instance_duplicate = json.loads(sys.argv[14])
duplicate_result_target = json.loads(sys.argv[15])
duplicate_result_toggle = json.loads(sys.argv[16])
module_reorder_target = json.loads(sys.argv[17])
module_reorder_move_before = json.loads(sys.argv[18])
module_reorder_move_before_noop = json.loads(sys.argv[19])
module_reorder_move_after = json.loads(sys.argv[20])
unknown_anchor_module_reorder = json.loads(sys.argv[21])
module_delete_nonbase_target = json.loads(sys.argv[22])
module_delete_nonbase = json.loads(sys.argv[23])
post_delete_nonbase_snapshot = json.loads(sys.argv[24])
module_delete_target = json.loads(sys.argv[25])
module_delete = json.loads(sys.argv[26])
post_delete_snapshot = json.loads(sys.argv[27])
module_delete_blocked_target = json.loads(sys.argv[28])
module_delete_blocked = json.loads(sys.argv[29])
post_delete_blocked_snapshot = json.loads(sys.argv[30])
fence_blocked_module_reorder = json.loads(sys.argv[31])
rule_blocked_module_reorder = json.loads(sys.argv[32])
unsupported_module_action = json.loads(sys.argv[33])
unknown_module_instance = json.loads(sys.argv[34])
unsupported_view_snapshot = json.loads(sys.argv[35])
unsupported_view_get = json.loads(sys.argv[36])
unsupported_view_set = json.loads(sys.argv[37])
unsupported_view_module_instance_action = json.loads(sys.argv[38])
requested = float(sys.argv[39])
requested_control = float(sys.argv[40])
asset = os.path.realpath(sys.argv[41])

EXPECTED_CONTROL_ID = 'exposure.exposure'

def expect_ok(name, payload):
    if payload.get('status') != 'ok':
        raise SystemExit(f'{name} status not ok: {payload}')
    active = payload.get('activeImage') or {}
    if os.path.realpath(active.get('sourceAssetPath') or '') != asset:
        raise SystemExit(f'{name} active image mismatch: {payload}')

def expect_close(name, value, target):
    if not isinstance(value, (int, float)) or math.isnan(value) or abs(value - target) > 1e-6:
        raise SystemExit(f'{name} expected {target}, got {value}')

def expect_control_metadata(name, control):
    if control.get('id') != EXPECTED_CONTROL_ID:
        raise SystemExit(f'{name} control id mismatch: {control}')
    if control.get('module') != 'exposure' or control.get('control') != 'exposure':
        raise SystemExit(f'{name} control metadata mismatch: {control}')
    if control.get('operations') != ['get', 'set']:
        raise SystemExit(f'{name} operations mismatch: {control}')
    value_type = control.get('valueType') or {}
    if value_type.get('type') != 'number' or value_type.get('minimum') != -3 or value_type.get('maximum') != 4:
        raise SystemExit(f'{name} valueType mismatch: {control}')
    requires = control.get('requires') or {}
    if requires.get('view') != 'darkroom' or requires.get('activeImage') is not True:
        raise SystemExit(f'{name} requires mismatch: {control}')

def expect_params_shape(name, params):
    if not isinstance(params, dict):
        raise SystemExit(f'{name} params missing: {params}')
    encoding = params.get('encoding')
    if encoding == 'introspection-v1':
        fields = params.get('fields')
        if not isinstance(fields, list):
            raise SystemExit(f'{name} introspection fields missing: {params}')
        for field in fields[:5]:
            if not isinstance(field, dict):
                raise SystemExit(f'{name} field entry malformed: {field}')
            if not isinstance(field.get('path'), str) or not isinstance(field.get('kind'), str) or 'value' not in field:
                raise SystemExit(f'{name} field shape mismatch: {field}')
    elif encoding != 'unsupported':
        raise SystemExit(f'{name} params encoding mismatch: {params}')

def expect_snapshot_item(name, item, expect_index):
    required = ['instanceKey', 'moduleOp', 'enabled', 'iopOrder', 'multiPriority', 'multiName', 'params']
    if expect_index:
        required = ['index', 'applied'] + required
    for key in required:
        if key not in item:
            raise SystemExit(f'{name} missing {key}: {item}')
    expect_params_shape(f'{name} params', item.get('params'))

def read_param_value(item, path):
    params = (item or {}).get('params') or {}
    if params.get('encoding') != 'introspection-v1':
        return None
    fields = params.get('fields') or []
    for field in fields:
        if isinstance(field, dict) and field.get('path') == path:
            return field.get('value')
    return None

def expect_module_instance_response(name, payload, target_key, action, expected_previous, expected_current):
    expect_ok(name, payload)
    action_payload = payload.get('moduleAction') or {}
    if action_payload.get('targetInstanceKey') != target_key:
        raise SystemExit(f'{name} target instance mismatch: {payload}')
    if action_payload.get('action') != action:
        raise SystemExit(f'{name} action mismatch: {payload}')
    if action_payload.get('requestedEnabled') != expected_current:
        raise SystemExit(f'{name} requested enabled mismatch: {payload}')
    if action_payload.get('previousEnabled') != expected_previous:
        raise SystemExit(f'{name} previous enabled mismatch: {payload}')
    if action_payload.get('currentEnabled') != expected_current:
        raise SystemExit(f'{name} current enabled mismatch: {payload}')
    snapshot_payload = payload.get('snapshot') or {}
    module_stack = snapshot_payload.get('moduleStack') or []
    matching_items = [item for item in module_stack if isinstance(item, dict) and item.get('instanceKey') == target_key]
    if len(matching_items) != 1:
        raise SystemExit(f'{name} target snapshot item mismatch: {payload}')
    if matching_items[0].get('enabled') != expected_current:
        raise SystemExit(f'{name} target snapshot enabled mismatch: {payload}')
    if not isinstance(action_payload.get('historyBefore'), int) or not isinstance(action_payload.get('historyAfter'), int):
        raise SystemExit(f'{name} history markers missing: {payload}')
    if action_payload.get('requestedHistoryEnd') != action_payload.get('historyAfter'):
        raise SystemExit(f'{name} requested history end mismatch: {payload}')

def expect_module_instance_create_like_response(name, payload, target_key, action, source_module_op):
    expect_ok(name, payload)
    action_payload = payload.get('moduleAction') or {}
    if action_payload.get('targetInstanceKey') != target_key:
        raise SystemExit(f'{name} target instance mismatch: {payload}')
    if action_payload.get('action') != action:
        raise SystemExit(f'{name} action mismatch: {payload}')
    result_key = action_payload.get('resultInstanceKey')
    if not isinstance(result_key, str) or not result_key:
        raise SystemExit(f'{name} result instance key missing: {payload}')
    if result_key == target_key:
        raise SystemExit(f'{name} result instance key did not change: {payload}')
    if action_payload.get('moduleOp') != source_module_op:
        raise SystemExit(f'{name} module op mismatch: {payload}')
    if not isinstance(action_payload.get('iopOrder'), int):
        raise SystemExit(f'{name} iopOrder missing: {payload}')
    if not isinstance(action_payload.get('multiPriority'), int):
        raise SystemExit(f'{name} multiPriority missing: {payload}')
    if not isinstance(action_payload.get('multiName'), str):
        raise SystemExit(f'{name} multiName missing: {payload}')
    if not isinstance(action_payload.get('historyBefore'), int) or not isinstance(action_payload.get('historyAfter'), int):
        raise SystemExit(f'{name} history markers missing: {payload}')
    if action_payload.get('requestedHistoryEnd') != action_payload.get('historyAfter'):
        raise SystemExit(f'{name} requested history end mismatch: {payload}')
    if action_payload.get('historyAfter') < action_payload.get('historyBefore'):
        raise SystemExit(f'{name} history did not advance: {payload}')

    snapshot_payload = payload.get('snapshot') or {}
    module_stack = snapshot_payload.get('moduleStack') or []
    matching_items = [item for item in module_stack if isinstance(item, dict) and item.get('instanceKey') == result_key]
    if len(matching_items) != 1:
        raise SystemExit(f'{name} result snapshot item mismatch: {payload}')
    result_item = matching_items[0]
    if result_item.get('moduleOp') != source_module_op:
        raise SystemExit(f'{name} snapshot module op mismatch: {payload}')
    if result_item.get('iopOrder') != action_payload.get('iopOrder'):
        raise SystemExit(f'{name} snapshot iopOrder mismatch: {payload}')
    if result_item.get('multiPriority') != action_payload.get('multiPriority'):
        raise SystemExit(f'{name} snapshot multiPriority mismatch: {payload}')
    if result_item.get('multiName') != action_payload.get('multiName'):
        raise SystemExit(f'{name} snapshot multiName mismatch: {payload}')
    source_items = [item for item in module_stack if isinstance(item, dict) and item.get('instanceKey') == target_key]
    if len(source_items) != 1:
        raise SystemExit(f'{name} source snapshot item mismatch: {payload}')
    sibling_items = [item for item in module_stack if isinstance(item, dict) and item.get('moduleOp') == source_module_op]
    if len(sibling_items) < 2:
        raise SystemExit(f'{name} module stack did not gain a sibling instance: {payload}')

    source_item = source_items[0]
    source_exposure = read_param_value(source_item, 'exposure')
    result_exposure = read_param_value(result_item, 'exposure')
    if action == 'duplicate':
        if result_item.get('enabled') != source_item.get('enabled'):
            raise SystemExit(f'{name} duplicate enabled state mismatch: {payload}')
        if isinstance(source_exposure, (int, float)) and isinstance(result_exposure, (int, float)):
            expect_close(f'{name} duplicate exposure copy', result_exposure, source_exposure)
    elif action == 'create':
        if isinstance(source_exposure, (int, float)) and isinstance(result_exposure, (int, float)) and abs(source_exposure - result_exposure) <= 1e-6:
            raise SystemExit(f'{name} create exposure unexpectedly matches source params: {payload}')

def expect_module_reorder_response(name, payload, target_key, anchor_key, action):
    expect_ok(name, payload)
    action_payload = payload.get('moduleAction') or {}
    if action_payload.get('targetInstanceKey') != target_key:
        raise SystemExit(f'{name} target instance mismatch: {payload}')
    if action_payload.get('anchorInstanceKey') != anchor_key:
        raise SystemExit(f'{name} anchor instance mismatch: {payload}')
    if action_payload.get('action') != action:
        raise SystemExit(f'{name} action mismatch: {payload}')
    for key in ('moduleOp', 'multiPriority', 'multiName'):
        if key not in action_payload:
            raise SystemExit(f'{name} missing {key}: {payload}')
    for key in ('iopOrder', 'previousIopOrder', 'currentIopOrder', 'historyBefore', 'historyAfter', 'requestedHistoryEnd'):
        if not isinstance(action_payload.get(key), int):
            raise SystemExit(f'{name} missing integer {key}: {payload}')
    if action_payload.get('iopOrder') != action_payload.get('currentIopOrder'):
        raise SystemExit(f'{name} current iopOrder mismatch: {payload}')
    if action_payload.get('requestedHistoryEnd') != action_payload.get('historyAfter'):
        raise SystemExit(f'{name} requested history end mismatch: {payload}')
    if action_payload.get('historyAfter') <= action_payload.get('historyBefore'):
        raise SystemExit(f'{name} history did not advance: {payload}')
    if action_payload.get('previousIopOrder') == action_payload.get('currentIopOrder'):
        raise SystemExit(f'{name} iopOrder did not change: {payload}')

    snapshot_payload = payload.get('snapshot') or {}
    module_stack = snapshot_payload.get('moduleStack') or []
    matching_items = [item for item in module_stack if isinstance(item, dict) and item.get('instanceKey') == target_key]
    if len(matching_items) != 1:
        raise SystemExit(f'{name} target snapshot item mismatch: {payload}')
    if matching_items[0].get('iopOrder') != action_payload.get('currentIopOrder'):
        raise SystemExit(f'{name} snapshot current iopOrder mismatch: {payload}')
    positions = {}
    for index, item in enumerate(module_stack):
        if isinstance(item, dict) and isinstance(item.get('instanceKey'), str):
            positions[item['instanceKey']] = index
    if target_key not in positions or anchor_key not in positions:
        raise SystemExit(f'{name} snapshot positions missing: {payload}')
    if action == 'move-before' and not (positions[target_key] < positions[anchor_key]):
        raise SystemExit(f'{name} target not before anchor in snapshot: {payload}')
    if action == 'move-after' and not (positions[target_key] > positions[anchor_key]):
        raise SystemExit(f'{name} target not after anchor in snapshot: {payload}')

def expect_reorder_unavailable(name, payload, target_key, anchor_key, action, reason):
    if payload.get('status') != 'unavailable' or payload.get('reason') != reason:
        raise SystemExit(f'{name} response mismatch: {payload}')
    action_payload = payload.get('moduleAction') or {}
    if action_payload.get('targetInstanceKey') != target_key:
        raise SystemExit(f'{name} target instance mismatch: {payload}')
    if action_payload.get('anchorInstanceKey') != anchor_key:
        raise SystemExit(f'{name} anchor instance mismatch: {payload}')
    if action_payload.get('action') != action:
        raise SystemExit(f'{name} action mismatch: {payload}')

def expect_module_delete_response(name, payload, latest_snapshot, target, blocked=False, expected_snapshot=None):
    action_payload = payload.get('moduleAction') or {}
    target_key = target.get('instanceKey')
    if action_payload.get('targetInstanceKey') != target_key:
        raise SystemExit(f'{name} target instance mismatch: {payload}')
    if action_payload.get('action') != 'delete':
        raise SystemExit(f'{name} action mismatch: {payload}')
    if action_payload.get('moduleOp') != target.get('moduleOp'):
        raise SystemExit(f'{name} module op mismatch: {payload}')
    if action_payload.get('iopOrder') != target.get('iopOrder'):
        raise SystemExit(f'{name} iopOrder mismatch: {payload}')
    if action_payload.get('multiPriority') != target.get('multiPriority'):
        raise SystemExit(f'{name} multiPriority mismatch: {payload}')
    if action_payload.get('multiName') != target.get('multiName'):
        raise SystemExit(f'{name} multiName mismatch: {payload}')
    if not isinstance(action_payload.get('historyBefore'), int) or not isinstance(action_payload.get('historyAfter'), int):
        raise SystemExit(f'{name} history markers missing: {payload}')
    if action_payload.get('requestedHistoryEnd') != action_payload.get('historyAfter'):
        raise SystemExit(f'{name} requested history end mismatch: {payload}')

    if blocked:
        if payload.get('status') != 'unavailable' or payload.get('reason') != 'module-delete-blocked-last-instance':
            raise SystemExit(f'{name} response mismatch: {payload}')
        if action_payload.get('historyBefore') != action_payload.get('historyAfter'):
            raise SystemExit(f'{name} blocked delete unexpectedly changed history: {payload}')
        expect_ok(f'{name} fresh snapshot', latest_snapshot)
        latest_snapshot_payload = latest_snapshot.get('snapshot') or {}
        if latest_snapshot_payload.get('appliedHistoryEnd') != action_payload.get('requestedHistoryEnd'):
            raise SystemExit(f'{name} blocked delete changed appliedHistoryEnd: {latest_snapshot}')
        if isinstance(expected_snapshot, dict) and latest_snapshot_payload != expected_snapshot:
            raise SystemExit(f'{name} blocked delete changed snapshot contents: {latest_snapshot}')
        latest_module_stack = latest_snapshot_payload.get('moduleStack') or []
        latest_history_items = latest_snapshot_payload.get('historyItems') or []
        if not any(isinstance(item, dict) and item.get('instanceKey') == target_key for item in latest_module_stack):
            raise SystemExit(f'{name} blocked delete lost target from moduleStack: {latest_snapshot}')
        if not any(isinstance(item, dict) and item.get('instanceKey') == target_key for item in latest_history_items):
            raise SystemExit(f'{name} blocked delete lost target from historyItems: {latest_snapshot}')
        return

    expect_ok(name, payload)
    if action_payload.get('historyAfter') >= action_payload.get('historyBefore'):
        raise SystemExit(f'{name} delete did not shrink history: {payload}')

    snapshot_payload = payload.get('snapshot') or {}
    if snapshot_payload.get('appliedHistoryEnd') != action_payload.get('requestedHistoryEnd'):
        raise SystemExit(f'{name} embedded snapshot history end mismatch: {payload}')
    module_stack = snapshot_payload.get('moduleStack') or []
    history_items = snapshot_payload.get('historyItems') or []
    if any(isinstance(item, dict) and item.get('instanceKey') == target_key for item in module_stack):
        raise SystemExit(f'{name} deleted target still present in snapshot: {payload}')
    if any(isinstance(item, dict) and item.get('instanceKey') == target_key for item in history_items):
        raise SystemExit(f'{name} deleted target still present in historyItems: {payload}')

    expect_ok(f'{name} fresh snapshot', latest_snapshot)
    latest_snapshot_payload = latest_snapshot.get('snapshot') or {}
    latest_module_stack = latest_snapshot_payload.get('moduleStack') or []
    latest_history_items = latest_snapshot_payload.get('historyItems') or []
    if latest_snapshot_payload.get('appliedHistoryEnd') != action_payload.get('requestedHistoryEnd'):
        raise SystemExit(f'{name} fresh snapshot history end mismatch: {latest_snapshot}')
    if latest_snapshot_payload != snapshot_payload:
        raise SystemExit(f'{name} embedded snapshot does not match fresh readback: {latest_snapshot}')
    if any(isinstance(item, dict) and item.get('instanceKey') == target_key for item in latest_module_stack):
        raise SystemExit(f'{name} deleted target still present in fresh moduleStack: {latest_snapshot}')
    if any(isinstance(item, dict) and item.get('instanceKey') == target_key for item in latest_history_items):
        raise SystemExit(f'{name} deleted target still present in fresh historyItems: {latest_snapshot}')

    family_instance = target.get('familyInstance')
    family_visible_count = target.get('familyVisibleCount')
    if isinstance(family_instance, str) and isinstance(family_visible_count, int):
        family_items = [
            item for item in latest_module_stack
            if isinstance(item, dict)
            and item.get('moduleOp') == target.get('moduleOp')
            and isinstance(item.get('instanceKey'), str)
            and item['instanceKey'].split('#', 3)[1] == family_instance
        ]
        if len(family_items) != family_visible_count - 1:
            raise SystemExit(f'{name} family size did not shrink by one: {latest_snapshot}')

    replacement_key = action_payload.get('replacementInstanceKey')
    if action_payload.get('multiPriority') == 0:
        if not isinstance(replacement_key, str) or not replacement_key:
            raise SystemExit(f'{name} replacementInstanceKey missing for base delete: {payload}')
        replacement_parts = replacement_key.split('#', 3)
        if len(replacement_parts) != 4:
            raise SystemExit(f'{name} replacement key shape mismatch: {payload}')
        family_instance = target.get('familyInstance')
        if isinstance(family_instance, str) and family_instance and replacement_parts[1] != family_instance:
            raise SystemExit(f'{name} replacement did not stay within the deleted module family: {payload}')
        matching_replacements = [item for item in module_stack if isinstance(item, dict) and item.get('instanceKey') == replacement_key]
        if len(matching_replacements) != 1:
            raise SystemExit(f'{name} replacement snapshot item mismatch: {payload}')
        replacement_item = matching_replacements[0]
        if replacement_item.get('moduleOp') != target.get('moduleOp'):
            raise SystemExit(f'{name} replacement module op mismatch: {payload}')
        if action_payload.get('replacementIopOrder') != replacement_item.get('iopOrder'):
            raise SystemExit(f'{name} replacement iopOrder mismatch: {payload}')
        if action_payload.get('replacementMultiPriority') != replacement_item.get('multiPriority'):
            raise SystemExit(f'{name} replacement multiPriority mismatch: {payload}')
        if action_payload.get('replacementMultiName') != replacement_item.get('multiName'):
            raise SystemExit(f'{name} replacement multiName mismatch: {payload}')
        if replacement_item.get('multiPriority') != 0:
            raise SystemExit(f'{name} replacement was not promoted to base priority: {payload}')
        fresh_replacement_items = [item for item in latest_module_stack if isinstance(item, dict) and item.get('instanceKey') == replacement_key]
        if len(fresh_replacement_items) != 1:
            raise SystemExit(f'{name} replacement missing from fresh snapshot: {latest_snapshot}')
        if not any(isinstance(item, dict) and item.get('instanceKey') == replacement_key for item in latest_history_items):
            raise SystemExit(f'{name} replacement missing from fresh historyItems: {latest_snapshot}')
        hint_key = target.get('replacementHintKey')
        if isinstance(hint_key, str) and hint_key and replacement_key == target_key:
            raise SystemExit(f'{name} replacement key repeated deleted target: {payload}')
    elif replacement_key is not None:
        matching_replacements = [item for item in module_stack if isinstance(item, dict) and item.get('instanceKey') == replacement_key]
        if len(matching_replacements) != 1:
            raise SystemExit(f'{name} optional replacement snapshot item mismatch: {payload}')
        if not any(isinstance(item, dict) and item.get('instanceKey') == replacement_key for item in latest_module_stack):
            raise SystemExit(f'{name} optional replacement missing from fresh snapshot: {latest_snapshot}')

expect_ok('initial', initial)
expect_ok('get-snapshot', snapshot)
if listed.get('status') != 'ok':
    raise SystemExit(f'list-controls status not ok: {listed}')
snapshot_payload = snapshot.get('snapshot') or {}
applied_history_end = snapshot_payload.get('appliedHistoryEnd')
if not isinstance(applied_history_end, int) or applied_history_end < 0:
    raise SystemExit(f'get-snapshot appliedHistoryEnd mismatch: {snapshot}')
snapshot_controls = snapshot_payload.get('controls')
if not isinstance(snapshot_controls, list) or len(snapshot_controls) < 1:
    raise SystemExit(f'get-snapshot controls mismatch: {snapshot}')
exposure_snapshot_controls = [control for control in snapshot_controls if isinstance(control, dict) and control.get('id') == EXPECTED_CONTROL_ID]
if len(exposure_snapshot_controls) != 1:
    raise SystemExit(f'get-snapshot exposure control missing: {snapshot}')
expect_control_metadata('get-snapshot control', exposure_snapshot_controls[0])
expect_close('get-snapshot control value', exposure_snapshot_controls[0].get('value'), (initial.get('exposure') or {}).get('current'))
module_stack = snapshot_payload.get('moduleStack')
history_items = snapshot_payload.get('historyItems')
if not isinstance(module_stack, list) or len(module_stack) < 1:
    raise SystemExit(f'get-snapshot moduleStack mismatch: {snapshot}')
if not isinstance(history_items, list) or len(history_items) < applied_history_end:
    raise SystemExit(f'get-snapshot historyItems mismatch: {snapshot}')
expect_snapshot_item('get-snapshot moduleStack[0]', module_stack[0], False)
if history_items:
    expect_snapshot_item('get-snapshot historyItems[0]', history_items[0], True)
elif applied_history_end != 0:
    raise SystemExit(f'get-snapshot historyItems unexpectedly empty: {snapshot}')
if not any(item.get('moduleOp') == 'exposure' for item in module_stack if isinstance(item, dict)):
    raise SystemExit(f'get-snapshot moduleStack missing exposure module: {snapshot}')
if applied_history_end > 0 and not any(isinstance(item, dict) and item.get('applied') is True for item in history_items[:applied_history_end]):
    raise SystemExit(f'get-snapshot applied history mismatch: {snapshot}')
controls = listed.get('controls')
if not isinstance(controls, list) or len(controls) != 1:
    raise SystemExit(f'list-controls unexpected controls: {listed}')
expect_control_metadata('list-controls', controls[0])
expect_ok('get-control', get_control)
expect_control_metadata('get-control', get_control.get('control') or {})
expect_ok('set-exposure', set_payload)
expect_ok('post-set-exposure', post_set_exposure)
expect_ok('set-control', set_control)
expect_ok('post-set-control', post_set_control)
initial_current = (initial.get('exposure') or {}).get('current')
if isinstance(initial_current, (int, float)) and not math.isnan(initial_current):
    if abs(initial_current - requested) <= 1e-6:
        raise SystemExit(f'initial exposure already equals requested exposure {requested}: {initial}')
expect_close('get-control current', (get_control.get('control') or {}).get('value'), initial_current)
expect_close('set-exposure requested', (set_payload.get('exposure') or {}).get('requested'), requested)
expect_close('set-exposure current', (set_payload.get('exposure') or {}).get('current'), requested)
expect_close('post-set-exposure current', (post_set_exposure.get('exposure') or {}).get('current'), requested)
expect_control_metadata('set-control', set_control.get('control') or {})
change = set_control.get('change') or {}
expect_close('set-control previous', change.get('previous'), requested)
expect_close('set-control requested', change.get('requested'), requested_control)
expect_close('set-control current', change.get('current'), requested_control)
expect_close('set-control control.value', (set_control.get('control') or {}).get('value'), requested_control)
expect_close('post-set-control current', (post_set_control.get('exposure') or {}).get('current'), requested_control)
if unsupported.get('status') != 'unavailable' or unsupported.get('reason') != 'unsupported-control':
    raise SystemExit(f'unsupported control response mismatch: {unsupported}')
if unsupported.get('requestedControlId') != 'unsupported.control':
    raise SystemExit(f'unsupported control id mismatch: {unsupported}')
if unsupported.get('session', {}).get('view') != 'darkroom':
    raise SystemExit(f'unsupported control session mismatch: {unsupported}')
if unsupported_view_snapshot.get('status') != 'unavailable' or unsupported_view_snapshot.get('reason') != 'unsupported-view':
    raise SystemExit(f'unsupported-view get-snapshot response mismatch: {unsupported_view_snapshot}')
if unsupported_view_snapshot.get('session', {}).get('view') != 'lighttable':
    raise SystemExit(f'unsupported-view get-snapshot session mismatch: {unsupported_view_snapshot}')
for name, payload in (
    ('unsupported-view get-control', unsupported_view_get),
    ('unsupported-view set-control', unsupported_view_set),
):
    if payload.get('status') != 'unavailable' or payload.get('reason') != 'unsupported-view':
        raise SystemExit(f'{name} response mismatch: {payload}')
    if payload.get('requestedControlId') != EXPECTED_CONTROL_ID:
        raise SystemExit(f'{name} requested control mismatch: {payload}')
    if payload.get('session', {}).get('view') != 'lighttable':
        raise SystemExit(f'{name} session mismatch: {payload}')
module_target_key = module_instance_target.get('instanceKey')
module_target_previous = module_instance_target.get('previousEnabled')
module_target_action = module_instance_target.get('action')
module_target_current = (module_target_action == 'enable')
module_target_revert_action = module_instance_target.get('revertAction')
expect_module_instance_response('apply-module-instance-action', module_instance_action, module_target_key, module_target_action, module_target_previous, module_target_current)
expect_module_instance_response('apply-module-instance-action revert', module_instance_revert, module_target_key, module_target_revert_action, module_target_current, module_target_previous)
expect_module_instance_create_like_response('apply-module-instance-action create', module_instance_create, module_target_key, 'create', module_instance_target.get('moduleOp'))
expect_module_instance_create_like_response('apply-module-instance-action duplicate', module_instance_duplicate, module_target_key, 'duplicate', module_instance_target.get('moduleOp'))
duplicate_result_key = duplicate_result_target.get('instanceKey')
duplicate_result_previous = duplicate_result_target.get('previousEnabled')
duplicate_result_action = duplicate_result_target.get('action')
duplicate_result_current = (duplicate_result_action == 'enable')
expect_module_instance_response('apply-module-instance-action duplicate-result-toggle', duplicate_result_toggle, duplicate_result_key, duplicate_result_action, duplicate_result_previous, duplicate_result_current)
module_reorder_target_key = module_reorder_target.get('targetInstanceKey')
module_reorder_anchor_key = module_reorder_target.get('anchorInstanceKey')
expect_module_reorder_response('apply-module-instance-action move-before', module_reorder_move_before, module_reorder_target_key, module_reorder_anchor_key, 'move-before')
expect_reorder_unavailable('apply-module-instance-action move-before no-op', module_reorder_move_before_noop, module_reorder_target_key, module_reorder_anchor_key, 'move-before', 'module-reorder-no-op')
expect_module_reorder_response('apply-module-instance-action move-after', module_reorder_move_after, module_reorder_target_key, module_reorder_anchor_key, 'move-after')
expect_reorder_unavailable('apply-module-instance-action unknown-anchor', unknown_anchor_module_reorder, module_reorder_target_key, 'unknown#-1#-1#missing-anchor', 'move-before', 'unknown-anchor-instance-key')
expect_module_delete_response('apply-module-instance-action delete-nonbase', module_delete_nonbase, post_delete_nonbase_snapshot, module_delete_nonbase_target)
expect_module_delete_response('apply-module-instance-action delete', module_delete, post_delete_snapshot, module_delete_target)
expect_module_delete_response(
    'apply-module-instance-action delete-blocked-last-instance',
    module_delete_blocked,
    post_delete_blocked_snapshot,
    module_delete_blocked_target,
    blocked=True,
    expected_snapshot=(post_delete_snapshot.get('snapshot') or {}),
)
if fence_blocked_module_reorder.get('status') != 'skipped':
    if fence_blocked_module_reorder.get('status') != 'unavailable' or fence_blocked_module_reorder.get('reason') != 'module-reorder-blocked-by-fence':
        raise SystemExit(f'apply-module-instance-action fence-blocked response mismatch: {fence_blocked_module_reorder}')
if rule_blocked_module_reorder.get('status') != 'skipped':
    if rule_blocked_module_reorder.get('status') != 'unavailable' or rule_blocked_module_reorder.get('reason') != 'module-reorder-blocked-by-rule':
        raise SystemExit(f'apply-module-instance-action rule-blocked response mismatch: {rule_blocked_module_reorder}')
if unsupported_module_action.get('status') != 'unavailable' or unsupported_module_action.get('reason') != 'unsupported-module-action':
    raise SystemExit(f'unsupported module action response mismatch: {unsupported_module_action}')
if (unsupported_module_action.get('moduleAction') or {}).get('targetInstanceKey') != module_target_key:
    raise SystemExit(f'unsupported module action target mismatch: {unsupported_module_action}')
if unknown_module_instance.get('status') != 'unavailable' or unknown_module_instance.get('reason') != 'unknown-instance-key':
    raise SystemExit(f'unknown module instance response mismatch: {unknown_module_instance}')
if (unknown_module_instance.get('moduleAction') or {}).get('targetInstanceKey') != 'unknown#-1#-1#missing':
    raise SystemExit(f'unknown module instance target mismatch: {unknown_module_instance}')
if unsupported_view_module_instance_action.get('status') != 'unavailable' or unsupported_view_module_instance_action.get('reason') != 'unsupported-view':
    raise SystemExit(f'unsupported-view module instance response mismatch: {unsupported_view_module_instance_action}')
if unsupported_view_module_instance_action.get('session', {}).get('view') != 'lighttable':
    raise SystemExit(f'unsupported-view module instance session mismatch: {unsupported_view_module_instance_action}')
print('initial:', json.dumps(initial, separators=(",", ":")))
print('get-snapshot:', json.dumps(snapshot, separators=(",", ":")))
print('list-controls:', json.dumps(listed, separators=(",", ":")))
print('get-control:', json.dumps(get_control, separators=(",", ":")))
print('set-exposure:', json.dumps(set_payload, separators=(",", ":")))
print('post-set-exposure:', json.dumps(post_set_exposure, separators=(",", ":")))
print('set-control:', json.dumps(set_control, separators=(",", ":")))
print('post-set-control:', json.dumps(post_set_control, separators=(",", ":")))
print('apply-module-instance-action:', json.dumps(module_instance_action, separators=(",", ":")))
print('apply-module-instance-action-revert:', json.dumps(module_instance_revert, separators=(",", ":")))
print('apply-module-instance-action-create:', json.dumps(module_instance_create, separators=(",", ":")))
print('apply-module-instance-action-duplicate:', json.dumps(module_instance_duplicate, separators=(",", ":")))
print('apply-module-instance-action-duplicate-result-toggle:', json.dumps(duplicate_result_toggle, separators=(",", ":")))
print('apply-module-instance-action-move-before:', json.dumps(module_reorder_move_before, separators=(",", ":")))
print('apply-module-instance-action-move-before-noop:', json.dumps(module_reorder_move_before_noop, separators=(",", ":")))
print('apply-module-instance-action-move-after:', json.dumps(module_reorder_move_after, separators=(",", ":")))
print('apply-module-instance-action-delete-nonbase:', json.dumps(module_delete_nonbase, separators=(",", ":")))
print('apply-module-instance-action-delete:', json.dumps(module_delete, separators=(",", ":")))
print('apply-module-instance-action-delete-blocked-last-instance:', json.dumps(module_delete_blocked, separators=(",", ":")))
print('unknown-anchor-module-reorder:', json.dumps(unknown_anchor_module_reorder, separators=(",", ":")))
print('fence-blocked-module-reorder:', json.dumps(fence_blocked_module_reorder, separators=(",", ":")))
print('rule-blocked-module-reorder:', json.dumps(rule_blocked_module_reorder, separators=(",", ":")))
print('unsupported-module-action:', json.dumps(unsupported_module_action, separators=(",", ":")))
print('unknown-module-instance:', json.dumps(unknown_module_instance, separators=(",", ":")))
print('post-delete-nonbase-snapshot:', json.dumps(post_delete_nonbase_snapshot, separators=(",", ":")))
print('post-delete-snapshot:', json.dumps(post_delete_snapshot, separators=(",", ":")))
print('post-delete-blocked-snapshot:', json.dumps(post_delete_blocked_snapshot, separators=(",", ":")))
print('unsupported-control:', json.dumps(unsupported, separators=(",", ":")))
print('unsupported-view-get-snapshot:', json.dumps(unsupported_view_snapshot, separators=(",", ":")))
print('unsupported-view-get-control:', json.dumps(unsupported_view_get, separators=(",", ":")))
print('unsupported-view-set-control:', json.dumps(unsupported_view_set, separators=(",", ":")))
print('unsupported-view-apply-module-instance-action:', json.dumps(unsupported_view_module_instance_action, separators=(",", ":")))
print('result: exposure controls and module-instance actions validated')
PY

python3 - "$snapshot_json" "$blend_target_json" "$blend_apply_json" "$blend_followup_snapshot_json" "$blend_revert_json" "$invalid_blend_mode_json" "$unsupported_blend_target_json" "$unsupported_blend_json" "$asset_path" <<'PY'
import json, math, os, sys

snapshot = json.loads(sys.argv[1])
blend_target = json.loads(sys.argv[2])
blend_apply = json.loads(sys.argv[3])
blend_followup = json.loads(sys.argv[4])
blend_revert = json.loads(sys.argv[5])
invalid_blend_mode = json.loads(sys.argv[6])
unsupported_target = json.loads(sys.argv[7])
unsupported_blend = json.loads(sys.argv[8])
asset = os.path.realpath(sys.argv[9])

def expect_close(name, value, target):
    if not isinstance(value, (int, float)) or math.isnan(value) or abs(value - target) > 1e-6:
        raise SystemExit(f'{name} expected {target}, got {value}')

def module_items(payload, instance_key):
    module_stack = ((payload.get('snapshot') or {}).get('moduleStack') or [])
    return [item for item in module_stack if isinstance(item, dict) and item.get('instanceKey') == instance_key]

def expect_blend_shape(name, item):
    blend = item.get('blend')
    if not isinstance(blend, dict):
        raise SystemExit(f'{name} missing blend object: {item}')
    if not isinstance(blend.get('supported'), bool):
        raise SystemExit(f'{name} blend supported missing: {item}')
    if not isinstance(blend.get('masksSupported'), bool):
        raise SystemExit(f'{name} blend masksSupported missing: {item}')
    if blend.get('supported') is True:
        if not isinstance(blend.get('blendMode'), str) or not blend.get('blendMode'):
            raise SystemExit(f'{name} blendMode missing: {item}')
        if not isinstance(blend.get('reverseOrder'), bool):
            raise SystemExit(f'{name} reverseOrder missing: {item}')
        if not isinstance(blend.get('blendColorspace'), str) or not blend.get('blendColorspace'):
            raise SystemExit(f'{name} blendColorspace missing: {item}')
        if not isinstance(blend.get('opacity'), (int, float)):
            raise SystemExit(f'{name} opacity missing: {item}')

for index, item in enumerate(((snapshot.get('snapshot') or {}).get('moduleStack') or [])[:8]):
    if isinstance(item, dict):
        expect_blend_shape(f'moduleStack[{index}]', item)
for index, item in enumerate(((snapshot.get('snapshot') or {}).get('historyItems') or [])[:8]):
    if isinstance(item, dict):
        expect_blend_shape(f'historyItems[{index}]', item)

target_key = blend_target.get('instanceKey')
requested_opacity = blend_target.get('requestedOpacity')
previous_opacity = blend_target.get('previousOpacity')
requested_blend_mode = blend_target.get('requestedBlendMode')
previous_blend_mode = blend_target.get('previousBlendMode')
requested_reverse_order = blend_target.get('requestedReverseOrder')
previous_reverse_order = blend_target.get('previousReverseOrder')
if not isinstance(target_key, str) or not target_key:
    raise SystemExit(f'blend target missing instance key: {blend_target}')

if blend_apply.get('status') != 'ok':
    raise SystemExit(f'blend apply failed: {blend_apply}')
active = blend_apply.get('activeImage') or {}
if os.path.realpath(active.get('sourceAssetPath') or '') != asset:
    raise SystemExit(f'blend apply active image mismatch: {blend_apply}')
module_blend = blend_apply.get('moduleBlend') or {}
if module_blend.get('targetInstanceKey') != target_key:
    raise SystemExit(f'blend apply target mismatch: {blend_apply}')
if not isinstance(module_blend.get('moduleOp'), str) or not module_blend.get('moduleOp'):
    raise SystemExit(f'blend apply moduleOp missing: {blend_apply}')
if not isinstance(module_blend.get('iopOrder'), int):
    raise SystemExit(f'blend apply iopOrder missing: {blend_apply}')
if not isinstance(module_blend.get('multiPriority'), int):
    raise SystemExit(f'blend apply multiPriority missing: {blend_apply}')
if not isinstance(module_blend.get('multiName'), str):
    raise SystemExit(f'blend apply multiName missing: {blend_apply}')
expect_close('blend apply previous opacity', module_blend.get('previousOpacity'), previous_opacity)
expect_close('blend apply requested opacity', module_blend.get('requestedOpacity'), requested_opacity)
expect_close('blend apply current opacity', module_blend.get('currentOpacity'), requested_opacity)
if module_blend.get('previousBlendMode') != previous_blend_mode:
    raise SystemExit(f'blend apply previous blend mode mismatch: {blend_apply}')
if module_blend.get('requestedBlendMode') != requested_blend_mode:
    raise SystemExit(f'blend apply requested blend mode mismatch: {blend_apply}')
if module_blend.get('currentBlendMode') != requested_blend_mode:
    raise SystemExit(f'blend apply current blend mode mismatch: {blend_apply}')
if module_blend.get('previousReverseOrder') is not previous_reverse_order:
    raise SystemExit(f'blend apply previous reverse order mismatch: {blend_apply}')
if module_blend.get('requestedReverseOrder') is not requested_reverse_order:
    raise SystemExit(f'blend apply requested reverse order mismatch: {blend_apply}')
if module_blend.get('currentReverseOrder') is not requested_reverse_order:
    raise SystemExit(f'blend apply current reverse order mismatch: {blend_apply}')
if not isinstance(module_blend.get('historyBefore'), int) or not isinstance(module_blend.get('historyAfter'), int):
    raise SystemExit(f'blend apply history markers missing: {blend_apply}')
if module_blend.get('requestedHistoryEnd') != module_blend.get('historyAfter'):
    raise SystemExit(f'blend apply requested history end mismatch: {blend_apply}')

apply_items = module_items(blend_apply, target_key)
if len(apply_items) != 1:
    raise SystemExit(f'blend apply snapshot missing target item: {blend_apply}')
expect_close('blend apply snapshot opacity', (apply_items[0].get('blend') or {}).get('opacity'), requested_opacity)
if (apply_items[0].get('blend') or {}).get('blendMode') != requested_blend_mode:
    raise SystemExit(f'blend apply snapshot blend mode mismatch: {blend_apply}')
if (apply_items[0].get('blend') or {}).get('reverseOrder') is not requested_reverse_order:
    raise SystemExit(f'blend apply snapshot reverse order mismatch: {blend_apply}')

followup_items = module_items(blend_followup, target_key)
if len(followup_items) != 1:
    raise SystemExit(f'blend follow-up snapshot missing target item: {blend_followup}')
expect_close('blend follow-up snapshot opacity', (followup_items[0].get('blend') or {}).get('opacity'), requested_opacity)
if (followup_items[0].get('blend') or {}).get('blendMode') != requested_blend_mode:
    raise SystemExit(f'blend follow-up snapshot blend mode mismatch: {blend_followup}')
if (followup_items[0].get('blend') or {}).get('reverseOrder') is not requested_reverse_order:
    raise SystemExit(f'blend follow-up snapshot reverse order mismatch: {blend_followup}')

if blend_revert.get('status') != 'ok':
    raise SystemExit(f'blend revert failed: {blend_revert}')
revert_module_blend = blend_revert.get('moduleBlend') or {}
expect_close('blend revert current opacity', revert_module_blend.get('currentOpacity'), previous_opacity)
if revert_module_blend.get('currentBlendMode') != previous_blend_mode:
    raise SystemExit(f'blend revert current blend mode mismatch: {blend_revert}')
if revert_module_blend.get('currentReverseOrder') is not previous_reverse_order:
    raise SystemExit(f'blend revert current reverse order mismatch: {blend_revert}')

if unsupported_target.get('status') == 'found':
    if unsupported_blend.get('status') != 'unavailable' or unsupported_blend.get('reason') != 'unsupported-module-blend':
        raise SystemExit(f'unsupported blend response mismatch: {unsupported_blend}')
    if (unsupported_blend.get('moduleBlend') or {}).get('targetInstanceKey') != unsupported_target.get('instanceKey'):
        raise SystemExit(f'unsupported blend target mismatch: {unsupported_blend}')
else:
    if unsupported_blend.get('status') != 'skipped':
        raise SystemExit(f'unsupported blend skip mismatch: {unsupported_blend}')

if isinstance(blend_target.get('invalidBlendMode'), str) and blend_target.get('invalidBlendMode'):
    if invalid_blend_mode.get('status') != 'unavailable' or invalid_blend_mode.get('reason') != 'unsupported-module-blend-mode':
        raise SystemExit(f'invalid blend mode response mismatch: {invalid_blend_mode}')
    invalid_module_blend = invalid_blend_mode.get('moduleBlend') or {}
    if invalid_module_blend.get('targetInstanceKey') != target_key:
        raise SystemExit(f'invalid blend mode target mismatch: {invalid_blend_mode}')
    if invalid_module_blend.get('requestedBlendMode') != blend_target.get('invalidBlendMode'):
        raise SystemExit(f'invalid blend mode requested mode mismatch: {invalid_blend_mode}')
    if invalid_module_blend.get('currentBlendMode') != previous_blend_mode:
        raise SystemExit(f'invalid blend mode current mode mismatch: {invalid_blend_mode}')
    if invalid_module_blend.get('historyBefore') != invalid_module_blend.get('historyAfter'):
        raise SystemExit(f'invalid blend mode history should stay unchanged: {invalid_blend_mode}')
else:
    if invalid_blend_mode.get('status') != 'skipped':
        raise SystemExit(f'invalid blend mode skip mismatch: {invalid_blend_mode}')

print('apply-module-instance-blend:', json.dumps(blend_apply, separators=(",", ":")))
print('apply-module-instance-blend-followup:', json.dumps(blend_followup, separators=(",", ":")))
print('apply-module-instance-blend-revert:', json.dumps(blend_revert, separators=(",", ":")))
print('apply-module-instance-blend-invalid-mode:', json.dumps(invalid_blend_mode, separators=(",", ":")))
print('apply-module-instance-blend-unsupported:', json.dumps(unsupported_blend, separators=(",", ":")))
print('result: blend snapshot and blend mutation controls validated')
PY

python3 - "$mask_target_json" "$mask_reuse_target_json" "$mask_reuse_json" "$mask_clear_json" "$mask_unknown_source_json" "$unsupported_view_module_mask_json" <<'PY'
import json, sys

mask_target = json.loads(sys.argv[1])
mask_reuse_target = json.loads(sys.argv[2])
mask_reuse = json.loads(sys.argv[3])
mask_clear = json.loads(sys.argv[4])
mask_unknown_source = json.loads(sys.argv[5])
unsupported_view_mask = json.loads(sys.argv[6])

target_key = mask_target.get('instanceKey')
if not isinstance(target_key, str) or not target_key:
    raise SystemExit(f'mask target missing instance key: {mask_target}')

if mask_reuse_target.get('status') == 'found':
    reuse_target_key = mask_reuse_target.get('instanceKey')
    if mask_reuse.get('status') == 'ok':
        reuse_module_mask = mask_reuse.get('moduleMask') or {}
        if reuse_module_mask.get('targetInstanceKey') != reuse_target_key:
            raise SystemExit(f'mask reuse target mismatch: {mask_reuse}')
        if reuse_module_mask.get('action') != 'reuse-same-shapes':
            raise SystemExit(f'mask reuse action mismatch: {mask_reuse}')
        if reuse_module_mask.get('sourceInstanceKey') != target_key:
            raise SystemExit(f'mask reuse source mismatch: {mask_reuse}')
        if reuse_module_mask.get('currentHasMask') is not True:
            raise SystemExit(f'mask reuse currentHasMask mismatch: {mask_reuse}')
        if not (reuse_module_mask.get('sourceForms') or []):
            raise SystemExit(f'mask reuse sourceForms should not be empty on success: {mask_reuse}')
        if (reuse_module_mask.get('currentForms') or []) != (reuse_module_mask.get('sourceForms') or []):
            raise SystemExit(f'mask reuse current/source forms mismatch: {mask_reuse}')
        if reuse_module_mask.get('changed') is not True:
            raise SystemExit(f'mask reuse should report changed=true on success: {mask_reuse}')
        if reuse_module_mask.get('historyBefore') == reuse_module_mask.get('historyAfter'):
            raise SystemExit(f'mask reuse should advance history on success: {mask_reuse}')
    elif mask_reuse.get('status') == 'unavailable':
        if mask_reuse.get('reason') not in ('source-module-mask-unavailable', 'target-module-mask-not-clear'):
            raise SystemExit(f'mask reuse unavailable mismatch: {mask_reuse}')
        reuse_module_mask = mask_reuse.get('moduleMask') or {}
        if reuse_module_mask.get('currentHasMask') != reuse_module_mask.get('previousHasMask'):
            raise SystemExit(f'mask reuse unavailable currentHasMask mismatch: {mask_reuse}')
        if (reuse_module_mask.get('currentForms') or []) != (reuse_module_mask.get('previousForms') or []):
            raise SystemExit(f'mask reuse unavailable currentForms mismatch: {mask_reuse}')
    else:
        raise SystemExit(f'mask reuse unexpected status: {mask_reuse}')
else:
    if mask_reuse.get('status') != 'skipped':
        raise SystemExit(f'mask reuse skip mismatch: {mask_reuse}')

if mask_clear.get('status') != 'ok':
    raise SystemExit(f'mask clear failed: {mask_clear}')
module_mask = mask_clear.get('moduleMask') or {}
if module_mask.get('targetInstanceKey') != target_key:
    raise SystemExit(f'mask clear target mismatch: {mask_clear}')
if module_mask.get('action') != 'clear-mask':
    raise SystemExit(f'mask clear action mismatch: {mask_clear}')
if module_mask.get('currentHasMask') is not False:
    raise SystemExit(f'mask clear currentHasMask mismatch: {mask_clear}')
if (module_mask.get('currentForms') or []) != []:
    raise SystemExit(f'mask clear currentForms should be empty: {mask_clear}')
if not isinstance(module_mask.get('changed'), bool):
    raise SystemExit(f'mask clear changed missing: {mask_clear}')
if not isinstance(module_mask.get('historyBefore'), int) or not isinstance(module_mask.get('historyAfter'), int):
    raise SystemExit(f'mask clear history markers missing: {mask_clear}')
if module_mask.get('requestedHistoryEnd') != module_mask.get('historyAfter'):
    raise SystemExit(f'mask clear requested history end mismatch: {mask_clear}')
if module_mask.get('previousHasMask') is True:
    if module_mask.get('changed') is not True:
        raise SystemExit(f'mask clear should report changed=true when clearing shapes: {mask_clear}')
    if module_mask.get('historyBefore') == module_mask.get('historyAfter'):
        raise SystemExit(f'mask clear should advance history when clearing shapes: {mask_clear}')
else:
    if module_mask.get('changed') is not False:
        raise SystemExit(f'mask clear should report changed=false when already clear: {mask_clear}')
    if module_mask.get('historyBefore') != module_mask.get('historyAfter'):
        raise SystemExit(f'mask clear should not advance history when already clear: {mask_clear}')

mask_snapshot = (mask_clear.get('snapshot') or {})
module_stack = mask_snapshot.get('moduleStack') or []
matching = [item for item in module_stack if isinstance(item, dict) and item.get('instanceKey') == target_key]
if len(matching) != 1:
    raise SystemExit(f'mask clear snapshot target mismatch: {mask_clear}')

if mask_unknown_source.get('status') != 'unavailable' or mask_unknown_source.get('reason') != 'unknown-source-instance-key':
    raise SystemExit(f'mask unknown source mismatch: {mask_unknown_source}')
unknown_mask = mask_unknown_source.get('moduleMask') or {}
if unknown_mask.get('targetInstanceKey') != target_key:
    raise SystemExit(f'mask unknown source target mismatch: {mask_unknown_source}')
if unknown_mask.get('sourceInstanceKey') != 'missing#0#0#':
    raise SystemExit(f'mask unknown source key mismatch: {mask_unknown_source}')
if unknown_mask.get('currentHasMask') != unknown_mask.get('previousHasMask'):
    raise SystemExit(f'mask unknown source currentHasMask mismatch: {mask_unknown_source}')
if (unknown_mask.get('currentForms') or []) != (unknown_mask.get('previousForms') or []):
    raise SystemExit(f'mask unknown source currentForms mismatch: {mask_unknown_source}')

if unsupported_view_mask.get('status') != 'unavailable' or unsupported_view_mask.get('reason') != 'unsupported-view':
    raise SystemExit(f'mask unsupported-view mismatch: {unsupported_view_mask}')
if (unsupported_view_mask.get('session') or {}).get('view') != 'lighttable':
    raise SystemExit(f'mask unsupported-view session mismatch: {unsupported_view_mask}')

print('apply-module-instance-mask-reuse:', json.dumps(mask_reuse, separators=(",", ":")))
print('apply-module-instance-mask-clear:', json.dumps(mask_clear, separators=(",", ":")))
print('apply-module-instance-mask-unknown-source:', json.dumps(mask_unknown_source, separators=(",", ":")))
print('apply-module-instance-mask-unsupported-view:', json.dumps(unsupported_view_mask, separators=(",", ":")))
print('result: module mask controls validated')
PY
