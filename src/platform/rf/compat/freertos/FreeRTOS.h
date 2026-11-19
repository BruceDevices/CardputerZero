/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * FreeRTOS shim (compile-only). The RMT ISR/queue path is never used on Linux.
 */
#pragma once
#include <cstdint>

typedef int BaseType_t;
#ifndef pdTRUE
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#endif

typedef volatile int portMUX_TYPE;
#ifndef portMUX_INITIALIZER_UNLOCKED
#define portMUX_INITIALIZER_UNLOCKED 0
#endif
#ifndef portTICK_PERIOD_MS
#define portTICK_PERIOD_MS 1
#endif

#define taskENTER_CRITICAL(mux) do { (void)(mux); } while (0)
#define taskEXIT_CRITICAL(mux) do { (void)(mux); } while (0)
#define taskENTER_CRITICAL_ISR(mux) do { (void)(mux); } while (0)
#define taskEXIT_CRITICAL_ISR(mux) do { (void)(mux); } while (0)
#define portENTER_CRITICAL(mux) do { (void)(mux); } while (0)
#define portEXIT_CRITICAL(mux) do { (void)(mux); } while (0)
#define portENTER_CRITICAL_ISR(mux) do { (void)(mux); } while (0)
#define portEXIT_CRITICAL_ISR(mux) do { (void)(mux); } while (0)
#define portYIELD_FROM_ISR(x) do { (void)(x); } while (0)

inline void vTaskDelay(uint32_t) {}
