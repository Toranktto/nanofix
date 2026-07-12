#!/usr/bin/env python3
"""Run a long libFuzzer campaign on a fresh GCE VM, print the summary.

Creates the VM, uploads the local working tree (gitignore respected,
uncommitted changes included), builds fuzz/ with clang (ASan+UBSan+fuzzer,
AVX2 on x86-64), runs each harness (--harness, default fuzz_reader and
fuzz_writer, sequentially) with N parallel jobs for --time seconds each,
prints per-job coverage and final stats to stdout (build/progress goes to
stderr), downloads any crash artifacts, and deletes the VM — also on
failure; --keep to skip deletion for debugging.

Exits non-zero when the campaign produced artifacts (crash/oom/timeout);
they land in ./fuzz-artifacts/<harness>/ for local replay:
build/fuzz/<harness> ./fuzz-artifacts/<harness>/<file> (see CLAUDE.md
## Fuzzing).

  scripts/gcloud_fuzz.py --project <gcp-project> \
      [--zone europe-west4-a] [--machine-type e2-highcpu-8] \
      [--time 3600] [--jobs <vCPUs>] [--harness fuzz_reader,fuzz_writer] \
      [--spot] [--keep]
"""

import argparse
import datetime
import pathlib
import subprocess
import sys
import tempfile
import time

REPO = pathlib.Path(__file__).resolve().parent.parent

REMOTE_SCRIPT = """
set -euxo pipefail
export DEBIAN_FRONTEND=noninteractive
sudo apt-get update -qq
sudo apt-get install -y -qq clang cmake ninja-build git > /dev/null

rm -rf ~/nanofix && mkdir ~/nanofix && cd ~/nanofix
tar xzf ~/nanofix.tar.gz
git init -q && git add -A -f && git -c user.email=fuzz@local -c user.name=fuzz commit -qm fuzz

cmake -S fuzz -B build/fuzz -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo > /dev/null
cmake --build build/fuzz --target {harnesses} fuzz_dataset >&2

cd build/fuzz
jobs={jobs}
[ "$jobs" -gt 0 ] || jobs=$(nproc)
for harness in {harnesses}; do
    mkdir -p "artifacts/$harness"
    rm -f fuzz-*.log
    set +e
    "./$harness" -print_final_stats=1 -timeout=10 \
        -dict=../../fuzz/fix.dict \
        -max_total_time={time} -jobs="$jobs" -workers="$jobs" \
        -artifact_prefix="artifacts/$harness/" dataset >&2
    rc=$?
    set -e

    echo "== $harness: max_total_time={time}s jobs=$jobs exit=$rc =="
    for log in fuzz-*.log; do
        [ -f "$log" ] || continue
        echo "-- $log"
        grep -Eo 'cov: [0-9]+ ft: [0-9]+' "$log" | tail -1 || true
        grep '^stat::' "$log" || true
    done
done
n_artifacts=$(find artifacts -type f | wc -l)
echo "artifacts: $n_artifacts"
find artifacts -type f || true
tar czf ~/artifacts.tar.gz artifacts
"""


def log(*a):
    print(*a, file=sys.stderr, flush=True)


def run(cmd, **kw):
    log("+", " ".join(map(str, cmd)))
    return subprocess.run(cmd, check=True, **kw)


def gcloud(args, base, **kw):
    return run(["gcloud", "compute"] + args + base, **kw)


