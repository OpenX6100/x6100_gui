/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JTTY source grammar: validation and rendering
 *  of one 34-bit atom
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "jtty_proto.h"

typedef enum {
    JTTY_ATOM_CALL,         /* i2 = 0, 1: call action */
    JTTY_ATOM_STRUCT,       /* i2 = 2: STRUCT30 */
    JTTY_ATOM_TEXT5,        /* i2 = 3: five characters */
} jtty_atom_kind_t;

typedef struct {
    jtty_atom_kind_t kind;
    bool             eom;           /* last atom of the message */
    char             text[32];      /* canonical rendering, no trailing space */
    char             call[12];      /* JTTY_ATOM_CALL: the call itself */
} jtty_atom_t;

/*
 * Checks a payload against the complete version-1 grammar (reserved bit,
 * all-zero sentinel, unassigned types, ranges, zero fields, call round
 * trip) and renders it. Returns false for anything invalid: such an atom
 * must not be shown or used at all.
 */
bool jtty_source_decode(jtty_payload_t payload, jtty_atom_t *atom);

/*
 * Appends an atom to a message being received, as the protocol renders
 * it: a call or STRUCT30 atom plus one implicit space, TEXT5 characters
 * verbatim. At EOM trailing spaces are dropped. The message is kept to
 * 80 characters. `msg` must hold JTTY_MSG_MAX + 1 bytes.
 */
#define JTTY_MSG_MAX 80
void jtty_source_append(char *msg, const jtty_atom_t *atom);

/* 77-bit standard callsign codec (28 bits), standard calls only. */
bool jtty_call28_unpack(uint32_t n28, char *call, size_t size);
bool jtty_call28_pack(const char *call, uint32_t *n28);

/* A standard call as WSJT-X's standard_call() and chkcall() take it: no
 * '/', not starting with Q, a letter in one of the first two places, a
 * digit in the second or third, then one to three letters, and the same
 * back through call28. "599" or "001" are not calls. */
bool jtty_call_standard(const char *call);
