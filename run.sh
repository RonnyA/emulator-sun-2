#!/bin/sh

QUIET="-q"
ETHER=""

while [ $# -gt 0 ]; do
    case "$1" in
        --log)
            QUIET=""
            shift
            ;;
        --ether=*)
            ETHER="$1"
            shift
            ;;
        *)
            break
            ;;
    esac
done

sim/sim $QUIET $ETHER --prom=media/rom/sun2-multi-rev-R.bin --disk=media/disk/disk.img --tape=media/tape/tape "$@"

#
# after the failed boot prompt, type "b vmunix"
#
