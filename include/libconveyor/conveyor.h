#ifndef LIBCONVEYOR_CONVEYOR_HPP
#define LIBCONVEYOR_CONVEYOR_HPP

#include <functional>
#include <cstddef> // For size_t
#include <fcntl.h> // For O_RDONLY, O_WRONLY, O_RDWR, O_APPEND, etc.

// MSVC specific definitions for ssize_t and off_t if they are not defined by other headers
#if defined(_MSC_VER)
#include <BaseTsd.h> // For SSIZE_T
typedef SSIZE_T ssize_t;
typedef long off_t; // off_t is typically long on 32-bit and 64-bit Windows when not using _FILE_OFFSET_BITS=64
#else
#include <sys/types.h> // For off_t, ssize_t on POSIX systems
#endif

#define LIBCONVEYOR_ERROR -1

// Opaque handle to the conveyor object
struct conveyor_t;

// Represents the underlying storage to be buffered
using storage_handle_t = int; // e.g., a file descriptor, or an iRODS file handle

// Callbacks for the library to interact with the real storage
struct storage_operations_t {
    std::function<ssize_t(storage_handle_t, const void*, size_t, off_t)> pwrite;
    std::function<ssize_t(storage_handle_t, void*, size_t, off_t)> pread;
    std::function<off_t(storage_handle_t, off_t, int)> lseek;
};

// --- API Functions ---

// Creates a conveyor instance with specified buffer sizes and open flags
conveyor_t* conveyor_create(
    storage_handle_t handle,
    int flags, // The flags from the open() call, e.g., O_RDWR | O_APPEND
    const storage_operations_t& ops,
    size_t write_buffer_size,
    size_t read_buffer_size
);

// Destroys the conveyor, flushing any remaining data in the write buffer
void conveyor_destroy(conveyor_t* conv);

// POSIX-like I/O operations
ssize_t conveyor_write(conveyor_t* conv, const void* buf, size_t count);
ssize_t conveyor_read(conveyor_t* conv, void* buf, size_t count);
off_t conveyor_lseek(conveyor_t* conv, off_t offset, int whence);

// Forces a flush of the write-buffer to the underlying storage
int conveyor_flush(conveyor_t* conv);

// Statistics structure for observability
struct conveyor_stats_t {
    // Volume in the last window
    size_t bytes_written;
    size_t bytes_read;

    // Latency in the last window (in milliseconds)
    size_t avg_write_latency_ms;
    size_t avg_read_latency_ms;

    // Congestion events in the last window
    size_t write_buffer_full_events;

    // Persistent sticky error code
    int last_error_code;
};

// Retrieves the latest statistics, resetting the counters for the next window.
int conveyor_get_stats(conveyor_t* conv, conveyor_stats_t* stats);

#endif // LIBCONVEYOR_CONVEYOR_HPP
