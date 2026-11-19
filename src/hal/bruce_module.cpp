/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "bruce_module.h"

#include "cap_bringup.h"  // platform::spidev_pick
#include "cc1101.h"       // platform::probe_cc1101 / probe_st25r / probe_nrf24 / Cc1101Radio
#include "gps.h"          // platform::probe_gps
#include "logger.h"
#include "rf_capture.h"   // platform::RfRxCapture (M7)
#include "rf_emit.h"      // platform::rf_ook_encode_pt2262 / RfTxLine (M9)
#include "rf_recording.h" // platform::RawRecording / rf_save_sub (M8)
#include "rf_receiver.h"
#include "rf_bridge.h"    // rfproto:: encode/decode (ported Bruce engine)
#include "subjects.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <unistd.h>
#endif

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
            // G22 (bit-bang). GDO0=G15 (data out), SW0=G14 (antenna) — confirmed
            // on HW: async RX data comes out on G15 (the G14/G15 pair was swapped
            // in the earlier unverified profile).
            m.cc1101 = Cc1101Pins{/*cs=*/7, /*gdo0=*/15, /*gdo2=*/-1,
                                  /*sw0=*/14, /*sw1=*/-1, /*rst=*/26};
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

namespace {

// First existing spidev node, matching do_detect()'s pick.
std::string pick_spidev() {
    const std::string picked =
        platform::spidev_pick({"/dev/spidev0.1", "/dev/spidev0.0"});
    return picked.empty() ? std::string("/dev/spidev0.1") : picked;
}

} // namespace

const char* RfReceiver::start(float mhz, bool capture_pulses) {
    stop();
    if (!ensure_cc1101()) return "Connect a CC1101 CAP";

    const auto pins = modules().cc1101;
    const auto device = pick_spidev();
    {
        std::lock_guard<std::mutex> lock(spi_bus_mutex());
        if (!platform::cc1101_radio_open(radio_, device.c_str(), pins.cs, pins.gdo0, pins.sw0, pins.rst)) {
            return "Could not open CC1101";
        }
        platform::cc1101_config_ook_async_rx(radio_, mhz);
        if (!platform::cc1101_version_valid(platform::cc1101_status_read(radio_, 0x31))) {
            platform::cc1101_radio_close(radio_);
            return "CC1101 did not respond";
        }
        platform::cc1101_rx(radio_);
    }
    if (capture_pulses && !capture_.begin(pins.gdo0)) {
        stop();
        return "Could not open GDO0 for capture";
    }
    return nullptr;
}

void RfReceiver::stop(std::deque<platform::RfRxFrame>* pending) {
    if (!active()) return;
    capture_.end(pending);
    std::lock_guard<std::mutex> lock(spi_bus_mutex());
    platform::cc1101_idle(radio_);
    platform::cc1101_radio_close(radio_);
}

bool RfReceiver::tune(float mhz) {
    const bool capturing = capture_.active();
    if (capturing) capture_.end();
    {
        std::lock_guard<std::mutex> lock(spi_bus_mutex());
        platform::cc1101_set_frequency(radio_, mhz);
        platform::cc1101_rx(radio_);
    }
    return !capturing || capture_.begin(radio_.gdo0_gpio);
}

int RfReceiver::rssi_dbm() const {
    std::lock_guard<std::mutex> lock(spi_bus_mutex());
    return platform::cc1101_rssi_dbm(radio_);
}

