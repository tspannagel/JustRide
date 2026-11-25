#!/bin/sh
# Build the NAS image for the DXP2800 (amd64) with the web app of a release built in, and push it
# to the NAS's registry (plain HTTP). Usage: sh nas_build_image.sh [version]  (default: newest tag)
set -e
cd "$(dirname "$0")"
# Newest release tag overall (not the newest one in the checked-out branch's history);
# versionsort.suffix sorts 1.0.0-beta.3 before 1.0.0
git fetch --tags --quiet 2>/dev/null || true
VERSION=${1:-$(git -c versionsort.suffix=- tag --list 'v*' --sort=-v:refname | head -n 1 | sed 's/^v//')}
echo "web app: v$VERSION"

podman build \
  --platform linux/amd64 \
  --format docker \
  --build-arg JR_VERSION="$VERSION" \
  -t dxp2800.local:5000/justride-nas:latest .

podman push --tls-verify=false dxp2800.local:5000/justride-nas:latest
