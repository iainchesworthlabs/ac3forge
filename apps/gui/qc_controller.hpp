#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QtQmlIntegration>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "background_jobs.hpp"

// The raw measurement, in the units the library reports them - kept OUTSIDE
// QcController itself so the measurement helpers in qc_controller.cpp (an
// anonymous namespace mirroring forge's own measure_qc_ac3/measure_qc_eac3,
// see that file's header comment) can build one without needing member
// access. Presentation (formatting, delta-against-a-preset) happens only in
// QcController::programmes() itself - the same "store the fact, compute the
// display" split every other *Meta/*Model getter in this app follows.
//
// One instance per programme measured - one for every layout except 1+1 dual
// mono, which is two (Ch1, Ch2): §E1.3 makes them unrelated, unmixed
// programmes sharing one syncframe rather than a single soundfield BS.1770
// could measure as one, the same reason apps/cli/main.cpp's own
// QcProgrammeResult exists in this same shape. Every field carries its own
// default member initializer for the same -Wmissing-field-initializers
// reason that type's own header comment gives - every construction here is a
// partial designated initializer too.
namespace qc_detail {

struct RawProgramme {
    QString label = QString();
    std::optional<double> integrated_lkfs = std::nullopt;
    std::optional<double> lra_lu = std::nullopt;
    std::optional<double> true_peak_dbtp = std::nullopt;
    int dialnorm = 31;
    std::optional<std::uint8_t> compr = std::nullopt;
    // AC-4 (ETSI TS 103 190-1 clause 4.3.12): its dialnorm, 0 to -31.75 dBFS
    // in steps of 0.25, in place of `dialnorm` and `compr`, and the integrated
    // loudness the stream states where it sends one - what `forge qc` reports
    // for AC-4.
    bool ac4 = false;
    std::optional<double> ac4_dialnorm_dbfs = std::nullopt;
    std::optional<double> stated_lkfs = std::nullopt;
};

struct RawResult {
    QString codec_label = QString();
    QString layout_label = QString();
    QString unit_label = QString();
    std::uint32_t sample_rate_hz = 0;
    std::size_t unit_count = 0;
    double seconds = 0.0;
    std::vector<RawProgramme> programmes = {};
    // AC-4: the table of contents' presentations, each by the label
    // forge_gui::ac4_presentation_label gives it, and the one measured.
    bool ac4 = false;
    QStringList presentations = {};
    std::size_t presentation = 0;
};

}  // namespace qc_detail

