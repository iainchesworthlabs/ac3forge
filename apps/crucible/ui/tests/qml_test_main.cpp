#include <QtQuickTest/quicktest.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QObject>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../../gui/language_manager.hpp"
#include "default_device.hpp"
#include "session_monitor.hpp"
#include "slots.hpp"
#include "virtual_device.hpp"
#include "../app_icon_provider.hpp"
#include "../crucible_controller.hpp"
#include "fake_devices.hpp"
#include "fake_services.hpp"

// Qt Quick Test entry point for the AC3Forge Crucible's window: runs every
// tst_*.qml under QUICK_TEST_SOURCE_DIR against the REAL CrucibleController the
// embedded Ac3ForgeCrucible module registers - the same rule apps/gui/tests
// follows, and for the same reason: a parallel fake API is a second thing
// the real one can silently disagree with. The controller starts the
// engine only when a test (or Main.qml) calls start(), so suites that never
// do are hardware-free; the ones that do skip when start() refuses, which
// is what happens on a machine with no audio endpoint.
//
// The setup object mirrors the GUI harness's SettingsIsolation: real
// organisation and application names plus a QTemporaryDir settings path,
// so CrucibleController's QSettings (organisation "ac3forge", application
// "Crucible", the shipped app's own) read and write a store that is empty at
// start and gone at exit rather than the developer's own. The store is seeded
// as having seen the first-run explanation, so no suite that instantiates the
// shell meets a modal it did not ask for; tst_firstrun.qml clears the key in
// its own init(). The
// Basic style is what ui/main.cpp sets; the offscreen platform (set on the
// ctest entries) has no native theme to consult, and the QML customises
// contentItems that a native style would refuse.

// The scripted default device, kept in step with the scripted devices the
// engine probes. The two fakes answer separately - FakeDefaultDevice is the
// sound settings, FakeDevices is the enumeration behind the engine's
// endpoint table - and on a real machine a move shows in both, so a move
// made through this one rewrites the other's is_default flags too. Without
// it the Signal path table would say applications still play on the old
// default after "Send applications here", which no machine does.
class LinkedDefaultDevice final : public ac3::crucible::DefaultDevice {
public:
    LinkedDefaultDevice(std::shared_ptr<ac3::crucible::testing::FakeDefaultDevice> settings,
                        std::shared_ptr<ac3::crucible::testing::FakeDevices> devices)
        : settings_(std::move(settings)), devices_(std::move(devices)) {}

    std::vector<ac3::crucible::RenderEndpoint> endpoints() override { return settings_->endpoints(); }
    std::string default_id() override { return settings_->default_id(); }
    std::expected<void, std::string> set_default(std::string_view endpoint_id) override {
        auto moved = settings_->set_default(endpoint_id);
        if (moved) {
            const std::lock_guard lock(devices_->mutex);
            for (auto& facts : devices_->devices) {
                facts.is_default = facts.id == endpoint_id;
            }
        }
        return moved;
    }
    bool moves_default() const override { return settings_->moves_default(); }
    std::string find_endpoint(std::string_view name_substring) override {
        return settings_->find_endpoint(name_substring);
    }
    void open_sound_settings() override { settings_->open_sound_settings(); }

private:
    std::shared_ptr<ac3::crucible::testing::FakeDefaultDevice> settings_;
    std::shared_ptr<ac3::crucible::testing::FakeDevices> devices_;
};

// A silent device this application makes itself, the way the Linux arm
// does: absent until install() creates it, which adds it to both fakes'
// endpoint lists, and gone again after remove(). Each action finishes at
// once with exit code 0, so the controller's action poll reports it on its
// next tick. What the "Create device" and "Remove device" buttons, and a
// Send that has to create the device first, are driven against.
class ScriptedSilentDevice final : public ac3::crucible::VirtualDevice {
public:
    ScriptedSilentDevice(std::shared_ptr<ac3::crucible::testing::FakeDefaultDevice> settings,
                         std::shared_ptr<ac3::crucible::testing::FakeDevices> devices)
        : settings_(std::move(settings)), devices_(std::move(devices)) {}