def wait_ssh(name, base, timeout):
    deadline = time.monotonic() + timeout
    while True:
        try:
            gcloud(["ssh", name, "--command", "true"], base,
                   timeout=60,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
            if time.monotonic() > deadline:
                raise RuntimeError("VM never became SSH-reachable")
            time.sleep(10)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--project", required=True)
    ap.add_argument("--zone", default="europe-west4-a")
    ap.add_argument("--machine-type", default="e2-highcpu-8")
    ap.add_argument("--time", type=int, default=3600,
                    help="max_total_time per fuzz job, seconds")
    ap.add_argument("--jobs", type=int, default=0,
                    help="parallel fuzz jobs (0 = one per vCPU)")
    ap.add_argument("--harness", default="fuzz_reader,fuzz_writer",
                    help="comma-separated harnesses to run sequentially, "
                         "--time each")
    ap.add_argument("--spot", action="store_true",
                    help="SPOT provisioning (cheaper, may be preempted mid-run)")
    ap.add_argument("--keep", action="store_true",
                    help="do not delete the VM afterwards")
    ap.add_argument("--ssh-timeout", type=int, default=300)
    args = ap.parse_args()

    harnesses = [h.strip() for h in args.harness.split(",") if h.strip()]
    known = {"fuzz_reader", "fuzz_writer"}
    unknown = set(harnesses) - known
    if not harnesses or unknown:
        ap.error(f"--harness must name harnesses from {sorted(known)}, "
                 f"got: {args.harness!r}")

    base = ["--project", args.project, "--zone", args.zone]
    name = "nanofix-fuzz-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S")

    with tempfile.TemporaryDirectory() as tmp:
        tarball = pathlib.Path(tmp) / "nanofix.tar.gz"
        listed = subprocess.run(
            ["git", "-C", REPO, "ls-files", "-co", "--exclude-standard", "-z"],
            check=True, capture_output=True).stdout
        files = [f for f in listed.split(b"\0") if f and (REPO / f.decode()).exists()]
        run(["tar", "-C", REPO, "--null", "-T", "-", "-czf", tarball],
            input=b"\0".join(files) + b"\0")

        create = ["instances", "create", name,
                  "--machine-type", args.machine_type,
                  "--image-family", "ubuntu-2404-lts-amd64",
                  "--image-project", "ubuntu-os-cloud",
                  "--boot-disk-size", "50GB",
                  "--boot-disk-type", "pd-balanced"]
        if args.spot:
            create += ["--provisioning-model", "SPOT",
                       "--instance-termination-action", "DELETE"]
        gcloud(create, base, stdout=sys.stderr)

        crashes = 0
        try:
            wait_ssh(name, base, args.ssh_timeout)
            gcloud(["scp", str(tarball), f"{name}:~/nanofix.tar.gz"], base,
                   stdout=sys.stderr)

            script = REMOTE_SCRIPT.format(time=args.time, jobs=args.jobs,
                                          harnesses=" ".join(harnesses))
            # generous margin over the fuzz budget for apt + build + summary
            fuzz = gcloud(["ssh", name, "--command", "bash -s"], base,
                          input=script.encode(),
                          timeout=args.time * len(harnesses) + 1800,
                          stdout=subprocess.PIPE)
            out = fuzz.stdout.decode()
            sys.stdout.write(out)
            sys.stdout.flush()

            for line in out.splitlines():
                if line.startswith("artifacts: "):
                    crashes = int(line.split()[1])
            if crashes:
                gcloud(["scp", f"{name}:~/artifacts.tar.gz", tmp], base,
                       stdout=sys.stderr)
                run(["tar", "xzf", f"{tmp}/artifacts.tar.gz"])
                run(["mv", "artifacts", "fuzz-artifacts"])
                log(f"{crashes} artifact(s) -> ./fuzz-artifacts/ (replay: "
                    "build/fuzz/<harness> ./fuzz-artifacts/<harness>/<file>)")
        finally:
            if args.keep:
                log(f"keeping VM {name} ({args.zone}); delete it yourself:")
                log(f"  gcloud compute instances delete {name} "
                    f"--project {args.project} --zone {args.zone} --quiet")
            else:
                subprocess.run(
                    ["gcloud", "compute", "instances", "delete", name,
                     "--quiet"] + base, check=False)

    return 1 if crashes else 0


if __name__ == "__main__":
    sys.exit(main())
