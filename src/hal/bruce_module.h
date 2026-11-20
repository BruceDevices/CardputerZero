/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <mutex>

// HAL de módulos da CAP (CardputerZero).
//
// Fonte da verdade, em runtime, dos pinos dedicados de cada módulo embarcado
// numa CAP (CC1101, nRF24, LoRa/SX1262, ST25R3916). O barramento SPI em si é
// um recurso GLOBAL/compartilhado (display, teclado, PMIC) e NÃO é guardado
// aqui. O pino de reset (`rst`) é POR MÓDULO — depende de quem fabrica a CAP.
//
// Os pinos são structs C++ comuns (um `lv_subject_t` só carrega escalar, não
// o conjunto de pinos). O estado de detecção reativo é exposto por Subjects:
// `cap_kind_subject()` e os BoolSubjects de presença por módulo. O fluxo é:
// a detecção atualiza os pinos *e* chama `.set()`/`.notify()` nos Subjects, e
// a UI/os módulos reagem relendo os pinos atuais.
//
// ATENÇÃO: os acessores de Subject (cap_kind_subject / *_present_subject) e
// `apply_profile()`/`reset_absent()` tocam objetos `lv_subject_t`, então só
// podem ser chamados DEPOIS de `lv_init()`. `modules()` (dados puros) é seguro
// a qualquer momento.

namespace reactive {
class IntSubject;
class BoolSubject;
} // namespace reactive