    std::string device_name() const override { return "Desktop Atmos"; }
    std::string how_to_get_one() const override { return "this application creates it"; }
    ac3::crucible::SilentDeviceState state(const ac3::crucible::SilentDeviceQuery& query) override {
        return {.needed = true,
                .present = query.endpoint_present,
                .in_use = query.endpoint_is_default,
                .can_install = true,
                .blocker = {},
                .detail = {"scripted: the application makes its own silent device"}};
    }
    std::expected<void, std::string> install() override {
        const auto facts = ac3::crucible::testing::null_sink();
        auto endpoints = settings_->endpoints();
        endpoints.push_back({.id = facts.id, .name = facts.name, .is_default = false});
        settings_->set_endpoints(std::move(endpoints));
        {
            const std::lock_guard lock(devices_->mutex);
            devices_->devices.push_back(facts);
        }
        last_ = {.running = false, .exit_code = 0, .log_tail = {"created Desktop Atmos"}};
        return {};
    }
    std::expected<void, std::string> remove() override {
        const auto id = ac3::crucible::testing::null_sink().id;
        auto endpoints = settings_->endpoints();
        std::erase_if(endpoints, [&](const auto& e) { return e.id == id; });
        settings_->set_endpoints(std::move(endpoints));
        {
            const std::lock_guard lock(devices_->mutex);
            std::erase_if(devices_->devices, [&](const auto& e) { return e.id == id; });
        }
        last_ = {.running = false, .exit_code = 0, .log_tail = {"removed Desktop Atmos"}};
        return {};
    }
    ac3::crucible::DeviceActionStatus action_status() override { return last_; }

private:
    std::shared_ptr<ac3::crucible::testing::FakeDefaultDevice> settings_;
    std::shared_ptr<ac3::crucible::testing::FakeDevices> devices_;
    ac3::crucible::DeviceActionStatus last_;
};

// The machine, scripted: the five platform seams replaced by the same fakes
// the engine's Catch2 cases use (tests/crucible/fake_services.hpp,
// fake_devices.hpp), so a suite can say "there are two applications with
// sound and one stereo endpoint" and then drive the real controller and the
// real engine over that.
//
// A suite that never calls scriptSessions() or scriptMachineWithNoOutput()
// sees the machine, exactly as before: this changes nothing for the suites
// that were written against it. One process per suite (one ctest entry
// each), so a scripted room in one cannot reach another.
class TestServices : public QObject {
    Q_OBJECT

public:
    explicit TestServices(QQmlEngine& engine) : engine_(&engine) {}

    // [{ app: 900, name: "Chrome", active: true }, ...]. False when the
    // controller singleton cannot be reached, so a suite can skip rather
    // than fail on a harness that did not register it. Optional per-entry
    // fields: `window` (false: a background process), `session` (false: a
    // running application with no audio session, listed greyed).
    Q_INVOKABLE bool scriptSessions(const QVariantList& apps) {
        // One real endpoint and one silent device, which is the least a
        // start() needs to choose an output and open a sink.
        return script(apps, {ac3::crucible::testing::realtek_default(),
                             ac3::crucible::testing::null_sink()});
    }

    // The same, over a chosen set of endpoints by name: "avr" (an HDMI
    // receiver taking E-AC-3 and AC-3), "realtek" (the stereo default),
    // "null" (the silent device), "headphones" (a spatial endpoint). What
    // the Signal path suites need to see a pin change the mode the engine
    // settles on, which one stereo endpoint cannot show.
    Q_INVOKABLE bool scriptMachine(const QVariantList& apps, const QStringList& endpoints) {
        std::vector<ac3::crucible::DeviceFacts> facts;
        for (const QString& name : endpoints) {
            if (name == QLatin1String("avr")) {
                facts.push_back(ac3::crucible::testing::hdmi_avr());
            } else if (name == QLatin1String("realtek")) {
                facts.push_back(ac3::crucible::testing::realtek_default());
            } else if (name == QLatin1String("null")) {
                facts.push_back(ac3::crucible::testing::null_sink());
            } else if (name == QLatin1String("headphones")) {
                facts.push_back(ac3::crucible::testing::headphones_spatial());
            } else {
                return false;
            }
        }
        return script(apps, facts);
    }

