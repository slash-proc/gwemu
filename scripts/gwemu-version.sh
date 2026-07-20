#!/bin/sh

set -eu

dir="$1"
GWEMU_DATE=$(date -u)
GWEMU_COMMIT=$( \
  cd "$dir"; \
  if test -e .git; then \
    git rev-parse HEAD 2>/dev/null | tr -d '\n'; \
  elif test -e GWEMU_COMMIT; then \
    cat GWEMU_COMMIT; \
  fi)
GWEMU_VERSION=$( \
  cd "$dir"; \
  if test -e .git; then \
    git describe --tags --match 'v*' 2>/dev/null | cut -c 2- | tr -d '\n' || true; \
  elif test -e GWEMU_VERSION; then \
    cat GWEMU_VERSION; \
  fi)

if [ "${GWEMU_VERSION}" = "" ]; then
  GWEMU_VERSION="0.0.0"
fi

get_version_field() {
  echo ${GWEMU_VERSION}-0 | cut -d- -f$1
}

get_version_dot () {
  echo $(get_version_field 1) | cut -d. -f$1
}

GWEMU_VERSION_MAJOR=$(get_version_dot 1)
GWEMU_VERSION_MINOR=$(get_version_dot 2)
GWEMU_VERSION_PATCH=$(get_version_dot 3)
GWEMU_VERSION_COMMIT=$(get_version_field 2)

cat <<EOF
#define GWEMU_VERSION       "$GWEMU_VERSION"
#define GWEMU_VERSION_MAJOR $GWEMU_VERSION_MAJOR
#define GWEMU_VERSION_MINOR $GWEMU_VERSION_MINOR
#define GWEMU_VERSION_PATCH $GWEMU_VERSION_PATCH
#define GWEMU_VERSION_COMMIT $GWEMU_VERSION_COMMIT
#define GWEMU_COMMIT        "$GWEMU_COMMIT"
#define GWEMU_DATE          "$GWEMU_DATE"
EOF