namespace hal {

enum class Bus { None, SPI, I2C };
enum class LoraModel { None, SX1262 /*, SX1276, LLCC68, ... */ };
enum class CapKind { None, CapCC1101, CapLora, Pingequa };

// rst é por módulo (varia por fabricante da CAP); -1 = sem reset dedicado.
struct Cc1101Pins {
    int cs = -1, gdo0 = -1, gdo2 = -1, sw0 = -1, sw1 = -1, rst = -1;
};
struct Nrf24Pins {
    int cs = -1, ce = -1, irq = -1, rst = -1;
};
struct LoraPins {
    int cs = -1, irq = -1, busy = -1, rst = -1;
    LoraModel model = LoraModel::None;
};
struct St25rPins {
    int cs = -1, irq = -1, rst = -1;
    Bus bus = Bus::None; // CAP = SPI | módulo externo = I2C
};
// GPS é UART (não SPI): tx/rx são os GPIOs do CM0 (CAP LORA: TX=G14, RX=G15 →
// /dev/serial0); `baud` é -1 até a detecção por NMEA confirmar o stream.
struct GpsPins {
    int tx = -1, rx = -1, baud = -1;
};

struct BruceModule {
    Cc1101Pins cc1101;
    Nrf24Pins nrf24;
    LoraPins lora;
    St25rPins st25r3916;
    GpsPins gps;
    CapKind kind = CapKind::None;
};

// Acessor do registro global (dados puros; seguro a qualquer momento).
BruceModule& modules();

// Presença por módulo (cs válido; ST25R também conta bus != None para I2C).
bool has_cc1101();
bool has_nrf24();
bool has_lora();
bool has_st25r();
bool has_gps(); // true quando a detecção NMEA confirmou o stream (baud != -1)

const char* cap_name(CapKind kind);
const char* lora_model_name(LoraModel model);
const char* bus_name(Bus bus);

// Retorna os pinos de um perfil conhecido SEM efeitos colaterais (não toca o
// registro global nem os Subjects, logo é seguro antes de `lv_init()` — ex.:
// no caminho CLI `--cc1101-detect`). `apply_profile` é construído sobre isto.
BruceModule make_profile(CapKind kind);

// Preenche os pinos a partir da tabela de perfis conhecidos e atualiza os
// Subjects. Módulos ausentes no perfil ficam com todos os pinos -1.
void apply_profile(CapKind kind);

// Identifica a CAP conectada pela assinatura de chip (sonda um chip único num
// CS único por CAP, via as primitivas SPI do platform), aplica o perfil
// correspondente (`apply_profile`, que já imprime o debug e atualiza os
// Subjects) e retorna o `CapKind` detectado (`None` se nenhuma). Faz o
// bring-up da CAP (energia best-effort) antes de sondar e serializa o acesso
// ao barramento SPI pelo `spi_bus_mutex()`.
CapKind detect_cap();

// Re-roda `detect_cap()` (sob o guard) — para troca de CAP com o aparelho
// ligado (hot-swap). Retorna true se alguma CAP foi detectada (kind != None).
bool redetect();

// Se o CC1101 já está presente, retorna true; senão dispara `redetect()` (cobre
// a troca de CAP) e reavalia. Os módulos funcionais chamam isto antes de
// desistir de usar o chip.
bool ensure_cc1101();

// Mutex do barramento SPI GLOBAL (compartilhado com display/teclado/PMIC). TODA
// transação SPI — detecção e uso de chip — deve ser serializada por ele para
// não colidir com a UI. `detect_cap()`/`redetect()` já o travam internamente;
// um consumidor de chip in-process trava este mutex em volta da sua transação.
std::mutex& spi_bus_mutex();

// Entrypoint CLI do "Use CC1101 CAP (detect)" (re-exec `--cc1101-detect`): roda
// a detecção completa, lê os pinos dinâmicos do CC1101 de `modules()` e, se
// ausente após `redetect()`, reporta "CC1101 ausente". Retorna 0 se presente.
int cc1101_detect_cli(int argc, char** argv);

// CLI `--rf-info [freq]`: run detection, read the dynamic CC1101 pins, configure
// the radio for OOK async RX and stream VERSION + RSSI. Proves the M6 driver.
int rf_info_cli(int argc, char** argv);

// CLI `--rf-record [freq] [save]`: configure OOK async RX and capture GDO0 edges
// via RfRxCapture (M7), printing the signed-µs durations of each frame. With the
// `save` token, also write the recording to a Flipper/Bruce SubGhz RAW `.sub`
// (M8). Env: RF_RECORD_SECS (default 20), RF_RX_GLITCH_US, RF_RX_MIN_TRANSITIONS,
// RF_GDO0_GPIO, RF_SAVE_DIR (default ./BruceRF).
int rf_record_cli(int argc, char** argv);

// CLI `--rf-send <key_hex> [freq] [te] [count]`: transmit an OOK PT2262 code via
// the CC1101 in async TX, bit-banging GDO0 (M9) — the same code `subghz tx`
// sends on Bruce, so a reference Bruce receiver decodes it. Env: RF_TX_POWER.
int rf_send_cli(int argc, char** argv);

// CLI `--rf-replay <file.sub>`: load a Flipper/Bruce Protocol: RAW recording
// and replay its signed durations at the frequency stored in the file (M9).
int rf_replay_cli(int argc, char** argv);

// CLI `--rf-protos`: list the ported Bruce protocol registry (name/bits/te).
int rf_protos_cli(int argc, char** argv);

// CLI `--rf-tx-proto <name> <key_hex> [bits] [te] [count] [freq]`: encode a
// named registry protocol with the ported engine and transmit it (bit-banged).
int rf_tx_proto_cli(int argc, char** argv);

// CLI `--rf-selftest`: pure-software encode->decode round-trip of the ported
// protocol engine (no radio). Returns 0 when every fixed-length protocol passes.
int rf_selftest_cli(int argc, char** argv);

// CLI `--rf-decode-sub <file.sub>`: decode every RAW frame in a .sub via the
// ported engine (e.g. a RAW capture made by another receiver).
int rf_raw_cli(int argc, char** argv);
int rf_carrier_cli(int argc, char** argv);
int rf_decode_sub_cli(int argc, char** argv);

// Zera tudo (-1 / None, kind=None) e atualiza os Subjects.
void reset_absent();

// Imprime o resumo de debug (LOG_INFO + stdout) no formato do plano.
void print_debug();

// Subjects reativos (requerem lv_init() previamente).
reactive::IntSubject& cap_kind_subject();
reactive::BoolSubject& cc1101_present_subject();
reactive::BoolSubject& nrf24_present_subject();
reactive::BoolSubject& lora_present_subject();
reactive::BoolSubject& st25r_present_subject();
reactive::BoolSubject& gps_present_subject();

} // namespace hal
