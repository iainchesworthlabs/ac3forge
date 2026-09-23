#include "ac3/sendspin/firewall.hpp"

#include <windows.h>
// windows.h must precede these.
#include <netfw.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <charconv>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Registers a per-executable, per-port inbound rule through INetFwPolicy2, the same COM policy
// object Windows' own Control Panel/Settings firewall pages edit, so a rule this adds looks and
// behaves exactly like one the user clicked "Allow access" to add themselves.
//
// Adding a rule needs an elevated token (it is the same privilege the Control Panel page itself
// demands before its own "Change settings" unlocks), which an ordinary desktop launch of Hearth
// or the test binaries does not have. maybe_run_as_firewall_helper_and_exit() is the other half
// of how ensure_inbound_rule() gets one anyway: relaunch this same executable with a UAC prompt
// for just long enough to make the one COM call that needs it, then let the original,
// unelevated process carry on. Every run after the first finds the rule already there and never
// needs any of this.

namespace ac3::sendspin::firewall {

namespace {

using Microsoft::WRL::ComPtr;

// COM lifetime for one thread, the same shape as ac3::windows_audio::ComScope
// (src/audio/src/backend/windows/windows_support.hpp) but not shared with it: sendspin does not
// otherwise depend on ac3::audio, and this is eight lines.
class ComScope {
   public:
    ComScope() : hr_(::CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(hr_)) {
            ::CoUninitialize();
        }
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    [[nodiscard]] bool ok() const { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }

   private:
    HRESULT hr_;
};

// Spelled out rather than taken from __uuidof, for the same reason windows_support.hpp's own
// CLSID/IID constants are: the SDK declares these but ships no import library that defines them
// as linkable symbols, and __uuidof is an MSVC extension clang rejects under -Wpedantic. Values
// are the DECLSPEC_UUID/MIDL_INTERFACE strings in netfw.h (Windows SDK 10.0.26100.0).
constexpr CLSID kClsidNetFwPolicy2 = {  // {e2b3c97f-6ae1-41ac-817a-f6f92166d7dd}
    0xe2b3c97f, 0x6ae1, 0x41ac, {0x81, 0x7a, 0xf6, 0xf9, 0x21, 0x66, 0xd7, 0xdd}};
constexpr IID kIidNetFwPolicy2 = {  // {98325047-c671-4174-8d81-defcd3f03186}
    0x98325047, 0xc671, 0x4174, {0x8d, 0x81, 0xde, 0xfc, 0xd3, 0xf0, 0x31, 0x86}};
constexpr CLSID kClsidNetFwRule = {  // {2c5bc43e-3369-4c33-ab0c-be9469677af4}
    0x2c5bc43e, 0x3369, 0x4c33, {0xab, 0x0c, 0xbe, 0x94, 0x69, 0x67, 0x7a, 0xf4}};
constexpr IID kIidNetFwRule = {  // {af230d27-baba-4e42-aced-f524f22cfce2}
    0xaf230d27, 0xbaba, 0x4e42, {0xac, 0xed, 0xf5, 0x24, 0xf2, 0x2c, 0xfc, 0xe2}};

// The argv this relaunches its own executable with; maybe_run_as_firewall_helper_and_exit()
// looks for the same literal. Not a user-facing flag - never documented in any --help text - so
// it is prefixed to stay out of the way of one that might be added later.
constexpr std::string_view kHelperFlag = "--ac3-sendspin-firewall-helper";
constexpr DWORD kElevatedWaitMs = 60'000;

// A BSTR's owner. INetFwRule's setters all take one, and none of them keep it past the call, so
// each is freed as soon as the setter that used it returns.
class Bstr {
   public:
    explicit Bstr(const std::wstring& text) : value_(::SysAllocString(text.c_str())) {}
    ~Bstr() {
        if (value_ != nullptr) {
            ::SysFreeString(value_);
        }
    }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    [[nodiscard]] BSTR get() const { return value_; }
    [[nodiscard]] explicit operator bool() const { return value_ != nullptr; }

   private:
    BSTR value_;
};

[[nodiscard]] std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), needed);
    return result;
}

// GetModuleFileNameW's own doubling-buffer idiom: a length strictly under the buffer's size
// means the path fit; anything else (including the truncated-but-still-nul-terminated case some
// Windows versions return) means try again bigger, up to a generous ceiling no real install path
// should ever reach.
[[nodiscard]] std::optional<std::wstring> own_executable_path() {
    std::vector<wchar_t> buffer(MAX_PATH);
    while (buffer.size() <= 32'768) {
        const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::nullopt;
        }
        if (length < buffer.size() - 1) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(buffer.size() * 2);
    }
    return std::nullopt;
}

