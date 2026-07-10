#!/usr/bin/env python3
"""Run compare2upstream on a fresh GCE x86-64 VM, print the rendered tables.

Creates the VM (SMT off via --threads-per-core=1), isolates the bench core
from the guest scheduler (isolcpus/nohz_full/rcu_nocbs/irqaffinity via GRUB +
one reboot), uploads the local working tree (gitignore respected, uncommitted
changes included), runs compare2upstream/run.sh there pinned to the isolated
core, prints the rendered tables to stdout (progress goes to stderr), and
deletes the VM — also on failure; --keep to skip deletion for debugging.

Requires an authenticated gcloud CLI with Compute Engine enabled on the
project. The VM needs outbound network (apt, pip, github clone of upstream).

  compare2upstream/gcloud_run.py --project <gcp-project> \
      [--zone europe-west4-a] [--machine-type c2-standard-4] \
      [--min-time 1s] [--repetitions 5] [--messages 500000] \
      [--spot] [--keep] > GCLOUD_X86_64.md

C2 (Cascade Lake) has stable all-core clocks, AVX2, and dedicated physical
cores, and the guest-side isolation keeps kthreads/IRQs/tick off the bench
core — but it is still a VM (no governor/turbo control from the guest), so
treat the numbers as one notch below a bare-metal isolcpus box.
"""

import argparse
import datetime
import pathlib
import subprocess
import sys
import tempfile
import time

REPO = pathlib.Path(__file__).resolve().parent.parent

ISOLATE_SCRIPT = """
set -euxo pipefail
sudo tee /etc/default/grub.d/99-bench-isol.cfg >/dev/null <<'EOF'
GRUB_CMDLINE_LINUX_DEFAULT="$GRUB_CMDLINE_LINUX_DEFAULT isolcpus={cpu} nohz_full={cpu} rcu_nocbs={cpu} irqaffinity=0"
EOF
sudo update-grub
"""

REMOTE_SCRIPT = """
set -euxo pipefail
export DEBIAN_FRONTEND=noninteractive
sudo apt-get update -qq
sudo apt-get install -y -qq build-essential cmake ninja-build git python3-venv > /dev/null

python3 -m venv ~/venv
~/venv/bin/pip install --quiet 'conan==2.30.0'
export PATH=~/venv/bin:$PATH

mkdir -p ~/nanofix && cd ~/nanofix
tar xzf ~/nanofix.tar.gz
# run.sh resolves the repo root via git; the tarball ships without .git.
git init -q && git add -A -f && git -c user.email=bench@local -c user.name=bench commit -qm bench

conan profile detect --force > /dev/null

{{ nproc; grep -m1 'model name' /proc/cpuinfo; \
   echo "isolated: $(cat /sys/devices/system/cpu/isolated)"; }} >&2
NANOFIX_BENCH_CPU={cpu} \
NANOFIX_BENCH_MIN_TIME={min_time} \
NANOFIX_BENCH_REPETITIONS={reps} \
NANOFIX_BENCH_MESSAGES={messages} \
    compare2upstream/run.sh
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
    ap.add_argument("--machine-type", default="c2-standard-4")
    ap.add_argument("--min-time", default="1s")
    ap.add_argument("--repetitions", default="5")
    ap.add_argument("--messages", default="500000")
    ap.add_argument("--spot", action="store_true",
                    help="SPOT provisioning (cheaper, may be preempted mid-run)")
    ap.add_argument("--keep", action="store_true",
                    help="do not delete the VM afterwards")
    ap.add_argument("--ssh-timeout", type=int, default=300)
    ap.add_argument("--bench-timeout", type=int, default=7200)
    args = ap.parse_args()

    base = ["--project", args.project, "--zone", args.zone]
    name = "nanofix-bench-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S")

    with tempfile.TemporaryDirectory() as tmp:
        tarball = pathlib.Path(tmp) / "nanofix.tar.gz"
        files = subprocess.run(
            ["git", "-C", REPO, "ls-files", "-co", "--exclude-standard", "-z"],
            check=True, capture_output=True).stdout
        run(["tar", "-C", REPO, "--null", "-T", "-", "-czf", tarball],
            input=files)

        create = ["instances", "create", name,
                  "--machine-type", args.machine_type,
                  "--threads-per-core", "1",
                  "--image-family", "ubuntu-2404-lts-amd64",
                  "--image-project", "ubuntu-os-cloud",
                  "--boot-disk-size", "50GB"]
        if args.spot:
            create += ["--provisioning-model", "SPOT",
                       "--instance-termination-action", "DELETE"]
        gcloud(create, base, stdout=sys.stderr)

        try:
            wait_ssh(name, base, args.ssh_timeout)

            gcloud(["ssh", name, "--command", "bash -s"], base,
                   input=ISOLATE_SCRIPT.format(cpu=1).encode(), timeout=300,
                   stdout=sys.stderr)
            # reboot kills the SSH connection; a non-zero exit is expected
            subprocess.run(["gcloud", "compute", "ssh", name,
                            "--command", "sudo reboot"] + base,
                           check=False, timeout=60,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(15)
            wait_ssh(name, base, args.ssh_timeout)

            gcloud(["scp", str(tarball), f"{name}:~/nanofix.tar.gz"], base,
                   stdout=sys.stderr)

            script = REMOTE_SCRIPT.format(
                cpu=1, min_time=args.min_time, reps=args.repetitions,
                messages=args.messages)

            bench = gcloud(["ssh", name, "--command", "bash -s"], base,
                           input=script.encode(), timeout=args.bench_timeout,
                           stdout=subprocess.PIPE)
            sys.stdout.write(bench.stdout.decode())
            sys.stdout.flush()
        finally:
            if args.keep:
                log(f"keeping VM {name} ({args.zone}); delete it yourself:")
                log(f"  gcloud compute instances delete {name} "
                    f"--project {args.project} --zone {args.zone} --quiet")
            else:
                subprocess.run(
                    ["gcloud", "compute", "instances", "delete", name,
                     "--quiet"] + base, check=False)


if __name__ == "__main__":
    sys.exit(main())
