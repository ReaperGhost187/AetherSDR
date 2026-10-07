// Model tables grounded in IC-7300 Full Manual 12a chapter 19 and
// IC-9700 CI-V Reference Guide April 2021. No sockets or hardware.
#include "core/backends/icom/IcomModels.h"
#include "core/backends/icom/IcomMemoryCodec.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace AetherSDR::icom;
int main()
{
    int failures = 0;
    const auto check = [&failures](bool pass, const char* what) {
        if (!pass) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
    };
    const IcomModel& mk1 = *modelForId(0x94);
    const IcomModel& mk2 = *modelForId(0xB6);
    const IcomModel& uhf = *modelForId(0xA2);
    const auto& original = profileFor(mk1);
    check(original.supportedBringup && mk1.verified && !mk1.hasNetwork,
          "original IC-7300 has its own USB-capable profile");
    check(original.modulation && original.modulation->usbLevelItem == 65
          && original.modulation->dataOffInputItem == 66 && original.modulation->dataInputItem == 67,
          "MK1 uses SET 0065/0066/0067, not the MK2's addresses");
    check(original.modulation->networkOnlyValue == 3
          && profileFor(mk2).modulation->networkOnlyValue == 5,
          "MK1 USB and MK2 LAN routing remain independent");
    check(original.txBandwidth && original.txBandwidth->wideItem == 14
          && original.txBandwidth->dataItem == 196
          && profileFor(mk2).txBandwidth->dataItem == 17,
          "MK1 SSB-D filter writes SET 0196, preserving MK2 SET 0017");
    check(original.scope.center && original.scope.fixed && original.scope.scrollCenter
          && original.scope.scrollFixed && original.scope.hasSweepSpeed,
          "current MK1 firmware supports the four documented scope modes");
    check(original.supports(IcomFeature::CwTextKeyer) && original.supports(IcomFeature::MemoryChannels)
          && original.supports(IcomFeature::AntennaTuner), "MK1 keyer, memories and tuner are exposed");
    check(original.fmRepeater && original.fmRepeater->separateCtcssFunctions
          && original.fmRepeater->accessModes.size() == 3
          && std::ranges::find(original.fmRepeater->accessModes, "ctcss_rx")
              == original.fmRepeater->accessModes.end(),
          "MK1 exposes TONE and TSQL without an unsupported access selector");
    check(!profileFor(uhf).supports(IcomFeature::AntennaTuner)
          && profileFor(uhf).txBandwidth->dataItem == 20,
          "IC-9700 keeps its own TX filter and has no tuner");
    check(profileFor(uhf).setMenu.civTransceiveItem == 127,
          "IC-9700 CI-V Transceive is SET 0127, not external keypad VOICE");
    check(std::abs(meterValue(MeterId::Comp, 241, -73.0, MeterCalibration::Ic7300) - 30.0) < 0.001,
          "MK1 COMP full scale is 241 at 30 dB");
    check(std::abs(interpolateCurve(powerCurveFor(mk1), 143) - 50.0) < 0.001,
          "MK1 power indication at raw 143 is 50 W");
    check(std::abs(meterValue(MeterId::Id, 146, -73.0, MeterCalibration::Ic7300) - 15.0) < 0.001,
          "MK1 PA current follows its own documented endpoints");
    std::vector<std::uint8_t> record(41, 0);
    record[1] = 0x42;
    const auto frequency = encodeFreq(14'074'000);
    std::copy(frequency.begin(), frequency.end(), record.begin() + 3);
    record[8] = 0x01; record[9] = 0x02; record[10] = 0x10;
    record[12] = 0x08; record[13] = 0x85;
    record[15] = 0x08; record[16] = 0x85;
    const char name[] = "FT8 20m";
    std::copy(std::begin(name), std::end(name) - 1, record.begin() + 31);
    const auto memory = decodeMemory(MemoryDialect::Ic7300, record);
    check(memory && memory->recallable && !memory->split && memory->mode == "DIGU"
          && memory->name == "FT8 20m" && memory->frequencyHz == 14'074'000,
          "MK1 memory uses two VFO blocks and a ten-character name");
    record[2] = 0x10;
    const auto split = decodeMemory(MemoryDialect::Ic7300, record);
    check(split && split->split && !split->recallable, "split memories stay visible without losing their TX frequency");
    return failures == 0 ? 0 : 1;
}
