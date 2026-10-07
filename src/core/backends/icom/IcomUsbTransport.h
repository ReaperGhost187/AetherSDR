#pragma once

#include <QAudioFormat>
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>
#include <memory>
#include <span>
#include <vector>

#include "core/TxCoordinator.h"
#include "core/backends/icom/CivCodec.h"
#include "core/backends/icom/IcomStream.h"

class QAudioSource;
class QAudioSink;
class QIODevice;
class QSerialPort;

namespace AetherSDR::icom {

// Local CI-V and the radio's explicitly selected USB sound device. All I/O
// callbacks run on the session thread; no radio state or UI lives here.
class IcomUsbTransport final : public QObject {
    Q_OBJECT
public:
    struct Params {
        QString portName;
        int baudRate = 115200;
        QByteArray inputDeviceId;
        QByteArray outputDeviceId;
        bool enableTx = true;
    };

    explicit IcomUsbTransport(QObject* parent = nullptr);
    ~IcomUsbTransport() override;
    bool start(const Params& params, QString& error);
    void stop();
    void sendCiv(std::span<const std::uint8_t> frame,
                 const std::optional<TxCoordinator::Command>& command);
    void sendAudio(std::span<const float> mono, const TxCoordinator::Context& context);
    void flushTxAudio();
    [[nodiscard]] int txAudioDrainMs() const;
    [[nodiscard]] IcomStream::Counters serialStats() const { return m_serialStats; }
    [[nodiscard]] IcomStream::Counters audioStats() const { return m_audioStats; }

    // Pure framing/PCM seams also used by socket-free tests.
    void acceptSerialBytes(const QByteArray& bytes);
    [[nodiscard]] static QByteArray encodeAudio(std::span<const float> mono,
                                                const QAudioFormat& format);
    [[nodiscard]] static std::vector<float> decodeAudio(const QByteArray& bytes,
                                                       const QAudioFormat& format);

signals:
    void civFrameReady(const AetherSDR::icom::CivFrame& frame);
    void audioReady(const std::vector<float>& mono);
    void failed(const QString& reason);

private:
    void pumpTx();
    void fail(const QString& reason);
    Params m_params;
#ifdef HAVE_SERIALPORT
    std::unique_ptr<QSerialPort> m_port;
#endif
    std::unique_ptr<QAudioSource> m_source;
    std::unique_ptr<QAudioSink> m_sink;
    QIODevice* m_capture = nullptr;
    QIODevice* m_playback = nullptr;
    QAudioFormat m_inputFormat;
    QAudioFormat m_outputFormat;
    QTimer m_txTimer;
    QTimer m_frameTimer;
    CivReassembler m_civ;
    QByteArray m_rxPending;
    QByteArray m_txPending;
    TxCoordinator::Context m_txContext;
    IcomStream::Counters m_serialStats;
    IcomStream::Counters m_audioStats;
    bool m_running = false;
};

} // namespace AetherSDR::icom
