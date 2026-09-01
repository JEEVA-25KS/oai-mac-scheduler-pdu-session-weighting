# 4. Results and Integer-Truncation Notes

## 4.1 Two independent sources of integer truncation

There are actually two separate places where integer division causes truncation —
one affects the byte split itself, the other affects only the PRB-equivalent
**logging** metric. Worth being precise about which is which, since they have
different consequences.

### Location 1 — inside the weighting logic itself
`gNB_scheduler_dlsch_default_policies.c`:
```c
int share = (int)((int64_t)tbs_available * weights[i] / total_weight);
```
This is a plain integer division. `tbs_available × weight` is very unlikely to
divide evenly by `total_weight`, so any fractional remainder is simply **dropped**,
not rounded. For example, if `tbs_available = 40,001` and `weight/total_weight =
35/100`:
```
40,001 × 35 / 100 = 1,400,035 / 100 = 14,000.35 → truncates to 14,000
```
That 0.35 of a byte is lost from that LCID's Pass-1 share. This is the source of
`remaining_tbs` sometimes being slightly positive even when every LCID has plenty
of demand — Pass 2's leftover-redistribution loop is what quietly absorbs this
truncation loss (usually just a byte or two per active LCID), which is exactly why
Pass 2 exists as a safety net, not only for handling under-demand sessions.

### Location 2 — inside the PRB-equivalent accounting
`gNB_scheduler_dlsch.c`:
```c
uint32_t prb_equiv = (uint32_t)((int64_t)sched_pdsch->rbSize * lcid_alloc[lcid] / tbs_for_lcid_alloc);
```
Same pattern, different quantity: `rbSize × lcid_alloc[lcid]` divided by
`tbs_for_lcid_alloc`, truncated to a whole PRB. This is computed **independently**
for each of the four LCIDs, and each one loses its own fractional remainder
separately.

This is the specific one responsible for the **103 vs 106 gap** observed in the log
captures (37+36+15+15 = 103 against NPRB 106). Each LCID's individual truncation
might only lose a fraction of a PRB, but summed across 4 independent LCIDs, those
small losses compound — up to just under 1 full PRB lost per LCID, so up to ~3–4
PRBs total can go "missing" from the sum purely from this rounding, even though the
actual byte-level allocation (Location 1) already accounted for every byte
correctly.

### Why this distinction matters

- **Location 1's truncation is real but self-correcting** — Pass 2 catches it, so
  no bytes are ever actually lost from the transport block itself. The TB is
  always fully used.
- **Location 2's truncation is purely cosmetic** — it only affects the displayed
  PRB-equiv numbers in the log, not any actual scheduling decision. The real
  `rbSize`/`NPRB` value is unaffected; only the four per-LCID estimates of how that
  `rbSize` should be apportioned lose a little precision when summed.

So the correct interpretation is: **`sum(PRB-equiv per LCID) ≤ NPRB`, not `=`**, and
this inequality is expected and harmless — it comes from Location 2's independent
per-LCID rounding in the logging code, not from any byte actually being dropped by
the scheduler.

---

## 4.2 Result

The modified `nr_dl_lcid_alloc_default()` policy, together with the supporting
accounting changes in `gNB_scheduler_dlsch.c`, `nr_mac_gNB.h`, and `main.c`, was
validated on a live OAI 5G SA testbed under four simultaneous `iperf3` downlink
sessions.

Runtime logs confirmed that once all four sessions reached sustained, simultaneous
demand, the per-slot PRB-equivalent split stabilized at approximately **35%, 34%,
15%, and 15%** across LCIDs 4 through 7 respectively — closely matching the intended
35/35/15/15 target within the bounds of integer-rounding error inherent to the
per-slot accounting formula (see §4.1).

The mechanism was further observed to correctly reassign unused capacity from an
idle or lightly-loaded session to the highest-priority session still carrying
backlog, ensuring the shared transport block's byte budget was never left partially
unallocated. A cross-check between the newly logged aggregate DL `NPRB` value and
the sum of per-LCID PRB-equivalent values confirmed internal consistency of the
added accounting logic.

### Sample runtime gNB-log (two consecutive captures)

