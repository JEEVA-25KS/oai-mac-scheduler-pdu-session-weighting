/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 gnb_scheduler_dlsch_default_policies_updated
 
 */

/*!
 * \brief       Default pluggable policy functions for the DL scheduler pipeline.
 *
 * These are the built-in implementations behind the function pointers
 * (dl_ri_pmi_select, dl_tda_select, dl_beam_select, dl_mcs_select, dl_rb_alloc, dl_lcid_alloc)
 * wired up at MAC init time.  They can be replaced at runtime by external
 * scheduler plug-ins without touching the core scheduling loop in
 * gNB_scheduler_dlsch.c.
 */

#include "common/utils/nr/nr_common.h"
#include "gNB_scheduler_dlsch_default_policies.h"
/*MAC*/
#include "NR_MAC_COMMON/nr_mac.h"
#include "NR_MAC_gNB/nr_mac_gNB.h"
#include "LAYER2/NR_MAC_gNB/mac_proto.h"
#include "openair2/LAYER2/nr_rlc/nr_rlc_oai_api.h"

/*TAG*/
#include "NR_TAG-Id.h"

/*Softmodem params*/
#include "executables/softmodem-common.h"
#include "../../../nfapi/oai_integration/vendor_ext.h"

// Default RI/PMI selector: reads rank and PMI from CSI feedback for new-tx,
// or from HARQ process state for retx.
void nr_dl_ri_pmi_select_default(const gNB_MAC_INST *mac, nr_dl_candidate_t *candidates, int n_candidates)
{
  FOR_EACH_CANDIDATE(cand, candidates, n_candidates)
  {
    NR_UE_sched_ctrl_t *sched_ctrl = &cand->UE->UE_sched_ctrl;
    NR_UE_DL_BWP_t *dl_bwp = &cand->UE->current_DL_BWP;
    if (cand->is_retx) {
      cand->sched_pdsch.nrOfLayers = sched_ctrl->harq_processes[cand->retx_harq_pid].sched_pdsch.nrOfLayers;
      cand->sched_pdsch.pm_index =
          get_pm_index(mac, cand->UE, dl_bwp->dci_format, cand->sched_pdsch.nrOfLayers, mac->radio_config.pdsch_AntennaPorts.XP);
    } else {
      cand->sched_pdsch.nrOfLayers = cand->csi_ri + 1;
      cand->sched_pdsch.pm_index = cand->csi_pm_index;
    }
  }
}

// Default TDA selector: picks the slot-wide TDA index from get_dl_tda(),
// then resolves tda_info per candidate using each UE's own BWP / search
// space / coreset. Marks invalids with skipped=true.
int nr_dl_tda_select_default(const gNB_MAC_INST *mac, nr_dl_candidate_t *candidates, int n_candidates, frame_t frame, slot_t slot)
{
  int tda = get_dl_tda(mac, slot);
  AssertFatal(tda >= 0, "Unable to find PDSCH time domain allocation in list\n");
  const NR_ServingCellConfigCommon_t *scc = mac->common_channels[0].ServingCellConfigCommon;

  int n_valid = 0;
  FOR_EACH_CANDIDATE(cand, candidates, n_candidates)
  {
    if (cand->skipped)
      continue;
    NR_UE_info_t *UE = cand->UE;
    NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
    NR_UE_DL_BWP_t *dl_bwp = &UE->current_DL_BWP;
    int coresetid = sched_ctrl->coreset->controlResourceSetId;
    NR_tda_info_t tda_info = get_dl_tda_info(dl_bwp,
                                             sched_ctrl->search_space->searchSpaceType->present,
                                             tda,
                                             scc->dmrs_TypeA_Position,
                                             1,
                                             TYPE_C_RNTI_,
                                             coresetid,
                                             false);
    if (!tda_info.valid_tda) {
      cand->skipped = true;
      continue;
    }

    /* For retransmissions with a changed TDA, refit rbSize to preserve TBS */
    if (cand->is_retx) {
      const NR_sched_pdsch_t *orig = &sched_ctrl->harq_processes[cand->retx_harq_pid].sched_pdsch;
      bool tda_changed =
          tda_info.startSymbolIndex != orig->tda_info.startSymbolIndex || tda_info.nrOfSymbols != orig->tda_info.nrOfSymbols;
      if (tda_changed) {
        uint16_t new_rbSize = check_dl_retx_feasibility(cand, tda, &tda_info, scc, dl_bwp->BWPSize);
        if (!new_rbSize) {
          cand->skipped = true;
          continue;
        }
        cand->retx_rbSize = new_rbSize;
      }
    }

    cand->sched_pdsch.time_domain_allocation = tda;
    cand->sched_pdsch.tda_info = tda_info;
    cand->alloc_slbitmap = SL_to_bitmap(tda_info.startSymbolIndex, tda_info.nrOfSymbols);
    n_valid++;
  }
  return n_valid;
}

