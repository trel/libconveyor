#ifndef LIBCONVEYOR_MODERN_HPP
#define LIBCONVEYOR_MODERN_HPP

#include "libconveyor/conveyor.h"
#include <expected>       // C++23: Error handling without exceptions
#include <span>           // C++20: View over contiguous memory
#include <memory>         // std::unique_ptr
#include <chrono>         // Strong typing for time
#include <system_error>   // std::error_code
#include <filesystem>     // std::filesystem::path

namespace libconveyor::v2 {

using namespace std::chrono_literals;

// --- 1. Concepts for Data ---
// Allows passing vectors, arrays, strings, or raw pointers transparently
template<typename T>
concept ByteContiguous = requires(T t) {
    { std::data(t) } -> std::convertible_to<const void*>;
    { std::size(t) } -> std::convertible_to<size_t>;
};

// --- 2. Strong Types for Configuration ---
struct Config {
    storage_handle_t handle;
    storage_operations_t ops;
    size_t write_capacity = 1024 * 1024; // 1MB default
    size_t read_capacity = 1024 * 1024;
    int open_flags = O_RDWR;
};

// --- 3. The Modern Conveyor Class ---
class Conveyor {
private:
    // RAII: Custom deleter automatically calls conveyor_destroy
    struct Deleter { 
        void operator()(conveyor_t* ptr) const { conveyor_destroy(ptr); } 
    };
    std::unique_ptr<conveyor_t, Deleter> impl_;

public:
    // No default constructor (resource must exist)
    Conveyor() = delete;

    // Factory: Returns value or error (no exceptions)
    static std::expected<Conveyor, std::error_code> create(Config cfg) {
        conveyor_t* raw = conveyor_create(cfg.handle, cfg.open_flags, &cfg.ops, 
                                        cfg.write_capacity, cfg.read_capacity);
        if (!raw) {
            return std::unexpected(std::error_code(errno, std::system_category()));
        }
        return Conveyor(raw);
    }

    // Explicit Move semantics (Rule of 5)
    Conveyor(Conveyor&&) noexcept = default;
    Conveyor& operator=(Conveyor&&) noexcept = default;

    // Private constructor for factory
    explicit Conveyor(conveyor_t* ptr) : impl_(ptr) {}

    // --- Modern Write API ---
    // Accepts anything that looks like a buffer (vector, string, array)
    // Returns bytes written or error code
    template<ByteContiguous Buffer>
    std::expected<size_t, std::error_code> write(const Buffer& buffer) {
        const void* ptr = std::data(buffer);
        size_t len = std::size(buffer) * sizeof(typename Buffer::value_type);
        
        ssize_t res = conveyor_write(impl_.get(), ptr, len);
        
        if (res == LIBCONVEYOR_ERROR) {
            return std::unexpected(std::error_code(errno, std::system_category()));
        }
        return static_cast<size_t>(res);
    }

    // --- Modern Read API ---
    // Accepts mutable span
    std::expected<size_t, std::error_code> read(std::span<std::byte> buffer) {
        ssize_t res = conveyor_read(impl_.get(), buffer.data(), buffer.size());
        
        if (res == LIBCONVEYOR_ERROR) {
            return std::unexpected(std::error_code(errno, std::system_category()));
        }
        return static_cast<size_t>(res);
    }

    // --- Modern Seek ---
    std::expected<off_t, std::error_code> seek(off_t offset, int whence = SEEK_SET) {
        off_t res = conveyor_lseek(impl_.get(), offset, whence);
        if (res == LIBCONVEYOR_ERROR) {
            return std::unexpected(std::error_code(errno, std::system_category()));
        }
        return res;
    }

    // --- Explicit Flush ---
    std::expected<void, std::error_code> flush() {
        if (conveyor_flush(impl_.get()) != 0) {
            return std::unexpected(std::error_code(errno, std::system_category()));
        }
        return {};
    }

    // --- Stats with std::chrono ---
    struct Stats {
        size_t bytes_written;
        size_t bytes_read;
        std::chrono::milliseconds avg_write_latency;
        std::chrono::milliseconds avg_read_latency;
    };

    Stats stats() {
        conveyor_stats_t raw;
        conveyor_get_stats(impl_.get(), &raw);
        return Stats{
            raw.bytes_written,
            raw.bytes_read,
            std::chrono::milliseconds(raw.avg_write_latency_ms),
            std::chrono::milliseconds(raw.avg_read_latency_ms)
        };
    }
};

} // namespace libconveyor::v2

#endif // LIBCONVEYOR_MODERN_HPP
