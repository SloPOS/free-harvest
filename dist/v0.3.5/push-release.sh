#!/usr/bin/env bash
#
# Publish Free Harvest v0.3.5 and mark the withdrawn 0.3.x releases.
#
# Safe to re-run: every step checks whether it is already done, so a partial
# failure (likely while GitHub is degraded) can simply be retried.
#
#   bash dist/v0.3.5/push-release.sh
#
set -uo pipefail

TAG="v0.3.5"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"

# Nothing to withdraw this time.
WITHDRAWN=()   # v0.3.4 stays published - it works fine for a single client

cd "$REPO_ROOT"

say() { printf '\n=== %s\n' "$*"; }
die() { printf '\nFAILED: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- preflight
say "Checking GitHub is reachable"
gh auth status >/dev/null 2>&1 || die "gh is not authenticated"
gh repo view --json name >/dev/null 2>&1 \
  || die "cannot reach the repo - GitHub may still be down. Try again later."

for f in bootloader.bin partition-table.bin ota_data_initial.bin \
         hr_wifi_adapter.bin RELEASE_NOTES.md; do
  [ -f "$HERE/$f" ] || die "missing asset: $HERE/$f"
done

# The two things this release exists to deliver. Refuse to ship without them.
say "Verifying the build actually contains the fixes"
grep -q "define CONFIG_TINYUSB_TASK_STACK_SIZE 8192" \
     "$REPO_ROOT/build/config/sdkconfig.h" \
  || die "TinyUSB stack is NOT 8192 - this build still has the crash. Rebuild."
grep -q "define CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1" \
     "$REPO_ROOT/build/config/sdkconfig.h" \
  || die "rollback is NOT enabled in this build - rebuild first."
grep -q "define CONFIG_LWIP_MAX_SOCKETS 16" \
     "$REPO_ROOT/build/config/sdkconfig.h" \
  || die "LWIP_MAX_SOCKETS is not 16 - this build still has the mobile ENFILE."

say "Running the host test suite"
bash test/run_tests.sh >/dev/null 2>&1 || die "tests fail - not shipping"

# ---------------------------------------------------------------- push code
say "Pushing commits"
git push origin HEAD || die "could not push commits"

say "Pushing tag $TAG"
if git ls-remote --tags origin | grep -q "refs/tags/$TAG$"; then
  echo "already on the remote, skipping"
else
  git push origin "$TAG" || die "could not push tag"
fi

# ---------------------------------------------------------------- release
say "Creating release $TAG"
if gh release view "$TAG" >/dev/null 2>&1; then
  echo "release exists; updating notes and re-uploading assets"
  gh release edit "$TAG" \
    --title "Free Harvest $TAG - fixes the mobile-only ENFILE socket exhaustion" \
    --notes-file "$HERE/RELEASE_NOTES.md" --latest || die "could not edit release"
  gh release upload "$TAG" \
    "$HERE/bootloader.bin" "$HERE/partition-table.bin" \
    "$HERE/ota_data_initial.bin" "$HERE/hr_wifi_adapter.bin" \
    --clobber || die "could not upload assets"
else
  gh release create "$TAG" \
    --title "Free Harvest $TAG - fixes the mobile-only ENFILE socket exhaustion" \
    --notes-file "$HERE/RELEASE_NOTES.md" \
    --latest \
    "$HERE/bootloader.bin" "$HERE/partition-table.bin" \
    "$HERE/ota_data_initial.bin" "$HERE/hr_wifi_adapter.bin" \
    || die "could not create release"
fi

# ------------------------------------------------------------- withdrawals
# Prepend the banner to each withdrawn release, PRESERVING its existing notes.
MARKER="Withdrawn — do not install"
for old in ${WITHDRAWN[@]+"${WITHDRAWN[@]}"}; do
  say "Marking $old withdrawn"
  if ! gh release view "$old" >/dev/null 2>&1; then
    echo "no such release, skipping"
    continue
  fi
  body="$(gh release view "$old" --json body --jq .body)" \
    || die "could not read $old notes"
  case "$body" in
    *"$MARKER"*) echo "already marked, skipping"; continue ;;
  esac
  tmp="$(mktemp)"
  cat "$HERE/DEPRECATE.md" > "$tmp"
  printf '%s\n' "$body" >> "$tmp"
  gh release edit "$old" --notes-file "$tmp" || die "could not update $old"
  rm -f "$tmp"
  echo "banner added"
done

say "Done"
gh release list -L 6
