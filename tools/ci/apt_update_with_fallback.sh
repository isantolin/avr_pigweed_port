#!/usr/bin/env bash
# Recover APT on GitHub-hosted Ubuntu runners when Canonical/Azure mirrors
# cannot be resolved or reached. Never proceed with incomplete package indexes.
set -Eeuo pipefail

readonly SOURCES_FILE="/etc/apt/sources.list.d/ubuntu.sources"
readonly FALLBACK_MIRRORS=(
  "https://ubuntu.c3sl.ufpr.br/ubuntu"
  "https://mirrors.edge.kernel.org/ubuntu"
  "https://mirror.units.it/ubuntu/ubuntu"
)
readonly APT_OPTIONS=(
  -o Acquire::Retries=2
  -o Acquire::http::Timeout=20
  -o Acquire::https::Timeout=20
)

apt_update() {
  local log status=0
  log="$(mktemp)"
  sudo timeout --signal=TERM --kill-after=15s 180s apt-get "${APT_OPTIONS[@]}" update 2>&1 | tee "$log" || status=$?

  # apt-get can return zero while retaining stale indexes for failed sources.
  if grep -Eqi 'Failed to fetch|Some index files failed|Temporary failure resolving' "$log"; then
    status=1
  fi
  rm -f "$log"
  return "$status"
}

echo "::group::APT update using the runner's configured sources"
if apt_update; then
  echo "Configured Ubuntu mirrors are reachable."
  echo "::endgroup::"
  exit 0
fi
echo "::endgroup::"

if ! command -v curl >/dev/null 2>&1; then
  echo "::error::curl is unavailable; cannot validate fallback mirrors."
  exit 1
fi
if ! sudo test -f "$SOURCES_FILE"; then
  echo "::error::Expected Ubuntu deb822 source file $SOURCES_FILE is missing; refusing to rewrite unknown APT sources."
  exit 1
fi

mapfile -t suites < <(
  sudo awk 'tolower($1) == "suites:" { for (i = 2; i <= NF; i++) print $i }' "$SOURCES_FILE" |
    sort -u
)
if (("${#suites[@]}" == 0)); then
  echo "::error::No Ubuntu suites found in $SOURCES_FILE."
  exit 1
fi

backup="${RUNNER_TEMP:-/tmp}/ubuntu.sources.original"
sudo cp --preserve=mode,ownership "$SOURCES_FILE" "$backup"

restore_sources() {
  sudo cp "$backup" "$SOURCES_FILE"
}

for mirror in "${FALLBACK_MIRRORS[@]}"; do
  echo "::group::Check fallback mirror $mirror"
  available=true
  for suite in "${suites[@]}"; do
    if ! curl --fail --silent --show-error --location \
        --retry 1 --connect-timeout 8 --max-time 20 \
        --output /dev/null "$mirror/dists/$suite/InRelease"; then
      echo "Suite '$suite' is unavailable from $mirror."
      available=false
      break
    fi
  done
  echo "::endgroup::"

  if [[ "$available" != true ]]; then
    continue
  fi

  # The hosted runner uses deb822 with a mirror+file: URI. Replace that
  # indirection as well as direct Canonical/Azure Ubuntu archive URIs.
  if ! MIRROR="$mirror" python3 - "$SOURCES_FILE" <<'PY'
import os
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
mirror = os.environ["MIRROR"].rstrip("/") + "/"
lines = path.read_text().splitlines()
replaced = 0
for index, line in enumerate(lines):
    if not line.lstrip().lower().startswith("uris:"):
        continue
    prefix, value = line.split(":", 1)
    uris = value.split()
    updated = []
    for uri in uris:
        if uri.startswith("mirror+file:") or "archive.ubuntu.com/ubuntu" in uri:
            updated.append(mirror)
            replaced += 1
        else:
            updated.append(uri)
    lines[index] = prefix + ": " + " ".join(updated)
if replaced == 0:
    raise SystemExit("No mirror+file or Canonical/Azure Ubuntu URI found")
path.write_text("\n".join(lines) + "\n")
print(f"Replaced {replaced} Ubuntu URI(s) with {mirror}")
PY
  then
    restore_sources
    echo "::warning::Could not update Ubuntu source URI; trying next mirror."
    continue
  fi

  if ! sudo grep -Fq "URIs: $mirror/" "$SOURCES_FILE"; then
    restore_sources
    echo "::warning::No Ubuntu URI was replaced; trying next mirror."
    continue
  fi

  echo "::notice::Retrying APT through $mirror"
  if apt_update; then
    echo "APT update succeeded using $mirror."
    exit 0
  fi
  restore_sources
  echo "::warning::APT update failed through $mirror; trying next candidate."
done

restore_sources
echo "::error::All Ubuntu mirrors failed. Original APT sources were restored."
exit 1
