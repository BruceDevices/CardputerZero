/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "bruce_module.h"

#include "cap_bringup.h" // platform::spidev_pick
#include "cc1101.h"      // platform::probe_cc1101 / probe_st25r / probe_nrf24
#include "gps.h"         // platform::probe_gps
#include "logger.h"
#include "subjects.h"

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace hal {

namespace {

// Registro global de dados puros. Seguro construir estaticamente (sem LVGL).
BruceModule g_modules{};

// Serializa TODA transação no barramento SPI compartilhado (detecção + uso de
// chip) para não colidir com display/teclado/PMIC/UI. Não-recursivo: o corpo
// interno de detecção (`do_detect`) roda sem travar; só os pontos de entrada
// públicos travam, uma vez.
std::mutex g_spi_mutex;

// Subjects reativos. Function-local statics → construídos de forma preguiçosa
// no primeiro acesso, que deve acontecer depois de lv_init().
reactive::IntSubject& cap_kind_subject_impl() {
    static reactive::IntSubject s{static_cast<int32_t>(CapKind::None)};
    return s;
}

// Formata um pino como "Gxx" ou "-" quando ausente.
std::string pin(int gpio) {
    return gpio < 0 ? std::string("-") : ("G" + std::to_string(gpio));
}

// Atualiza os Subjects reativos a partir do estado atual de g_modules.
void publish_subjects() {
    cap_kind_subject().set(static_cast<int32_t>(g_modules.kind));
    cc1101_present_subject().set(has_cc1101());
    nrf24_present_subject().set(has_nrf24());
    lora_present_subject().set(has_lora());
    st25r_present_subject().set(has_st25r());
    gps_present_subject().set(has_gps());
}

// Corpo da detecção SEM travar o mutex (o chamador já travou). Faz o bring-up
// de energia (best-effort) e sonda a assinatura de cada CAP na ordem do §6.
CapKind do_detect() {
    // Energia da CAP antes de sondar (best-effort; não-fatal). A semântica
    // energia-vs-RST está em revisão no M5, mas sem isto a CAP não responde.
    const platform::CapBringupReport br = platform::cap_bringup();
    LOG_INFO("{}", br.text);

    const std::string picked =
        platform::spidev_pick({"/dev/spidev0.1", "/dev/spidev0.0"});
    const std::string dev = picked.empty() ? std::string("/dev/spidev0.1") : picked;

    CapKind kind = CapKind::None;
    uint8_t version = 0;
    uint16_t sx_scratch = 0;
    uint8_t sx_status = 0;
    uint8_t nrf_status = 0;
    uint8_t nrf_config = 0;

    // Ordem por assinatura (confirmada no HW). No G7 (CS de hardware, modo SPI 0)
    // convivem o SX1262 (CAP LORA) e o CC1101 (CAP CC1101) — distinguidos pelo
    // framing: SX1262 responde ao scratch do reg 0x0740; o CC1101 a VERSION.
    // Observado: o chip secundário de cada CAP costuma ficar mudo (ST25R na CAP
    // CC1101, CC1101 na Pingequa), então a assinatura é sempre o chip primário.
    if(platform::probe_sx1262(dev.c_str(), /*cs=*/7, /*busy=*/22, /*rst=*/26,
                              sx_scratch, sx_status)) {
        // SX1262 @ G7 → CAP LORA (GPS no M5/5d).
        LOG_INFO("sx1262: status=0x{:02X} scratch=0x{:04X}", sx_status, sx_scratch);
        kind = CapKind::CapLora;
    } else if(platform::probe_cc1101(dev.c_str(), /*cs=*/7, /*rst=*/26, version)) {
        // CC1101 @ G7 (HW CS) → CAP CC1101 (ST25R no G22, normalmente mudo).
        LOG_INFO("cc1101@G7: VERSION=0x{:02X}", version);
        kind = CapKind::CapCC1101;
    } else if(platform::probe_cc1101(dev.c_str(), /*cs=*/14, /*rst=*/26, version)) {
        // CC1101 @ G14 (bit-bang) → Pingequa (CS confirmado no HW).
        LOG_INFO("cc1101@G14: VERSION=0x{:02X}", version);
        kind = CapKind::Pingequa;
    } else if(platform::probe_nrf24(dev.c_str(), /*cs=*/22, /*ce=*/23, nrf_status, nrf_config)) {
        // nRF24 @ G22 → Pingequa (assinatura de reserva quando o CC1101 fica mudo).
        LOG_INFO("nrf24: status=0x{:02X} config=0x{:02X}", nrf_status, nrf_config);
        kind = CapKind::Pingequa;
    }

    g_modules = make_profile(kind);

    // GPS (CAP LORA): detecção por NMEA na UART (sem precisar de fix de posição).
    if(kind == CapKind::CapLora) {
        int baud = -1;
        std::string sample;
        if(platform::probe_gps("/dev/serial0", /*bauds=*/nullptr, /*n=*/0,
                               /*ms_per_baud=*/800, baud, sample)) {
            g_modules.gps.baud = baud;
            LOG_INFO("gps: NMEA @ {} baud -> {}", baud, sample);
        } else {
            LOG_INFO("gps: sem NMEA (sinal fraco/sem antena ou UART ocupada)");
        }
    }

    publish_subjects(); // atualiza os Subjects (inclui GPS)
    print_debug();      // resumo único (já com o baud do GPS, se detectado)
    return kind;
}

} // namespace

BruceModule& modules() {
    return g_modules;
}

bool has_cc1101() {
    return g_modules.cc1101.cs != -1;
}
bool has_nrf24() {
    return g_modules.nrf24.cs != -1;
}
bool has_lora() {
    return g_modules.lora.cs != -1;
}
bool has_st25r() {
    return g_modules.st25r3916.cs != -1 || g_modules.st25r3916.bus != Bus::None;
}
bool has_gps() {
    return g_modules.gps.baud != -1;
}

const char* cap_name(CapKind kind) {
    switch(kind) {
        case CapKind::CapCC1101: return "CAP CC1101";
        case CapKind::CapLora: return "CAP LORA";
        case CapKind::Pingequa: return "Pingequa";
        case CapKind::None:
        default: return "nenhuma";
    }
}

const char* lora_model_name(LoraModel model) {
    switch(model) {
        case LoraModel::SX1262: return "SX1262";
        case LoraModel::None:
        default: return "none";
    }
}

const char* bus_name(Bus bus) {
    switch(bus) {
        case Bus::SPI: return "SPI";
        case Bus::I2C: return "I2C";
        case Bus::None:
        default: return "none";
    }
}

BruceModule make_profile(CapKind kind) {
    BruceModule m{};
    m.kind = kind;
    switch(kind) {
        case CapKind::CapCC1101:
            // Pinout confirmado no HW: CC1101 no CS de hardware (G7); ST25R no
            // G22 (bit-bang). gdo0/sw0 em G14/G15 (não verificados por probe).
            m.cc1101 = Cc1101Pins{/*cs=*/7, /*gdo0=*/14, /*gdo2=*/-1,
                                  /*sw0=*/15, /*sw1=*/-1, /*rst=*/26};
            m.st25r3916 = St25rPins{/*cs=*/22, /*irq=*/23, /*rst=*/26, Bus::SPI};
            break;
        case CapKind::CapLora:
            m.lora = LoraPins{/*cs=*/7, /*irq=*/23, /*busy=*/22,
                              /*rst=*/26, LoraModel::SX1262};
            m.gps = GpsPins{/*tx=*/14, /*rx=*/15, /*baud=*/-1}; // baud via NMEA
            break;
        case CapKind::Pingequa:
            // CC1101 CS confirmado no HW em G14 (não G15); gdo0 não verificado.
            m.cc1101 = Cc1101Pins{/*cs=*/14, /*gdo0=*/7, /*gdo2=*/-1,
                                  /*sw0=*/-1, /*sw1=*/-1, /*rst=*/26};
            m.nrf24 = Nrf24Pins{/*cs=*/22, /*ce=*/23, /*irq=*/-1, /*rst=*/26};
            break;
        case CapKind::None:
        default:
            break;
    }
    return m;
}

void apply_profile(CapKind kind) {
    g_modules = make_profile(kind);
    publish_subjects();
    print_debug();
}

CapKind detect_cap() {
    std::lock_guard<std::mutex> lock(g_spi_mutex);
    return do_detect();
}

bool redetect() {
    return detect_cap() != CapKind::None;
}

bool ensure_cc1101() {
    if(has_cc1101()) {
        return true;
    }
    redetect(); // cobre a troca de CAP com o aparelho ligado
    return has_cc1101();
}

std::mutex& spi_bus_mutex() {
    return g_spi_mutex;
}

int cc1101_detect_cli(int /*argc*/, char** /*argv*/) {
    // Detecção completa (energia + assinatura), lendo os pinos dinâmicos.
    ensure_cc1101();
    if(!has_cc1101()) {
        std::fputs("CC1101 ausente — conecte a CAP CC1101 ou Pingequa\n", stdout);
        std::fflush(stdout);
        return 1;
    }
    const Cc1101Pins& c = g_modules.cc1101;
    const std::string sw0 = c.sw0 < 0 ? std::string("-") : ("G" + std::to_string(c.sw0));
    std::printf("Use CC1101 CAP: CAP=%s  CC1101 CS=G%d GDO0=G%d SW0=%s\n",
                cap_name(g_modules.kind), c.cs, c.gdo0, sw0.c_str());
    std::fflush(stdout);
    return 0;
}

void reset_absent() {
    g_modules = BruceModule{};
    publish_subjects();
}

void print_debug() {
    std::string out;
    out += "[cap-detect] ";
    out += cap_name(g_modules.kind);
    out += "\n";

    if(has_cc1101()) {
        const auto& c = g_modules.cc1101;
        out += "  CC1101    : CS=" + pin(c.cs) + " GDO0=" + pin(c.gdo0) +
               " SW0=" + pin(c.sw0) + " RST=" + pin(c.rst) + "\n";
    } else {
        out += "  CC1101    : ausente\n";
    }

    if(has_st25r()) {
        const auto& s = g_modules.st25r3916;
        out += "  ST25R3916 : CS=" + pin(s.cs) + " IRQ=" + pin(s.irq) +
               " RST=" + pin(s.rst) + " BUS=" + bus_name(s.bus) + "\n";
    } else {
        out += "  ST25R3916 : ausente\n";
    }

    if(has_nrf24()) {
        const auto& n = g_modules.nrf24;
        out += "  nRF24     : CS=" + pin(n.cs) + " CE=" + pin(n.ce) +
               " IRQ=" + pin(n.irq) + " RST=" + pin(n.rst) + "\n";
    } else {
        out += "  nRF24     : ausente\n";
    }

    if(has_lora()) {
        const auto& l = g_modules.lora;
        out += "  LoRa      : CS=" + pin(l.cs) + " IRQ=" + pin(l.irq) +
               " BUSY=" + pin(l.busy) + " RST=" + pin(l.rst) +
               " model=" + lora_model_name(l.model) + "\n";
    } else {
        out += "  LoRa      : ausente\n";
    }

    if(g_modules.gps.tx != -1 || g_modules.gps.rx != -1) {
        const auto& g = g_modules.gps;
        out += "  GPS       : TX=" + pin(g.tx) + " RX=" + pin(g.rx) +
               (has_gps() ? (" NMEA@" + std::to_string(g.baud) + " baud")
                          : std::string(" (sem NMEA)")) +
               "\n";
    }

    // stdout (terminal) + logger.
    std::fputs(out.c_str(), stdout);
    std::fflush(stdout);
    LOG_INFO("{}", out);
}

reactive::IntSubject& cap_kind_subject() {
    return cap_kind_subject_impl();
}

reactive::BoolSubject& cc1101_present_subject() {
    static reactive::BoolSubject s{false};
    return s;
}
reactive::BoolSubject& nrf24_present_subject() {
    static reactive::BoolSubject s{false};
    return s;
}
reactive::BoolSubject& lora_present_subject() {
    static reactive::BoolSubject s{false};
    return s;
}
reactive::BoolSubject& st25r_present_subject() {
    static reactive::BoolSubject s{false};
    return s;
}
reactive::BoolSubject& gps_present_subject() {
    static reactive::BoolSubject s{false};
    return s;
}

} // namespace hal