```
[NR_MAC] Frame.Slot 640.0
UE RNTI 7dc0 CU-UE-ID 1 in-sync PH 29 dB PCMAX 20 dBm, average RSRP -105 (16 meas)
UE 7dc0: CQI 15, RI 1, PMI (0,0)
UE 7dc0: UL-RI 1, TPMI 0
UE 7dc0: dlsch_rounds 3077/187/5/0, dlsch_errors 0, pucch0_DTX 16 (SNR 21.7+1.7 dB), BLER 0.11711 MCS (0) 13 NPRB 106 CCE fail 15, goodput 25.54 Mbps
UE 7dc0: ulsch_rounds 5186/149/46/22, ulsch_errors 19, ulsch_DTX 81, BLER 0.01660 MCS (0) 11 (Qm 4 deltaMCS 0 dB) NPRB 5 SNR 17.6 (+2.6) dB CCE fail 1, goodput 0.03 Mbps
UE 7dc0: LCID 1: TX           5537 RX            571 bytes, PRB-equiv(slot)   0/106, PRB-equiv(cumulative) 1493
UE 7dc0: LCID 2: TX              0 RX              0 bytes, PRB-equiv(slot)   0/106, PRB-equiv(cumulative) 0
UE 7dc0: LCID 4: TX        1920711 RX           2810 bytes, PRB-equiv(slot)  37/106, PRB-equiv(cumulative) 83075
UE 7dc0: LCID 5: TX        1868816 RX           2493 bytes, PRB-equiv(slot)  36/106, PRB-equiv(cumulative) 80235
UE 7dc0: LCID 6: TX         802362 RX           2952 bytes, PRB-equiv(slot)  15/106, PRB-equiv(cumulative) 34177
UE 7dc0: LCID 7: TX         796030 RX           2031 bytes, PRB-equiv(slot)  15/106, PRB-equiv(cumulative) 38140

[NR_MAC] Frame.Slot 512.0
UE RNTI 7dc0 CU-UE-ID 1 in-sync PH 30 dB PCMAX 20 dBm, average RSRP -105 (16 meas)
UE 7dc0: CQI 15, RI 1, PMI (0,0)
UE 7dc0: UL-RI 1, TPMI 0
UE 7dc0: dlsch_rounds 1689/14/2/0, dlsch_errors 0, pucch0_DTX 16 (SNR 22.0+2.0 dB), BLER 0.00652 MCS (0) 9 NPRB 106 CCE fail 15, goodput 9.83 Mbps
UE 7dc0: ulsch_rounds 4959/149/46/22, ulsch_errors 19, ulsch_DTX 81, BLER 0.06532 MCS (0) 0 (Qm 2 deltaMCS 0 dB) NPRB 23 SNR 17.2 (+2.2) dB CCE fail 1, goodput 0.07 Mbps
UE 7dc0: LCID 1: TX           5537 RX            571 bytes, PRB-equiv(slot)   0/106, PRB-equiv(cumulative) 1493
UE 7dc0: LCID 2: TX              0 RX              0 bytes, PRB-equiv(slot)   0/106, PRB-equiv(cumulative) 0
UE 7dc0: LCID 4: TX         405735 RX           2746 bytes, PRB-equiv(slot)  37/106, PRB-equiv(cumulative) 32995
UE 7dc0: LCID 5: TX         394986 RX           2429 bytes, PRB-equiv(slot)  36/106, PRB-equiv(cumulative) 31543
UE 7dc0: LCID 6: TX         171217 RX           2893 bytes, PRB-equiv(slot)  15/106, PRB-equiv(cumulative) 13879
UE 7dc0: LCID 7: TX         185835 RX           1970 bytes, PRB-equiv(slot)  15/106, PRB-equiv(cumulative) 17842
```


<img width="1250" height="261" alt="image" src="https://github.com/user-attachments/assets/821617e2-f2af-4c4a-b794-74f335479a56" />

## 4.3 Conclusion

These results confirm that while the NR air interface does not support literal,
independently-addressed PRB partitions across multiple PDU sessions within a single
slot, the implemented weighted byte-share mechanism achieves an **equivalent and
verifiable differentiation** in delivered throughput and resource consumption per
session, and does so without requiring any change to the scheduler's underlying
single-transport-block-per-UE-per-slot architecture.


## 4.4 Appendix 
The corresponding scripts(updated version) are organized under the repo's scripts folder for reference.
