#!/usr/bin/env bash
# Fetch the prjxray Zynq-7000 database (sparse) and the prjxray Python package.
set -euo pipefail
cd "$(dirname "$0")"
python3 -m venv venv && ./venv/bin/pip install -q fasm "git+https://github.com/f4pga/prjxray.git"
git clone -q --filter=blob:none --no-checkout https://github.com/f4pga/prjxray-db.git db
git -C db sparse-checkout init --cone && git -C db sparse-checkout set zynq7 && git -C db checkout -q
echo "db ready: $(du -sh db/zynq7 | cut -f1)"
