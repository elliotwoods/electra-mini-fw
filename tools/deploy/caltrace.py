"""Capture the device-local touch calibration state machine without stopping it.

    python tools/deploy/caltrace.py --pot 0

The application console shares WinUSB with EMP. The EMP decoder removes framed traffic and
leaves the ASCII CAL records emitted by the caltrace command. The complete 50 Hz record is
saved while the terminal prints phase changes and periodic live samples.
"""

import argparse
import os
import sys
import time
from datetime import datetime

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from emp import Decoder
from link import open_app


PHASE = {
    0: "IDLE",
    1: "BASELINE",
    2: "TOUCH",
    3: "HOLD",
    4: "RELEASE",
    5: "VALIDATE",
    6: "COMPLETE",
    7: "FAILED",
}


def parse_sample(line):
    parts = line.strip().split()
    if len(parts) != 14 or parts[0] != "CAL":
        return None
    values = [int(value, 16) for value in parts[1:]]
    keys = (
        "seq", "ms", "phase", "pot", "cycle", "raw", "filtered", "moved",
        "delta", "noise", "peak", "hold_ms", "saw_move",
    )
    return dict(zip(keys, values))


def describe(sample, wanted):
    return (
        f"{sample['ms']:8d} ms  {PHASE.get(sample['phase'], '?'):8s} "
        f"pass={sample['cycle'] + 1} raw={sample['raw']:02X} "
        f"filt={sample['filtered']:02X} moved={sample['moved']:02X} "
        f"delta={sample['delta']:4d} noise={sample['noise']:3d} "
        f"peak={sample['peak']:4d} hold={sample['hold_ms']:4d} "
        f"touch={'Y' if sample['raw'] & wanted else 'N'} "
        f"turn={'Y' if sample['saw_move'] else 'N'}"
    )


def explain(samples, target):
    if not samples:
        return "No calibration samples were received."
    wanted = 1 << target
    active = [s for s in samples if s["pot"] == target]
    if not active:
        return f"Calibration never selected B{target + 1} (pot index {target})."

    last = active[-1]
    wrong = next((s for s in active if s["raw"] & ~wanted), None)
    moved = any(s["moved"] & wanted for s in active)
    saw_move = any(s["saw_move"] for s in active)
    terminal = PHASE.get(last["phase"], str(last["phase"]))

    if wrong:
        return (
            f"{terminal}: another touch bit appeared (raw=0x{wrong['raw']:02X}) "
            f"at {wrong['ms']} ms."
        )
    if last["phase"] == 7 and last["hold_ms"] < 1000:
        return (
            f"FAILED: raw B{target + 1} released after {last['hold_ms']} ms; "
            "the one-second minimum was not reached."
        )
    if last["phase"] == 7 and not (moved or saw_move):
        return (
            f"FAILED: no B{target + 1} movement bit was observed during the hold "
            f"(last moved=0x{last['moved']:02X})."
        )
    if last["phase"] == 7:
        return (
            f"FAILED after hold={last['hold_ms']} ms, movement={int(saw_move)}, "
            f"delta={last['delta']}, noise={last['noise']}, peak={last['peak']}."
        )
    return (
        f"{terminal}: max delta={max(s['delta'] for s in active)}, "
        f"max peak={max(s['peak'] for s in active)}, movement={int(moved or saw_move)}."
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pot", type=int, default=0, choices=range(8),
                        help="panel pot index; B1 is 0")
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--out")
    args = parser.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    if args.out:
        out_path = args.out
    else:
        stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        out_path = os.path.join(root, "build", f"caltrace-b{args.pot + 1}-{stamp}.log")
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)

    dev = open_app()
    decoder = Decoder()
    pending = bytearray()
    samples = []
    last_seq = None
    last_phase = None
    last_raw = None
    started = False
    wanted = 1 << args.pot

    dev.drain()
    dev.c.write(b"caltrace on\r")
    print(f"capture ready: perform the B{args.pot + 1} calibration now", flush=True)
    print(f"full trace: {out_path}", flush=True)

    deadline = time.time() + args.timeout
    terminal_seen = None
    with open(out_path, "w", encoding="utf-8", newline="\n") as log:
        log.write("# seq ms phase pot cycle raw filt moved delta noise peak hold saw\n")
        try:
            while time.time() < deadline:
                chunk = dev.c.read(8192)
                if not chunk:
                    time.sleep(0.005)
                    continue
                decoder.feed(chunk)
                pending.extend(decoder.take_text())
                while b"\n" in pending:
                    raw_line, _, pending = pending.partition(b"\n")
                    line = raw_line.decode("ascii", "replace").strip()
                    sample = parse_sample(line)
                    if not sample:
                        continue
                    log.write(line + "\n")
                    log.flush()
                    samples.append(sample)

                    if last_seq is not None and sample["seq"] != last_seq + 1:
                        print(
                            f"TRACE GAP: expected {last_seq + 1:08X}, "
                            f"received {sample['seq']:08X}",
                            flush=True,
                        )
                    last_seq = sample["seq"]

                    phase = sample["phase"]
                    if phase in (1, 2, 3, 4, 5):
                        started = True
                    important = (
                        phase != last_phase
                        or sample["raw"] != last_raw
                        or bool(sample["moved"] & wanted)
                        or sample["seq"] % 5 == 0
                    )
                    if important:
                        print(describe(sample, wanted), flush=True)
                    last_phase = phase
                    last_raw = sample["raw"]

                    if started and phase in (6, 7):
                        terminal_seen = time.time()
                if terminal_seen and time.time() - terminal_seen >= 0.25:
                    break
        except KeyboardInterrupt:
            print("capture interrupted", flush=True)
        finally:
            dev.c.write(b"caltrace off\r")
            time.sleep(0.1)
            dev.close()

    print("diagnosis: " + explain(samples, args.pot), flush=True)
    if not started:
        raise SystemExit("timed out before calibration started")


if __name__ == "__main__":
    main()