int rf_info_cli(int argc, char** argv) {
    // --rf-info [freq_mhz]: detect the CAP, configure the CC1101 for OOK async
    // RX, read VERSION and stream RSSI. Proves the radio config + synthesis (M6).
    float mhz = 433.92f;
    if(argc >= 1 && argv[0] && argv[0][0] != '\0') {
        // Accept Hz (e.g. 433920000) or MHz (433.92), same as --rf-send.
        const double v = std::atof(argv[0]);
        if(v > 0.0) mhz = (v > 10000.0) ? static_cast<float>(v / 1e6) : static_cast<float>(v);
    }

    ensure_cc1101();
    if(!has_cc1101()) {
        std::fputs("CC1101 absent — connect the CAP CC1101 or Pingequa\n", stderr);
        return 1;
    }
    const Cc1101Pins c = g_modules.cc1101;
    const std::string dev = pick_spidev();

    std::lock_guard<std::mutex> lock(g_spi_mutex);
    platform::Cc1101Radio r;
    if(!platform::cc1101_radio_open(r, dev.c_str(), c.cs, c.gdo0, c.sw0, c.rst)) {
        std::fprintf(stderr, "failed to open radio on %s\n", dev.c_str());
        return 1;
    }
    platform::cc1101_config_ook_async_rx(r, mhz);
    const uint8_t ver = platform::cc1101_status_read(r, 0x31); // VERSION
    platform::cc1101_rx(r);

    std::printf("CC1101 VERSION=0x%02X  freq=%.2f MHz  CS=G%d GDO0=G%d SW0=%s\n",
                ver, static_cast<double>(mhz), c.cs, c.gdo0,
                c.sw0 < 0 ? "-" : ("G" + std::to_string(c.sw0)).c_str());
    std::fflush(stdout);

#if defined(__linux__)
    usleep(5 * 1000); // let IDLE->calibration->RX settle before reading state
#endif
    // Live register/state dump to compare against Bruce's expected RX config.
    std::printf("MARCSTATE=0x%02X (0x0D=RX)\n", platform::cc1101_marcstate(r));
    static const struct { const char* n; uint8_t a; } kRegs[] = {
        {"IOCFG2",0x00},{"IOCFG0",0x02},{"FIFOTHR",0x03},{"PKTCTRL0",0x08},
        {"FSCTRL1",0x0B},{"FSCTRL0",0x0C},{"FREQ2",0x0D},{"FREQ1",0x0E},{"FREQ0",0x0F},
        {"MDMCFG4",0x10},{"MDMCFG3",0x11},{"MDMCFG2",0x12},{"DEVIATN",0x15},
        {"MCSM0",0x18},{"FOCCFG",0x19},{"AGCCTRL2",0x1B},{"AGCCTRL1",0x1C},{"AGCCTRL0",0x1D},
        {"FREND1",0x21},{"FREND0",0x22},{"FSCAL3",0x23},{"FSCAL2",0x24},{"FSCAL1",0x25},
        {"FSCAL0",0x26},{"TEST2",0x2C},{"TEST1",0x2D},{"TEST0",0x2E},
    };
    for(const auto& rg : kRegs)
        std::printf("  %-8s 0x%02X = 0x%02X\n", rg.n, rg.a, platform::cc1101_reg_read(r, rg.a));
    std::fflush(stdout);

    for(int i = 0; i < 20; ++i) {
        std::printf("RSSI: %d dBm\n", platform::cc1101_rssi_dbm(r));
        std::fflush(stdout);
#if defined(__linux__)
        usleep(200 * 1000);
#endif
    }

    platform::cc1101_idle(r);
    platform::cc1101_radio_close(r);
    return platform::cc1101_version_valid(ver) ? 0 : 1;
}