[[nodiscard]] std::wstring executable_stem(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    const std::wstring filename = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    const std::size_t dot = filename.find_last_of(L'.');
    return (dot == std::wstring::npos) ? filename : filename.substr(0, dot);
}

[[nodiscard]] bool is_elevated() {
    HANDLE token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == 0) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const BOOL ok = ::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    ::CloseHandle(token);
    return ok != 0 && elevation.TokenIsElevated != 0;
}

// Session 0 (a service) and any other window station with no visible desktop cannot show the
// elevation consent prompt ShellExecuteEx's "runas" verb needs, and would otherwise leave the
// caller waiting out the full kElevatedWaitMs for a dialog nobody can ever see or answer. A CI
// runner is ordinarily session 0 too, which is the behaviour this exists to give it.
[[nodiscard]] bool has_interactive_session() {
    const HWINSTA station = ::GetProcessWindowStation();
    if (station == nullptr) {
        return false;
    }
    USEROBJECTFLAGS flags{};
    DWORD needed = 0;
    if (::GetUserObjectInformationW(station, UOI_FLAGS, &flags, sizeof(flags), &needed) == 0) {
        return false;
    }
    return (flags.dwFlags & WSF_VISIBLE) != 0;
}

[[nodiscard]] LONG protocol_number(Protocol protocol) {
    return protocol == Protocol::kTcp ? NET_FW_IP_PROTOCOL_TCP : NET_FW_IP_PROTOCOL_UDP;
}

[[nodiscard]] std::wstring_view protocol_word(Protocol protocol) { return protocol == Protocol::kTcp ? L"TCP" : L"UDP"; }

// Distinguishes ac3hearth's rule from ac3hearth-testsink's: both link ac3::sendspin and may ask
// for "the same" named rule (mdns.cpp's is literally identical text from both), but a rule is
// only useful if it is scoped to the executable that is actually going to hold the socket open,
// and INetFwRules::Item() looks a rule up by name alone. Folding the executable's own name in
// here is what keeps the two from shadowing each other: whichever asks first would otherwise
// leave the other believing it already has a rule that in fact only covers its sibling.
[[nodiscard]] std::wstring build_rule_name(const RuleSpec& spec, const std::wstring& exe_stem) {
    return widen(spec.name) + L" (" + exe_stem + L")";
}

[[nodiscard]] std::wstring build_description(const RuleSpec& spec) {
    return L"Inbound rule for " + widen(spec.name) + L" [" + std::wstring(protocol_word(spec.protocol)) + L" " +
           std::to_wstring(spec.port) + L"]";
}

[[nodiscard]] std::optional<ComPtr<INetFwRules>> open_rules() {
    ComPtr<INetFwPolicy2> policy;
    if (FAILED(::CoCreateInstance(kClsidNetFwPolicy2, nullptr, CLSCTX_INPROC_SERVER, kIidNetFwPolicy2, &policy))) {
        return std::nullopt;
    }
    ComPtr<INetFwRules> rules;
    if (FAILED(policy->get_Rules(&rules))) {
        return std::nullopt;
    }
    return rules;
}

// Both the passive path (already elevated) and the relaunched helper end up here: the one place
// that actually calls INetFwRules::Add(), with the same shape of rule either way. Profiles are
// PRIVATE | DOMAIN only, never PUBLIC, so an elevated run on an untrusted network (a laptop at a
// coffee shop) does not open the socket to it - the discovery/streaming this protects is
// LAN-only by nature, and RemoteAddresses is narrowed to match: "LocalSubnet", not "*".
[[nodiscard]] bool add_rule_now(INetFwRules* rules, const RuleSpec& spec, const std::wstring& rule_name,
                                const std::wstring& exe_path) {
    ComPtr<INetFwRule> rule;
    if (FAILED(::CoCreateInstance(kClsidNetFwRule, nullptr, CLSCTX_INPROC_SERVER, kIidNetFwRule, &rule))) {
        return false;
    }
    const Bstr name(rule_name);
    const Bstr description(build_description(spec));
    const Bstr application(exe_path);
    const Bstr ports(std::to_wstring(spec.port));
    const Bstr remote(L"LocalSubnet");
    if (!name || !description || !application || !ports || !remote) {
        return false;
    }
    rule->put_Name(name.get());
    rule->put_Description(description.get());
    rule->put_ApplicationName(application.get());
    rule->put_Protocol(protocol_number(spec.protocol));
    rule->put_LocalPorts(ports.get());
    rule->put_RemoteAddresses(remote.get());
    rule->put_Direction(NET_FW_RULE_DIR_IN);
    rule->put_Action(NET_FW_ACTION_ALLOW);
    rule->put_Enabled(VARIANT_TRUE);
    rule->put_Profiles(NET_FW_PROFILE2_PRIVATE | NET_FW_PROFILE2_DOMAIN);
    return SUCCEEDED(rules->Add(rule.Get()));
}

