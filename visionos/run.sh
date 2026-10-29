#!/bin/bash
# Build, install, stage and run the Klepton app — simulator or device.
#
#   ./run.sh              # the booted visionOS simulator (rung 2, dyld)
#   ./run.sh device       # a physical Vision Pro   (rung 3, AMFI)
#
# Env overrides:
#   KLEPTON_TARGET=<name>   which guest — `python3 targets.py --list`
#   KLEPTON_DEVICE=<udid>   skip device auto-detection
#   KLEPTON_SIM_UDID=<udid> which booted simulator (default: the first booted one —
#                           another session's iPhone counts; name yours)
#   KLEPTON_TEAM=<teamid>   override signing team
#   KLEPTON_BUNDLE_ID=...   override bundle id
#   KL_SKIP_STAGE=1         never stage, even on a target that has never had it
#   KL_STAGE=1              always stage, whatever the stamp says
#   KL_ANGLE_PREBUILT=1     wrap the ANGLE already in vendor/out, no relink
#   KL_NO_LAUNCH=1          build, install, stage — then stop (device AND simulator).
#                           On device Austin launches from the Home View, worn;
#                           on the simulator, launch with `xcrun simctl launch`.
#   KL_NO_INSTALL=1         device only: build the signed .app and a local IPA,
#                           then stop. Does not call devicectl. Austin installs.
#   KL_BUNDLE_PAYLOAD=1     copy the staged jars and assets into the .app
#                           (KleptonPayload.swift materializes a fresh container).
#                           Default ON for a vivecraft device build, OFF for the
#                           simulator. =0 keeps the devicectl stage as the only copy.
#
# Minecraft ownership gate: compiled IN for this script's Debug build and for
# Release. KL_MSA, KL_MC_PROFILES, KL_LAUNCHER, KL_AUTOBOOT, the env file and
# KL_LAUNCHER_SCRIPT do not bypass it. The only bypass is the Swift condition
# KL_OWNERSHIP_GATE_OFF, which this script does not pass and which Debug and
# Release in gen_xcodeproj.py do not define. A Release build that adds the
# condition fails (#error in KleptonMSA.swift). A public build must not add it
# to SWIFT_ACTIVE_COMPILATION_CONDITIONS. See DEVICE-INSTALL "Launch 19".
#
# Everything downstream of the target — the product name, and with it the
# .xcodeproj, the scheme, the .app and the derived-data directory — comes from
# targets.py, so two apps built from this tree never write to the same place.
set -euo pipefail
cd "$(dirname "$0")"
# 26.2 is the instance this tree builds. Export before targets.py so the
# LWJGL 3.4.1 natives are selected when the caller left MCV unset.
export MCV="${MCV:-26.2}"
eval "$(python3 targets.py "${KLEPTON_TARGET:-}")"
export KLEPTON_TARGET="$KLT_NAME"
BUNDLE_ID="${KLEPTON_BUNDLE_ID:-$KLT_BUNDLE}"
PRODUCT="${KLEPTON_PRODUCT_NAME:-$KLT_PRODUCT}"
PROJECT="${KLEPTON_PROJECT_NAME:-${KLT_PROJECT:-$PRODUCT}}"
MODE="${1:-sim}"

# Build the slice THIS run will install and not the other one. A device build
# never loads the simulator's runtime and vice versa, and each slice is a full
# compile of the shipping runtime — 14.5 s of a 30 s incremental `make xros` on
# an M1 Max. Exported rather than passed, because mkguest.sh reads the same
# variable and the two must agree: an app whose runtime is device-only and whose
# guest is simulator-only fails as an arch mismatch that names neither.
#
# `make xros` on its own still builds both, and still says so — that pair IS the
# gate (the simulator is a macOS kernel behind an iOS-shaped SDK, so a header
# present only there passes the simulator and fails the device), and this is a
# run of the app rather than a run of the gate.
export XROS_PLATFORMS
case "$MODE" in
  device) XROS_PLATFORMS=xros  ;;
  *)      XROS_PLATFORMS=xrsim ;;
esac

