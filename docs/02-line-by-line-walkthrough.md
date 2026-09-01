# 2. Line-by-Line Walkthrough

Detailed explanation of every changed line, for `gNB_scheduler_dlsch_default_policies.c`.

## Before
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
- `(void)mac;` — the `mac` parameter is unused; this cast to void just silences the compiler's unused-parameter warning.
- `(void)tbs_available;` — **the critical line**. The TB's actual byte budget is explicitly discarded. This is the root cause of the whole problem: the function has no idea how big the TB is, so it can't ration anything against it.
- `memset(lcid_alloc, 0, ...)` — zeroes the output array before filling it, standard init.
- The `for` loop — for every possible LCID (0 to `NR_MAX_NUM_LCID`), copy that LCID's entire queued backlog (`pending_bytes_per_lcid[lcid]`) straight into the output. No comparison against demand, no cap, no weighting — just a direct copy.

## After — declarations and setup
```c
void nr_dl_lcid_alloc_default(const gNB_MAC_INST *mac,
                               const nr_dl_candidate_t *candidate,
                               int tbs_available,
                               int lcid_alloc[NR_MAX_NUM_LCID])
{
  (void)mac;
  memset(lcid_alloc, 0, NR_MAX_NUM_LCID * sizeof(int));
```
- `(void)mac;` kept — still unused.
- `(void)tbs_available;` **removed** — the whole point of the patch is to actually use this value now.
- `memset` unchanged.

```c
  const seq_arr_t *lc_config = &candidate->UE->UE_sched_ctrl.lc_config;
```
- Grabs a pointer to this UE's list of configured logical channels (`lc_config`), so the function can iterate only over LCIDs that actually exist for this UE, rather than blindly looping 0 to `NR_MAX_NUM_LCID` like the original did.

```c
  static const int lcid_weight_table[NR_MAX_NUM_LCID] = {
      [4] = 35, /* PDU session 1 -> LCID 4 */
      [5] = 35, /* PDU session 2 -> LCID 5 */
      [6] = 15, /* PDU session 3 -> LCID 6 */
      [7] = 15, /* PDU session 4 -> LCID 7 */
  };
```
- `static const int [...]` — a compile-time-initialized lookup table, indexed directly by LCID number. `static` means it's allocated once, not re-created every function call. Using designated initializers (`[4] = 35`) means every LCID not explicitly listed defaults to 0 automatically — no need to zero-fill the rest by hand.

```c
  int lcids[NR_MAX_NUM_LCID];
  int weights[NR_MAX_NUM_LCID];
  int pending[NR_MAX_NUM_LCID];
  int n_active = 0;
  int total_weight = 0;
```
- Three parallel arrays to track, for each **active** LCID (one with actual backlog): its LCID number, its weight, and its pending byte count. `n_active` counts how many LCIDs are actually in play this slot; `total_weight` will be the sum of weights across only those active LCIDs — important, since it's not a fixed 100, given some sessions might be idle.

## After — building the active-LCID list
```c
  for (int i = 0; i < seq_arr_size(lc_config); i++) {
    const nr_lc_config_t *c = seq_arr_at(lc_config, i);
    int lcid = c->lcid;
    int bytes = candidate->pending_bytes_per_lcid[lcid];
    if (bytes <= 0)
      continue;
```
- Iterate this UE's actual configured logical channels (not all 64 possible LCIDs). For each one, look up its queued backlog. `if (bytes <= 0) continue;` — skip any LCID with nothing queued; it shouldn't be counted as "active" or receive any weight this slot.

```c
    int weight = lcid_weight_table[lcid];
    if (weight < 1)
      weight = 1; /* any LC not in the table still gets a minimal share */
```
- Look up this LCID's weight from the table. If it's an LCID not explicitly listed (e.g. some other bearer type not in your 4-session setup), it would default to 0 from the table — the `if (weight < 1) weight = 1;` guard prevents a 0 weight from breaking the later division and ensures any unlisted-but-active LC still gets something rather than being silently starved.

```c
    lcids[n_active] = lcid;
    weights[n_active] = weight;
    pending[n_active] = bytes;
    total_weight += weight;
    n_active++;
  }
```
- Record this LCID into the "active" arrays at index `n_active`, accumulate its weight into the running total, then increment the active count.

```c
  if (n_active == 0)
    return;
```
- If nothing has any backlog at all, there's nothing to allocate — exit early. `lcid_alloc[]` stays all-zero from the earlier `memset`.

## After — Pass 1 (proportional allocation)
```c
  int remaining_tbs = tbs_available;
  for (int i = 0; i < n_active; i++) {
    int share = (int)((int64_t)tbs_available * weights[i] / total_weight);
    lcid_alloc[lcids[i]] = min(share, pending[i]);
    remaining_tbs -= lcid_alloc[lcids[i]];
  }
```
- **Pass 1.** For each active LCID: `share` = its proportional cut of the total TB (`tbs_available × its_weight / total_weight`). The cast to `int64_t` before multiplying prevents integer overflow if `tbs_available` and `weight` are both largish. `min(share, pending[i])` caps the allocation at what that LCID actually has queued — a session can never be given more than it needs. `remaining_tbs` is decremented by whatever was actually allocated, tracking how much of the TB is still unspent after this pass.

