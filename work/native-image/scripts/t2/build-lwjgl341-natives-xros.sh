#!/bin/bash
# build-lwjgl341-natives-xros.sh: the visionOS halves (xrsimulator, then xros) of build-lwjgl341-natives.sh.
exec "$(dirname "$0")/build-lwjgl341-natives.sh" xrsimulator xros
