/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * ESP-IDF RMT TX driver shim. Provides just enough types/functions for the
 * Bruce RMT transmit/receive code to COMPILE on Linux. These paths are never
 * executed on the CM0 (TX/RX are bridged to rf_bitbang / RfRxCapture), so the
 * functions are no-ops returning ESP_OK.
 */
#pragma once
#include <cstdint>
#include <cstddef>

typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
typedef int gpio_num_t;

// RMT symbol: two (level,duration) halves packed in 32 bits, as in ESP-IDF.
typedef union {
    struct {
        uint16_t duration0 : 15;
        uint16_t level0 : 1;
        uint16_t duration1 : 15;
        uint16_t level1 : 1;
    };
    uint32_t val;
} rmt_symbol_word_t;

typedef void *rmt_channel_handle_t;
typedef void *rmt_encoder_handle_t;

typedef enum { RMT_CLK_SRC_DEFAULT = 0 } rmt_clock_source_t;

typedef struct {
    rmt_clock_source_t clk_src;
    gpio_num_t gpio_num;
    uint32_t resolution_hz;
    size_t mem_block_symbols;
    size_t trans_queue_depth;
    struct {
        uint32_t invert_out : 1;
        uint32_t with_dma : 1;
        uint32_t io_loop_back : 1;
        uint32_t io_od_mode : 1;
    } flags;
} rmt_tx_channel_config_t;

typedef struct {
    int loop_count;
    struct {
        uint32_t eot_level : 1;
        uint32_t queue_nonblocking : 1;
    } flags;
} rmt_transmit_config_t;

typedef struct {
    int dummy;
} rmt_copy_encoder_config_t;

inline esp_err_t rmt_new_tx_channel(const rmt_tx_channel_config_t *, rmt_channel_handle_t *out) {
    if (out) *out = nullptr;
    return ESP_OK;
}
inline esp_err_t rmt_new_copy_encoder(const rmt_copy_encoder_config_t *, rmt_encoder_handle_t *out) {
    if (out) *out = nullptr;
    return ESP_OK;
}
inline esp_err_t rmt_enable(rmt_channel_handle_t) { return ESP_OK; }
inline esp_err_t rmt_disable(rmt_channel_handle_t) { return ESP_OK; }
inline esp_err_t rmt_transmit(rmt_channel_handle_t, rmt_encoder_handle_t, const void *, size_t,
                              const rmt_transmit_config_t *) {
    return ESP_OK;
}
inline esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t, int) { return ESP_OK; }
inline esp_err_t rmt_del_channel(rmt_channel_handle_t) { return ESP_OK; }
inline esp_err_t rmt_del_encoder(rmt_encoder_handle_t) { return ESP_OK; }
