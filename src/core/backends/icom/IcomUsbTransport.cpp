#include "core/backends/icom/IcomUsbTransport.h"

#include <QAudioDevice>
#include <QAudioSink>
#include <QAudioSource>
#include <QIODevice>
#include <QElapsedTimer>
#include <QMediaDevices>
#include <QtEndian>
#ifdef HAVE_SERIALPORT
#include <QSerialPort>
#endif
#include <algorithm>
#include <cmath>
#include <cstring>

namespace AetherSDR::icom {
namespace {
QAudioDevice deviceFor(const QList<QAudioDevice>& devices, const QByteArray& id)
{
    for (const QAudioDevice& device : devices) {
        if (!id.isEmpty() && device.id() == id) {
            return device;
        }
    }
    return {};
}

QAudioFormat formatFor(const QAudioDevice& device)
{
    for (const QAudioFormat::SampleFormat sample : {QAudioFormat::Int16, QAudioFormat::Float}) {
        for (const int channels : {2, 1}) {
            QAudioFormat format;
            format.setSampleRate(48000);
            format.setChannelCount(channels);
            format.setSampleFormat(sample);
            if (device.isFormatSupported(format)) {
                return format;
            }
        }
    }
    return {};
}
} // namespace

IcomUsbTransport::IcomUsbTransport(QObject* parent) : QObject(parent)
{
    m_txTimer.setInterval(10);
    m_txTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_txTimer, &QTimer::timeout, this, &IcomUsbTransport::pumpTx);
    m_frameTimer.setSingleShot(true);
    m_frameTimer.setInterval(100);
    connect(&m_frameTimer, &QTimer::timeout, this, [this] { m_civ.timeout(); });
}

IcomUsbTransport::~IcomUsbTransport() { stop(); }

bool IcomUsbTransport::start(const Params& params, QString& error)
{
    stop();
    m_params = params;
#ifndef HAVE_SERIALPORT
    error = QStringLiteral("USB Icom support requires a build with Qt SerialPort.");
    return false;
#else
    if (params.portName.isEmpty() || params.baudRate != 115200) {
        error = QStringLiteral("Select a USB CI-V port and set the radio's USB baud rate to 115200.");
        return false;
    }
    const QAudioDevice input = deviceFor(QMediaDevices::audioInputs(), params.inputDeviceId);
    const QAudioDevice output = deviceFor(QMediaDevices::audioOutputs(), params.outputDeviceId);
    if (input.isNull() || (params.enableTx && output.isNull())) {
        error = QStringLiteral("Select this radio's USB receive and transmit sound devices. A selected device is unavailable.");
        return false;
    }
    m_inputFormat = formatFor(input);
    m_outputFormat = params.enableTx ? formatFor(output) : QAudioFormat{};
    if (!m_inputFormat.isValid() || (params.enableTx && !m_outputFormat.isValid())) {
        error = QStringLiteral("The selected radio sound device must support 48 kHz mono or stereo PCM audio.");
        return false;
    }
    m_serialStats = {};
    m_audioStats = {};
    m_port = std::make_unique<QSerialPort>();
    m_port->setPortName(params.portName);
    m_port->setBaudRate(params.baudRate);
    m_port->setDataBits(QSerialPort::Data8);
    m_port->setParity(QSerialPort::NoParity);
    m_port->setStopBits(QSerialPort::OneStop);
    m_port->setFlowControl(QSerialPort::NoFlowControl);
    if (!m_port->open(QIODevice::ReadWrite)) {
        error = QStringLiteral("Cannot open USB CI-V port %1: %2").arg(params.portName, m_port->errorString());
        stop();
        return false;
    }
    // CI-V controls PTT. Handshake lines must never key USB SEND or CW.
    m_port->setDataTerminalReady(false);
    m_port->setRequestToSend(false);
    connect(m_port.get(), &QSerialPort::readyRead, this, [this] {
        if (m_running && m_port) {
            acceptSerialBytes(m_port->readAll());
        }
    });
    connect(m_port.get(), &QSerialPort::errorOccurred, this, [this](QSerialPort::SerialPortError code) {
        if (m_running && code != QSerialPort::NoError) {
            ++m_serialStats.socketErrors;
            fail(QStringLiteral("USB CI-V port failed: %1").arg(m_port->errorString()));
        }
    });
    m_source = std::make_unique<QAudioSource>(input, m_inputFormat);
    m_source->setBufferSize(m_inputFormat.bytesForDuration(40000));
    m_capture = m_source->start();
    if (!m_capture || m_source->error() != QAudio::NoError) {
        error = QStringLiteral("Cannot start the selected radio USB receive audio device.");
        stop();
        return false;
    }
    connect(m_capture, &QIODevice::readyRead, this, [this] {
        if (!m_running || !m_capture) { return; }
        m_rxPending.append(m_capture->readAll());
        const qsizetype count = m_rxPending.size() / m_inputFormat.bytesPerFrame()
            * m_inputFormat.bytesPerFrame();
        if (count == 0) { return; }
        const std::vector<float> mono = decodeAudio(m_rxPending.first(count), m_inputFormat);
        m_rxPending.remove(0, count);
        m_audioStats.rxBytes += static_cast<quint64>(count);
        ++m_audioStats.rxPackets;
        emit audioReady(mono);
    });
    connect(m_source.get(), &QAudioSource::stateChanged, this, [this](QAudio::State state) {
        if (m_running && state == QAudio::StoppedState && m_source->error() != QAudio::NoError) {
            fail(QStringLiteral("Radio USB receive audio stopped unexpectedly."));
        }
    });
    if (params.enableTx) {
        m_sink = std::make_unique<QAudioSink>(output, m_outputFormat);
        m_sink->setBufferSize(m_outputFormat.bytesForDuration(40000));
        m_playback = m_sink->start();
        if (!m_playback || m_sink->error() != QAudio::NoError) {
            error = QStringLiteral("Cannot start the selected radio USB transmit audio device.");
            stop();
            return false;
        }
        connect(m_sink.get(), &QAudioSink::stateChanged, this, [this](QAudio::State state) {
            if (m_running && state == QAudio::StoppedState && m_sink->error() != QAudio::NoError) {
                fail(QStringLiteral("Radio USB transmit audio stopped unexpectedly."));
            }
        });
    }
    m_running = true;
    m_txTimer.start();
    return true;
#endif
}

