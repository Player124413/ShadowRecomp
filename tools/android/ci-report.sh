#!/usr/bin/env bash
#
# Puts the lines that explain a failed build on the summary page of a GitHub Actions run.
#
# Usage: tools/android/ci-report.sh <log directory>
#
# The build-android workflow keeps the output of each long phase in <phase>.log and
# creates <phase>.ok once the phase is through. The log of the native build alone runs to
# tens of thousands of lines, so for every phase that did not finish this prints error
# annotations with the first lines that look like errors, the last ones, and the end of
# the log. An annotation holds 4096 characters, and a step can make ten of them.

set -u

logs="${1:?usage: ci-report.sh <log directory>}"
limit=3900

annotate() {    # title, text
    local text="$2"
    [ -n "${text}" ] || return 0
    text="${text//'%'/'%25'}"
    text="${text//$'\r'/'%0D'}"
    text="${text//$'\n'/'%0A'}"
    echo "::error title=$1::${text}"
}

reported=0
for phase in tests configure build gradle; do
    log="${logs}/${phase}.log"
    if [ ! -s "${log}" ] || [ -e "${logs}/${phase}.ok" ]; then
        continue
    fi
    reported=1

    # Shorter lines fit more of them: drop the checkout path and anything past 300 characters.
    clean="$(sed -e "s#${GITHUB_WORKSPACE:-/nonexistent}/##g" -e 's/\x1b\[[0-9;]*[A-Za-z]//g' "${log}" | cut -c1-300)"

    # Not errors: CMake's "-- Looking for ... - not found", Ninja's progress lines (file
    # names can have "Error" in them) and compiler warnings.
    errors="$(printf '%s\n' "${clean}" \
        | grep -n -E '(error|Error|ERROR|FAILED|fatal|undefined (reference|symbol)|What went wrong|Exception|No such file|: not found|cannot|Could not|denied)' \
        | grep -v -E '(^[0-9]+:-- |^[0-9]+:\[[0-9]+/[0-9]+\] |-Werror|-Wno-error|error_code|ErrorCode|warning: |\[-W[a-zA-Z#-]+\]$)' || true)"

    first="${errors:0:${limit}}"
    annotate "${phase}: first errors" "${first}"
    if [ "${#errors}" -gt "${limit}" ]; then
        annotate "${phase}: last errors" "${errors: -${limit}}"
    fi
    annotate "${phase}: end of the log" "$(printf '%s\n' "${clean}" | tail -n 60 | tail -c "${limit}")"
done

if [ "${reported}" -eq 0 ]; then
    echo "No phase with a log failed; the failed step's own output has the reason."
fi