int rf_record_cli(int argc, char** argv) {
    // --rf-record [freq_mhz] [save]: configure OOK async RX, capture GDO0 edges
    // through RfRxCapture (M7), print each frame's signed-µs durations, and — if
    // the `save` token is given — write the whole recording to a Flipper/Bruce
    // SubGhz RAW `.sub` file (M8).
    float mhz = 433.92f;
    bool save = false;
    for(int i = 0; i < argc; ++i) {
        if(!argv[i] || argv[i][0] == '\0') continue;
        if(std::strcmp(argv[i], "save") == 0 || std::strcmp(argv[i], "--save") == 0) {
            save = true;
        } else {
            // Accept Hz (e.g. 433920000) or MHz (433.92), same as --rf-send.
            const double v = std::atof(argv[i]);
            if(v > 0.0) mhz = (v > 10000.0) ? static_cast<float>(v / 1e6) : static_cast<float>(v);
        }
    }

    ensure_cc1101();
    if(!has_cc1101()) {
        std::fputs("CC1101 absent — connect the CAP CC1101 or Pingequa\n", stderr);
        return 1;
    }
    const Cc1101Pins c = g_modules.cc1101;
    const std::string dev = pick_spidev();

    platform::Cc1101Radio r;
    {
        std::lock_guard<std::mutex> lock(g_spi_mutex);
        if(!platform::cc1101_radio_open(r, dev.c_str(), c.cs, c.gdo0, c.sw0, c.rst)) {
            std::fprintf(stderr, "failed to open radio on %s\n", dev.c_str());
            return 1;
        }
        platform::cc1101_config_ook_async_rx(r, mhz);
        platform::cc1101_rx(r);
    }

    // RF_GDO0_GPIO overrides the data pin for bring-up (the CAP CC1101 GDO0 wiring
    // was never probe-verified); lets us sweep candidates without a rebuild.
    int gdo0 = c.gdo0;
    if(const char* ov = std::getenv("RF_GDO0_GPIO"); ov && ov[0] != '\0') gdo0 = std::atoi(ov);

    platform::RfRxCapture cap;
    if(!cap.begin(gdo0)) {
        std::fprintf(stderr,
                     "failed to claim GDO0=G%d for edge capture "
                     "(if it is the SPI CE1 pin G7, that CAP needs a different path)\n",
                     gdo0);
        std::lock_guard<std::mutex> lock(g_spi_mutex);
        platform::cc1101_idle(r);
        platform::cc1101_radio_close(r);
        return 1;
    }

    const int secs = [] {
        const char* v = std::getenv("RF_RECORD_SECS");
        return (v && v[0] != '\0') ? std::atoi(v) : 20;
    }();
    std::printf("Recording on %.2f MHz (CC1101 GDO0=G%d) for %ds — press a 433 remote...\n",
                static_cast<double>(mhz), gdo0, secs);
    std::fflush(stdout);

    platform::RawRecording rec;
    rec.frequency_hz = static_cast<double>(mhz) * 1e6;

    int frames = 0;
    std::vector<int> durs;
    const time_t start = std::time(nullptr);
    while(std::time(nullptr) - start < secs) {
        if(cap.poll(durs)) {
            ++frames;
            std::printf("[frame %d] %zu durations:", frames, durs.size());
            for(size_t i = 0; i < durs.size() && i < 40; ++i) std::printf(" %d", durs[i]);
            if(durs.size() > 40) std::printf(" ...");
            std::printf("\n");
            // Identify the protocol via the ported Bruce engine (OOK + KeeLoq).
            rfproto::Decoded d = rfproto::decode(durs);
            if(d.ok) {
                std::printf("  -> DECODED protocol=%s preset=%s key=0x%llX bits=%d te=%d%s%s\n",
                            d.protocol.c_str(), d.preset.c_str(),
                            static_cast<unsigned long long>(d.key), d.bits, d.te,
                            d.keeloq ? " [KeeLoq]" : "",
                            d.decodedText.empty() ? "" : (" " + d.decodedText).c_str());
            }
            std::fflush(stdout);
            if(save) rec.add_frame(std::move(durs), platform::rf_monotonic_ms());
        } else {
#if defined(__linux__)
            usleep(5 * 1000);
#endif
        }
    }

    const unsigned long long total_edges = static_cast<unsigned long long>(cap.edges());
    cap.end();
    {
        std::lock_guard<std::mutex> lock(g_spi_mutex);
        platform::cc1101_idle(r);
        platform::cc1101_radio_close(r);
    }
    std::printf("Done. %d frame(s) captured. Total edges on G%d: %llu\n",
                frames, gdo0, total_edges);

    if(save) {
        if(rec.empty()) {
            std::fputs("Nothing captured — no .sub written.\n", stdout);
        } else {
            // Destination dir: $RF_SAVE_DIR, else ./BruceRF (cwd). The app proper
            // will pass the configured storage path instead.
            const char* envdir = std::getenv("RF_SAVE_DIR");
            const std::string dir = (envdir && envdir[0] != '\0') ? envdir : "BruceRF";
            const std::string path = platform::rf_save_sub(rec, dir);
            if(path.empty()) std::fprintf(stderr, "failed to write .sub into %s\n", dir.c_str());
            else std::printf("Saved: %s (%zu frames, %zu durations)\n",
                             path.c_str(), rec.frame_count(), rec.total_durations());
        }
    }
    std::fflush(stdout);
    return frames > 0 ? 0 : 1;
}