// The QObject facade for GUI QC verification — "the same QC verification in the GUI".
//
// This is deliberately NOT a member of EncoderController. Every other panel
// in this app configures and runs an ENCODE: a source is loaded, a plan is
// built, the plan is applied. QC is the opposite shape entirely — an
// ALREADY-ENCODED file the user already has is opened, decoded and measured
// against its own embedded claims, with no plan, no source and no encoder
// involved anywhere in the path. Folding this into EncoderController would
// mean every one of its 700-odd lines of encode-workflow state carries a
// second, unrelated "what does this arbitrary file already contain" concern
// alongside it - see encoder_controller.hpp's own header comment on why
// nothing here should disagree with what forge would say, which is exactly
// the property a bolted-on second concern risks breaking first. QcController
// mirrors `forge qc` (apps/cli/main.cpp's run_qc/measure_qc_ac3/
// measure_qc_eac3) instead: reads a file, decodes it with the same
// iclforge::ac3::FrameDecoder/iclforge::ac3::Eac3Decoder EncoderController's own monitor path
// already uses, measures it with iclforge::ac3::meta::LoudnessMeter (the same meter
// `dialnorm=auto` uses) and reports it against iclforge::ac3::meta::qc's three named
// delivery presets. See docs/forge/gui/qc.md for where this surfaces in the
// window and why.
class QcController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // The file last handed to measureFile(), even while busy measuring it or
    // after a failed measurement - so the dialog can always say what it is
    // reporting on (or attempted to).
    Q_PROPERTY(QString filePath READ filePath NOTIFY filePathChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // True only once a measurement has actually completed successfully -
    // false both before the first run and after a failed one, so the report
    // area and the error text are never shown at the same time.
    Q_PROPERTY(bool hasResult READ hasResult NOTIFY resultChanged)
    // Empty when the last attempt (if any) succeeded or nothing has been
    // attempted yet.
    Q_PROPERTY(QString error READ error NOTIFY resultChanged)
    // "AC-3 · 5.1 · 48000 Hz · 500 frame(s) · 16.00 s" - run_qc's own opening
    // line, restated for the report header.
    Q_PROPERTY(QString summaryLine READ summaryLine NOTIFY resultChanged)
    // One entry per programme measured - one for every layout except 1+1
    // dual mono, which is two (Ch1, Ch2), the same split QcProgrammeResult
    // draws in the CLI and for the same reason (§E1.3: two unrelated
    // programmes sharing a syncframe). Each entry is
    // {label, hasLoudness, integratedLkfs, hasLra, lra, hasTruePeak,
    //  truePeakDbtp, dialnorm, claimedLkfs, deltaDb, impliedDialnorm,
    //  dialnormMatches, hasCompr, comprDb, presets}. `presets` is a list of
    // {id, name, source, targetLkfs, toleranceLu, loudnessIsCeiling,
    //  maxTruePeakDbtp, loudnessDelta, loudnessPass, truePeakMargin,
    //  truePeakPass, pass} - every preset when presetIndex is 0 ("All
    // presets"), exactly one otherwise - mirroring forge qc's own
    // preset=<name>|all split, see presetIndex's own comment.
    Q_PROPERTY(QVariantList programmes READ programmes NOTIFY resultChanged)
    // "All presets", then every named delivery gate in
    // iclforge::ac3::meta::kQcPresetIds order - index 0 is the "preset=all" concept and
    // 1..kQcPresetIds.size() select one, so this list grows with that table
    // rather than with a count repeated here.
    Q_PROPERTY(QStringList presetNames READ presetNames CONSTANT)
    Q_PROPERTY(int presetIndex READ presetIndex WRITE setPresetIndex NOTIFY presetChanged)
    // AC-4: whether the file measured is AC-4, its presentations, and which
    // one to measure - `forge qc presentation=<n>`'s n, or -1 for the one the
    // decoder chooses with no preference, which is qc's default. Setting it
    // measures the file again.
    Q_PROPERTY(bool isAc4 READ isAc4 NOTIFY resultChanged)
    Q_PROPERTY(QStringList presentationNames READ presentationNames NOTIFY resultChanged)
    Q_PROPERTY(int presentationIndex READ presentationIndex WRITE setPresentationIndex NOTIFY
                   presentationChanged)
    // The presentation the last measurement decoded, by position; -1 before one.
    Q_PROPERTY(int measuredPresentation READ measuredPresentation NOTIFY resultChanged)

   public:
    explicit QcController(QObject* parent = nullptr);
    // Waits for a measurement still running - see background_jobs.hpp.
    ~QcController() override;

    [[nodiscard]] QString filePath() const { return file_path_; }
    [[nodiscard]] bool busy() const { return busy_; }
    [[nodiscard]] bool hasResult() const { return result_.has_value(); }
    [[nodiscard]] QString error() const { return error_; }
    [[nodiscard]] QString summaryLine() const;
    [[nodiscard]] QVariantList programmes() const;
    [[nodiscard]] QStringList presetNames() const;
    [[nodiscard]] int presetIndex() const { return preset_index_; }
    void setPresetIndex(int index);
    [[nodiscard]] bool isAc4() const { return result_ && result_->ac4; }
    [[nodiscard]] QStringList presentationNames() const {
        return result_ ? result_->presentations : QStringList{};
    }
    [[nodiscard]] int presentationIndex() const { return presentation_index_; }
    void setPresentationIndex(int index);
    [[nodiscard]] int measuredPresentation() const {
        return result_ && result_->ac4 ? static_cast<int>(result_->presentation) : -1;
    }

    // Reads `url`, decodes it as AC-4 (by its sync word) or as AC-3 or E-AC-3
    // (by bsid, same dispatch run_qc uses) and measures it - off the GUI
    // thread via QtConcurrent, the same worker pattern every one of
    // EncoderController's own encode/decode paths already uses, since a long
    // stream's decode-and-measure pass is
    // exactly the kind of work that must not stall the window. Refused
    // (silently, the app's usual convention for a start-a-thing entry point)
    // while already busy.
    Q_INVOKABLE void measureFile(const QUrl& url);

   signals:
    void filePathChanged();
    void busyChanged();
    void resultChanged();
    void presetChanged();
    void presentationChanged();

   private:
    void measurePath(const QString& path);

    QString file_path_;
    bool busy_ = false;
    QString error_;
    std::optional<qc_detail::RawResult> result_;
    int preset_index_ = 0;
    int presentation_index_ = -1;

    // Where the measurement runs. ~QcController() waits for it before any
    // member above is destroyed; declared last, so ~BackgroundJobs() would
    // wait ahead of them too.
    forge_gui::BackgroundJobs jobs_;
};
