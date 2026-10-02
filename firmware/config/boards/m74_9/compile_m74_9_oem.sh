#!/usr/bin/env bash
# Kept as an alias: the OEM three-stage flow IS the default m74_9 build now
# (see compile_m74_9.sh). Run `compile_m74_9.sh` instead.
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/compile_m74_9.sh" "$@"
