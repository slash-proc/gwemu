#!/bin/sh

set -eu

dir="$1"
GWEMU_DATE=$(date -u)
# Try git first, then the stamped files -- NOT "git if .git exists, else files".
# In a git worktree .git is a FILE (a gitdir: pointer), so `test -e .git`
# succeeds while git itself fails whenever the real git dir is not reachable --
# exactly what happens inside the aarch64 cross-build container, which bind
# mounts only the worktree. That produced a silent 0.0.0 build with an empty
# commit: a binary that cannot say which build it is, which is worse than a
# build failure during a cross-platform test round.
GWEMU_COMMIT=$( \
  cd "$dir"; \
  git rev-parse HEAD 2>/dev/null | tr -d '\n' || true)
if [ "${GWEMU_COMMIT}" = "" ] && test -e "$dir/GWEMU_COMMIT"; then
  GWEMU_COMMIT=$(cat "$dir/GWEMU_COMMIT")
fi

GWEMU_VERSION=$( \
  cd "$dir"; \
  git describe --tags --match 'v*' 2>/dev/null | cut -c 2- | tr -d '\n' || true)
if [ "${GWEMU_VERSION}" = "" ] && test -e "$dir/GWEMU_VERSION"; then
  GWEMU_VERSION=$(cat "$dir/GWEMU_VERSION")
fi

if [ "${GWEMU_VERSION}" = "" ]; then
  GWEMU_VERSION="0.0.0"
  echo "gwemu-version.sh: WARNING: no git metadata and no GWEMU_VERSION file" \
       "in $dir -- this binary will report version 0.0.0 and cannot be" \
       "identified. Stamp a GWEMU_VERSION/GWEMU_COMMIT file for out-of-git" \
       "builds." >&2
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