# --- Staging: skip by default, but not blindly ------------------------------
#
# The 2.2 GB upload is a ~20 minute step and the assets change only when the APK
# does, so re-running it every build is the difference between the device loop
# working and not. Hence: skip by default.
#
# The trap that stops this being a plain `KL_SKIP_STAGE=1` default is that a
# target which has NEVER been staged then launches with no assets, and Unity
# does not report that as missing assets — it reports it three layers up as
# "Not enough storage space to install required resources". That is
# an expensive sentence to debug when the real answer is "nothing was uploaded".
#
# So a stamp records that this target has been staged with this APK. No stamp
# means stage; stamp means skip. The stamp is keyed on the APK's size and mtime,
# so replacing the APK re-stages on its own rather than needing to be
# remembered. It cannot know about a data container the OS rotated underneath
# us — KL_STAGE=1 is the answer to that, and the symptom is the same sentence.
STAMP_DIR="build/staged"
# ...and the stamp is keyed on the LAYOUT as well as the APK, because what gets
# staged is not fixed. The Qt plugin .so files are staged too, and a stamp that
# knew only about the APK would skip them on every machine that had ever staged
# this target — a shell that aborts with `Could not find the Qt
# platform plugin "virtual"` and a run.sh that says the assets are already
# there. Bump this whenever stage_assets.sh stages something new.
STAGE_SCHEMA=4
stage_stamp() {   # <target-key>
  local apk="../$KLT_APK" sig=""
  [ -f "$apk" ] && sig=$(stat -f '%z-%m' "$apk" 2>/dev/null || true)
  # MCV (native vivecraft, stage_native.sh): another Minecraft instance is another stage; 1.21.5 keeps its old key.
  [ "${MCV:-26.2}" != 1.21.5 ] && sig="$sig-mc$MCV"
  echo "$STAMP_DIR/$(echo "$KLT_NAME-$1-$BUNDLE_ID-$sig-v$STAGE_SCHEMA" \
       | tr -c 'A-Za-z0-9._-' '_')"
}
# stage_if_needed <target-key> <stage_assets.sh args...>
#
# The metadata beside assets/ — apktool.yml, AndroidManifest.xml — goes across
# on EVERY run, outside the stamp and outside KL_SKIP_STAGE. 12 KB against the
# 2.2 GB the gate exists for, and gating them with the rest is how a device
# ended up carrying the right 1.3 GB OBB while the guest asked for 1.28's
# versionCode: the fix was in the build, the file was on the device, and the
# device loop runs KL_SKIP_STAGE=1 as a matter of course, so nothing ever
# re-staged. Non-fatal on its own — a container that is not there yet is the
# full stage's report to make, not this one's.
stage_if_needed() {
  local key="$1"; shift
  local stamp; stamp=$(stage_stamp "$key")
  if [ -n "${KL_SKIP_STAGE:-}" ]; then
    echo "  assets: skipped (KL_SKIP_STAGE=1)"
  elif [ -z "${KL_STAGE:-}" ] && [ -f "$stamp" ]; then
    echo "  assets: already staged for this target and APK (KL_STAGE=1 to redo)"
  else
    ./stage_assets.sh "$@"
    mkdir -p "$STAMP_DIR" && : > "$stamp"
    return 0
  fi
  ./stage_assets.sh --meta "$@" || echo "  metadata: could not stage (see above)"
}

# Knobs forwarded from this shell into the app, on both the device and simulator
# paths. A list rather than a line each, because the two paths spell the same
# thing differently (devicectl JSON vs SIMCTL_CHILD_*) and they were already
# drifting apart. KL_ANGLE_DIR is deliberately NOT here: only the app knows
# where its own bundle is, so kl_app_configure sets it (and an explicit one
# still wins, since it is set with overwrite=0).
#
# KL_IMMERSIVE was missing from this list until 2026-08-07, which made the
# documented `KL_IMMERSIVE=1 ./run.sh` a no-op: the shell had it, the app never
# did, and the run took the window-and-report path while looking like the
# compositor had failed to come up.
# Everything KL_* in this shell is forwarded, minus the few that belong to this
# script rather than to the app. A hand-maintained allow-list was the previous
# shape and it silently dropped KL_IMMERSIVE for a whole session: the shell had
# it, the app never did, and the run took the window path while looking like the
# compositor had failed to come up. A knob that is set and not delivered is worse
# than one that does not exist, so the default is now "forward it".
FORWARD_SKIP="KL_ANGLE_DIR KL_ANGLE_PREBUILT KL_SKIP_STAGE KL_STAGE KL_LOG_OUT KL_WATCH KL_QUIET KL_KEEP_LONGEST KL_NO_LAUNCH KL_NO_INSTALL KL_BUNDLE_PAYLOAD"
FORWARD=""
for K in $(env | sed -n 's/^\(KL_[A-Za-z0-9_]*\)=.*/\1/p' | sort); do
  case " $FORWARD_SKIP " in *" $K "*) continue ;; esac
  FORWARD="$FORWARD $K"