// One argv token for spec.name, quoted for ShellExecuteExW's lpParameters (a single string the
// new process's C runtime re-splits the same way a typed command line would be, so a name with
// spaces has to come back as one token). spec.name is this module's own call sites, never
// outside input, so the only thing asked of it is RuleSpec's own contract: no embedded quote.
[[nodiscard]] std::wstring build_helper_parameters(const RuleSpec& spec) {
    std::wstring parameters(widen(kHelperFlag));
    parameters += L" \"";
    parameters += widen(spec.name);
    parameters += L"\" ";
    parameters += protocol_word(spec.protocol);
    parameters += L' ';
    parameters += std::to_wstring(spec.port);
    return parameters;
}

// Spawns this same executable elevated, with the one rule to add on its command line, and waits
// for it to finish. A no on the consent prompt surfaces here as ShellExecuteExW itself failing
// with ERROR_CANCELLED - not a crash, just the same outcome as not asking: the caller falls back
// to leaving it for Windows' own prompt.
[[nodiscard]] bool relaunch_elevated_and_wait(const std::wstring& exe_path, const RuleSpec& spec) {
    const std::wstring parameters = build_helper_parameters(spec);

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = exe_path.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_HIDE;

    if (::ShellExecuteExW(&info) == 0 || info.hProcess == nullptr) {
        return false;
    }
    const DWORD wait_result = ::WaitForSingleObject(info.hProcess, kElevatedWaitMs);
    DWORD exit_code = 1;
    if (wait_result == WAIT_OBJECT_0) {
        ::GetExitCodeProcess(info.hProcess, &exit_code);
    }
    ::CloseHandle(info.hProcess);
    return wait_result == WAIT_OBJECT_0 && exit_code == 0;
}

}  // namespace

bool ensure_inbound_rule(const RuleSpec& spec) {
    const ComScope com;
    if (!com.ok()) {
        return false;
    }
    const std::optional<std::wstring> exe_path = own_executable_path();
    if (!exe_path) {
        return false;
    }
    const std::wstring rule_name = build_rule_name(spec, executable_stem(*exe_path));
    const Bstr name_bstr(rule_name);
    if (!name_bstr) {
        return false;
    }
    const std::optional<ComPtr<INetFwRules>> rules = open_rules();
    if (!rules) {
        return false;
    }

    ComPtr<INetFwRule> existing;
    if (SUCCEEDED((*rules)->Item(name_bstr.get(), &existing))) {
        return true;
    }
    if (is_elevated()) {
        return add_rule_now(rules->Get(), spec, rule_name, *exe_path);
    }
    if (!has_interactive_session()) {
        return false;
    }
    if (!relaunch_elevated_and_wait(*exe_path, spec)) {
        return false;
    }
    ComPtr<INetFwRule> confirm;
    return SUCCEEDED((*rules)->Item(name_bstr.get(), &confirm));
}

void maybe_run_as_firewall_helper_and_exit(int argc, char** argv) {
    if (argc != 5 || argv[1] == nullptr || kHelperFlag != argv[1]) {
        return;
    }

    RuleSpec spec;
    spec.name = argv[2];
    const std::string_view protocol_text = argv[3];
    if (protocol_text == "TCP") {
        spec.protocol = Protocol::kTcp;
    } else if (protocol_text == "UDP") {
        spec.protocol = Protocol::kUdp;
    } else {
        std::exit(EXIT_FAILURE);
    }
    const std::string_view port_text = argv[4];
    std::uint16_t port = 0;
    const std::from_chars_result parsed = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (parsed.ec != std::errc{}) {
        std::exit(EXIT_FAILURE);
    }
    spec.port = port;

    const ComScope com;
    if (!com.ok()) {
        std::exit(EXIT_FAILURE);
    }
    const std::optional<std::wstring> exe_path = own_executable_path();
    if (!exe_path) {
        std::exit(EXIT_FAILURE);
    }
    const std::wstring rule_name = build_rule_name(spec, executable_stem(*exe_path));
    const std::optional<ComPtr<INetFwRules>> rules = open_rules();
    if (!rules) {
        std::exit(EXIT_FAILURE);
    }
    std::exit(add_rule_now(rules->Get(), spec, rule_name, *exe_path) ? EXIT_SUCCESS : EXIT_FAILURE);
}

}  // namespace ac3::sendspin::firewall
