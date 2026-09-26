// The console's recent output, for GET /log. See ../include/ac3forge/log.hpp
// and planning/esp32-ota.md (O4).
//
// stdout and stderr are reopened on a device of this file's own, /dev/ac3log,
// whose writes go on to /dev/console as before and into the ring as well:
// ESP-IDF has no public way to add an output to the console, and picolibc,
// ESP-IDF's C library, has one stdout for every task, so the reopening covers
// a printf from any task and the ESP_LOGx that reach stdout. ESP-IDF's own
// OpenThread console does the same (components/openthread/src/ncp/
// esp_openthread_ncp_console.cpp). Not kept: what was printed before
// log_start, and what esp_rom_printf writes, which includes a panic's
// registers and backtrace. The core dump is the record of a crash.

#include "ac3forge/log.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>

#include "sdkconfig.h"

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_vfs.h"
#include "esp_vfs_ops.h"
#include "freertos/FreeRTOS.h"

namespace ac3forge {
namespace {

constexpr const char* kDevice = "/dev/ac3log";

LogRing* g_ring = nullptr;
portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
int g_console = -1;  // /dev/console, which every write goes on to
thread_local bool t_console_only = false;

ssize_t log_write(void* /*ctx*/, int /*fd*/, const void* data, std::size_t size) {
    const ssize_t written = ::write(g_console, data, size);
    if (!t_console_only && g_ring != nullptr) {
        portENTER_CRITICAL(&g_lock);
        g_ring->put({static_cast<const char*>(data), size});
        portEXIT_CRITICAL(&g_lock);
    }
    return written;
}

int log_open(void* /*ctx*/, const char* /*path*/, int /*flags*/, int /*mode*/) { return 0; }

int log_close(void* /*ctx*/, int /*fd*/) { return 0; }

// A character device, as the console is: stdio then buffers a line at a time.
int log_fstat(void* /*ctx*/, int /*fd*/, struct stat* st) {
    std::memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR;
    return 0;
}

// The console's own: the USB-Serial-JTAG console sends a line without a
// newline only when told to, which Improv's packets rely on.
int log_fsync(void* /*ctx*/, int /*fd*/) { return fsync(g_console); }

// In the order esp_vfs_fs_ops_t declares its members, the first of each
// union being the context-pointer operation ESP_VFS_FLAG_CONTEXT_PTR uses.
const esp_vfs_fs_ops_t kOps = {
    {&log_write},  // write_p
    {nullptr},     // lseek_p
    {nullptr},     // read_p
    {nullptr},     // pread_p
    {nullptr},     // pwrite_p
    {&log_open},   // open_p
    {&log_close},  // close_p
    {&log_fstat},  // fstat_p
    {nullptr},     // fcntl_p
    {nullptr},     // ioctl_p
    {&log_fsync},  // fsync_p
#ifdef CONFIG_VFS_SUPPORT_DIR
    nullptr,  // dir
#endif
#ifdef CONFIG_VFS_SUPPORT_TERMIOS
    nullptr,  // termios
#endif
#if CONFIG_VFS_SUPPORT_SELECT
    nullptr,  // select
#endif
};

}  // namespace

bool log_start(std::size_t bytes) {
    if (g_ring != nullptr || bytes == 0) {
        return g_ring != nullptr;
    }
    // PSRAM where the board has it: the ring is read only when someone asks.
    auto* storage = static_cast<char*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (storage == nullptr) {
        storage = static_cast<char*>(heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (storage == nullptr) {
        std::printf("log: no room for %u bytes of console output; GET /log keeps none\n",
                    static_cast<unsigned>(bytes));
        return false;
    }
    g_console = open("/dev/console", O_WRONLY);
    if (g_console < 0 ||
        esp_vfs_register_fs(kDevice, &kOps, ESP_VFS_FLAG_STATIC | ESP_VFS_FLAG_CONTEXT_PTR, nullptr) != ESP_OK) {
        std::printf("log: could not tee the console; GET /log keeps nothing\n");
        if (g_console >= 0) {
            close(g_console);
            g_console = -1;
        }
        heap_caps_free(storage);
        return false;
    }
    auto* ring = new LogRing(std::span<char>(storage, bytes));
    std::fflush(stdout);
    std::fflush(stderr);
    if (std::freopen(kDevice, "w", stdout) == nullptr || std::freopen(kDevice, "w", stderr) == nullptr) {
        // Back to the console as it was, whatever happens to the log.
        (void)std::freopen("/dev/console", "w", stdout);
        (void)std::freopen("/dev/console", "w", stderr);
        std::printf("log: could not reopen stdout; GET /log keeps nothing\n");
        delete ring;
        heap_caps_free(storage);
        return false;
    }
    // Published under the lock, which orders the ring's construction before
    // any task's write can see it. It is never taken back.
    portENTER_CRITICAL(&g_lock);
    g_ring = ring;
    portEXIT_CRITICAL(&g_lock);
    std::printf("log: GET /log keeps the console's last %u bytes, in %s\n", static_cast<unsigned>(bytes),
                esp_ptr_external_ram(storage) ? "PSRAM" : "internal RAM");
    return true;
}

std::optional<LogRing::Read> log_read(std::uint64_t from, std::size_t limit) {
    if (g_ring == nullptr) {
        return std::nullopt;
    }
    // Allocated first, then filled under the lock: nothing allocates inside
    // a critical section.
    LogRing::Read out;
    out.text.resize(std::min(limit, g_ring->capacity()));
    portENTER_CRITICAL(&g_lock);
    const LogRing::Copied copied = g_ring->copy(from, out.text);
    portEXIT_CRITICAL(&g_lock);
    out.text.resize(copied.size);
    out.from = copied.from;
    out.next = copied.next;
    return out;
}

ConsoleOnly::ConsoleOnly() {
    // What is already written goes where it would have gone, before this
    // task's output stops going to the ring.
    std::fflush(stdout);
    t_console_only = true;
}

ConsoleOnly::~ConsoleOnly() {
    std::fflush(stdout);
    t_console_only = false;
}

}  // namespace ac3forge
