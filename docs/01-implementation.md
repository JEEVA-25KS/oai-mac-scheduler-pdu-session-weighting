# 1. Implementation — Modified Files and Diffs

## Objective

Implement and validate a mechanism for differential downlink resource allocation
across multiple PDU sessions belonging to a single UE in an OAI 5G Standalone gNB.
Specifically: assign four simultaneously active PDU sessions a fixed relative
resource share of **35%, 35%, 15%, and 15%** respectively, and modify the DL MAC
scheduler so this weighting is enforced consistently at the logical-channel level —
since the NR air interface does not permit independently-addressed PRB allocations
per session within a single transport block. The work also extends the scheduler's
logging output so the effect of this weighting can be observed and verified directly
from gNB runtime logs, without external packet capture or offline analysis.

## Background

Downlink packet scheduling in NR happens once per UE per slot: a single transport
block, sized according to the UE's aggregate buffer occupancy and current channel
conditions, is built and transmitted via one PDSCH per scheduling interval. When a UE
carries multiple PDU sessions — each mapped to its own DRB and logical channel
(LCID) — this shared transport block must be divided among all active sessions
before transmission.

In the **unmodified** OAI scheduler, this division was handled by
`nr_dl_lcid_alloc_default()`, a policy function that allocated bytes to logical
channels strictly in **ascending LCID order** with no regard for session priority or
fairness — meaning a lower-numbered LCID could consume an entire transport block
while higher-numbered LCIDs received nothing, regardless of their actual traffic
demand.

## Contributing files

All four files live in `openair2/LAYER2/NR_MAC_gNB/`:

| File | Full path |
|------|-----------|
| `nr_mac_gNB.h` | `openairinterface5g/openair2/LAYER2/NR_MAC_gNB/nr_mac_gNB.h` |
| `gNB_scheduler_dlsch.c` | `openairinterface5g/openair2/LAYER2/NR_MAC_gNB/gNB_scheduler_dlsch.c` |
| `gNB_scheduler_dlsch_default_policies.c` | `openairinterface5g/openair2/LAYER2/NR_MAC_gNB/gNB_scheduler_dlsch_default_policies.c` |
| `main.c` | `openairinterface5g/openair2/LAYER2/NR_MAC_gNB/main.c` |

---

## 1.1 `gNB_scheduler_dlsch_default_policies.c` — the core change

**Before** (line 281):
```c
void nr_dl_lcid_alloc_default(const gNB_MAC_INST *mac,
                               const nr_dl_candidate_t *candidate,
                               int tbs_available,
                               int lcid_alloc[NR_MAX_NUM_LCID])
{
  (void)mac;
  (void)tbs_available;
  memset(lcid_alloc, 0, NR_MAX_NUM_LCID * sizeof(int));
  for (int lcid = 0; lcid < NR_MAX_NUM_LCID; lcid++)
    lcid_alloc[lcid] = candidate->pending_bytes_per_lcid[lcid];
}
```
The critical line is `(void)tbs_available;` — the TB's actual byte budget is
explicitly discarded. This is the root cause: the function has no idea how big the
TB is, so it can't ration anything against it. It just copies each LC's entire
queued backlog straight into the output — no comparison against demand, no cap, no
weighting.

