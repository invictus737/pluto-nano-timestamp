# Self-hosted build runner

GitHub-hosted runners cannot run Vivado (tens of GB, x86 only). The `firmware` workflow runs on a
self-hosted runner labelled `vivado-2022.2` (ADI v0.38 was built with 2022.2; 2025.2 works with
`ADI_IGNORE_VERSION_CHECK=1` and `unset MAKEFLAGS`, see `firmware/build.sh`).

Known host: the x86 "MacPro" Linux server with the Ubuntu Podman container that has Vivado mounted at
`/tools/Xilinx` (24 threads). `setup.sh` installs and registers the runner on such a host:

```sh
# on the build host, in the container or a shell that has vivado on PATH
TOKEN=$(gh api -X POST repos/invictus737/pluto-nano-timestamp/actions/runners/registration-token --jq .token)
runner/setup.sh "$TOKEN"
```
