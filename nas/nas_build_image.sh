#!/bin/sh
# Build the NAS image for the DXP2800 (amd64) with the web app of a release built in, and push it
# to the NAS's registry (plain HTTP). Usage: sh nas_build_image.sh [version]  (default: newest tag)
set -e
cd "$(dirname "$0")"
VERSION=${1:-$(git describe --tags --abbrev=0 | sed 's/^v//')}
echo "web app: v$VERSION"

podman build \
  --platform linux/amd64 \
  --format docker \
  --build-arg JR_VERSION="$VERSION" \
  -t dxp2800.local:5000/justride-nas:latest .

podman push --tls-verify=false dxp2800.local:5000/justride-nas:latest
