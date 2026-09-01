# OAI MAC Scheduler — Per-PDU-Session Downlink Resource Weighting

Implementation and validation of a **weighted, priority-aware downlink scheduling
policy** in the OpenAirInterface (OAI) 5G SA gNB MAC layer, enforcing a fixed
**35% / 35% / 15% / 15%** relative resource share across four simultaneous PDU
sessions belonging to a single UE — plus the supporting instrumentation added to
observe and verify that split directly from gNB runtime logs.

## The problem

NR downlink scheduling builds **one shared transport block (TB) per UE per slot** —
there's no native way to give independent PDU sessions their own PRB allocation
within that block. OAI's stock `nr_dl_lcid_alloc_default()` policy made this worse by
packing logical channels (LCIDs) into the TB in strict ascending order with **no
regard for demand or priority** — meaning the lowest-numbered LCID could consume an
entire TB while every other session got nothing, slot after slot, purely due to
loop ordering.

## The fix

A two-pass weighted proportional allocator that replaces the naive copy-through:
- **Pass 1**: each active LCID gets `tbs_available × weight / total_weight` bytes,
  capped at its own actual demand (so nothing is over-allocated)
- **Pass 2**: any leftover TB space (from rounding, or an under-demanding session)
  is handed to the highest-weight LCID that still wants more — so the TB is never
  left partially wasted

## Contents

| Doc | Covers |
|-----|--------|
| [01-implementation.md](docs/01-implementation.md) | The 4 modified files, full before/after diffs, and file-by-file rationale |
| [02-line-by-line-walkthrough.md](docs/02-line-by-line-walkthrough.md) | Detailed line-by-line explanation of every changed line |
| [03-before-vs-after-behavior.md](docs/03-before-vs-after-behavior.md) | Worked numeric walkthroughs: greedy ascending-LCID starvation vs. the weighted split, including Pass 2 in action |
| [04-results-and-rounding-notes.md](docs/04-results-and-rounding-notes.md) | Live testbed validation, real gNB log captures, and an explanation of the two independent integer-truncation sources |

## Contributing files (all in `openair2/LAYER2/NR_MAC_gNB/`)

| File | Role |
|------|------|
| `gNB_scheduler_dlsch_default_policies.c` | **The core fix** — the weighted allocation decision itself |
| `nr_mac_gNB.h` | New struct fields to store per-LCID PRB-equivalent stats |
| `gNB_scheduler_dlsch.c` | Computes the PRB-equivalent accounting number every slot |
| `main.c` | Prints the new stats to the gNB runtime log |

## Result

Validated on a live OAI 5G SA testbed under four simultaneous `iperf3` downlink
sessions. Once all four sessions reached sustained demand, the per-slot
PRB-equivalent split stabilized at **~35% / 34% / 15% / 15%** across LCIDs 4–7 —
matching the intended target within expected integer-rounding error. The mechanism
correctly reassigned unused capacity from an idle/lightly-loaded session to the
highest-priority session still carrying backlog, so the TB's byte budget was never
left partially unallocated.

This confirms that although NR has no literal per-session PRB partition within a
single slot, a weighted byte-share mechanism achieves an equivalent, verifiable
differentiation in delivered throughput — without any change to the underlying
single-TB-per-UE-per-slot architecture.