done

echo "[1/5] runtime + guest translations ($KLT_NAME)…"
# klepton-ld as well as the runtime, and not for tidiness: the translator bakes
# KLX_TSD_SLOT into every veneer it emits, so a stale one produces dylibs the
# fresh runtime refuses — "translated against TSD slot 300 but this runtime uses
# 500". The loader catches it by name, which is the right failure, but it is not
# a failure anyone should have to have.
(cd .. && make -s build/klepton-ld xros XROS_PLATFORMS="$XROS_PLATFORMS")
# Keep the embedded ANGLE current, but only when the checkout is already
# there: this must not turn `run.sh` into a surprise 12 GB clone. Without it,
# mkangle.sh below stops and names the bootstrap command instead.
# Only THIS run's slice, for the reason XROS_PLATFORMS exists: since the iOS 27
# SDK became the only one (2026-09-18) the device slice's `angle-ios` relink
# fails (undefined libSystem symbols against iPhoneOS27.0), and a simulator run
# has no use for that slice — it stopped every `./run.sh` at step 1.
# KL_ANGLE_PREBUILT=1 skips the relink and wraps whatever vendor/out/<slice>
# already holds: the way past the device slice's iOS-27-SDK relink failure for a
# guest that never draws through ANGLE (a Vulkan one), until that is fixed.
if [ -d ../vendor/.git ] && [ "${KL_ANGLE_PREBUILT:-0}" != 1 ]; then
  (cd .. && make -s "angle-$XROS_PLATFORMS")
elif [ "${KL_ANGLE_PREBUILT:-0}" = 1 ]; then
  prebuilt_egl="../vendor/out/$XROS_PLATFORMS/libEGL.framework/libEGL"
  if [ ! -f "$prebuilt_egl" ]; then
    echo "error: KL_ANGLE_PREBUILT=1 but $prebuilt_egl is missing — run 'make angle-$XROS_PLATFORMS' once, or unset KL_ANGLE_PREBUILT" >&2
    exit 1
  fi
  echo "  ANGLE: prebuilt vendor/out/$XROS_PLATFORMS kept (KL_ANGLE_PREBUILT=1), $(stat -f %Sm "$prebuilt_egl")"
fi
# A native target (no APK) has Darwin dylibs to wrap rather than ELF to
# translate — mknative.sh is its mkguest.sh.
if [ "$KLT_KIND" = native ]; then ./mknative.sh | tail -1; else ./mkguest.sh | tail -1; fi
./mkangle.sh | tail -1
# ...and the Vulkan renderer, for a guest whose graphics API is not GLES. Unlike
# ANGLE this needs no source checkout and no build — `make mvk` is a 180 MB
# prebuilt — so the script just re-wraps whatever is vendored and says so when
# nothing is. gen_xcodeproj.py embeds it only if this produced something.
./mkmvk.sh | tail -1
# mkicon.py imports Pillow. Homebrew `python3` may be a newer build that
# does not have it; python3.13 on this machine does. Use the first
# interpreter that can import it.
MKICON_PY=python3
for c in python3.13 python3; do
  if command -v "$c" >/dev/null 2>&1 && "$c" -c 'import PIL' >/dev/null 2>&1; then
    MKICON_PY=$c
    break
  fi
done
"$MKICON_PY" ./mkicon.py | tail -1

echo "[2/5] project…"
KLEPTON_BUILD_MODE="$MODE" python3 gen_xcodeproj.py | grep -E "^(wrote|  device bundle)"

