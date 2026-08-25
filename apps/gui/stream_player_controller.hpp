#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration>

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "ac3/analysis/levels.hpp"
#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"

namespace ac3::audio {
class MonitorSink;
}

// The decoded-in-memory result of one openFile() pass, kept OUTSIDE
// StreamPlayerController itself for the same reason objdec_detail::RawResult
// is (see object_decode_controller.hpp's own comment): the free decode
// helper in stream_player_controller.cpp can build one without member
// access, and it is reference-counted (shared_ptr, not a plain member) so a
// playback worker mid-flight on its own thread keeps its OWN handle on the
// result it started with even if openFile() replaces the controller's
// result_ with a new one for a second file - see play()'s own comment in
// stream_player_controller.cpp on why a raw reference into result_ would be
// a dangling-reference hazard here that objdec_detail::RawResult never had
// to consider (nothing there is read from a background thread after the
// decode itself finishes).
namespace splayer_detail {

struct RawResult {
    QString codec_label;              // "AC-3" or "E-AC-3"
    QString layout_label;             // ac3::analysis::layout_name(acmod, lfe)
    std::uint32_t sample_rate_hz = 0;
    ac3::Acmod acmod = ac3::Acmod::k2_0;
    bool lfe = false;
    std::uint64_t frame_count = 0;    // samples per channel, whole file
    std::uint64_t unit_count = 0;     // syncframes/access units decoded
    double duration_seconds = 0.0;
    bool has_objects = false;
    int object_count = 0;
    // Planar bed audio, one vector per channel, already reordered to
    // monitor/WAV playback order (ac3::io::wav_channel_order /
    // ac3::plan::monitor_order) - channels[i] is playback position i's
    // whole-file audio, ready to hand straight to write_wav_f32 for export.
    std::vector<std::vector<float>> channels;
    // Location per channel, parallel to `channels` above - drives
    // channelMeta()/the soundfield ring's geometry.
    std::vector<ac3::eac3::chanmap::Location> locations;
    // One mono buffer per JOC-reconstructed object, present only for an
    // Atmos E-AC-3 stream - export only (exportObjects()), never shown as a
    // QVariantList property the way the bed's channels are.
    std::vector<std::vector<float>> object_audio;
};

}  // namespace splayer_detail

