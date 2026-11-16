#!/usr/bin/env bash
# Run minivisor in Bochs. Port 0xE9 output goes to stdout; Bochs log in build/bochs.log.
#   ./run.sh          run to completion
#   ./run.sh debug    stop at the first instruction in the Bochs debugger
set -euo pipefail
cd "$(dirname "$0")"
BOCHS_HOME=${BOCHS_HOME:-$HOME/.local/opt/bochs-3.1}
export BXSHARE=$BOCHS_HOME/share/bochs
if [[ ${1:-} == debug ]]; then
	exec "$BOCHS_HOME/bin/bochs" -q -f bochsrc
fi
"$BOCHS_HOME/bin/bochs" -q -f bochsrc -rc <(echo c) </dev/null 2>/dev/null \
	| grep -av '^\(<bochs:\|Next at\|(0)\|\[0x\|Bochs is exiting\|textconfig:\)' || true
