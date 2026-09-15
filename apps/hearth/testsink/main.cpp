// ac3hearth-testsink: a Sendspin player for Hearth's tests and contributors
// (planning/hearth-reference-player.md, The test sink).

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "sink.hpp"

namespace {

namespace testsink = ac3::hearth::testsink;
using namespace std::chrono_literals;

constexpr std::string_view kUsage = R"(usage: ac3hearth-testsink [options]

A Sendspin player that writes each player@v1 stream it plays to a WAV file.

  --name NAME            the name servers show (default "Hearth test sink")
  --address ADDRESS      the address to listen on (default 0.0.0.0)
  --port PORT            the port to listen on (default 8928; 0 for any)
  --state DIRECTORY      identity, pairing PSK and pairing records
                         (default ./hearth-testsink-state)
  --out DIRECTORY        WAV files and play-time logs; none to count only
  --pair METHOD          the pairing code method offered: dynamic (default),
                         static or none; the pairing PSK is always offered
  --static-code DIGITS   the eight-digit code for --pair static
  --codecs LIST          the codecs offered, most preferred first, from pcm,
                         flac and opus (default pcm,flac,opus)
  --unpaired-access      admit servers that have not paired
  --no-mdns              do not advertise _sendspin._tcp
  --mdns-interface ADDR  advertise on this IPv4 interface only; repeatable
  --run-for SECONDS      exit after this long instead of waiting for 'quit'

Commands on standard input: window (open the static code's pairing window),
reset (reset the dynamic code's round limit), cancel (cancel pairing),
status, quit.
)";

class ConsoleLog final : public testsink::SinkLog {
   public:
    void line(std::string_view text) override {
        const std::lock_guard lock(mutex_);
        std::cout << text << std::endl;
    }

   private:
    std::mutex mutex_;
};

[[nodiscard]] std::optional<std::uint16_t> parse_port(std::string_view text) {
    unsigned value = 0;
    if (text.empty() || text.size() > 5) {
        return std::nullopt;
    }
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = (value * 10) + static_cast<unsigned>(c - '0');
    }
    if (value > 65535) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

}  // namespace

int main(int argc, char** argv) {
    testsink::SinkOptions options;
    options.state_directory = "hearth-testsink-state";
    std::optional<std::chrono::seconds> run_for;

    const std::vector<std::string_view> arguments(argv + 1, argv + argc);
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string_view argument = arguments[i];
        const auto value = [&]() -> std::optional<std::string_view> {
            if (i + 1 >= arguments.size()) {
                std::cerr << argument << " needs a value\n";
                return std::nullopt;
            }
            return arguments[++i];
        };
        if (argument == "--help" || argument == "-h") {
            std::cout << kUsage;
            return EXIT_SUCCESS;
        }
        if (argument == "--unpaired-access") {
            options.unpaired_access = true;
            continue;
        }
        if (argument == "--no-mdns") {
            options.advertise = false;
            continue;
        }
        const std::optional<std::string_view> given = value();
        if (!given) {
            return EXIT_FAILURE;
        }
        if (argument == "--name") {
            options.name = *given;
        } else if (argument == "--address") {
            options.address = *given;
        } else if (argument == "--port") {
            const std::optional<std::uint16_t> port = parse_port(*given);
            if (!port) {
                std::cerr << "--port takes a number from 0 to 65535\n";
                return EXIT_FAILURE;
            }
            options.port = *port;
        } else if (argument == "--state") {
            options.state_directory = std::filesystem::path(*given);
        } else if (argument == "--out") {
            options.output_directory = std::filesystem::path(*given);
        } else if (argument == "--pair") {
            if (*given == "dynamic") {
                options.code_method = testsink::CodeMethod::kDynamic;
            } else if (*given == "static") {
                options.code_method = testsink::CodeMethod::kStatic;
            } else if (*given == "none") {
                options.code_method = testsink::CodeMethod::kNone;
            } else {
                std::cerr << "--pair takes dynamic, static or none\n";
                return EXIT_FAILURE;
            }
        } else if (argument == "--static-code") {
            options.static_code = *given;
        } else if (argument == "--codecs") {
            options.codecs.clear();
            std::string_view list = *given;
            while (!list.empty()) {
                const std::size_t comma = list.find(',');
                const std::string_view name = list.substr(0, comma);
                if (name == "pcm") {
                    options.codecs.push_back(ac3::sendspin::messages::Codec::kPcm);
                } else if (name == "flac") {
                    options.codecs.push_back(ac3::sendspin::messages::Codec::kFlac);
                } else if (name == "opus") {
                    options.codecs.push_back(ac3::sendspin::messages::Codec::kOpus);
                } else {
                    std::cerr << "--codecs takes pcm, flac and opus, separated by commas\n";
                    return EXIT_FAILURE;
                }
                list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
            }
            if (options.codecs.empty()) {
                std::cerr << "--codecs needs at least one codec\n";
                return EXIT_FAILURE;
            }
        } else if (argument == "--mdns-interface") {
            options.mdns_interfaces.emplace_back(*given);
        } else if (argument == "--run-for") {
            const std::optional<std::uint16_t> seconds = parse_port(*given);
            if (!seconds) {
                std::cerr << "--run-for takes a number of seconds up to 65535\n";
                return EXIT_FAILURE;
            }
            run_for = std::chrono::seconds(*seconds);
        } else {
            std::cerr << "unknown option " << argument << "\n\n" << kUsage;
            return EXIT_FAILURE;
        }
    }

    ConsoleLog log;
    auto sink = testsink::Sink::start(options, log);
    if (!sink) {
        std::cerr << "ac3hearth-testsink: " << sink.error() << "\n";
        return EXIT_FAILURE;
    }
    log.line("listening on " + options.address + ":" + std::to_string((*sink)->port()) + " as \"" + options.name + "\"");
    log.line("client_id " + (*sink)->client_id());
    log.line("pairing token " + (*sink)->pairing_token());

    if (run_for) {
        std::this_thread::sleep_for(*run_for);
        return EXIT_SUCCESS;
    }
    std::string command;
    while (std::getline(std::cin, command)) {
        if (command == "quit" || command == "exit") {
            return EXIT_SUCCESS;
        }
        if (command == "window") {
            (*sink)->open_window();
        } else if (command == "reset") {
            (*sink)->reset_rounds();
        } else if (command == "cancel") {
            (*sink)->cancel_pairing();
        } else if (command == "status") {
            const testsink::Sink::Totals totals = (*sink)->totals();
            log.line(std::to_string(totals.connections) + " connections, " + std::to_string(totals.streams) +
                     " streams, " + std::to_string(totals.chunks) + " chunks, " + std::to_string(totals.frames) +
                     " frames");
        } else if (!command.empty()) {
            log.line("commands: window, reset, cancel, status, quit");
        }
    }
    // Standard input closed, as it is when run in the background: keep playing until killed.
    while (true) {
        std::this_thread::sleep_for(1h);
    }
}
