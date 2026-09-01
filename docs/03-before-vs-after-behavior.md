# 3. Before vs. After — Worked Behavioral Walkthroughs

## 3.1 Before the patch — greedy, ascending-LCID order

```
RLC buffers (pre-filled, independent of scheduling)

 LCID 4        LCID 5        LCID 6        LCID 7
 (session1)    (session2)    (session3)    (session4)
 50,000 B      48,000 B      20,000 B      19,000 B
 queued        queued        queued        queued
    |             |             |             |
 pulled first  pulled 2nd    pulled 3rd    pulled last
 (if TB room)  (if TB room)  (if TB room)  (if TB room)
    v             v             v             v
┌─────────────────────────────────────────────────────┐
│           ONE shared Transport Block (TB)             │
│      total size fixed BEFORE this loop even starts    │
│              e.g. tbs_available = 40,000 B             │
└─────────────────────────────────────────────────────┘
```

Loop logic in `gNB_scheduler_dlsch.c`, for lcid = 4, 5, 6, 7 in order:
- `while (TB has room AND this LCID still has queued bytes)`:
  - pull a chunk from this LCID's RLC buffer
  - write it into the TB (`buf` pointer advances)
  - shrink this LCID's remaining backlog
- exits when EITHER:
  - (a) this LCID's buffer is now empty → move to next LCID
  - (b) TB is completely full → STOP, done

**Walkthrough with the numbers above (TB = 40,000 B):**
```
LCID 4 (50,000 queued) ─── pulls until TB full ──▶ takes 40,000 B
                                                     TB space left: 0
LCID 5 (48,000 queued) ─── TB already full ──────▶ takes 0 B
LCID 6 (20,000 queued) ─── TB already full ──────▶ takes 0 B
LCID 7 (19,000 queued) ─── TB already full ──────▶ takes 0 B
```

**Result**: LCID 4 alone consumes the entire transport block. LCID 5, 6, and 7 get
nothing this slot — regardless of how much traffic their sessions actually had
queued. "Priority" was really just an accident of ascending LCID number, not
fairness or intended weighting.

### Where does LCID 4's leftover 10,000 B go?

It stays exactly where it was — in LCID 4's own RLC buffer. `nr_mac_rlc_data_req()`
only removes from RLC's queue what actually got copied into the TB; nothing is
discarded or redirected. The whole process then repeats on the next slot, starting
the LCID loop over from LCID 4 again.

```
Slot N (TB = 40,000 B)
-------------------------
LCID 4 buffer: 50,000 B queued
 -> pulls 40,000 B into TB, TB now full
 -> LCID 4 buffer AFTER this slot: 10,000 B remaining
LCID 5/6/7: get 0 B (TB was already full)

Slot N+1 (new TB, freshly sized — say 40,000 B again)
-------------------------------------------------------
MAC re-polls all buffers fresh (update_dlsch_buffer runs every slot):
LCID 4 buffer: 10,000 B queued  <- only what's left
LCID 5 buffer: 48,000 B queued  <- untouched since it got nothing last slot
LCID 6 buffer: 20,000 B queued
LCID 7 buffer: 19,000 B queued

Packing loop runs again, LCID 4 first (still lowest LCID number):
 -> pulls remaining 10,000 B from LCID 4 -> buffer now EMPTY
 -> TB still has 30,000 B of room left -> moves on to LCID 5

LCID 5 buffer: 48,000 B queued
 -> pulls until TB full: takes 30,000 B
 -> TB now full, 0 B left
 LCID 5 buffer AFTER this slot: 18,000 B remaining

LCID 6, 7: still get 0 B — TB filled again before reaching them
```

**Key mechanism**: nothing about scheduling is "remembered" across slots except the
RLC buffer's own state. Every slot is a completely fresh scheduling decision — new
TB size, new pass through the LCID loop, starting again from LCID 4. The only thing
carrying over slot to slot is how much backlog is still sitting in each RLC buffer,
because RLC simply wasn't asked to give up bytes it wasn't asked for.

This is exactly why, without the weighting patch, LCID 4 (and eventually 5, once 4
drains) would dominate slot after slot, and LCID 6/7 could go many consecutive slots
getting nothing — not because of any deliberate "wait your turn" logic, but purely
because the loop always restarts from LCID 4 every single slot, and as long as LCID
4 (then 5) has any backlog left, it keeps winning the race to fill the TB first.

---

## 3.2 After the patch — weighted split

Same scenario, TB = 40,000 B, weights 35 / 35 / 15 / 15.

### Pass 1 — proportional share, capped at actual demand

"Demand" = that LCID's current RLC buffer occupancy, at the moment this slot's
scheduling decision is being made.

```
total_weight = 35+35+15+15 = 100

LCID 4: share = 40,000 x 35/100 = 14,000   demand=50,000 -> capped? no -> alloc = 14,000
LCID 5: share = 40,000 x 35/100 = 14,000   demand=48,000 -> alloc = 14,000
LCID 6: share = 40,000 x 15/100 =  6,000   demand=20,000 -> alloc =  6,000
LCID 7: share = 40,000 x 15/100 =  6,000   demand=19,000 -> alloc =  6,000
------------------------------
total allocated = 40,000
remaining_tbs = 0
```
Every session had plenty of demand here, so nothing gets capped and Pass 2 has
nothing left to redistribute — a clean, exact 14,000 / 14,000 / 6,000 / 6,000 split
(35% / 35% / 15% / 15% of the TB, exactly as intended).

