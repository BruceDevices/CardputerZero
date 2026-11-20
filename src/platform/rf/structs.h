/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Compat replacement for Bruce's src/modules/rf/structs.h. The upstream file
 * pulls in core/display.h (the whole UI stack) and the RMT driver; here we keep
 * only the data structures the copied protocols/ tree references, plus the RMT
 * symbol type (via the driver shim) that rf_decoder.h needs in its signatures.
 */
#ifndef RF_STRUCTS_H
#define RF_STRUCTS_H

#include "protocols/rf_codes.h"
#include <driver/rmt_rx.h>
#include <driver/rmt_tx.h>
#include <vector>

// OOK protocol timing descriptor (rc-switch style); referenced by some decoders.
struct Protocol {
    uint16_t pulseLength;
    HighLow syncFactor;
    HighLow zero;
    HighLow one;
    bool invertedSignal;
};

// A manufacturer KeeLoq key, loaded from the keystore (/mfcodes or embedded).
struct KeeloqKey {
    String mf_name{};
    uint64_t key = 0;
    uint32_t type = 0;
};

#endif
