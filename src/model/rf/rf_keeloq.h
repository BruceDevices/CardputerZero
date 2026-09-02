/*
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Ported from Bruce (AGPL-3.0-or-later), itself DERIVED FROM the Flipper Zero
 * firmware KeeLoq cipher + keeloq_common learning schemes (GPL-3.0-or-later),
 * (C) Flipper Devices Inc. and contributors. Manufacturer list mirrors Momentum
 * firmware's keeloq_mfcodes (GPL-3.0-or-later).
 */

#pragma once

#include <cstdint>
#include <string>

namespace model {
namespace rf {

// KeeLoq block cipher + manufacturer key-derivation ("learning") schemes and
// hop framing. All pure - no filesystem, no AES keystore. The `/mfcodes`
// keystore reader (encrypted built-in blob) and the full keeloq_identify loop
// stay out of this layer for now; they depend on the mfcodes data + mbedTLS and
// belong with the platform/asset layer.

// Learning-type ids. Values match the Flipper/SubGhz keystore `type` column.
enum : uint32_t {
    KEELOQ_UNKNOWN_LEARNING = 0,
    KEELOQ_SIMPLE_LEARNING = 1,
    KEELOQ_NORMAL_LEARNING = 2,
    KEELOQ_SECURE_LEARNING = 3,
    KEELOQ_MAGIC_XOR_TYPE1_LEARNING = 4,
    KEELOQ_FAAC_LEARNING = 5,               // separate protocol - not handled here
    KEELOQ_MAGIC_SERIAL_TYPE1_LEARNING = 6,
    KEELOQ_MAGIC_SERIAL_TYPE2_LEARNING = 7,
    KEELOQ_MAGIC_SERIAL_TYPE3_LEARNING = 8,
    KEELOQ_SIMPLE_KINGGATES_LEARNING = 10,  // separate protocol - not handled here
    KEELOQ_NORMAL_JAROLIFT_LEARNING = 11,   // separate protocol - not handled here
    KEELOQ_ERREKA_LEARNING = 12,
    KEELOQ_PUJOL_LEARNING = 13,
    KEELOQ_AERF_LEARNING = 14,
    KEELOQ_SIMPLE_JCM_LEARNING = 15,
};

// --- Cipher core (identical NLF 0x3A5C742E, 528 rounds, taps) ---------------
uint32_t keeloq_encrypt(uint32_t data, uint64_t key);
uint32_t keeloq_decrypt(uint32_t data, uint64_t key);
uint64_t keeloq_normal_learning(uint32_t data, uint64_t key);

// --- Manufacturer-specific learning schemes --------------------------------
uint64_t keeloq_secure_learning(uint32_t data, uint32_t seed, uint64_t key);
uint64_t keeloq_magic_xor_type1_learning(uint32_t data, uint64_t xorv);
uint64_t keeloq_magic_serial_type1_learning(uint32_t data, uint64_t man);
uint64_t keeloq_magic_serial_type2_learning(uint32_t data, uint64_t man);
uint64_t keeloq_magic_serial_type3_learning(uint32_t data, uint64_t man);
uint64_t keeloq_learning_aerf(uint32_t data, uint64_t key);
uint64_t keeloq_learning_erreka(uint32_t data, uint32_t mix, uint64_t key);
uint64_t keeloq_learning_pujol(uint32_t data, uint64_t key);

// Single dispatch: derive the manufacturer key for `type`. `fix` is
// button|serial; `seed` is only consulted by secure/erreka. Unknown/simple
// learning returns `key` unchanged.
uint64_t keeloq_derive_man(uint32_t type, uint32_t fix, uint32_t seed, uint64_t key);

// Build the KeeLoq "hop" plaintext (button | manufacturer-masked serial |
// counter) for a manufacturer. Pure; mirrors the reference branch table.
uint32_t keeloq_build_hop(const std::string& mf_name, uint8_t btn, uint32_t serial, uint16_t cnt);

// --- Decrypt validation (decode side of keeloq_identify) --------------------
// Given a decrypted hop, confirm the button + serial low byte match this
// receiver; on success write the recovered counter to `cnt_out` and return true.
bool keeloq_check_decrypt(uint32_t decrypt, uint8_t btn, uint32_t serial, uint16_t& cnt_out);
// Centurion uses a fixed 0x1CE serial field, hence a dedicated check.
bool keeloq_check_decrypt_centurion(uint32_t decrypt, uint8_t btn, uint16_t& cnt_out);

} // namespace rf
} // namespace model