**After** (lines 281–363):
```c
// Priority-weighted proportional split of the UE's allocated TBS across its
// active logical channels (i.e. across its active PDU sessions' DRBs).
// A single PDSCH allocation still gives the UE one contiguous PRB block —
// NR Type 1 frequency-domain RA has no concept of per-LC PRBs within one TB —
// but this function determines how much of that TB's *byte* budget each
// PDU session gets, weighted by its LCP priority (TS 38.321, 1=highest,
// 16=lowest), which is the effective per-session share of the allocation.
void nr_dl_lcid_alloc_default(const gNB_MAC_INST *mac,
                               const nr_dl_candidate_t *candidate,
                               int tbs_available,
                               int lcid_alloc[NR_MAX_NUM_LCID])
{
  (void)mac;
  memset(lcid_alloc, 0, NR_MAX_NUM_LCID * sizeof(int));

  const seq_arr_t *lc_config = &candidate->UE->UE_sched_ctrl.lc_config;

  /* Fixed per-LCID share of the TB's bytes. Indexed directly by LCID, so
     update this table to match your DRB/LCID assignment. Values are relative
     weights (35/35/15/15 sums to 100, but any consistent ratio works, e.g.
     7/7/3/3): */
  static const int lcid_weight_table[NR_MAX_NUM_LCID] = {
      [4] = 35, /* PDU session 1 -> LCID 4 */
      [5] = 35, /* PDU session 2 -> LCID 5 */
      [6] = 15, /* PDU session 3 -> LCID 6 */
      [7] = 15, /* PDU session 4 -> LCID 7 */
  };

  int lcids[NR_MAX_NUM_LCID];
  int weights[NR_MAX_NUM_LCID];
  int pending[NR_MAX_NUM_LCID];
  int n_active = 0;
  int total_weight = 0;

  for (int i = 0; i < seq_arr_size(lc_config); i++) {
    const nr_lc_config_t *c = seq_arr_at(lc_config, i);
    int lcid = c->lcid;
    int bytes = candidate->pending_bytes_per_lcid[lcid];
    if (bytes <= 0)
      continue;

    int weight = lcid_weight_table[lcid];
    if (weight < 1)
      weight = 1; /* any LC not in the table still gets a minimal share */

    lcids[n_active] = lcid;
    weights[n_active] = weight;
    pending[n_active] = bytes;
    total_weight += weight;
    n_active++;
  }

  if (n_active == 0)
    return;

  int remaining_tbs = tbs_available;

  /* Pass 1: give each active session its weighted share, capped by its own demand */
  for (int i = 0; i < n_active; i++) {
    int share = (int)((int64_t)tbs_available * weights[i] / total_weight);
    lcid_alloc[lcids[i]] = min(share, pending[i]);
    remaining_tbs -= lcid_alloc[lcids[i]];
  }

  /* Pass 2: hand out leftover bytes (rounding, or a session capped by its own
   * demand) to the highest-priority session that still wants more */
  bool progress = true;
  while (remaining_tbs > 0 && progress) {
    progress = false;
    int best = -1;
    for (int i = 0; i < n_active; i++) {
      if (pending[i] - lcid_alloc[lcids[i]] <= 0)
        continue;
      if (best < 0 || weights[i] > weights[best])
        best = i;
    }
    if (best < 0)
      break;
    int give = min(pending[best] - lcid_alloc[lcids[best]], remaining_tbs);
    lcid_alloc[lcids[best]] += give;
    remaining_tbs -= give;
    progress = give > 0;
  }
}
```

---

## 1.2 `nr_mac_gNB.h` — storage for the new observability data

**Before** (line 751):
```c
int NPRB;
} NR_mac_stats_t;
```

**After** (lines 751–758):
```c
int NPRB;
  /* Estimated PRB share per LCID within the shared DL transport block.
   * Not a real scheduling quantity (PHY allocates one PRB range per UE,
   * not per LC) — this is a derived accounting metric: rbSize * (LC's
   * share of that slot's TB bytes), for validating per-session weighting
   * in nr_dl_lcid_alloc_default(). */
  uint32_t dl_lc_prb_equiv[64];            // cumulative since attach/reset
  uint32_t dl_lc_prb_equiv_last_slot[64];  // most recent slot only
} NR_mac_stats_t;
```
Purely plumbing — doesn't affect scheduling behavior. It exists because the change
in 1.1 is invisible unless you can see it working.

---

## 1.3 `gNB_scheduler_dlsch.c` — computing the observability number, every slot

**Before** (line 1085):
```c
if (sched_ctrl->num_total_bytes > 0) {
  /* ask the LCID allocation policy how many bytes each LC gets */
  int lcid_alloc[NR_MAX_NUM_LCID] = {0};
  mac->dl_lcid_alloc(mac, candidate, bufEnd - buf, lcid_alloc);
```