int rf_send_cli(int argc, char** argv) {
    // --rf-send <key_hex> [freq] [te] [count]: transmit an OOK PT2262 code the
    // way Bruce's `subghz tx` does, so a reference Bruce receiver decodes it (M9).
    // freq accepts Hz (e.g. 433920000) or MHz (433.92). Defaults: 433.92 / 174 / 10.
    if(argc < 1 || !argv[0] || argv[0][0] == '\0') {
        std::fputs("usage: --rf-send <key_hex> [freq] [te_us] [count]\n", stderr);
        return 2;
    }
    const uint64_t key = std::strtoull(argv[0], nullptr, 16);
    float mhz = 433.92f;
    if(argc >= 2 && argv[1] && argv[1][0]) {
        const double v = std::atof(argv[1]);
        mhz = (v > 10000.0) ? static_cast<float>(v / 1e6) : static_cast<float>(v);
    }
    const int te = (argc >= 3 && argv[2] && argv[2][0]) ? std::atoi(argv[2]) : 174;
    const int count = (argc >= 4 && argv[3] && argv[3][0]) ? std::atoi(argv[3]) : 10;
    const int bits = 24; // PT2262 default (matches Bruce's `subghz tx`)

    ensure_cc1101();
    if(!has_cc1101()) {
        std::fputs("CC1101 absent — connect the CAP CC1101 or Pingequa\n", stderr);
        return 1;
    }
    const Cc1101Pins c = g_modules.cc1101;
    const std::string dev = pick_spidev();

    std::vector<int> durs;
    platform::rf_ook_encode_pt2262(key, bits, te, count, durs);

    platform::RfTxLine tx_line;
    if(!platform::rf_tx_line_open(tx_line, c.gdo0)) {
        std::fprintf(stderr, "TX failed (could not drive GDO0=G%d)\n", c.gdo0);
        return 1;
    }
    platform::Cc1101Radio r;
    {
        std::lock_guard<std::mutex> lock(g_spi_mutex);
        if(!platform::cc1101_radio_open(r, dev.c_str(), c.cs, c.gdo0, c.sw0, c.rst)) {
            std::fprintf(stderr, "failed to open radio on %s\n", dev.c_str());
            platform::rf_tx_line_close(tx_line);
            return 1;
        }
        platform::cc1101_config_ook_async_tx(r, mhz);
        platform::cc1101_tx(r); // enter TX; carrier keyed by GDO0 (held low = off)
#if defined(__linux__)
        usleep(2000); // allow IDLE -> calibration -> TX to settle
#endif
        const uint8_t state = platform::cc1101_marcstate(r);
        if(state != 0x13) {
            std::fprintf(stderr, "CC1101 failed to enter TX (MARCSTATE=0x%02X)\n", state);
            platform::cc1101_idle(r);
            platform::rf_tx_line_close(tx_line);
            platform::cc1101_radio_close(r);
            return 1;
        }
        std::printf("CC1101 MARCSTATE=0x%02X (TX)\n", state);
    }

    std::printf("TX 0x%llX (%d bits) @ %.2f MHz te=%dus x%d on GDO0=G%d ...\n",
                static_cast<unsigned long long>(key), bits, static_cast<double>(mhz),
                te, count, c.gdo0);
    std::fflush(stdout);

    const bool ok = platform::rf_tx_line_emit(tx_line, durs);

    {
        std::lock_guard<std::mutex> lock(g_spi_mutex);
        platform::cc1101_idle(r);
        platform::rf_tx_line_close(tx_line);
        platform::cc1101_radio_close(r);
    }
    std::printf(ok ? "Done.\n" : "TX failed (could not drive GDO0=G%d)\n", c.gdo0);
    std::fflush(stdout);
    return ok ? 0 : 1;
}

int rf_protos_cli(int, char**) {
    // List the ported Bruce protocol registry (name / bits / te / enc / dec).
    const int n = rfproto::protocol_count();
    std::printf("RF protocol registry: %d entries\n", n);
    for(int i = 0; i < n; ++i) {
        rfproto::ProtoInfo p = rfproto::protocol_at(i);
        std::printf("  %-16s bits=%-3d te=%-4d %s%s\n", p.name.c_str(), p.bits, p.te,
                    p.has_encoder ? "ENC " : "    ", p.has_decoder ? "DEC" : "");
    }
    std::fflush(stdout);
    return 0;
}

