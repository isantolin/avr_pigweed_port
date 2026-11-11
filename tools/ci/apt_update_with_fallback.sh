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

  # Preserve deb822 suites/components/signing settings; change only Ubuntu URIs.
  if ! sudo sed -i -E \
      "s#^URIs:[[:space:]]*https?://[^[:space:]]*ubuntu[^[:space:]]*/ubuntu/?[[:space:]]*\$#URIs: $mirror/#I" \
      "$SOURCES_FILE"; then
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
