#include <QtQuickTest/quicktest.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QObject>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
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

// The machine, scripted: the five platform seams replaced by the same fakes
// the engine's Catch2 cases use (tests/crucible/fake_services.hpp,
// fake_devices.hpp), so a suite can say "there are two applications with
// sound and one stereo endpoint" and then drive the real controller and the
// real engine over that.
//
// A suite that never calls scriptSessions() sees the machine, exactly as
// before: this changes nothing for the suites that were written against it.
// One process per suite (one ctest entry each), so a scripted room in one
// cannot reach another.
class TestServices : public QObject {
    Q_OBJECT

public:
    explicit TestServices(QQmlEngine& engine) : engine_(&engine) {}

    // [{ app: 900, name: "Chrome", active: true }, ...]. False when the
    // controller singleton cannot be reached, so a suite can skip rather
    // than fail on a harness that did not register it.
    Q_INVOKABLE bool scriptSessions(const QVariantList& apps) {
        auto* controller = find_controller();
        if (controller == nullptr) {
            return false;
        }
        auto sessions = std::make_shared<ac3::crucible::testing::FakeSessionMonitor>();
        std::vector<ac3::crucible::AppSession> listed;
        listed.reserve(static_cast<std::size_t>(apps.size()));
        for (const QVariant& entry : apps) {
            const QVariantMap fields = entry.toMap();
            ac3::crucible::AppSession session;
            session.app = static_cast<ac3::crucible::AppId>(fields.value(QStringLiteral("app")).toUInt());
            session.name = fields.value(QStringLiteral("name")).toString().toStdString();
            session.active = fields.value(QStringLiteral("active"), true).toBool();
            session.has_window = true;
            session.has_session = true;
            session.session_pids.push_back(session.app);
            listed.push_back(std::move(session));
        }
        sessions->set_apps(std::move(listed));

        // One real endpoint and one silent device, which is the least a
        // start() needs to choose an output and open a sink.
        auto devices = std::make_shared<ac3::crucible::testing::FakeDevices>();
        devices->devices = {ac3::crucible::testing::realtek_default(),
                            ac3::crucible::testing::null_sink()};

        auto foreground = std::make_shared<ac3::crucible::testing::FakeForeground>();

        auto default_device = std::make_shared<ac3::crucible::testing::FakeDefaultDevice>();
        default_device->set_endpoints({{.id = "realtek", .name = "Speakers (Realtek)", .is_default = true},
                                       {.id = "null", .name = "Speakers (Desktop Atmos)", .is_default = false}});

        auto virtual_device = std::make_shared<ac3::crucible::testing::FakeVirtualDevice>();
        virtual_device->set_device_name("Desktop Atmos");
        virtual_device->set_state({.needed = true,
                                   .present = true,
                                   .in_use = false,
                                   .can_install = false,
                                   .blocker = {},
                                   .detail = {}});

        controller->set_test_services(std::move(sessions), std::move(devices), std::move(foreground),
                                      std::move(default_device), std::move(virtual_device));
        scripted_ = true;
        return true;
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
        return true;
    }

private:
    [[nodiscard]] CrucibleController* find_controller() const {
        return engine_->singletonInstance<CrucibleController*>(QStringLiteral("Ac3ForgeCrucible"),
                                                               QStringLiteral("CrucibleController"));
    }

    QQmlEngine* engine_ = nullptr;
    bool scripted_ = false;
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
