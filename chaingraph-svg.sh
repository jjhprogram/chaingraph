#!/bin/bash
# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#
# Trace with chaingraph and render an interactive chain graph SVG.
#
#   sudo ./chaingraph-svg.sh OUT.svg [chaingraph options...] DURATION
#
# Example:
#   sudo ./chaingraph-svg.sh chain.svg -d 4 -m 100 10
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
if [ $# -lt 2 ]; then
	echo "usage: $0 OUT.svg [chaingraph options...] DURATION" >&2
	exit 2
fi
out=$1
shift

folded=$(mktemp)
trap 'rm -f "$folded"' EXIT

"$here/build/chaingraph" -f "$@" > "$folded"
"$here/FlameGraph/flamegraph.pl" --colors=chain --countname=us \
	--title="Chain Graph" --subtitle="off-CPU stack (bottom), waker chain above" \
	< "$folded" > "$out"
if [ -n "${SUDO_USER:-}" ]; then
	chown "$SUDO_USER": "$out"
fi
echo "wrote $out ($(wc -l < "$folded") chains)" >&2