int rf_tx_proto_cli(int argc, char** argv) {
    // --rf-tx-proto <name> <key_hex> <bits> <te> <count> [freq]
    // Encodes a named registry protocol via the ported engine and bit-bangs it.
    if(argc < 2) {
        std::fputs("usage: --rf-tx-proto <name> <key_hex> [bits] [te] [count] [freq]\n", stderr);
        return 2;
    }
    const std::string name = argv[0];
    const uint64_t key = std::strtoull(argv[1], nullptr, 16);
    const int bits = (argc >= 3 && argv[2][0]) ? std::atoi(argv[2]) : 0;   // 0 = registry default
    const int te = (argc >= 4 && argv[3][0]) ? std::atoi(argv[3]) : 0;     // 0 = registry default
    const int count = (argc >= 5 && argv[4][0]) ? std::atoi(argv[4]) : 10;
    float mhz = 433.92f;
    if(argc >= 6 && argv[5][0]) {
        const double v = std::atof(argv[5]);
        mhz = (v > 10000.0) ? static_cast<float>(v / 1e6) : static_cast<float>(v);
    }

    std::vector<int> durs;
    bool enc;
    if(name == "KeeLoq" || name == "keeloq")
        enc = rfproto::encode_keeloq(key, count, durs);
    else
        enc = rfproto::encode(name, key, bits, te, count, durs);
    if(!enc || durs.empty()) {
        std::fprintf(stderr, "encode failed for protocol '%s' (unknown or no encoder)\n", name.c_str());
        return 1;
    }

    ensure_cc1101();
    if(!has_cc1101()) {
        std::fputs("CC1101 absent — connect the CAP CC1101 or Pingequa\n", stderr);
        return 1;
    }
    const Cc1101Pins c = g_modules.cc1101;
    const std::string dev = pick_spidev();

    platform::RfTxLine tx_line;
    if(!platform::rf_tx_line_open(tx_line, c.gdo0)) {
        std::fprintf(stderr, "TX failed (could not drive GDO0=G%d)\n", c.gdo0);
        return 1;
    }
    platform::Cc1101Radio r;
    {
        std::lock_guard<std::mutex> lock(g_spi_mutex);
        if(!platform::cc1101_radio_open(r, dev.c_str(), c.cs, c.gdo0, c.sw0, c.rst)) {
            std::fprintf(stderr, "failed to open radio on %s\n", dev.c_str());
            platform::rf_tx_line_close(tx_line);
            return 1;
        }
        platform::cc1101_config_ook_async_tx(r, mhz);
        platform::cc1101_tx(r);
#if defined(__linux__)
        usleep(2000);
#endif
        const uint8_t state = platform::cc1101_marcstate(r);
        if(state != 0x13) {
            std::fprintf(stderr, "CC1101 failed to enter TX (MARCSTATE=0x%02X)\n", state);
            platform::cc1101_idle(r);
            platform::rf_tx_line_close(tx_line);
            platform::cc1101_radio_close(r);
            return 1;
        }
        std::printf("CC1101 MARCSTATE=0x%02X (TX)\n", state);
    }

    std::printf("TX proto=%s key=0x%llX bits=%d te=%d x%d @ %.2f MHz (%zu durations) on GDO0=G%d ...\n",
                name.c_str(), static_cast<unsigned long long>(key), bits, te, count,
                static_cast<double>(mhz), durs.size(), c.gdo0);
    std::fflush(stdout);

    const bool ok = platform::rf_tx_line_emit(tx_line, durs);
    {
        std::lock_guard<std::mutex> lock(g_spi_mutex);
        platform::cc1101_idle(r);
        platform::rf_tx_line_close(tx_line);
        platform::cc1101_radio_close(r);
    }
    std::printf(ok ? "Done.\n" : "TX failed\n");
    std::fflush(stdout);
    return ok ? 0 : 1;
}

