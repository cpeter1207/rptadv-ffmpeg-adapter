#!/bin/sh
# Instrument the adapter-owned C bridge for the amd64 production coverage gate.
exec cc --coverage "$@"