# The device IPA has to launch with an empty data container (SideStore, no
# devicectl). The simulator edit loop does not pay for a 600 MB clone unless
# the variable is set. An explicit 0 stays off on device too.
if [ "$MODE" = device ] && [ "$KLT_NAME" = vivecraft ] && [ -z "${KL_BUNDLE_PAYLOAD+x}" ]; then
  KL_BUNDLE_PAYLOAD=1
  echo "  payload: on (vivecraft device default; KL_BUNDLE_PAYLOAD=0 to skip)"
fi
export KL_BUNDLE_PAYLOAD
XCODE_SETTINGS=("KL_BUNDLE_PAYLOAD=${KL_BUNDLE_PAYLOAD:-0}" "KLEPTON_TARGET=$KLT_NAME")
[ -n "${MCV:-}" ] && XCODE_SETTINGS+=("MCV=$MCV")

if [ "$MODE" = "device" ]; then
  # Device discovery goes through JSON: device names contain non-breaking
  # spaces, so column parsing silently picks up the wrong field.
  find_device() {
    local json; json=$(mktemp)
    xcrun devicectl list devices --json-output "$json" >/dev/null 2>&1 || true
    python3 -c '
import json, sys
try:
    devs = json.load(open(sys.argv[1]))["result"]["devices"]
except Exception:
    sys.exit(0)
phys = [d for d in devs
        if d.get("hardwareProperties", {}).get("reality") == "physical"
        and d.get("hardwareProperties", {}).get("platform") == "visionOS"]
if not phys:
    sys.exit(0)
phys.sort(key=lambda d: d.get("connectionProperties", {}).get("tunnelState") != "connected")
print(phys[0]["identifier"])
' "$json"
    rm -f "$json"
  }
  # KL_NO_INSTALL builds a local IPA and must not touch a paired headset.
  if [ -n "${KL_NO_INSTALL:-}" ]; then
    DEVID=""
    echo "[3/5] building for device (KL_NO_INSTALL=1, no device lookup)…"
  else
    DEVID="${KLEPTON_DEVICE:-$(find_device || true)}"
    [ -n "$DEVID" ] || { echo "!! no physical Vision Pro — pair it in Xcode, or set KLEPTON_DEVICE"; exit 1; }
    echo "[3/5] building for device ${DEVID}…"
  fi
  # generic/platform, not id= : the build must not depend on the device being awake.
  # Release is the public IPA. Debug is KL_XCODE_CONFIG=Debug. The ownership
  # gate's #error fires only if KL_OWNERSHIP_GATE_OFF is defined, which neither
  # configuration in gen_xcodeproj.py sets.
  CONFIG="${KL_XCODE_CONFIG:-Release}"
  set +e
  xcodebuild -project "$PROJECT.xcodeproj" -scheme "$PRODUCT" -configuration "$CONFIG" \
    -destination 'generic/platform=visionOS' -derivedDataPath "build/dd-device-$KLT_NAME" \
    -allowProvisioningUpdates "${XCODE_SETTINGS[@]}" build 2>&1 \
    | tee /tmp/vivecraft-visionos-xcode-device.log \
    | grep -E 'error:|bundle_payload:|\*\* BUILD'
  # xcodebuild's status, through the pipe. Testing for the .app is NOT enough: a
  # failed *incremental* build leaves the previous, complete and perfectly valid
  # bundle in place, so the install and launch below both succeed and run the OLD
  # binary — and the log then reads as "the change had no effect", which is the
  # most expensive possible way to be told about a compile error.
  BUILD_RC=${PIPESTATUS[0]}
  set -e
  APP="build/dd-device-$KLT_NAME/Build/Products/${CONFIG}-xros/$PRODUCT.app"
  [ "$BUILD_RC" = 0 ] || { echo "!! build FAILED (see errors above) — not installing a stale app"; exit 1; }
  [ -f "$APP/Info.plist" ] || { echo "!! no usable .app at $APP"; exit 1; }

  # Compile paths in this app and in the reused dylibs name the build
  # machine. Rewrite those copies, then sign again with the identity
  # xcodebuild just used. The identity name is not printed. The app's
  # entitlements (application-identifier first of all) are read before the
  # re-sign and given back to it: a plain --force drops them, and installd
  # then refuses the IPA ("integrity could not be verified"). Nested code is
  # signed one by one without entitlements, never with --deep.
  ENTS="build/dd-device-$KLT_NAME/app-entitlements.plist"
  codesign -d --entitlements - --xml "$APP" > "$ENTS" 2>/dev/null
  grep -q application-identifier "$ENTS" \
    || { echo "!! $APP has no application-identifier before the re-sign"; exit 1; }
  python3 scrub_paths.py "$APP"
  IDENT=$(codesign -dvv "$APP" 2>&1 | sed -n 's/^Authority=//p' | head -1)
  if [ -n "$IDENT" ]; then
    for fw in "$APP"/Frameworks/*; do
      codesign --force --sign "$IDENT" --timestamp=none "$fw"
    done
    codesign --force --sign "$IDENT" --timestamp=none --entitlements "$ENTS" "$APP"
  fi
  codesign --verify --deep --strict "$APP"
  codesign -d --entitlements - "$APP" 2>/dev/null | grep -q application-identifier \
    || { echo "!! re-signed $APP lost its entitlements"; exit 1; }

  if [ -n "${KL_NO_INSTALL:-}" ]; then
    # Signed .app only. Packaging the IPA is a local zip; nothing is sent to the headset.
    IPA_DIR="build/device-$KLT_NAME"
    rm -rf "$IPA_DIR"
    mkdir -p "$IPA_DIR/Payload"
    cp -R "$APP" "$IPA_DIR/Payload/"
    IPA="$IPA_DIR/$PRODUCT.ipa"
    ( cd "$IPA_DIR" && zip -qr "$PRODUCT.ipa" Payload )
    echo "[4/5] NOT installing (KL_NO_INSTALL=1)."
    echo "      app: $APP"
    echo "      ipa: $IPA"
    exit 0
  fi

  echo "[4/5] installing…"
  xcrun devicectl device install app --device "$DEVID" "$APP" | tail -2
  # Installing can rotate the data container, so assets are staged *after*.
  stage_if_needed "$DEVID" "$DEVID"

  # KL_NO_LAUNCH: install and stage, then STOP. Austin launches from the Home
  # View wearing the headset and reports back — a devicectl launch of a worn
  # title is not the same run (no passthrough hand-off, and the immersive space
  # opens against a device nobody is looking through). This is the default shape
  # of a "first device round" here, so it is a knob rather than a hand-run
  # `devicectl device install app` that would skip the staging step above.
  if [ -n "${KL_NO_LAUNCH:-}" ]; then
    echo "[5/5] NOT launching (KL_NO_LAUNCH=1) — installed and staged."
    echo "      Launch Klepton from the Home View wearing the headset."
    exit 0
  fi

  echo "[5/5] launching (report also renders on-screen)…"
  # KL_FRAMES carries the app past the initJni gate into the Android lifecycle
  # — which is where libil2cpp and its 3,083 veneers first load under
  # AMFI. Unset, the run stops at initJni, which is the gate's measurement and has to
  # stay takeable.
  ENVJSON='{"KL_AUTOBOOT":"1"'
  for K in $FORWARD; do
    eval "V=\${$K:-}"
    [ -z "$V" ] || ENVJSON="$ENVJSON,\"$K\":\"$V\""
  done
  ENVJSON="$ENVJSON}"
  # Which launch mode: --console only suits a run that ENDS. Two things make a
  # run open-ended, and it used to be only one of them — KL_FRAMES. The app now
  # opens the immersive space by default, and an immersive run has no frame
  # budget to exhaust, so `./run.sh device` with no KL_FRAMES would have sat on
  # a blocking --console forever waiting for an app that never exits.
  # ...and KL_SLINK_WAIT is the third: it is the Steam Link target's KL_FRAMES,
  # a bounded pump on the window path, and a run doing one still never exits on
  # its own — so --console would sit there until the app was quit by hand while
  # the log it should be pulling sat in the container.
  OPEN_ENDED=""
  [ -z "${KL_FRAMES:-}" ]     || OPEN_ENDED=1
  [ -z "${KL_SLINK_WAIT:-}" ] || OPEN_ENDED=1
  case "${KL_IMMERSIVE:-1}" in 0|no|off|false|"") ;; *) OPEN_ENDED=1 ;; esac

  # ...and the third mode, which is the one an open-ended run should take:
  # --console in the BACKGROUND, streaming into the local log.
  #
  # The two above are a false dichotomy that cost every device run minutes of
  # pure waiting. --console blocks until the app exits, so an immersive run
  # could not use it — but "blocks" is a property of running it in the
  # foreground, not of the bridge. Backgrounded, it delivers the app's stdout
  # and stderr live at no cost, and the watch loop becomes a local `wc -l`.
  #
  # What it replaces was expensive in a way the log's SIZE never was:
  # `devicectl device copy from` is ~27 s per call on this hardware regardless
  # of how many bytes it moves, and the poller made one every 5 s until the log
  # stopped growing for KL_QUIET (32) consecutive samples. That is ~32 s a
  # sample and up to an hour of transfer for a run measured in minutes.
  #
  # It needs KL_LOG_FILE=0 to be worth anything: kl_app.c redirects stdout into
  # the container by default, and a bridged stdout that has been freopen'd to a
  # file carries nothing. Set here rather than left to the caller, because the
  # two halves are one decision — a console launch whose app still logs to a
  # file is a run with no output at all, which is the worst of both.
  #
  # KL_CONSOLE=0 restores the copy-polling path. Keep it working: a launch from
  # the Home View has no console, and a run whose app dies still leaves the
  # container's file behind for the next launch to pull.
  CONSOLE=""
  case "${KL_CONSOLE:-1}" in 0|no|off|false|"") ;; *) [ -n "$OPEN_ENDED" ] && CONSOLE=1 ;; esac

  if [ -n "$CONSOLE" ]; then
    LOCAL="${KL_LOG_OUT:-/tmp/klepton-device.log}"
    ENVJSON="${ENVJSON%\}},\"KL_LOG_FILE\":\"0\"}"
    : > "$LOCAL"
    # A sentinel rather than a second copy of the OPEN_ENDED/KL_CONSOLE test:
    # build_run_vpro.sh has to know whether this log was STREAMED, because
    # pulling the container's file on top of a streamed one replaces this run's
    # output with an older run's. Recomputing the condition there would be two
    # copies of one decision, which is how they come to disagree.
    : > "$LOCAL.streamed"
    xcrun devicectl device process launch --device "$DEVID" --console \
      --terminate-existing --environment-variables "$ENVJSON" \
      "$BUNDLE_ID" >> "$LOCAL" 2>&1 &
    CONSOLE_PID=$!
    # Ctrl-C stops the app as well as the wait, which is what the old loop's
    # "Ctrl-C to stop" promised and could not deliver: there, the app was
    # detached and kept running.
    trap 'kill $CONSOLE_PID 2>/dev/null; echo; echo "    log: $LOCAL"; exit 0' INT
    echo "    streaming the app's output live (--console); done when it stops growing"
    last=-1; same=0
    for _ in $(seq 1 "${KL_WATCH:-120}"); do
      sleep 5
      n=$(wc -l < "$LOCAL" | tr -d ' ')
      printf '    %s lines\n' "$n"
      # No relaunch rule here, and none is needed: this file is OURS and is
      # appended to, so a device-side relaunch cannot truncate the evidence the
      # way it truncates the container's copy. It shows up as the log simply
      # continuing, which is the honest rendering of what happened.
      if [ "$n" = "$last" ]; then
        same=$((same+1))
        [ "$same" -lt "${KL_QUIET:-32}" ] || { echo "    output stopped"; break; }
      else
        same=0
      fi
      last="$n"
      kill -0 $CONSOLE_PID 2>/dev/null || { echo "    app exited"; break; }
    done
    trap - INT
    # NOT killed when the watch ends, and that is the whole difference between
    # this and a foreground --console: signals sent to devicectl are FORWARDED
    # to the app, so tearing the bridge down at the end of the loop would quit
    # the thing on the headset the moment the script stopped watching it. The
    # detached-launch path never did that, and a run you cannot keep looking at
    # is not the same measurement.
    #
    # Left streaming instead: it keeps appending to $LOCAL after this script
    # exits, and the next run's --terminate-existing ends both the app and this
    # bridge with it, so at most one is ever outstanding.
    if kill -0 $CONSOLE_PID 2>/dev/null; then
      if [ "${KL_CONSOLE_STOP:-0}" = 1 ]; then
        kill $CONSOLE_PID 2>/dev/null || true
        echo "    app stopped (KL_CONSOLE_STOP=1)"
      else
        echo "    app is STILL RUNNING on the headset and still streaming into"
        echo "    the log (KL_CONSOLE_STOP=1 to quit it here instead)"
      fi
    fi
    echo "    log: $LOCAL"
  elif [ -z "$OPEN_ENDED" ]; then
    # Boot-only shape: --console blocks until the app terminates, which for a UI app
    # means until it is quit by hand. Fine for a run that finishes in seconds.
    xcrun devicectl device process launch --device "$DEVID" --console \
      --terminate-existing --environment-variables "$ENVJSON" "$BUNDLE_ID"
  else
    # A lifecycle run takes minutes and the app still never exits on its own, so
    # --console would just sit there waiting to be force-quit — and force-quitting
    # is indistinguishable from the run having finished. Launch detached and watch
    # the log instead: it is line-buffered precisely so it can be read live.
    # --terminate-existing is NOT optional. Without it, launching while the
    # app is already running does not start a new process — it ACTIVATES the
    # existing one, which keeps the environment it was launched with and the
    # binary it already mapped. The launch reports success either way. So a
    # rebuilt app with new knobs silently does not run, the log is not rewritten
    # (it belongs to the old process, which has already finished writing), and
    # what you are looking at in the headset is the PREVIOUS build.
    #
    # This cost a wrong conclusion: two runs of a new floor-test immersive space
    # reported "black" that were the previous run still on screen. The earlier
    # runs in the same session had relaunched correctly only because the process
    # happened to have died first, which is what made it look reproducible.
    xcrun devicectl device process launch --device "$DEVID" --terminate-existing \
      --environment-variables "$ENVJSON" "$BUNDLE_ID" | tail -2
    echo
    echo "    watching the log; it is done when it stops growing (Ctrl-C to stop)"
    LOCAL="${KL_LOG_OUT:-/tmp/klepton-device.log}"
    rm -f "$LOCAL.streamed"        # this run's log came from the container
    last=-1; same=0
    for _ in $(seq 1 "${KL_WATCH:-120}"); do
      sleep 5
      # Into a temp first, and never let a SHORTER log replace a longer one: a
      # run that dies is relaunched by the system, and the fresh process
      # truncates klepton-boot.log on its way up. Copying straight onto $LOCAL
      # therefore replaces the crash we care about with the clean boot that
      # followed it, a few seconds later — the evidence destroys itself while
      # the poller reports steadily shrinking line counts.
      TMPLOG="$LOCAL.new"
      xcrun devicectl device copy from --device "$DEVID" \
        --domain-type appDataContainer --domain-identifier "$BUNDLE_ID" \
        --source Documents/klepton-boot.log --destination "$TMPLOG" >/dev/null 2>&1 || continue
      n=$(wc -l < "$TMPLOG" | tr -d ' ')
      prev=0; [ -f "$LOCAL" ] && prev=$(wc -l < "$LOCAL" | tr -d ' ')
      if [ "$n" -lt "$prev" ]; then
        mv "$TMPLOG" "$LOCAL.relaunch"
        echo "    app was relaunched ($n lines) — keeping the $prev-line log, new one in $LOCAL.relaunch"
        break
      fi
      mv "$TMPLOG" "$LOCAL"
      printf '    %s lines\n' "$n"
      if [ "$n" = "$last" ]; then
        same=$((same+1))
        # Quiet has to outlast the in-app watchdog (KL_ALARM, default 120 s) or
        # the poller declares the run over before the thing that would explain it
        # has had a chance to fire — which is how the first stalled device run was
        # read as "no report" when the report simply had not happened yet.
        [ "$same" -lt "${KL_QUIET:-32}" ] || { echo "    log stopped growing"; break; }
      else
        same=0
      fi
      last="$n"
    done
    echo "    log: $LOCAL"
  fi
  exit 0
fi

UDID="${KLEPTON_SIM_UDID:-$(xcrun simctl list devices booted | grep -o '[0-9A-F-]\{36\}' | head -1)}"
[ -n "$UDID" ] || { echo "!! no booted visionOS simulator"; exit 1; }

echo "[3/5] building for the simulator…"
set +e
xcodebuild -project "$PROJECT.xcodeproj" -scheme "$PRODUCT" \
  -destination "platform=visionOS Simulator,id=$UDID" \
  -derivedDataPath "build/dd-sim-$KLT_NAME" CODE_SIGNING_ALLOWED=NO "${XCODE_SETTINGS[@]}" build 2>&1 \
  | tee /tmp/vivecraft-visionos-xcode-sim.log \
  | grep -E 'error:|bundle_payload:|\*\* BUILD'
BUILD_RC=${PIPESTATUS[0]}
set -e
APP="build/dd-sim-$KLT_NAME/Build/Products/Debug-xrsimulator/$PRODUCT.app"
[ "$BUILD_RC" = 0 ] || { echo "!! build FAILED (see errors above) — not installing a stale app"; exit 1; }
[ -f "$APP/Info.plist" ] || { echo "!! no usable .app at $APP"; exit 1; }
# Same compile-path rewrite as the device app, before the ad-hoc sign below.
python3 scrub_paths.py "$APP"
# Ad-hoc sign what Frameworks/ embeds. CODE_SIGNING_ALLOWED=NO also turns off
# CodeSignOnCopy, and under Xcode 27 nothing else signs the klepton-ld
# translations or MoltenVK any more (August's simulator bundles came out
# `Signature=adhoc`; 2026-09-27's came out "not signed at all"). An arm64 page
# must carry a signature to execute even on the simulator, so every guest dlopen
# then failed at the first library ("translated dylib present but failed to
# load"). Ad-hoc is all the simulator asks for; the device build signs for real.
for fw in "$APP"/Frameworks/*.framework "$APP"/Frameworks/*.dylib; do
  [ -e "$fw" ] || continue
  codesign --force --sign - --timestamp=none "$fw" >/dev/null 2>&1 \
    || echo "!! could not ad-hoc sign $fw"
done
# The path rewrite touches the main executable too. Ad-hoc is enough here.
codesign --force --sign - --timestamp=none "$APP/$PRODUCT" >/dev/null 2>&1 \
  || echo "!! could not ad-hoc sign $APP/$PRODUCT"

echo "[4/5] installing + staging…"
xcrun simctl install "$UDID" "$APP"
# After install, not before: simctl rotates the data container on reinstall, so
# assets staged earlier end up in an orphaned one and the app reports them
# missing. Cost a confusing run once.
stage_if_needed "sim-$UDID"

# The simulator honours KL_NO_LAUNCH too: install and stage onto a device
# another session may be testing on is safe, launching is not (it backgrounds
# their app — ~/dev/CLAUDE.md, "Lanes").
if [ -n "${KL_NO_LAUNCH:-}" ]; then
  echo "[5/5] NOT launching (KL_NO_LAUNCH=1) — installed and staged on $UDID."
  exit 0
fi

echo "[5/5] launching…"
# `env`, not a bare prefix: bash only treats NAME=value as an assignment when it
# is literally there before expansion, so ${KL_FRAMES:+SIMCTL_CHILD_KL_FRAMES=…}
# is parsed as a *command* name and dies with "command not found".
SIMENV=(SIMCTL_CHILD_KL_AUTOBOOT=1)
for K in $FORWARD; do
  eval "V=\${$K:-}"
  [ -z "$V" ] || SIMENV+=("SIMCTL_CHILD_$K=$V")
done
env "${SIMENV[@]}" xcrun simctl launch --terminate-running-process "$UDID" "$BUNDLE_ID"
LOG="$(xcrun simctl get_app_container "$UDID" "$BUNDLE_ID" data)/Documents/klepton-boot.log"
echo "    log: $LOG"
n=0; until [ -f "$LOG" ] || [ $n -ge 20 ]; do sleep 1; n=$((n+1)); done
sleep 10
LC_ALL=C grep -aE 'EXIT CRITERION|FAIL|translated dylib|natives registered|ids requested' "$LOG" || true
