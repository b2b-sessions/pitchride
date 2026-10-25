#!/bin/bash

set -e

DMG_FILE="${1}"
[ -z "${DMG_FILE}" ] && echo "Pass DMG file name as first argument." >&2 && exit 1
[ -z "${APPLE_CODESIGN_IDENTITY}" ] && echo 'Please set the APPLE_CODESIGN_IDENTITY env var.' >&2 && exit 1
[ -z "${APPLE_ID_USERNAME}" ] && echo 'Please set the APPLE_ID_USERNAME env var.' >&2 && exit 1
[ -z "${APPLE_APP_SPECIFIC_PASSWORD}" ] && echo 'Please set the APPLE_APP_SPECIFIC_PASSWORD env var.' >&2 && exit 1
[ -z "${APPLE_TEAM_ID}" ] && echo 'Please set the APPLE_TEAM_ID env var.' >&2 && exit 1

tmp_dir="$(mktemp -dt mixxx_notarize)"
# We want $tmp_dir to expand now, therefore we disable the check
# shellcheck disable=SC2064
trap "rm -rf '$tmp_dir'" EXIT

# B2B: the app's updater opens the disk image with nobody there to click
# Agree, so it must not ask for a licence.
if hdiutil imageinfo "${DMG_FILE}" 2>/dev/null | grep -qi "Software License Agreement: true"; then
    echo "::error title=The installer asks to Agree to a licence::The app couldn't update itself from it. Check CPACK_RESOURCE_FILE_LICENSE in packaging/CPackConfig.cmake."
    exit 1
fi

echo "==> Signing $DMG_FILE"
codesign --verbose=4 --sign "${APPLE_CODESIGN_IDENTITY}" "${DMG_FILE}"


echo "==> Notarizing $DMG_FILE"

credentials=(
    --apple-id "${APPLE_ID_USERNAME}"
    --password "${APPLE_APP_SPECIFIC_PASSWORD}"
    --team-id "${APPLE_TEAM_ID}"
)
submit_out="$tmp_dir/submit_out.txt"

xcrun notarytool submit "${credentials[@]}" "${DMG_FILE}" 2>&1 | tee "$submit_out"
REQUEST_ID="$(grep -e " id: " "$submit_out" | grep -oE '([0-9a-f-]{36})'| head -n1)"
rm "$submit_out"
xcrun notarytool wait "$REQUEST_ID" "${credentials[@]}" --timeout "${NOTARY_WAIT:-15m}" ||:
info="$(xcrun notarytool info "$REQUEST_ID" "${credentials[@]}" 2>&1 ||:)"
echo "$info"
status="$(echo "$info" | sed -n 's/^ *status: *//p' | head -n1)"

# B2B: Apple sometimes takes longer than the build should wait (a new
# developer account's first submissions can take hours). Still checking: the
# signed installer is uploaded as it is, and opens normally on a Mac that's
# online once Apple has finished (stapling only matters offline).
case "$status" in
    Accepted)
        # Right after Accepted, Apple's servers sometimes don't have the
        # approval to hand out yet (stapler exits 65 or 68): try again for a
        # few minutes. Still nothing: upload it unstapled (it opens normally
        # on a Mac that's online; stapling only matters offline).
        stapled=0
        for wait in 0 30 60 90 120; do
            sleep "$wait"
            echo "==> Stapling $DMG_FILE"
            if xcrun stapler staple "${DMG_FILE}"; then
                stapled=1
                break
            fi
        done
        if [ "$stapled" != 1 ]; then
            echo "::warning title=Approved by Apple, but not stapled::Submission ${REQUEST_ID} was accepted, but Apple's servers didn't hand out the approval to attach in time. The installer is uploaded; it opens normally on a Mac that's online."
        fi
        ;;
    "In Progress")
        echo "::warning title=Apple is still checking the installer::Submission ${REQUEST_ID}. The installer is signed and uploaded; it opens without warnings once Apple has finished (usually minutes; a new developer account's first ones can take hours). Not stapled, so the first open needs the internet."
        ;;
    *)
        xcrun notarytool log "$REQUEST_ID" "${credentials[@]}" ||:
        echo "::error title=Apple didn't accept the installer::Status: ${status:-unknown}. Apple's reasons are in the log above (submission ${REQUEST_ID})."
        exit 1
        ;;
esac
