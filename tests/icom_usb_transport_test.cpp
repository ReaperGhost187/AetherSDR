// Socket-free USB framing and PCM tests. No serial port or sound device opens.
#include "core/backends/icom/IcomUsbTransport.h"
#include <QCoreApplication>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace AetherSDR::icom;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int failures = 0;
    const auto check = [&failures](bool pass, const char* what) {
        if (!pass) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
    };
    IcomUsbTransport transport;
    std::vector<CivFrame> frames;
    QObject::connect(&transport, &IcomUsbTransport::civFrameReady, &app,
                     [&frames](const CivFrame& frame) { frames.push_back(frame); });
    transport.acceptSerialBytes(QByteArray::fromHex("fefe94e01900fd"));
    transport.acceptSerialBytes(QByteArray::fromHex("fefe00e01900fd"));
    check(frames.empty(), "controller echoes cannot become radio state");
    transport.acceptSerialBytes(QByteArray::fromHex("fefee09419"));
    check(frames.empty(), "a partial USB read is retained");
    transport.acceptSerialBytes(QByteArray::fromHex("0094fdfefee0a21900a2fd"));
    check(frames.size() == 2 && frames[0].from == 0x94 && frames[0].data[0] == 0x94
              && frames[1].from == 0xA2, "fragmented and coalesced USB model replies decode");
    transport.acceptSerialBytes(QByteArray::fromHex("fefeaa94190094fd"));
    check(frames.size() == 2, "traffic for another controller is ignored");
    transport.acceptSerialBytes(QByteArray::fromHex("fefee09419"));
    transport.stop();
    transport.acceptSerialBytes(QByteArray::fromHex("0094fd"));
    check(frames.size() == 2, "stop clears a partial serial frame");

    for (const QAudioFormat::SampleFormat type : {QAudioFormat::Int16, QAudioFormat::Float}) {
        for (const int channels : {1, 2}) {
            QAudioFormat format;
            format.setSampleRate(48000);
            format.setChannelCount(channels);
            format.setSampleFormat(type);
            const std::vector<float> samples{-2.0F, -0.5F, 0.0F, 0.5F, 2.0F,
                                             std::numeric_limits<float>::quiet_NaN()};
            const QByteArray encoded = IcomUsbTransport::encodeAudio(samples, format);
            const std::vector<float> decoded = IcomUsbTransport::decodeAudio(encoded, format);
            check(decoded.size() == samples.size(), "PCM frame count survives format conversion");
            check(std::abs(decoded[1] + 0.5F) < 0.0001F && std::abs(decoded[3] - 0.5F) < 0.0001F,
                  "USB PCM level and sign round trip");
            check(decoded.front() == -1.0F && decoded[4] > 0.999F && decoded.back() == 0.0F,
                  "out-of-range and nonfinite PCM is bounded");
            if (channels == 2) {
                QByteArray independent = encoded;
                std::fill(independent.begin() + format.bytesPerSample(),
                          independent.begin() + format.bytesPerFrame(), 0);
                const auto main = IcomUsbTransport::decodeAudio(independent, format);
                check(main.front() == -1.0F, "IC-9700 sub audio cannot attenuate the main receiver");
            }
        }
    }
    QAudioFormat unsupported;
    check(IcomUsbTransport::encodeAudio(std::vector<float>{1.0F}, unsupported).isEmpty(),
          "an unsupported output format is refused");
    QString error;
    check(!transport.start({}, error) && !error.isEmpty(), "missing USB configuration fails before opening I/O");
    return failures == 0 ? 0 : 1;
}