void IcomUsbTransport::stop()
{
    m_running = false;
    m_txTimer.stop();
    m_frameTimer.stop();
    if (m_source) { m_source->stop(); }
    if (m_sink) { m_sink->reset(); }
    m_capture = nullptr;
    m_playback = nullptr;
    m_source.reset();
    m_sink.reset();
#ifdef HAVE_SERIALPORT
    if (m_port) {
        // The backend queues its authorized unkey before stopping the session.
        // QSerialPort::close discards pending writes, so give that final command
        // a bounded opportunity to reach the driver before closing the port.
        QElapsedTimer deadline;
        deadline.start();
        while (m_port->isOpen() && m_port->bytesToWrite() > 0 && deadline.elapsed() < 150) {
            if (!m_port->waitForBytesWritten(static_cast<int>(std::max<qint64>(1, 150 - deadline.elapsed())))) {
                break;
            }
        }
        m_port->close();
    }
    m_port.reset();
#endif
    m_civ.reset();
    m_rxPending.clear();
    m_txPending.clear();
    m_txContext = {};
}

void IcomUsbTransport::fail(const QString& reason)
{
    if (!m_running) { return; }
    m_running = false;
    m_txTimer.stop();
    // Leave the current device callback before the session tears down its I/O.
    QTimer::singleShot(0, this, [this, reason] { emit failed(reason); });
}

void IcomUsbTransport::acceptSerialBytes(const QByteArray& bytes)
{
    m_serialStats.rxBytes += static_cast<quint64>(bytes.size());
    const auto rawFrames = m_civ.feed({reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                                      static_cast<std::size_t>(bytes.size())});
    for (const std::vector<std::uint8_t>& raw : rawFrames) {
        const std::optional<CivFrame> frame = parseFrame(raw);
        if (frame && frame->from != kControllerAddress
            && (frame->to == kControllerAddress || frame->to == kBroadcastAddress)) {
            ++m_serialStats.rxPackets;
            emit civFrameReady(*frame);
        }
    }
    if (m_civ.framePending()) { m_frameTimer.start(); }
    else { m_frameTimer.stop(); }
}

void IcomUsbTransport::sendCiv(std::span<const std::uint8_t> frame,
                              const std::optional<TxCoordinator::Command>& command)
{
#ifdef HAVE_SERIALPORT
    if (!m_running || !m_port || (command && !command->permitsDispatch(TxCoordinator::monotonicMs()))) {
        return;
    }
    const qint64 written = m_port->write(reinterpret_cast<const char*>(frame.data()),
                                         static_cast<qint64>(frame.size()));
    if (written != static_cast<qint64>(frame.size())) {
        fail(QStringLiteral("Could not write a complete USB CI-V command."));
        return;
    }
    m_serialStats.txBytes += static_cast<quint64>(written);
    ++m_serialStats.txPackets;
#else
    Q_UNUSED(frame);
    Q_UNUSED(command);
#endif
}

