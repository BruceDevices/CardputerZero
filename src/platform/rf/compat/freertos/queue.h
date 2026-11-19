/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * FreeRTOS queue shim (compile-only).
 */
#pragma once
#include "FreeRTOS.h"
#include <cstddef>

typedef void *QueueHandle_t;

inline QueueHandle_t xQueueCreate(unsigned, unsigned) { return nullptr; }
inline BaseType_t xQueueSend(QueueHandle_t, const void *, unsigned) { return pdTRUE; }
inline BaseType_t xQueueSendFromISR(QueueHandle_t, const void *, BaseType_t *) { return pdTRUE; }
inline BaseType_t xQueueReceive(QueueHandle_t, void *, unsigned) { return pdFALSE; }
inline void vQueueDelete(QueueHandle_t) {}
