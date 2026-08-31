#!/bin/sh
set -eu
export PATH="$HOME/.local/bin:$PATH"
pmbootstrap config service_manager openrc
echo "--- status ---"
pmbootstrap status 2>&1 | head -10