static int compare_dl_pf_ptrs(const void *a, const void *b)
{
  const nr_dl_candidate_t *ca = *(const nr_dl_candidate_t *const *)a;
  const nr_dl_candidate_t *cb = *(const nr_dl_candidate_t *const *)b;
  /* retx first (INFINITY weight), then highest PF weight */
  float wa = ca->is_retx ? INFINITY : dl_pf_weight(ca->current_mcs, ca->mcs_table, ca->sched_pdsch.nrOfLayers, ca->avg_throughput);
  float wb = cb->is_retx ? INFINITY : dl_pf_weight(cb->current_mcs, cb->mcs_table, cb->sched_pdsch.nrOfLayers, cb->avg_throughput);
  return (wa < wb) - (wa > wb);
}

int nr_dl_beam_select_default(NR_beam_info_t *beam_info,
                              const int16_t *beam_index_list,
                              nr_dl_candidate_t *candidates,
                              int n_candidates,
                              frame_t frame,
                              slot_t slot,
                              int slots_per_frame)
{
  /* Build pointer array sorted by PF priority so retx and high-priority UEs claim beams first. */
  nr_dl_candidate_t *order[MAX_MOBILES_PER_GNB];
  int n_active = 0;
  FOR_EACH_CANDIDATE(cand, candidates, n_candidates)
  if (!cand->skipped)
    order[n_active++] = cand;
  qsort(order, n_active, sizeof(*order), compare_dl_pf_ptrs);

  int n_valid = 0;
  for (int i = 0; i < n_active; i++) {
    nr_dl_candidate_t *cand = order[i];
    NR_beam_alloc_t beam = beam_allocation_procedure(beam_info, frame, slot, cand->alloc_beam_dir, slots_per_frame);
    if (beam.idx < 0) {
      cand->skipped = true;
      continue;
    }

    cand->alloc_beam_idx = beam.idx;
    cand->alloc_new_beam = beam.new_beam;
    n_valid++;
  }
  return n_valid;
}

void nr_dl_mcs_select_default(const gNB_MAC_INST *mac, nr_dl_candidate_t *candidates, int n_candidates)
{
  const NR_bler_options_t *bo = &mac->dl_bler;
  FOR_EACH_CANDIDATE(cand, candidates, n_candidates)
  {
    int mcs;
    if (cand->is_retx) {
      mcs = cand->current_mcs; /* retx MCS is fixed by the HARQ round */
    } else if (bo->harq_round_max == 1) {
      mcs = max(bo->min_mcs, min(bo->max_mcs, cand->max_mcs));
    } else if (!cand->bler_updated) {
      mcs = cand->current_mcs;
    } else {
      mcs = nr_adapt_mcs_from_bler(cand->current_mcs,
                                   bo->min_mcs,
                                   cand->max_mcs,
                                   cand->bler,
                                   bo->lower,
                                   bo->upper,
                                   cand->last_num_sched);
    }
    cand->sched_pdsch.mcs = mcs;
    /* Persist for all candidates — BLER-based MCS ramps even for UEs the
     * policy rejects this slot (failed CCE, no free RBs, etc.). */
    if (!cand->is_retx)
      cand->UE->UE_sched_ctrl.dl_bler_stats.mcs = mcs;
  }
}

static int compare_dl_pf_rb_ptrs(const void *a, const void *b)
{
  const nr_dl_candidate_t *ca = *(const nr_dl_candidate_t *const *)a;
  const nr_dl_candidate_t *cb = *(const nr_dl_candidate_t *const *)b;
  /* retx first, then highest PF weight (uses sched_pdsch.mcs, which is set by mcs_select) */
  float wa =
      ca->is_retx ? INFINITY : dl_pf_weight(ca->sched_pdsch.mcs, ca->mcs_table, ca->sched_pdsch.nrOfLayers, ca->avg_throughput);
  float wb =
      cb->is_retx ? INFINITY : dl_pf_weight(cb->sched_pdsch.mcs, cb->mcs_table, cb->sched_pdsch.nrOfLayers, cb->avg_throughput);
  return (wa < wb) - (wa > wb);
}

