#!/usr/bin/env bash
set -euo pipefail

VERSION="$1"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Validate version format (semver-like: digits.digits.digits with optional pre-release)
if ! [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[a-zA-Z0-9.]+)?$ ]]; then
  echo "Error: Invalid version format '$VERSION'. Expected semver (e.g., 1.2.3 or 1.2.3-beta.1)"
  exit 1
fi

echo "Stamping version $VERSION..."

VERSION="$VERSION" PKG_PATH="$ROOT/ts/package.json" node -e '
  const fs = require("fs");
  const pkg = JSON.parse(fs.readFileSync(process.env.PKG_PATH, "utf8"));
  pkg.version = process.env.VERSION;
  fs.writeFileSync(process.env.PKG_PATH, JSON.stringify(pkg, null, 2) + "\n");
'
echo "  Updated @breakawaydata/rive-render -> $VERSION"

echo "Done."