int rf_raw_cli(int argc, char** argv) {
    // --rf-raw [hi_us] [lo_us] [pulses] [bursts] [freq]: transmit a RAW OOK
    // pattern (hi/lo)*pulses per burst, `bursts` bursts separated by a 10 ms gap,
    // with the NORMAL OOK PATABLE (0x00/0xC0). Lets a RAW receiver (Flipper time
    // view) and an RSSI spectrum (T-Embed) both see the modulation directly.
    const int hi = (argc >= 1 && argv[0][0]) ? std::atoi(argv[0]) : 200;
    const int lo = (argc >= 2 && argv[1][0]) ? std::atoi(argv[1]) : 400;
    const int pulses = (argc >= 3 && argv[2][0]) ? std::atoi(argv[2]) : 20;
    const int bursts = (argc >= 4 && argv[3][0]) ? std::atoi(argv[3]) : 300;
    float mhz = 433.92f;
    if(argc >= 5 && argv[4][0]) {
        const double v = std::atof(argv[4]);
        mhz = (v > 10000.0) ? static_cast<float>(v / 1e6) : static_cast<float>(v);
    }
    std::vector<int> durs;
    durs.reserve(static_cast<size_t>(bursts) * (pulses * 2 + 1));
    for(int b = 0; b < bursts; ++b) {
        for(int p = 0; p < pulses; ++p) { durs.push_back(hi); durs.push_back(-lo); }
        durs.push_back(-10000); // 10 ms inter-burst gap
    }
    ensure_cc1101();
    if(!has_cc1101()) { std::fputs("CC1101 absent\n", stderr); return 1; }
    const Cc1101Pins c = g_modules.cc1101;
    const std::string dev = pick_spidev();

    platform::RfTxLine tx_line;
    if(!platform::rf_tx_line_open(tx_line, c.gdo0)) {
        std::fprintf(stderr, "could not drive GDO0=G%d\n", c.gdo0);
        return 1;
    }
    platform::Cc1101Radio r;
    {
        std::lock_guard<std::mutex> lock(g_spi_mutex);
        if(!platform::cc1101_radio_open(r, dev.c_str(), c.cs, c.gdo0, c.sw0, c.rst)) {
            std::fprintf(stderr, "radio open failed\n");
            platform::rf_tx_line_close(tx_line);
            return 1;
        }
        platform::cc1101_config_ook_async_tx(r, mhz);
        platform::cc1101_tx(r);
#if defined(__linux__)
        usleep(2000);
#endif
        std::printf("CC1101 MARCSTATE=0x%02X  RAW (%d -%d)x%d x%d bursts @ %.2f MHz GDO0=G%d SW0=G%d\n",
                    platform::cc1101_marcstate(r), hi, lo, pulses, bursts,
                    static_cast<double>(mhz), c.gdo0, c.sw0);
        std::fflush(stdout);
    }
    const bool ok = platform::rf_tx_line_emit(tx_line, durs);
    {
        std::lock_guard<std::mutex> lock(g_spi_mutex);
        platform::cc1101_idle(r);
        platform::rf_tx_line_close(tx_line);
        platform::cc1101_radio_close(r);
    }
    std::printf(ok ? "RAW tx done.\n" : "RAW tx failed\n");
    std::fflush(stdout);
    return ok ? 0 : 1;
}

