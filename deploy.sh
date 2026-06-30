#!/bin/bash
set -e
source ~/emsdk/emsdk_env.sh
make deploy
git add -A
git commit -m "Deploy: update WASM build"
git push origin main
echo "✓ Deployed to https://clare.dobuki.net"
