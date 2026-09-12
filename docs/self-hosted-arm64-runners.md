# Self-hosted arm64 runners

The three PDF backend repos are private and publish amd64 only: GitHub's
free `ubuntu-24.04-arm` runners cover public repos only, and an emulated
QEMU arm64 C++ build does not finish in any usable time. (grpc-asr,
grpc-libreoffice, and grpc-vlm-convert went public and have built their
arm64 images natively on the hosted arm runner since 2026-09-12; only the
private trio still needs hardware.) Each of the three carries an
`arm64-publish` branch that adds a native arm64 build to
`.github/workflows/publish.yml`, gated on a self-hosted runner:

| Repo | What the arm64 build compiles | Timeout |
|---|---|---|
| `grpc-poppler` | poppler 26.08.0 + gRPC from source (the heaviest) | 720 min |
| `grpc-pdfium` | PDFium-based tree | 360 min |
| `grpc-qparse` | qpdf-based tree | 360 min |

Every arm64 build job uses `runs-on: [self-hosted, linux, arm64]`. Those
three labels are automatic for a self-hosted runner on 64-bit ARM Linux;
no custom labels are needed and none should be added.

## Hardware and OS prerequisites

- **64-bit OS.** `uname -m` must report `aarch64`. A 32-bit Raspberry Pi
  OS does not satisfy the `arm64` label and cannot run the buildx arm64
  jobs.
- **Docker with buildx**, and the runner user in the `docker` group. The
  workflows drive `docker buildx` directly.
- **Storage on real disks, not the SD card.** A poppler build writes
  gigabytes of object files and layer data. On the Pi point Docker's
  `data-root` at the NVMe/USB disk (`/etc/docker/daemon.json`, then
  restart docker); the SD card will wear out or throttle first otherwise.
  The Jetson Nano's eMMC or NVMe deserves the same treatment.
- **Memory.** 4 GB (the Nano) is enough for the builds as configured, but
  only just: grpc-poppler's workflow already drops to 4 parallel g++ jobs
  for the arm64 build. Add zram (or a modest swap file on the disk, not
  the SD card) so a link step under memory pressure fails slowly instead
  of invoking the OOM killer mid-compile.
- **One runner per device.** Register a single runner with `--ephemeral`
  off but concurrency 1 (the default); two builds on one of these machines
  will thrash. Two devices means a two-runner pool, not two runners per
  device.

## Registering a runner

Decide org-level or repo-level. Org-level (`ai-pipestream`) is the right
choice: one registration serves all three repos, and the runners are
dedicated CI machines anyway. Repo-level only makes sense to scope a
machine to one repo.

The registration token comes from the GitHub org page: **Settings >
Actions > Runners > New self-hosted runner**, which prints a token good
for about an hour. Generating it needs org admin, so it is done by a human
in the browser or with an admin token via
`gh api /orgs/ai-pipestream/actions/runners/registration-token -X POST`.
Then, on the machine, as the CI user:

```bash
mkdir -p ~/actions-runner && cd ~/actions-runner
curl -o runner.tar.gz -L https://github.com/actions/runner/releases/download/v<VER>/actions-runner-linux-arm64-<VER>.tar.gz
tar xzf runner.tar.gz
./config.sh --url https://github.com/ai-pipestream --token <TOKEN> \
  --name nano1 --labels ""   # self-hosted/linux/arm64 are automatic; no extras
sudo ./svc.sh install && sudo ./svc.sh start
```

`--url` is the org URL for an org-level runner, or
`https://github.com/ai-pipestream/<repo>` for a repo-level one. Leave
`--labels` empty: the arch labels arrive on their own, and extra labels
only create a second labeling scheme to maintain. `svc.sh` installs a
systemd unit so the runner survives reboots; check `sudo ./svc.sh status`
and the org's Runners page, where the machine shows Idle once it has
polled in.

**Org runners and public repos:** the org now mixes private and public
repos. By default an org runner group is not available to public repos,
and it should stay that way: keep the runner group scoped to the three
private repos (or at least exclude public ones) so a fork PR can never
reach this hardware.

## Security posture

Self-hosted runners on private repos are fine for trusted workloads: the
code that runs is from the org's own branches, and PRs from forks do not
run on private-repo runners, so fork attacks are not a concern here. The
real exposure is the docker socket: every workflow job runs with access to
`/var/run/docker.sock`, which is root on the host. Treat both machines as
CI appliances: no personal data, no other services, no inbound exposure,
prompt security updates.

## Queueing behavior and merge order

A queued arm64 job has no fallback. If no runner with the
`self-hosted, linux, arm64` labels is online when a publish fires, the job
sits in the queue and GitHub fails it after 24 hours; on the publish
workflow that means the whole publish stalls or half-publishes. So the
order is fixed: **register and verify at least one runner before merging
any `arm64-publish` branch.** Verify with a trivial test (a
`workflow_dispatch` or a push to a scratch branch) that a job actually
picks up on the machine before merging the real workflows.

## Which machine carries which load

The Raspberry Pi is the primary runner; the Jetson Nano is the secondary.
Both can take every build, but schedule grpc-poppler onto the Pi:

- **grpc-poppler (720 min, poppler + gRPC from source)**: the long pole.
  Run it on the Pi, with the zram/swap advice above. The Nano takes it
  only when the Pi is busy.
- **grpc-pdfium and grpc-qparse (360 min each)**: moderate. Either
  machine; good filler for the Nano, and a good first end-to-end proof
  that the pool works.

Rebuild acceleration matters more than raw CPU on this hardware: the
workflows share buildx cache mounts across multi-arch builds, and a warm
cache turns a 6-hour poppler build into a fraction of that on a rebuild.
Do not clean the buildx cache between runs; if a Dockerfile grows a
compile-from-source stage, wire ccache or sccache into it and persist its
directory on the disk-backed data-root.

## Follow-up cleanup once arm64 manifests exist

Each native arm64 image published to Docker Hub removes one reason for
the QEMU pins:

- When a service has a native arm64 manifest, drop its
  `platform: linux/amd64` entry from `compose.stack.arm64.yaml` so arm64
  hosts pull it natively, and update that file's header and
  `compose/README.md` to match. Once all three are native, retire the
  overlay's pin section entirely (the CPU-swap block alone duplicates
  compose.stack.cpu.yaml).
- At the same time, update the "Publishing images" section of the
  workspace `AGENTS.md`, which records which repos publish amd64-only.
- `gRParse/scripts/stack-e2e.sh` has `NO_GPU`/`INTEL` toggles but no
  arm64 toggle; extending it to layer `compose.stack.arm64.yaml` is
  future work, tracked here so it is not forgotten when the pins start
  coming out.