QByteArray IcomUsbTransport::encodeAudio(std::span<const float> mono, const QAudioFormat& format)
{
    if (format.channelCount() < 1 || (format.sampleFormat() != QAudioFormat::Int16
        && format.sampleFormat() != QAudioFormat::Float)) { return {}; }
    QByteArray out(static_cast<qsizetype>(mono.size()) * format.bytesPerFrame(), Qt::Uninitialized);
    char* cursor = out.data();
    for (const float value : mono) {
        const float sample = std::isfinite(value) ? std::clamp(value, -1.0F, 1.0F) : 0.0F;
        for (int channel = 0; channel < format.channelCount(); ++channel) {
            if (format.sampleFormat() == QAudioFormat::Int16) {
                const qint16 pcm = static_cast<qint16>(std::clamp(std::lround(sample * 32768.0F), -32768L, 32767L));
                qToLittleEndian(pcm, cursor);
            } else {
                std::memcpy(cursor, &sample, sizeof(sample));
            }
            cursor += format.bytesPerSample();
        }
    }
    return out;
}

std::vector<float> IcomUsbTransport::decodeAudio(const QByteArray& bytes, const QAudioFormat& format)
{
    if (format.bytesPerFrame() <= 0 || (format.sampleFormat() != QAudioFormat::Int16
        && format.sampleFormat() != QAudioFormat::Float)) { return {}; }
    std::vector<float> mono(static_cast<std::size_t>(bytes.size() / format.bytesPerFrame()));
    const char* cursor = bytes.constData();
    for (float& sample : mono) {
        // Main receiver is the left channel on the IC-9700. Mixing in its
        // independent sub receiver would corrupt decoders and recordings.
        if (format.sampleFormat() == QAudioFormat::Int16) {
            sample = static_cast<float>(qFromLittleEndian<qint16>(cursor)) / 32768.0F;
        } else {
            std::memcpy(&sample, cursor, sizeof(sample));
            sample = std::isfinite(sample) ? std::clamp(sample, -1.0F, 1.0F) : 0.0F;
        }
        cursor += format.bytesPerFrame();
    }
    return mono;
}

void IcomUsbTransport::sendAudio(std::span<const float> mono, const TxCoordinator::Context& context)
{
    if (!m_running || !m_params.enableTx || !m_sink
        || !context.permitsDispatch(TxCoordinator::monotonicMs())) { return; }
    if (!m_txContext.sameContext(context)) {
        flushTxAudio();
        m_txContext = context;
    }
    const QByteArray bytes = encodeAudio(mono, m_outputFormat);
    const qsizetype maximum = m_outputFormat.bytesForDuration(250000);
    if (m_txPending.size() + bytes.size() > maximum) {
        fail(QStringLiteral("Radio USB transmit audio queue overflowed."));
        return;
    }
    m_txPending.append(bytes);
    pumpTx();
}

void IcomUsbTransport::pumpTx()
{
    if (!m_running || !m_sink) { return; }
    if (!m_txContext.sameContext(TxCoordinator::Context{})
        && !m_txContext.permitsDispatch(TxCoordinator::monotonicMs())) {
        flushTxAudio();
        return;
    }
    if (m_txPending.isEmpty()) { return; }
    const qint64 count = std::min<qint64>(m_sink->bytesFree(), m_txPending.size());
    if (count <= 0 || !m_playback) { return; }
    const qint64 written = m_playback->write(m_txPending.constData(), count);
    if (written < 0) {
        fail(QStringLiteral("Could not write radio USB transmit audio."));
        return;
    }
    m_txPending.remove(0, static_cast<qsizetype>(written));
    m_audioStats.txBytes += static_cast<quint64>(written);
    ++m_audioStats.txPackets;
}

void IcomUsbTransport::flushTxAudio()
{
    m_txPending.clear();
    m_txContext = {};
    if (m_sink && m_running) {
        m_sink->reset();
        m_playback = m_sink->start();
        if (!m_playback || m_sink->error() != QAudio::NoError) {
            fail(QStringLiteral("Cannot restart radio USB transmit audio."));
        }
    }
}

int IcomUsbTransport::txAudioDrainMs() const
{
    if (!m_sink || !m_outputFormat.isValid()) { return 0; }
    const qint64 pending = m_txPending.size() + std::max<qint64>(0, m_sink->bufferSize() - m_sink->bytesFree());
    return static_cast<int>((m_outputFormat.durationForBytes(pending) + 999) / 1000) + 10;
}

} // namespace AetherSDR::icom