### Packing loop — same mechanics, pre-rationed numbers
```
LCID 4: lcid_alloc[4]=14,000 -> pulls 14,000 B (had 50,000) -> buffer now 36,000 remaining
LCID 5: lcid_alloc[5]=14,000 -> pulls 14,000 B (had 48,000) -> buffer now 34,000 remaining
LCID 6: lcid_alloc[6]= 6,000 -> pulls  6,000 B (had 20,000) -> buffer now 14,000 remaining
LCID 7: lcid_alloc[7]= 6,000 -> pulls  6,000 B (had 19,000) -> buffer now 13,000 remaining
```
All four LCIDs get served in this single slot — nobody is shut out, unlike the
unweighted version where LCID 6 and 7 got zero.

### Next slot
Backlogs are smaller now but all four still have data, so the same weighted split
repeats — each buffer drains at a rate proportional to its weight.
```
Slot N+1 (fresh TB, 40,000 B again):
LCID 4: 36,000 queued -> alloc 14,000 -> buffer -> 22,000
LCID 5: 34,000 queued -> alloc 14,000 -> buffer -> 20,000
LCID 6: 14,000 queued -> alloc  6,000 -> buffer ->  8,000
LCID 7: 13,000 queued -> alloc  6,000 -> buffer ->  7,000
```

### Pass 2 in action — low demand on one LCID

A few slots later, LCID 6's buffer has drained to only 2,000 B, while the others are
still heavily backlogged:
```
Pass 1:
LCID 4: share=14,000, demand=huge  -> alloc=14,000
LCID 5: share=14,000, demand=huge  -> alloc=14,000
LCID 6: share= 6,000, demand=2,000 -> alloc= 2,000  (capped — can't take more than it has)
LCID 7: share= 6,000, demand=huge  -> alloc= 6,000
total so far = 36,000
remaining_tbs = 4,000   <- leftover from LCID 6's cap

Pass 2: give the leftover 4,000 to the highest-weight LC that still wants more
 -> LCID 4 and LCID 5 are tied at weight 35, both still have demand
 -> best = whichever the loop finds first with the highest weight (LCID 4)
 -> LCID 4 gets +4,000 -> final alloc = 18,000
```
The TB still ends up fully used (40,000 B, none wasted) even though LCID 6 didn't
need its full share that slot — the surplus flows to the highest-priority session
still hungry for more, instead of sitting idle the way it would in the old version.

---

## 3.3 Real gNB log capture

```
[NR_MAC] Frame.Slot 0.0
UE RNTI 4d08 CU-UE-ID 1 in-sync PH 44 dB PCMAX 20 dBm, average RSRP -93 (16 meas)
UE 4d08: CQI 15, RI 1, PMI (0,0)
UE 4d08: UL-RI 1, TPMI 0
UE 4d08: dlsch_rounds 2389/6/2/0, dlsch_errors 0, pucch0_DTX 8 (SNR 21.8+1.8 dB), BLER 0.00159 MCS (0) 15 NPRB 106 CCE fail 6, goodput 22.33 Mbps
UE 4d08: ulsch_rounds 5355/26/3/2, ulsch_errors 2, ulsch_DTX 21, BLER 0.00928 MCS (0) 15 (Qm 4 deltaMCS 0 dB) NPRB 5 SNR 16.6 (+1.6) dB CCE fail 0, goodput 0.03 Mbps
UE 4d08: LCID 1: TX           1851 RX            542 bytes, PRB-equiv(slot)   0/106, PRB-equiv(cumulative) 518
UE 4d08: LCID 2: TX              0 RX              0 bytes, PRB-equiv(slot)   0/106, PRB-equiv(cumulative) 0
UE 4d08: LCID 4: TX        1186071 RX           2010 bytes, PRB-equiv(slot)  37/106, PRB-equiv(cumulative) 60899   ------> PDU 1 ( ≈ 35 % )
UE 4d08: LCID 5: TX        1149706 RX           1960 bytes, PRB-equiv(slot)  36/106, PRB-equiv(cumulative) 58296   ------> PDU 2 ( ≈ 35 % )
UE 4d08: LCID 6: TX         510219 RX           1712 bytes, PRB-equiv(slot)  15/106, PRB-equiv(cumulative) 27435   ------> PDU 3 ( ≈ 15 % )
UE 4d08: LCID 7: TX         466464 RX           2219 bytes, PRB-equiv(slot)  15/106, PRB-equiv(cumulative) 23430   ------> PDU 4 ( ≈ 15 % )
```

---

## 3.4 Core contrast

| | Without weighting | With weighting |
|---|---|---|
| **Who decides `lcid_alloc[]`** | Nobody — it's just raw demand, uncapped | Weight-proportional, capped, leftover redistributed |
| **Packing loop behavior** | First LCID(s) can consume the entire TB | Every active LCID gets served, in proportion, every slot |
| **LCID 6/7 outcome under load** | Starved indefinitely while 4/5 have backlog | Guaranteed ~15% share every slot they have data |
| **TB utilization** | Wasted only if LCID 4 alone can't fill it | Always fully used — leftovers reassigned by priority |
