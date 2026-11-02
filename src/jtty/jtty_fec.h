/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JTTY soft-decision decoder for the tail-biting
 *  convolutional code
 */

#pragma once

#include "jtty_proto.h"

/* Called for each CRC-valid candidate, best path metric first; return
 * true to take it (e.g. the source grammar is valid). */
typedef bool (*jtty_accept_fn)(jtty_payload_t payload, void *ctx);

/*
 * metric[t][k] is the log-likelihood-like score of tone k for coded
 * symbol t (bigger = more likely). Up to `list_size` tail-biting paths
 * are tried, each with its own start/end state, in order of path metric.
 *
 * Returns true and the payload of the first candidate whose CRC matches
 * and that `accept` takes (NULL: any). `rank` (may be NULL) receives its
 * position in the list, 0 being the best path: a decode found deep in
 * the list is less certain.
 */
bool jtty_fec_decode(const float metric[JTTY_NDATA][JTTY_TONES], int list_size,
                     jtty_accept_fn accept, void *ctx,
                     jtty_payload_t *payload, int *rank);