    // The machine with nothing to play into. start() refuses on this,
    // which is the branch every engine-driving suite has a skip for and no
    // real machine here produces to order: a developer's box has endpoints,
    // and whether a CI runner does is not a thing a test should rest on.
    Q_INVOKABLE bool scriptMachineWithNoOutput() { return script({}, {}); }

    // A receiver and a stereo default, and no silent device yet - one this
    // application can make itself (ScriptedSilentDevice above).
    Q_INVOKABLE bool scriptMachineThatMakesItsSilentDevice(const QVariantList& apps) {
        return script(apps, {ac3::crucible::testing::hdmi_avr(), ac3::crucible::testing::realtek_default()},
                      /*makes_its_own=*/true);
    }

    // A scripted room changed while the engine runs: an application
    // arriving or leaving is the next refresh's answer.
    Q_INVOKABLE bool setSessions(const QVariantList& apps) {
        if (!sessions_) {
            return false;
        }
        sessions_->set_apps(to_sessions(apps));
        return true;
    }

    // The application (by id) that is full-screen now, or 0 for none.
    Q_INVOKABLE bool setFullscreen(int app) {
        if (!foreground_) {
            return false;
        }
        foreground_->set_fullscreen_pid(app > 0 ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(app))
                                                : std::nullopt);
        return true;
    }

    // How many times the scripted machine was asked to open its sound
    // settings: what "Open Sound settings" and a refused move do.
    Q_INVOKABLE int soundSettingsOpened() const {
        return default_device_ ? static_cast<int>(default_device_->settings_opened()) : -1;
    }

    // Makes the scripted default device refuse every move from now on,
    // the way the platform's policy can.
    Q_INVOKABLE bool refuseDefaultMoves(const QString& reason) {
        if (!default_device_) {
            return false;
        }
        default_device_->refuse_set_default(reason.toStdString());
        return true;
    }

    // A key as the isolated settings store has it on disk, read through a
    // QSettings of its own rather than the controller's: what the next
    // launch would read. Invalid when the key is absent.
    Q_INVOKABLE QVariant storedSetting(const QString& key) const {
        const QSettings store(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("ac3forge"),
                              QStringLiteral("Crucible"));
        return store.value(key);
    }

    // The machine back. Every scripted suite calls this in cleanup(),
    // because two of the five seams are held by the controller rather than
    // handed to the engine at start(): a case that ran after a scripted one
    // would otherwise read the fake default device and the fake silent
    // device, and CONSTANT properties over them would answer from before the
    // swap. A no-op when nothing was scripted, so an unscripted case pays
    // nothing for it.
    Q_INVOKABLE bool clear() {
        if (!scripted_) {
            return true;
        }
        auto* controller = find_controller();
        if (controller == nullptr) {
            return false;
        }
        controller->set_test_services(nullptr, nullptr, nullptr, nullptr, nullptr);
        scripted_ = false;
        sessions_.reset();
        foreground_.reset();
        default_device_.reset();
        return true;
    }