int nr_dl_proportional_fair(const nr_dl_sched_params_t *params, nr_dl_candidate_t *candidates, int n_candidates)
{
  const int min_rbSize = 5;
  int n_scheduled = 0;

  /* Build pointer array sorted by PF priority (retx first, then highest weight) */
  nr_dl_candidate_t *order[MAX_MOBILES_PER_GNB];
  int n_active = 0;
  FOR_EACH_CANDIDATE(cand, candidates, n_candidates)
  if (!cand->skipped)
    order[n_active++] = cand;
  qsort(order, n_active, sizeof(*order), compare_dl_pf_rb_ptrs);

  /* Phase 1: HARQ retransmissions (highest priority, exact RBs) */
  for (int j = 0; j < n_active; j++) {
    nr_dl_candidate_t *cand = order[j];
    if (!cand->is_retx)
      continue;

    int needed_rbs = cand->retx_rbSize;
    uint16_t *vrb_map = params->vrb_map[cand->alloc_beam_idx];
    int rbStart, rbSize;
    if (!get_rb_alloc(needed_rbs,
                      cand->bwp_size,
                      cand->bwp_start,
                      cand->bwp_size,
                      vrb_map,
                      cand->alloc_slbitmap,
                      &rbStart,
                      &rbSize))
      continue;

    COMMIT_ALLOC(params, cand, rbStart, needed_rbs, cand->sched_pdsch.mcs, n_scheduled);
  }

  /* Phase 2: No-data UEs (TA command or beam switch MAC CE, no RLC data) */
  for (int j = 0; j < n_active; j++) {
    nr_dl_candidate_t *cand = order[j];
    if (cand->is_retx || cand->pending_bytes > 0)
      continue;

    uint16_t *vrb_map = params->vrb_map[cand->alloc_beam_idx];
    int rbStart, rbSize;
    if (!get_rb_alloc(min_rbSize,
                      cand->bwp_size,
                      cand->bwp_start,
                      cand->bwp_size,
                      vrb_map,
                      cand->alloc_slbitmap,
                      &rbStart,
                      &rbSize))
      continue;

    COMMIT_ALLOC(params, cand, rbStart, min_rbSize, cand->sched_pdsch.mcs, n_scheduled);
  }

  /* Phase 3: New data UEs — PF priority order, largest free block */
  for (int j = 0; j < n_active; j++) {
    nr_dl_candidate_t *cand = order[j];
    if (cand->is_retx || cand->pending_bytes == 0)
      continue;

    int rbStart;
    uint16_t *vrb_map = params->vrb_map[cand->alloc_beam_idx];
    int max_rbSize = find_largest_free_block(vrb_map, cand->alloc_slbitmap, cand->bwp_start, cand->bwp_size, &rbStart);
    if (max_rbSize < min_rbSize)
      continue;

    int mcs = cand->sched_pdsch.mcs;
    uint8_t Qm = nr_get_Qm_dl(mcs, cand->mcs_table);
    uint16_t R = nr_get_code_rate_dl(mcs, cand->mcs_table);
    NR_pdsch_dmrs_t dmrs = get_dl_dmrs_params(params->mac->common_channels->ServingCellConfigCommon,
                                              &cand->UE->current_DL_BWP,
                                              &cand->sched_pdsch.tda_info,
                                              cand->sched_pdsch.nrOfLayers);
    const int oh = 3 * 4 + (cand->UE->UE_sched_ctrl.ta_apply ? 2 : 0);
    uint32_t tbs;
    uint16_t rbSize;
    nr_find_nb_rb(Qm,
                  R,
                  1,
                  cand->sched_pdsch.nrOfLayers,
                  cand->sched_pdsch.tda_info.nrOfSymbols,
                  dmrs.N_PRB_DMRS * dmrs.N_DMRS_SLOT,
                  cand->pending_bytes + oh,
                  min_rbSize,
                  max_rbSize,
                  &tbs,
                  &rbSize);

    COMMIT_ALLOC(params, cand, rbStart, rbSize, mcs, n_scheduled);
  }

  return n_scheduled;
}

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
