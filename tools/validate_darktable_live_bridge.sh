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
unsupported_module_action_json=$(run_bridge apply-module-instance-action "$module_instance_key" unsupported-action)
unknown_module_instance_json=$(run_bridge apply-module-instance-action unknown#-1#-1#missing enable)

switch_to_lighttable
unsupported_view_snapshot_json=$(run_bridge get-snapshot)
unsupported_view_get_control_json=$(run_bridge get-control exposure.exposure)
unsupported_view_set_control_json=$(run_bridge set-control exposure.exposure "$requested_control_exposure")
unsupported_view_module_instance_action_json=$(run_bridge apply-module-instance-action "$module_instance_key" "$module_instance_action")
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

python3 - "$initial_json" "$snapshot_json" "$module_instance_target_json" "$list_json" "$get_control_json" "$set_json" "$post_set_exposure_json" "$set_control_json" "$post_set_control_json" "$unsupported_control_json" "$module_instance_action_json" "$module_instance_revert_json" "$unsupported_module_action_json" "$unknown_module_instance_json" "$unsupported_view_snapshot_json" "$unsupported_view_get_control_json" "$unsupported_view_set_control_json" "$unsupported_view_module_instance_action_json" "$requested_exposure" "$requested_control_exposure" "$asset_path" <<'PY'
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
unsupported_module_action = json.loads(sys.argv[13])
unknown_module_instance = json.loads(sys.argv[14])
unsupported_view_snapshot = json.loads(sys.argv[15])
unsupported_view_get = json.loads(sys.argv[16])
unsupported_view_set = json.loads(sys.argv[17])
unsupported_view_module_instance_action = json.loads(sys.argv[18])
requested = float(sys.argv[19])
requested_control = float(sys.argv[20])
asset = os.path.realpath(sys.argv[21])

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
print('unsupported-module-action:', json.dumps(unsupported_module_action, separators=(",", ":")))
print('unknown-module-instance:', json.dumps(unknown_module_instance, separators=(",", ":")))
print('unsupported-control:', json.dumps(unsupported, separators=(",", ":")))
print('unsupported-view-get-snapshot:', json.dumps(unsupported_view_snapshot, separators=(",", ":")))
print('unsupported-view-get-control:', json.dumps(unsupported_view_get, separators=(",", ":")))
print('unsupported-view-set-control:', json.dumps(unsupported_view_set, separators=(",", ":")))
print('unsupported-view-apply-module-instance-action:', json.dumps(unsupported_view_module_instance_action, separators=(",", ":")))
print('result: exposure controls and module-instance enable/disable actions validated')
PY
