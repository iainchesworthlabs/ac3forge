#pragma once

#include <QObject>

#include <algorithm>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include "engine.hpp"

namespace ac3::desk {

// One application as the window sees it: a live object whose properties
// change in place as the engine's status changes. The rail, the room views,
// the bed tray and the 3D room all bind to these, so a level or position
// change updates the delegate that shows it rather than recreating every
// delegate (which is what a value list did, eight times a second, and what
// lost a marker mid-drag).
class AppEntry final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by DeskController")
    Q_PROPERTY(int app READ app CONSTANT)
    Q_PROPERTY(QString name READ name NOTIFY changed)
    Q_PROPERTY(QString imagePath READ imagePath NOTIFY changed)
    Q_PROPERTY(bool background READ background NOTIFY changed)
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(bool tapped READ tapped NOTIFY changed)
    Q_PROPERTY(bool fullscreen READ fullscreen NOTIFY changed)
    Q_PROPERTY(int slot READ slot NOTIFY changed)
    Q_PROPERTY(int width READ width NOTIFY changed)
    Q_PROPERTY(double size READ size NOTIFY changed)
    Q_PROPERTY(double x READ x NOTIFY changed)
    Q_PROPERTY(double y READ y NOTIFY changed)
    Q_PROPERTY(double z READ z NOTIFY changed)
    // A split pair's two objects, and whether they were placed on their own.
    Q_PROPERTY(double lx READ lx NOTIFY changed)
    Q_PROPERTY(double ly READ ly NOTIFY changed)
    Q_PROPERTY(double lz READ lz NOTIFY changed)
    Q_PROPERTY(double rx READ rx NOTIFY changed)
    Q_PROPERTY(double ry READ ry NOTIFY changed)
    Q_PROPERTY(double rz READ rz NOTIFY changed)
    Q_PROPERTY(bool pairCustom READ pairCustom NOTIFY changed)
    Q_PROPERTY(double level READ level NOTIFY levelChanged)

public:
    explicit AppEntry(int app, QObject* parent = nullptr) : QObject(parent), app_(app) {}

    // Returns true when anything other than the level changed.
    bool update(const ac3::windemo::AppStatus& s, const QString& name, bool background) {
        bool structural = false;
        auto set = [&structural](auto& field, const auto& value) {
            if (field != value) {
                field = value;
                structural = true;
            }
        };
        set(name_, name);
        set(image_path_, QString::fromStdString(s.image_path));
        set(background_, background);
        set(active_, s.active);
        set(tapped_, s.tapped);
        set(fullscreen_, s.fullscreen);
        set(slot_, s.slot ? *s.slot : -1);
        set(width_, s.width);
        set(size_, s.size);
        set(x_, s.position.x);
        set(y_, s.position.y);
        set(z_, s.position.z);
        set(lx_, s.left.x);
        set(ly_, s.left.y);
        set(lz_, s.left.z);
        set(rx_, s.right.x);
        set(ry_, s.right.y);
        set(rz_, s.right.z);
        set(pair_custom_, s.pair_custom);
        if (structural) {
            emit changed();
        }
        // Meter ballistics: a rise is shown at once, a fall at a steady
        // rate, so the bar moves continuously between polls rather than
        // stepping to each new reading.
        const double reading = static_cast<double>(s.level_dbfs);
        const double fall_per_poll = 2.4;  // dB per poll, 40 dB/s at 60 ms
        const double level = reading >= level_ ? reading : std::max(reading, level_ - fall_per_poll);
        if (level != level_) {
            level_ = level;
            emit levelChanged();
        }
        return structural;
    }

    [[nodiscard]] int app() const { return app_; }
    [[nodiscard]] QString name() const { return name_; }
    [[nodiscard]] QString imagePath() const { return image_path_; }
    [[nodiscard]] bool background() const { return background_; }
    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] bool tapped() const { return tapped_; }
    [[nodiscard]] bool fullscreen() const { return fullscreen_; }
    [[nodiscard]] int slot() const { return slot_; }
    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] double size() const { return size_; }
    [[nodiscard]] double x() const { return x_; }
    [[nodiscard]] double y() const { return y_; }
    [[nodiscard]] double z() const { return z_; }
    [[nodiscard]] double lx() const { return lx_; }
    [[nodiscard]] double ly() const { return ly_; }
    [[nodiscard]] double lz() const { return lz_; }
    [[nodiscard]] double rx() const { return rx_; }
    [[nodiscard]] double ry() const { return ry_; }
    [[nodiscard]] double rz() const { return rz_; }
    [[nodiscard]] bool pairCustom() const { return pair_custom_; }
    [[nodiscard]] double level() const { return level_; }

signals:
    void changed();
    void levelChanged();

private:
    int app_;
    QString name_;
    QString image_path_;
    bool background_ = false;
    bool active_ = false;
    bool tapped_ = false;
    bool fullscreen_ = false;
    int slot_ = -1;
    int width_ = 1;
    double size_ = 0.0;
    double x_ = 0.5;
    double y_ = 0.5;
    double z_ = 0.0;
    double lx_ = 0.5, ly_ = 0.5, lz_ = 0.0, rx_ = 0.5, ry_ = 0.5, rz_ = 0.0;
    bool pair_custom_ = false;
    double level_ = -120.0;
};

}  // namespace ac3::desk
