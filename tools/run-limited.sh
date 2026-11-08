#!/bin/sh
# Bound each process's address space and the whole command's wall-clock time.
set -eu

memory_mib=512
seconds=120
while [ "$#" -gt 0 ]; do
	case "$1" in
	--memory-mib) memory_mib=$2; shift 2 ;;
	--seconds) seconds=$2; shift 2 ;;
	--) shift; break ;;
	*) break ;;
	esac
done
case "$memory_mib:$seconds" in
	*[!0-9:]*|:*|*:) echo 'Limits must be positive integers' >&2; exit 2 ;;
esac
if [ "$memory_mib" -eq 0 ] || [ "$seconds" -eq 0 ] || [ "$#" -eq 0 ]; then
	echo 'Usage: run-limited.sh [--memory-mib N] [--seconds N] command [args...]' >&2
	exit 2
fi
ulimit -v "$((memory_mib * 1024))"
# Keep timeout's own process group so descendants are terminated as well.
exec timeout --kill-after=5s "${seconds}s" "$@"