## After — Pass 2 (leftover redistribution)
```c
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
- **Pass 2.** This loop keeps running as long as there's leftover TB space (`remaining_tbs > 0`) and the previous iteration actually gave something out (`progress`) — the `progress` flag prevents an infinite loop if nothing more can be allocated.
- Inside: scan all active LCIDs to find `best` — the one with unmet demand (`pending[i] - lcid_alloc[lcids[i]] > 0`) that has the highest weight among those still wanting more. This is the priority tie-break: leftover always goes to the highest-weight hungry session first.
- If no LCID has any unmet demand left (`best < 0`), break out — nothing more to give.
- Otherwise, give that LCID as much of the leftover as it can use (`min` of its remaining unmet demand and what's left of `remaining_tbs`), update its allocation, decrement `remaining_tbs`, and set `progress = give > 0` so the loop knows whether to continue.

---

## `nr_mac_gNB.h` — field-level notes

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
- The comment block explicitly documents that this is a **derived** quantity, not something PHY/MAC actually tracks — important so future readers of this struct don't mistake it for a real scheduling primitive.
- `uint32_t dl_lc_prb_equiv[64];` — a fixed-size array, one slot per possible LCID (0–63), holding the running cumulative total. `uint32_t` (unsigned 32-bit) is large enough to accumulate over a long-running connection without overflow risk in practice.
- `uint32_t dl_lc_prb_equiv_last_slot[64];` — same size/type, but holds only the most recent slot's value, overwritten (via `memset` + assignment) every time the accounting code runs.

---

## `gNB_scheduler_dlsch.c` — field-level notes

```c
if (sched_ctrl->num_total_bytes > 0) {
  int lcid_alloc[NR_MAX_NUM_LCID] = {0};
  int tbs_for_lcid_alloc = bufEnd - buf;
  mac->dl_lcid_alloc(mac, candidate, tbs_for_lcid_alloc, lcid_alloc);
```
- `if (sched_ctrl->num_total_bytes > 0)` — only bother with LCID allocation at all if there's any aggregate backlog for this UE.
- `int lcid_alloc[NR_MAX_NUM_LCID] = {0};` — declares and zero-initializes the output array that the policy function will fill.
- `int tbs_for_lcid_alloc = bufEnd - buf;` — new line: pulls `bufEnd - buf` out into its own named variable before the call, instead of computing it inline as an anonymous expression. This is necessary because the accounting code below needs to reuse this exact same value — if it were only computed inline inside the function call, there'd be no way to reference it afterward.
- The `dl_lcid_alloc` call — functionally identical to before, just passing the now-named variable instead of the inline expression.

```c
  if (tbs_for_lcid_alloc > 0) {
    memset(UE->mac_stats.dl_lc_prb_equiv_last_slot, 0,
           sizeof(UE->mac_stats.dl_lc_prb_equiv_last_slot));
```
- `if (tbs_for_lcid_alloc > 0)` — guards against a divide-by-zero in the formula below; if there was no TB space at all, skip this entirely.
- `memset(...)` — zeroes out the entire per-slot array first. This is what makes `PRB-equiv(slot)` a true snapshot: any LCID that gets nothing this slot will correctly show 0, not a stale value from a previous slot.

```c
    for (int lcid = 0; lcid < NR_MAX_NUM_LCID; lcid++) {
      if (lcid_alloc[lcid] <= 0)
        continue;
```
- Loop over every possible LCID; skip any that got zero bytes this slot (nothing to account for, and avoids doing pointless work / storing meaningless zeros redundantly since the `memset` above already zeroed them).

```c
      uint32_t prb_equiv = (uint32_t)((int64_t)sched_pdsch->rbSize * lcid_alloc[lcid] / tbs_for_lcid_alloc);
```
- The core formula. `sched_pdsch->rbSize` is this slot's real PRB count for this UE. Multiplying by `lcid_alloc[lcid]` and dividing by `tbs_for_lcid_alloc` computes this LCID's proportional share of that PRB count, based on its byte share of the TB. The `(int64_t)` cast before the multiplication prevents integer overflow (`rbSize × bytes` could exceed a 32-bit int's range before the division brings it back down), and the final `(uint32_t)` cast truncates the result down to a whole PRB count — this is the source of the "≤" relationship discussed in [doc 4](04-results-and-rounding-notes.md): fractional PRBs are simply dropped, not rounded.

```c
      UE->mac_stats.dl_lc_prb_equiv_last_slot[lcid] = prb_equiv;
      UE->mac_stats.dl_lc_prb_equiv[lcid] += prb_equiv;
    }
  }
```
- The per-slot value is **assigned** (`=`) — it fully replaces whatever was there before, since it was already zeroed for all LCIDs and this sets the ones that had activity.
- The cumulative value is **accumulated** (`+=`) — added on top of the running total from all previous slots, never reset.

---

## `main.c` — field-level notes

**Aggregate DL line**: `NPRB %d` inserted into the format string, between the MCS field and CCE-fail field — matching the position where the pre-existing UL line already has its own NPRB. `stats->dl.current_rbs,` is the matching new argument, in the correct positional order to line up with the new `%d`. This field already existed elsewhere in the codebase (set to `sched_pdsch->rbSize` after scheduling); this line is simply the first time it's printed for DL.

**Per-LCID line**: `, PRB-equiv(slot) %3u/106, PRB-equiv(cumulative) %u` appended to the format string. `%3u` right-pads the number to at least 3 characters wide for column alignment in the log; `/106` is a **literal hardcoded string** (the known BWP size), not a format specifier — this assumes a fixed 106-PRB grid and would need updating for a different bandwidth config. Two new arguments appended in matching order: `stats->dl_lc_prb_equiv_last_slot[c->lcid]` (per-slot) and `stats->dl_lc_prb_equiv[c->lcid]` (cumulative) — pulling directly from the fields defined in `nr_mac_gNB.h`, populated by the code in `gNB_scheduler_dlsch.c`.
