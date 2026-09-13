#!/bin/sh
## @file
## @brief Run a command in a labeled, disposable exact-workspace container.
set -eu

if [ "$#" -lt 2 ]; then
	printf '%s\n' "usage: $0 IMAGE COMMAND [ARG...]" >&2
	exit 2
fi

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
image=$1
shift
scope=$(printf '%s' "$root" | cksum | awk '{print $1}')
project_label='org.rptadvanced.test.project=rptadv-ffmpeg-adapter'
scope_label="org.rptadvanced.test.scope=$scope"
name="rptadv-ffmpeg-adapter-test-$scope-$$"
pull_image=${RPTADV_CONTAINER_PULL:-1}

cleanup_stale()
{
	stale=$(docker container ls --all --quiet --filter 'label=rpt_advanced.test=true' \
		--filter "label=$project_label" --filter "label=$scope_label")
	[ -n "$stale" ] || return 0
	while IFS= read -r container; do
		[ -n "$container" ] || continue
		docker container rm --force "$container" >/dev/null
	done <<EOF
$stale
EOF
}

cleanup_current()
{
	docker container rm --force "$name" >/dev/null 2>&1 || true
}

cleanup_stale
trap cleanup_current EXIT

case $pull_image in
	1)
		docker image pull "$image" >&2
		;;
	0)
		docker image inspect "$image" >/dev/null
		;;
	*)
		printf '%s\n' 'RPTADV_CONTAINER_PULL must be 0 or 1' >&2
		exit 2
		;;
esac

host_root=$root
case $(uname -s) in
	MINGW*|MSYS*)
		host_root=$(cd "$root" && pwd -W)
		export MSYS_NO_PATHCONV=1
		;;
esac

docker run --rm --name "$name" --label rpt_advanced.test=true \
	--label "$project_label" --label "$scope_label" \
	--volume "$host_root:/workspace" --workdir /workspace "$image" "$@"