**After** (lines 1085–1100):
```c
if (sched_ctrl->num_total_bytes > 0) {
  /* ask the LCID allocation policy how many bytes each LC gets */
  int lcid_alloc[NR_MAX_NUM_LCID] = {0};
  int tbs_for_lcid_alloc = bufEnd - buf;
  mac->dl_lcid_alloc(mac, candidate, tbs_for_lcid_alloc, lcid_alloc);

  /* Derived accounting only: apportion this slot's rbSize across LCIDs
   * by their share of the TB's bytes. Not a real PHY/MAC quantity —
   * see NR_mac_stats_t.dl_lc_prb_equiv comment. */
  if (tbs_for_lcid_alloc > 0) {
    memset(UE->mac_stats.dl_lc_prb_equiv_last_slot, 0,
           sizeof(UE->mac_stats.dl_lc_prb_equiv_last_slot));
    for (int lcid = 0; lcid < NR_MAX_NUM_LCID; lcid++) {
      if (lcid_alloc[lcid] <= 0)
        continue;
      uint32_t prb_equiv = (uint32_t)((int64_t)sched_pdsch->rbSize * lcid_alloc[lcid] / tbs_for_lcid_alloc);
      UE->mac_stats.dl_lc_prb_equiv_last_slot[lcid] = prb_equiv;
      UE->mac_stats.dl_lc_prb_equiv[lcid] += prb_equiv;
    }
  }
```
This is the only point in the code where three necessary pieces of information —
the real PRB count, the byte split, and the byte budget that split was computed
against — are simultaneously available. It translates the byte-level decision made
in 1.1 into a PRB-equivalent number directly comparable to a literal "N PRBs to
session X" framing, even though no such literal PRB partition exists at the PHY
level.

---

## 1.4 `main.c` — printing all of it to the log

**Aggregate DL line — before**:
```c
output = st_append(output, end,
    ", dlsch_errors %" PRIu64
    ", pucch0_DTX %d (SNR %.1f%+.1f dB), BLER %.5f MCS (%d) %d CCE fail %d, goodput %.2f Mbps\n",
    stats->dl.errors, stats->pucch0_DTX, pucch_snr, pucch_snr_diff,
    sched_ctrl->dl_bler_stats.bler, UE->current_DL_BWP.mcsTableIdx,
    sched_ctrl->dl_bler_stats.mcs, sched_ctrl->dl_cce_fail, UE->dl_thr_ue_display / 1e6);
```

**Aggregate DL line — after**:
```c
output = st_append(output, end,
    ", dlsch_errors %" PRIu64
    ", pucch0_DTX %d (SNR %.1f%+.1f dB), BLER %.5f MCS (%d) %d NPRB %d CCE fail %d, goodput %.2f Mbps\n",
    stats->dl.errors, stats->pucch0_DTX, pucch_snr, pucch_snr_diff,
    sched_ctrl->dl_bler_stats.bler, UE->current_DL_BWP.mcsTableIdx,
    sched_ctrl->dl_bler_stats.mcs, stats->dl.current_rbs, sched_ctrl->dl_cce_fail,
    UE->dl_thr_ue_display / 1e6);
```
`NPRB %d` inserted between the MCS field and CCE-fail field, with
`stats->dl.current_rbs` as the new argument — mirroring the pre-existing UL line's
NPRB field, now printed for DL for the first time.

**Per-LCID line — before**:
```c
"UE %04x: LCID %d: TX %14"PRIu64" RX %14"PRIu64" bytes\n",
UE->rnti, c->lcid, stats->dl.lc_bytes[c->lcid], stats->ul.lc_bytes[c->lcid]);
```

**Per-LCID line — after**:
```c
"UE %04x: LCID %d: TX %14"PRIu64" RX %14"PRIu64" bytes, PRB-equiv(slot) %3u/106, PRB-equiv(cumulative) %u\n",
UE->rnti, c->lcid, stats->dl.lc_bytes[c->lcid], stats->ul.lc_bytes[c->lcid],
stats->dl_lc_prb_equiv_last_slot[c->lcid], stats->dl_lc_prb_equiv[c->lcid]);
```
`%3u` right-pads for column alignment; `/106` is a **hardcoded literal** for the
known BWP size (106 PRB) — not a format specifier, and an assumption worth noting if
reusing this on a different bandwidth config.

---

## Summary — one sentence per file

1. `gNB_scheduler_dlsch_default_policies.c` is **the fix itself**.
2. `nr_mac_gNB.h` is **where to store proof it's working**.
3. `gNB_scheduler_dlsch.c` is **how that proof gets computed**.
4. `main.c` is **how you actually see it**.