int rf_carrier_cli(int argc, char** argv) {
    // --rf-carrier [secs] [freq]: hold a CONTINUOUS unmodulated carrier (GDO0
    // driven HIGH for the whole time) to isolate the PA/PLL/antenna path from
    // the OOK bit-bang. A spectrum analyzer / Flipper should show a steady
    // carrier at the frequency if TX radiates at all.
    const int secs = (argc >= 1 && argv[0][0]) ? std::atoi(argv[0]) : 3;
    float mhz = 433.92f;
    if(argc >= 2 && argv[1][0]) {
        const double v = std::atof(argv[1]);
        mhz = (v > 10000.0) ? static_cast<float>(v / 1e6) : static_cast<float>(v);
    }
    // "blink" mode: NORMAL OOK PATABLE (0x00/0xC0) and a 1 Hz square wave on GDO0,
    // so a spectrum watcher can see whether driving GDO0 actually keys the carrier.
    bool blink = false;
    for(int i = 0; i < argc; ++i) if(argv[i] && std::strcmp(argv[i], "blink") == 0) blink = true;
    ensure_cc1101();
    if(!has_cc1101()) { std::fputs("CC1101 absent\n", stderr); return 1; }
    const Cc1101Pins c = g_modules.cc1101;
    const std::string dev = pick_spidev();

    // Continuous mode: one long HIGH. Blink mode: 1 Hz square wave for `secs`.
    std::vector<int> durs;
    if(blink) {
        for(int i = 0; i < secs; ++i) { durs.push_back(1000000); durs.push_back(-1000000); }
    } else {
        durs.push_back(secs * 1000000);
    }

    platform::RfTxLine tx_line;
    if(!platform::rf_tx_line_open(tx_line, c.gdo0)) {
        std::fprintf(stderr, "could not drive GDO0=G%d\n", c.gdo0);
        return 1;
    }
    platform::Cc1101Radio r;
    {
        std::lock_guard<std::mutex> lock(g_spi_mutex);
        if(!platform::cc1101_radio_open(r, dev.c_str(), c.cs, c.gdo0, c.sw0, c.rst)) {
            std::fprintf(stderr, "radio open failed\n");
            platform::rf_tx_line_close(tx_line);
            return 1;
        }
        platform::cc1101_config_ook_async_tx(r, mhz);
        // Continuous mode forces BOTH OOK levels ON (carrier independent of GDO0)
        // to isolate PA/PLL/antenna; blink mode keeps the normal 0x00/0xC0 levels
        // so the 1 Hz square wave actually tests whether GDO0 keys the carrier.
        if(!blink) platform::cc1101_set_patable(r, 0xC0, 0xC0);
        platform::cc1101_tx(r);
#if defined(__linux__)
        usleep(2000);
#endif
        std::printf("CC1101 MARCSTATE=0x%02X (0x13=TX)  carrier ON %ds @ %.2f MHz (SW0=G%d, PATABLE=C0/C0)\n",
                    platform::cc1101_marcstate(r), secs, static_cast<double>(mhz), c.sw0);
        std::fflush(stdout);
    }
    // Also drive GDO0 high throughout (belt and suspenders); carrier is on either way.
    const bool ok = platform::rf_tx_line_emit(tx_line, durs);
    {
        std::lock_guard<std::mutex> lock(g_spi_mutex);
        platform::cc1101_idle(r);
        platform::rf_tx_line_close(tx_line);
        platform::cc1101_radio_close(r);
    }
    std::printf(ok ? "carrier done.\n" : "carrier emit failed\n");
    std::fflush(stdout);
    return ok ? 0 : 1;
}

