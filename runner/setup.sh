#!/usr/bin/env bash
# Install and register a GitHub Actions self-hosted runner for this repository on an x86-64 Linux
# host that has Vivado. usage: runner/setup.sh <registration-token> [labels]
set -euo pipefail
TOKEN=${1:?registration token}; LABELS=${2:-vivado-2022.2}
VER=2.325.0
mkdir -p "$HOME/actions-runner" && cd "$HOME/actions-runner"
[ -f run.sh ] || { curl -sL -o runner.tar.gz "https://github.com/actions/runner/releases/download/v$VER/actions-runner-linux-x64-$VER.tar.gz"; tar xzf runner.tar.gz; rm runner.tar.gz; }
./config.sh --unattended --url https://github.com/invictus737/pluto-nano-timestamp --token "$TOKEN" --labels "$LABELS" --name "$(hostname)-vivado" --work _work
sudo ./svc.sh install && sudo ./svc.sh start