// The GUI twin of `ac3cli monitor` plus `ac3cli decode [objects_dir]`: opens
// an already-encoded .ac3/.ec3 file, plays its decoded bed through an
// ordinary shared-mode output with real transport (play/pause/seek), and can
// export the decode to a WAV (and, for an Atmos stream, one WAV per
// JOC-reconstructed object) without ever going through the CLI. Reuses the
// MonitorSink plumbing ObjectDecodeController's audition_sink_ and
// EncoderController's motion-preview/live-monitor sinks already established
// (open a shared-mode sink, submit interleaved chunks from a worker thread
// with a 4ms retry while the queue is full) rather than inventing a second
// way to reach an audio device.
//
// Deliberately its own controller, not folded into ObjectDecodeController or
// EncoderController, for the reason both of their own header comments
// already give for staying separate from each other: this opens an
// ALREADY-ENCODED file with no plan, no source and no encoder involved, and
// it is neither an object-metadata report (ObjectDecodeController) nor a
// loudness measurement (QcController) - just playback and a decode-to-file
// export, the GUI's third "distinct surface, reachable from the header"
// dialog alongside QC and Inspect objects.
//
// Whole-file decode to memory, like ObjectDecodeController::inspectFile -
// not the CLI's incremental object-WAV-per-access-unit streaming write -
// because a real seek needs the samples already resident, and every other
// interactive GUI decode dialog in this window already accepts the same
// trade for the same reason. A feature-length programme is a lot of RAM this
// way; nothing here pretends otherwise.
class StreamPlayerController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // The file last handed to openFile(), even while busy decoding it or
    // after a failed attempt - so the dialog can always say what it is
    // playing.
    Q_PROPERTY(QString filePath READ filePath NOTIFY filePathChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool hasResult READ hasResult NOTIFY resultChanged)
    Q_PROPERTY(QString error READ error NOTIFY resultChanged)
    // "E-AC-3 · 3/2 + LFE · 48000 Hz · 62 frame(s) · 1.98 s"
    Q_PROPERTY(QString summaryLine READ summaryLine NOTIFY resultChanged)
    Q_PROPERTY(bool hasObjects READ hasObjects NOTIFY resultChanged)
    Q_PROPERTY(int objectCount READ objectCount NOTIFY resultChanged)
    Q_PROPERTY(double durationSeconds READ durationSeconds NOTIFY resultChanged)
    // channelMeta's directionality/ceiling split - see SoundfieldView.qml's
    // own atmosEnabled read, reused verbatim ("objects panned in" vs a bare
    // per-speaker fed caption).
    Q_PROPERTY(bool atmosEnabled READ atmosEnabled NOTIFY resultChanged)

    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(double positionSeconds READ positionSeconds NOTIFY positionChanged)

    // Same shapes as EncoderController's own channelLevels/channelMeta/
    // soundfield/meterFloorDb - see publishLevels()'s own comment - so
    // ChannelMeter.qml/SoundfieldView.qml read either controller identically
    // once given a `controller` property to address it by.
    Q_PROPERTY(QVariantList channelLevels READ channelLevels NOTIFY levelsChanged)
    Q_PROPERTY(QVariantList channelMeta READ channelMeta NOTIFY resultChanged)
    Q_PROPERTY(QVariantMap soundfield READ soundfield NOTIFY levelsChanged)
    Q_PROPERTY(double meterFloorDb READ meterFloorDb CONSTANT)

    Q_PROPERTY(bool exporting READ exporting NOTIFY exportingChanged)
    Q_PROPERTY(QString exportError READ exportError NOTIFY exportFinished)

   public:
    explicit StreamPlayerController(QObject* parent = nullptr);
    ~StreamPlayerController() override;

    [[nodiscard]] QString filePath() const { return file_path_; }
    [[nodiscard]] bool busy() const { return busy_; }
    [[nodiscard]] bool hasResult() const { return result_ != nullptr; }
    [[nodiscard]] QString error() const { return error_; }
    [[nodiscard]] QString summaryLine() const;
    [[nodiscard]] bool hasObjects() const { return result_ && result_->has_objects; }
    [[nodiscard]] int objectCount() const { return result_ ? result_->object_count : 0; }
    [[nodiscard]] double durationSeconds() const { return result_ ? result_->duration_seconds : 0.0; }
    [[nodiscard]] bool atmosEnabled() const { return result_ && result_->has_objects; }

    [[nodiscard]] bool playing() const { return playing_; }
    [[nodiscard]] double positionSeconds() const;

    [[nodiscard]] QVariantList channelLevels() const { return channel_levels_; }
    [[nodiscard]] QVariantList channelMeta() const;
    [[nodiscard]] QVariantMap soundfield() const { return soundfield_; }
    [[nodiscard]] double meterFloorDb() const;

    [[nodiscard]] bool exporting() const { return exporting_; }
    [[nodiscard]] QString exportError() const { return export_error_; }

    // Reads and decodes `url` off the GUI thread (QtConcurrent, mirroring
    // ObjectDecodeController::inspectFile/QcController::measureFile) into
    // the whole-file planar buffers above. Stops any playback of a
    // previously-open file first. Refused while already busy.
    Q_INVOKABLE void openFile(const QUrl& url);

    // Opens the monitor sink if needed and resumes from positionSeconds().
    // A no-op while a previous play()/pause() cycle's worker has not yet
    // finished tearing itself down - see the .cpp's own comment on
    // worker_active_.
    Q_INVOKABLE void play();
    // Stops submitting audio, leaving positionSeconds() where it was so a
    // later play() resumes from the same point.
    Q_INVOKABLE void pause();
    // Moves the playback position. Safe whether or not playback is active.
    Q_INVOKABLE void seek(double seconds);

    Q_INVOKABLE void clearClipLatch(int channel);

    // Writes the whole decoded bed to `url` as a float32 WAV
    // (ac3::io::write_wav_f32) - the GUI twin of `ac3cli decode`'s primary
    // output, from data already resident rather than a second decode pass.
    Q_INVOKABLE void exportDecodedWav(const QUrl& url);
    // Writes one object_NN.wav per JOC-reconstructed object into the
    // directory `url` names, the same naming `ac3cli decode`'s own
    // objects_dir writes - a no-op (with exportError set) when hasObjects()
    // is false.
    Q_INVOKABLE void exportObjects(const QUrl& url);

   signals:
    void filePathChanged();
    void busyChanged();
    void resultChanged();
    void playingChanged();
    void positionChanged();
    void levelsChanged();
    void exportingChanged();
    void exportFinished();

   private:
    // `source` is always the worker's own captured shared_ptr, never read
    // from result_ directly - see stream_player_controller.cpp's own
    // comment on why a superseded worker (openFile() loaded a second file
    // while this one was still mid-flight) must never let its OWN geometry
    // leak into whatever result_ points to by the time this runs.
    void publishLevels(const splayer_detail::RawResult& source,
                       std::span<const ac3::analysis::ChannelLevel> levels);

    QString file_path_;
    bool busy_ = false;
    QString error_;
    std::shared_ptr<const splayer_detail::RawResult> result_;

    bool playing_ = false;
    std::unique_ptr<ac3::audio::MonitorSink> sink_;
    bool worker_active_ = false;
    std::atomic<bool> should_play_{false};
    std::atomic<std::uint64_t> read_frame_{0};
    std::atomic<bool> seek_pending_{false};
    std::atomic<std::uint64_t> seek_target_{0};

    QVariantList channel_levels_;
    QVariantMap soundfield_;
    std::vector<bool> clip_latched_;

    bool exporting_ = false;
    QString export_error_;
};