private:
    static std::vector<ac3::crucible::AppSession> to_sessions(const QVariantList& apps) {
        std::vector<ac3::crucible::AppSession> listed;
        listed.reserve(static_cast<std::size_t>(apps.size()));
        for (const QVariant& entry : apps) {
            const QVariantMap fields = entry.toMap();
            ac3::crucible::AppSession session;
            session.app = static_cast<ac3::crucible::AppId>(fields.value(QStringLiteral("app")).toUInt());
            session.name = fields.value(QStringLiteral("name")).toString().toStdString();
            session.active = fields.value(QStringLiteral("active"), true).toBool();
            session.has_window = fields.value(QStringLiteral("window"), true).toBool();
            session.has_session = fields.value(QStringLiteral("session"), true).toBool();
            session.session_pids.push_back(session.app);
            listed.push_back(std::move(session));
        }
        return listed;
    }

    bool script(const QVariantList& apps, const std::vector<ac3::crucible::DeviceFacts>& endpoints,
                bool makes_its_own = false) {
        auto* controller = find_controller();
        if (controller == nullptr) {
            return false;
        }
        auto sessions = std::make_shared<ac3::crucible::testing::FakeSessionMonitor>();
        sessions->set_apps(to_sessions(apps));

        auto devices = std::make_shared<ac3::crucible::testing::FakeDevices>();
        devices->devices = endpoints;

        auto foreground = std::make_shared<ac3::crucible::testing::FakeForeground>();

        // The sound settings agree with what the engine can see, so a
        // machine with no output has an empty endpoint list there too.
        auto default_device = std::make_shared<ac3::crucible::testing::FakeDefaultDevice>();
        std::vector<ac3::crucible::RenderEndpoint> in_settings;
        in_settings.reserve(endpoints.size());
        for (const auto& endpoint : endpoints) {
            in_settings.push_back({.id = endpoint.id, .name = endpoint.name, .is_default = endpoint.is_default});
        }
        default_device->set_endpoints(std::move(in_settings));

        std::shared_ptr<ac3::crucible::VirtualDevice> virtual_device;
        if (makes_its_own) {
            virtual_device = std::make_shared<ScriptedSilentDevice>(default_device, devices);
        } else {
            auto fake = std::make_shared<ac3::crucible::testing::FakeVirtualDevice>();
            fake->set_device_name("Desktop Atmos");
            fake->set_state({.needed = true,
                             .present = true,
                             .in_use = false,
                             .can_install = false,
                             .blocker = {},
                             .detail = {}});
            virtual_device = std::move(fake);
        }

        sessions_ = sessions;
        foreground_ = foreground;
        default_device_ = default_device;
        auto linked = std::make_shared<LinkedDefaultDevice>(default_device, devices);
        controller->set_test_services(std::move(sessions), std::move(devices), std::move(foreground),
                                      std::move(linked), std::move(virtual_device));
        scripted_ = true;
        return true;
    }

    [[nodiscard]] CrucibleController* find_controller() const {
        return engine_->singletonInstance<CrucibleController*>(QStringLiteral("Ac3ForgeCrucible"),
                                                               QStringLiteral("CrucibleController"));
    }

    QQmlEngine* engine_ = nullptr;
    bool scripted_ = false;
    // The scripted machine's seams, kept so a case can change them while
    // the engine runs over them.
    std::shared_ptr<ac3::crucible::testing::FakeSessionMonitor> sessions_;
    std::shared_ptr<ac3::crucible::testing::FakeForeground> foreground_;
    std::shared_ptr<ac3::crucible::testing::FakeDefaultDevice> default_device_;
};

// The isolation described above: settings, style, language and the icon
// provider, plus the scripted machine registered for the suites that ask.
class DeskIsolation : public QObject {
    Q_OBJECT

public slots:
    void applicationAvailable() {
        QCoreApplication::setOrganizationName(QStringLiteral("ac3forge"));
        QCoreApplication::setApplicationName(QStringLiteral("Crucible"));
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        scratch_.emplace();
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, scratch_->path());
        QSettings seed(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("ac3forge"),
                       QStringLiteral("Crucible"));
        seed.setValue(QStringLiteral("firstRun/acknowledgedVersion"), kFirstRunVersion);
        seed.sync();
    }

    // Once per tst_*.qml: the same LanguageManager singleton ui/main.cpp
    // registers, under the same URI, pointed at this app's translations.
    void qmlEngineAvailable(QQmlEngine* engine) {
        language_manager_.emplace(*qGuiApp, *engine, QStringLiteral("ac3crucible"));
        language_manager_->applyInitialLanguage();
        qmlRegisterSingletonInstance("Ac3ForgeCrucibleLanguage", 1, 0, "LanguageManager", &*language_manager_);
        // The same appicon image provider ui/main.cpp registers, so an
        // AppIcon under test reaches the platform's provider (tst_icons.qml)
        // rather than Image.Error for every id. The engine owns it.
        engine->addImageProvider(QStringLiteral("appicon"), new ac3::crucible::ui::AppIconProvider);
        // The scripted machine, under its own URI so nothing the window
        // itself imports can reach it.
        test_services_.emplace(*engine);
        qmlRegisterSingletonInstance("Ac3ForgeCrucibleTest", 1, 0, "TestServices", &*test_services_);
    }

private:
    std::optional<QTemporaryDir> scratch_;
    std::optional<LanguageManager> language_manager_;
    std::optional<TestServices> test_services_;
};

QUICK_TEST_MAIN_WITH_SETUP(ac3crucible, DeskIsolation)

#include "qml_test_main.moc"