int rf_decode_sub_cli(int argc, char** argv) {
    // --rf-decode-sub <file.sub>: load a Flipper/Bruce RAW .sub and run every
    // frame through the ported protocol engine. Used to decode a RAW capture
    // taken by another receiver (e.g. a T-Embed `subghz rx -raw`), so the CZ TX
    // path can be verified without exposing the CZ RX to close-range saturation.
    if(argc < 1 || !argv[0] || argv[0][0] == '\0') {
        std::fputs("usage: --rf-decode-sub <file.sub>\n", stderr);
        return 2;
    }
    platform::RawRecording rec;
    std::string error;
    if(!platform::rf_load_sub(argv[0], rec, error)) {
        std::fprintf(stderr, "load failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("Loaded %zu frame(s) from %s\n", rec.frame_count(), argv[0]);
    int decoded = 0;
    for(size_t i = 0; i < rec.frames.size(); ++i) {
        rfproto::Decoded d = rfproto::decode(rec.frames[i]);
        if(d.ok) {
            ++decoded;
            std::printf("  [frame %zu] DECODED protocol=%s preset=%s key=0x%llX bits=%d te=%d%s\n",
                        i, d.protocol.c_str(), d.preset.c_str(),
                        static_cast<unsigned long long>(d.key), d.bits, d.te,
                        d.keeloq ? " [KeeLoq]" : "");
        } else {
            std::printf("  [frame %zu] no match (%zu durations)\n", i, rec.frames[i].size());
        }
    }
    std::printf("Decoded %d / %zu frames\n", decoded, rec.frame_count());
    std::fflush(stdout);
    return decoded > 0 ? 0 : 1;
}

int rf_selftest_cli(int, char**) {
    // Pure-software round-trip of the ported engine: for every fixed-length
    // registry protocol, encode a known key, feed the durations back through
    // the decoder, and check the recovered key matches. No radio involved.
    const int n = rfproto::protocol_count();
    int pass = 0, fail = 0, skipped = 0;
    std::printf("RF engine encode->decode self-test (%d protocols)\n", n);
    for(int i = 0; i < n; ++i) {
        rfproto::ProtoInfo p = rfproto::protocol_at(i);
        if(p.bits <= 0) { ++skipped; continue; } // variable-length (RcSwitch_*)
        const uint64_t mask = (p.bits >= 64) ? ~0ULL : ((1ULL << p.bits) - 1);
        const uint64_t key = 0xA5A5A5A5A5A5A5A5ULL & mask;
        std::vector<int> durs;
        if(!rfproto::encode(p.name, key, p.bits, 0, 4, durs) || durs.empty()) {
            std::printf("  %-16s ENCODE-FAIL\n", p.name.c_str());
            ++fail;
            continue;
        }
        rfproto::Decoded d = rfproto::decode(durs);
        const bool ok = d.ok && (d.key == key) && (d.bits == p.bits);
        std::printf("  %-16s %s enc=%zu durs -> dec=%s key=0x%llX bits=%d\n", p.name.c_str(),
                    ok ? "PASS" : "FAIL", durs.size(), d.ok ? d.protocol.c_str() : "(none)",
                    static_cast<unsigned long long>(d.key), d.bits);
        ok ? ++pass : ++fail;
    }
    std::printf("Self-test: %d PASS, %d FAIL, %d skipped (variable-length)\n", pass, fail, skipped);
    std::fflush(stdout);
    return fail == 0 ? 0 : 1;
}

int rf_replay_cli(int argc, char** argv) {
    if(argc < 1 || !argv[0] || argv[0][0] == '\0') {
        std::fputs("usage: --rf-replay <file.sub>\n", stderr);
        return 2;
    }

    platform::RawRecording rec;
    std::string error;
    if(!platform::rf_load_sub(argv[0], rec, error)) {
        std::fprintf(stderr, "failed to load %s: %s\n", argv[0], error.c_str());
        return 1;
    }
    const float mhz = static_cast<float>(rec.frequency_hz / 1e6);
    if(mhz < 280.0f || mhz > 928.0f) {
        std::fprintf(stderr, "frequency %.3f MHz is outside the supported range\n",
                     static_cast<double>(mhz));
        return 1;
    }

    ensure_cc1101();
    if(!has_cc1101()) {
        std::fputs("CC1101 absent — connect the CAP CC1101 or Pingequa\n", stderr);
        return 1;
    }
    const Cc1101Pins c = g_modules.cc1101;
    const std::string dev = pick_spidev();
    const std::vector<int>& durations = rec.frames.front();

    platform::RfTxLine tx_line;
    if(!platform::rf_tx_line_open(tx_line, c.gdo0)) {
        std::fprintf(stderr, "Replay failed (could not drive GDO0=G%d)\n", c.gdo0);
        return 1;
    }
    platform::Cc1101Radio r;
    {
        std::lock_guard<std::mutex> lock(g_spi_mutex);
        if(!platform::cc1101_radio_open(r, dev.c_str(), c.cs, c.gdo0, c.sw0, c.rst)) {
            std::fprintf(stderr, "failed to open radio on %s\n", dev.c_str());
            platform::rf_tx_line_close(tx_line);
            return 1;
        }
        platform::cc1101_config_ook_async_tx(r, mhz);
        platform::cc1101_tx(r);
#if defined(__linux__)
        usleep(2000);
#endif
        const uint8_t state = platform::cc1101_marcstate(r);
        if(state != 0x13) {
            std::fprintf(stderr, "CC1101 failed to enter TX (MARCSTATE=0x%02X)\n", state);
            platform::cc1101_idle(r);
            platform::rf_tx_line_close(tx_line);
            platform::cc1101_radio_close(r);
            return 1;
        }
        std::printf("CC1101 MARCSTATE=0x%02X (TX)\n", state);
    }

    std::printf("Replay %s @ %.3f MHz (%zu durations) on GDO0=G%d ...\n",
                argv[0], static_cast<double>(mhz), durations.size(), c.gdo0);
    std::fflush(stdout);
    const bool ok = platform::rf_tx_line_emit(tx_line, durations);

    {
        std::lock_guard<std::mutex> lock(g_spi_mutex);
        platform::cc1101_idle(r);
        platform::rf_tx_line_close(tx_line);
        platform::cc1101_radio_close(r);
    }
    std::printf(ok ? "Done.\n" : "Replay failed (could not drive GDO0=G%d)\n", c.gdo0);
    std::fflush(stdout);
    return ok ? 0 : 1;
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
