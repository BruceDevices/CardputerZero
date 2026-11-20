/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * ESP-IDF RMT RX driver shim (compile-only; see rmt_tx.h).
 */
#pragma once
#include "rmt_tx.h"

typedef struct {
    rmt_symbol_word_t *received_symbols;
    size_t num_symbols;
} rmt_rx_done_event_data_t;

typedef bool (*rmt_rx_done_callback_t)(rmt_channel_handle_t, const rmt_rx_done_event_data_t *, void *);

typedef struct {
    rmt_rx_done_callback_t on_recv_done;
} rmt_rx_event_callbacks_t;

typedef struct {
    uint32_t signal_range_min_ns;
    uint32_t signal_range_max_ns;
} rmt_receive_config_t;

typedef struct {
    rmt_clock_source_t clk_src;
    gpio_num_t gpio_num;
    uint32_t resolution_hz;
    size_t mem_block_symbols;
    struct {
        uint32_t invert_in : 1;
        uint32_t with_dma : 1;
        uint32_t io_loop_back : 1;
    } flags;
} rmt_rx_channel_config_t;

inline esp_err_t rmt_new_rx_channel(const rmt_rx_channel_config_t *, rmt_channel_handle_t *out) {
    if (out) *out = nullptr;
    return ESP_OK;
}
inline esp_err_t rmt_rx_register_event_callbacks(rmt_channel_handle_t, const rmt_rx_event_callbacks_t *,
                                                 void *) {
    return ESP_OK;
}
inline esp_err_t rmt_receive(rmt_channel_handle_t, void *, size_t, const rmt_receive_config_t *) {
    return ESP_OK;
}
