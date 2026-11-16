#!/usr/bin/env bash
# Sets up an isolated environment for ui_flow_test: fresh XDG dirs (so
# the real user's calendar cache/settings are never touched), a private
# D-Bus session with an unlocked gnome-keyring as the Secret Service, and
# the fake CalDAV server. Usage: tests/run_ui_flow_test.sh <build-dir>
set -euo pipefail
BUILD="${1:-build}"
HERE="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"
export XDG_DATA_HOME="$TMP/data" XDG_CONFIG_HOME="$TMP/config" XDG_RUNTIME_DIR="$TMP/run" HOME="$TMP/home"
mkdir -p "$XDG_DATA_HOME" "$XDG_CONFIG_HOME" "$XDG_RUNTIME_DIR" "$HOME"
chmod 700 "$XDG_RUNTIME_DIR"
export QT_QPA_PLATFORM=offscreen

python3 "$HERE/fake_caldav.py" 8765 &
SERVER=$!
python3 "$HERE/fake_google.py" 8766 &
GOOGLE=$!
trap 'kill $SERVER $GOOGLE 2>/dev/null; rm -rf "$TMP"' EXIT
# Point the real Google code at the fake Google.
export FAKE_GOOGLE_URL="http://127.0.0.1:8766"
export LOWACAL_GOOGLE_CLIENT_ID="test-client-id.apps.googleusercontent.com"
export LOWACAL_GOOGLE_AUTH_URL="$FAKE_GOOGLE_URL/auth"
export LOWACAL_GOOGLE_TOKEN_URL="$FAKE_GOOGLE_URL/token"
export LOWACAL_GOOGLE_CALDAV_URL="$FAKE_GOOGLE_URL/caldav/v2/"
sleep 0.5
export FAKE_CALDAV_URL="http://127.0.0.1:8765"

# Three runs: English system, Ukrainian system, and English system with
# Ukrainian picked in Settings. Each gets fresh data/config/keychain.
run() {
  rm -rf "$XDG_DATA_HOME" "$XDG_CONFIG_HOME"; mkdir -p "$XDG_DATA_HOME" "$XDG_CONFIG_HOME"
  env LANG="$1" LC_ALL="$1" LOWACAL_TEST_UI_LANGUAGE="$2" dbus-run-session -- bash -c '
    eval "$(echo -n testpass | gnome-keyring-daemon --unlock --components=secrets 2>/dev/null)"
    sleep 0.5
    "'"$BUILD"'/lowacal_ui_flow_test"
  '
}
status=0
rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"
dbus-run-session -- "$BUILD/lowacal_platform_test" || status=1
run en_US.UTF-8 ""   || status=1
run uk_UA.UTF-8 ""   || status=1
run en_US.UTF-8 "uk" || status=1
exit $status
