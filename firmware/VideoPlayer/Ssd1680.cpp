#include "Ssd1680.h"

#include <Arduino.h>
#include <driver/spi_master.h>
#include <esp_attr.h>
#include <initializer_list>

namespace panel {
namespace {

constexpr int PinBusy = 7;
constexpr int PinReset = 8;
constexpr int PinDataCommand = 9;
constexpr int PinChipSelect = 10;
constexpr int PinData = 11;
constexpr int PinClock = 12;
constexpr unsigned RegisterHz = 4000000;
constexpr unsigned PlaneHz = 20000000;
constexpr uint32_t ControlDeadlineUs = 6000000;

spi_device_handle_t spi = nullptr;
unsigned configuredHz = RegisterHz;
WaitHook service = nullptr;
bool powered = false;
portMUX_TYPE resetGuard = portMUX_INITIALIZER_UNLOCKED;
DMA_ATTR uint8_t masks[2][PlaneBytes];

bool configureSpi() {
    if (spi) return true;
    pinMode(PinBusy, INPUT);
    pinMode(PinReset, OUTPUT);
    pinMode(PinDataCommand, OUTPUT);
    digitalWrite(PinReset, HIGH);

    spi_bus_config_t bus{};
    bus.mosi_io_num = PinData;
    bus.miso_io_num = -1;
    bus.sclk_io_num = PinClock;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = PlaneBytes;
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

    spi_device_interface_config_t device{};
    device.clock_speed_hz = RegisterHz;
    device.spics_io_num = PinChipSelect;
    device.cs_ena_pretrans = 1;
    device.cs_ena_posttrans = 1;
    device.queue_size = 1;
    if (spi_bus_add_device(SPI2_HOST, &device, &spi) == ESP_OK) return true;
    spi_bus_free(SPI2_HOST);
    return false;
}

bool transfer(bool data, const uint8_t* bytes, size_t size, unsigned hz) {
    digitalWrite(PinDataCommand, data ? HIGH : LOW);
    spi_transaction_t packet{};
    packet.length = size * 8;
    if (configuredHz != hz) packet.override_freq_hz = hz;
    if (size <= sizeof(packet.tx_data)) {
        packet.flags = SPI_TRANS_USE_TXDATA;
        memcpy(packet.tx_data, bytes, size);
    } else {
        packet.tx_buffer = bytes;
    }
    if (spi_device_polling_transmit(spi, &packet) != ESP_OK) return false;
    configuredHz = hz;
    return true;
}

bool writeRegister(uint8_t command, std::initializer_list<uint8_t> value = {}) {
    return transfer(false, &command, 1, RegisterHz) &&
           (value.size() == 0 || transfer(true, value.begin(), value.size(), RegisterHz));
}

bool busy() { return digitalRead(PinBusy) == HIGH; }

bool waitIdle(uint32_t timeoutUs, bool fast = false) {
    const uint32_t started = micros();
    while (busy()) {
        if (micros() - started >= timeoutUs) return false;
        if (fast) delayMicroseconds(10);
        else {
            if (service) service();
            delay(1);
        }
    }
    return true;
}

bool waitBusyRise(uint32_t timeoutUs) {
    const uint32_t started = micros();
    while (!busy()) {
        if (micros() - started >= timeoutUs) return false;
        delayMicroseconds(5);
    }
    return true;
}

bool trigger(uint8_t flags) {
    return !busy() && writeRegister(0x22, {flags}) && writeRegister(0x20);
}

bool controlCycle(uint8_t flags) {
    if (!trigger(flags)) return false;
    delay(1);
    return waitIdle(ControlDeadlineUs);
}

bool setupWindow() {
    return writeRegister(0x11, {0x03}) && writeRegister(0x44, {0, RamSources / 8 - 1}) &&
           writeRegister(0x45, {0, 0, Width - 1, 0});
}

bool loadMask(uint8_t command, const uint8_t* bytes) {
    return writeRegister(0x4e, {0}) && writeRegister(0x4f, {0, 0}) &&
           writeRegister(command) && transfer(true, bytes, PlaneBytes, PlaneHz);
}

bool resetAndInitialize() {
    digitalWrite(PinReset, HIGH);
    delay(20);
    portENTER_CRITICAL(&resetGuard);
    digitalWrite(PinReset, LOW);
    delayMicroseconds(2000);
    digitalWrite(PinReset, HIGH);
    portEXIT_CRITICAL(&resetGuard);
    delay(20);
    if (!waitIdle(ControlDeadlineUs) || !writeRegister(0x12)) return false;
    delay(10);
    return waitIdle(ControlDeadlineUs) && writeRegister(0x01, {Width - 1, 0, 0}) &&
           writeRegister(0x0f, {0, 0}) && setupWindow() && writeRegister(0x3c, {0x05}) &&
           writeRegister(0x21, {0, 0x80}) && writeRegister(0x18, {0x80});
}

bool establishWhite() {
    memset(masks[0], 0xff, PlaneBytes);
    if (!loadMask(0x26, masks[0]) || !loadMask(0x24, masks[0]) ||
        !writeRegister(0x1a, {0x64, 0}) || !trigger(0xd7)) return false;
    if (!waitBusyRise(200000)) return false;
    const uint32_t started = micros();
    // The full update can briefly deassert BUSY between stages.
    do {
        if (!waitIdle(ControlDeadlineUs)) return false;
        delay(20);
    } while ((busy() || micros() - started < 1750000) && micros() - started < ControlDeadlineUs);
    return !busy() && micros() - started < ControlDeadlineUs;
}

bool loadFr7() {
    // The active G1 A phase is one controller frame. The four RAM combinations
    // are HOLD, drive in one direction, drive in the other direction, and HOLD.
    alignas(4) uint8_t waveform[153]{};
    waveform[12] = waveform[13] = 0x80;
    waveform[24] = waveform[25] = 0x40;
    waveform[67] = 1;
    memset(waveform + 144, 0x77, 6);
    if (!writeRegister(0x1a, {0x64, 0}) || !controlCycle(0x99) ||
        !writeRegister(0x32) || !transfer(true, waveform, sizeof waveform, RegisterHz) ||
        !writeRegister(0x3f, {0x02}) || !writeRegister(0x37, {0,0,0,0,0,0,0,0,0,0}) ||
        !writeRegister(0x3c, {0x80})) return false;
    clearMasks();
    return setupWindow() && loadMask(0x26, masks[0]) && loadMask(0x24, masks[1]) &&
           writeRegister(0x01, {Width - 1, 0, 0}) && writeRegister(0x0f, {0, 0}) && controlCycle(0xc0);
}

} // namespace

bool prepare(WaitHook onWait) {
    service = onWait;
    powered = false;
    if (!configureSpi() || !resetAndInitialize() || !establishWhite() || !loadFr7()) {
        digitalWrite(PinReset, LOW);
        return false;
    }
    powered = true;
    return true;
}

uint8_t* darkMask() { return masks[0]; }
uint8_t* lightMask() { return masks[1]; }
void clearMasks() { memset(masks, 0, sizeof masks); }

bool scan() {
    return powered && !busy() && loadMask(0x26, masks[0]) && loadMask(0x24, masks[1]) &&
           trigger(0x0c) && waitBusyRise(10000) && waitIdle(200000, true);
}

bool finish(bool healthy) {
    const bool slept = healthy && powered && controlCycle(0x03) && writeRegister(0x10, {0x01});
    if (slept) delay(200);
    digitalWrite(PinReset, LOW);
    powered = false;
    return slept;
}

} // namespace panel
