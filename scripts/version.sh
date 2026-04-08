#!/usr/bin/env bash
set -euo pipefail

VERSION="$1"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

echo "Stamping version $VERSION across all packages..."

# Update all platform package versions
for dir in "$ROOT"/npm/*/; do
  pkg="$dir/package.json"
  node -e "
    const fs = require('fs');
    const pkg = JSON.parse(fs.readFileSync('$pkg', 'utf8'));
    pkg.version = '$VERSION';
    fs.writeFileSync('$pkg', JSON.stringify(pkg, null, 2) + '\n');
  "
  echo "  Updated $(basename "$dir") -> $VERSION"
done

# Update main package version + optionalDependencies
node -e "
  const fs = require('fs');
  const pkgPath = '$ROOT/ts/package.json';
  const pkg = JSON.parse(fs.readFileSync(pkgPath, 'utf8'));
  pkg.version = '$VERSION';
  if (pkg.optionalDependencies) {
    for (const key of Object.keys(pkg.optionalDependencies)) {
      if (key.startsWith('@breakawaydata/rive-render-')) {
        pkg.optionalDependencies[key] = '$VERSION';
      }
    }
  }
  fs.writeFileSync(pkgPath, JSON.stringify(pkg, null, 2) + '\n');
"
echo "  Updated @breakawaydata/rive-render -> $VERSION"

echo "Done."
