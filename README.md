# libconveyor: High-Performance I/O Buffering Library

[![License](https://img.shields.io/badge/License-BSD_3--Clause-blue.svg)](./LICENSE.md)

`libconveyor` is a high-performance, thread-safe C++ library designed to hide I/O latency through intelligent dual ring-buffering. It provides a POSIX-like API (`read`, `write`, `lseek`) while asynchronously managing data transfer to and from underlying storage via background worker threads.

## Features

*   **Dual Ring Buffers:** Separate, configurable buffers for write-behind caching and read-ahead prefetching.
*   **I/O Latency Hiding:** Asynchronous background threads perform actual storage operations, allowing application threads to proceed quickly.
*   **Thread-Safe API:** All public API calls are fully thread-safe, protecting internal data structures and ensuring consistent state.
*   **`pread`/`pwrite` Semantics:** Interacts with underlying storage using stateless, offset-based `pread`/`pwrite` operations for robust multithreaded I/O.
*   **Pluggable Storage Backend:** Abstracted storage operations (`storage_operations_t`) allow `libconveyor` to be easily integrated with any block-storage mechanism (e.g., file systems, network storage APIs, custom drivers).
*   **Observability:** Provides detailed runtime statistics (`conveyor_stats_t`) including bytes transferred, latency, and buffer congestion events, with a "reset-on-read" model for windowed monitoring.
*   **Robust Error Handling:** Detects and reports the first asynchronous I/O error to the user via sticky error codes.
*   **Fail-Fast for Invalid Writes:** Prevents indefinite hangs by failing writes that exceed the buffer's total capacity.

## Motivation

This library was developed as a foundational component to enhance I/O performance within data management systems, particularly for creating pass-through resource plugins for systems like iRODS. By buffering I/O, `libconveyor` can significantly reduce perceived latency for client applications, making operations feel more responsive even when interacting with slow or distant storage.

## High-Level Architecture

`libconveyor` operates by decoupling application I/O requests from the physical I/O operations.

1.  **Application Thread:** Calls `conveyor_read()`, `conveyor_write()`, or `conveyor_lseek()`.
    *   `conveyor_write()`: Places data into an in-memory queue (`WriteRequest`) and immediately returns.
    *   `conveyor_read()`: Attempts to satisfy requests from an in-memory read buffer. If data is unavailable, it signals the `readWorker` and waits.
    *   `conveyor_lseek()`: Flushes pending writes, invalidates read buffers, and updates internal file pointers before performing the underlying seek.
2.  **`writeWorker` Thread (Write-Behind):** Runs in the background, consuming `WriteRequest` objects from a queue. It performs `ops.pwrite()` to the actual storage at the specified offset.
3.  **`readWorker` Thread (Read-Ahead):** Runs in the background, proactively fetching data from storage using `ops.pread()` into its ring buffer. It anticipates future reads to minimize latency.
4.  **`storage_operations_t`:** A set of function pointers (`pwrite`, `pread`, `lseek`) provided during `conveyor_create` that define how `libconveyor` interacts with the specific underlying storage backend.

## Building

`libconveyor` uses CMake for its build system.

```bash
mkdir build
cd build
cmake ..
cmake --build .
```

To run the tests:

```bash
cd build
./test/Debug/conveyor_basic_test.exe # On Windows
# Or on Linux/macOS:
# ./test/conveyor_basic_test
```

## Usage Example (Conceptual)

```cpp
#include "libconveyor/conveyor.h"
#include <iostream>
#include <string>
#include <vector>
#include <fcntl.h> // For O_RDWR, etc.

// Example mock storage operations (these would typically interact with a real file system or network)
ssize_t my_pwrite(storage_handle_t, const void* buf, size_t count, off_t offset) {
    // ... implement actual pwrite to storage ...
    std::cout << "Storage: pwrite " << count << " bytes at " << offset << std::endl;
    return count;
}

ssize_t my_pread(storage_handle_t, void* buf, size_t count, off_t offset) {
    // ... implement actual pread from storage ...
    std::cout << "Storage: pread " << count << " bytes at " << offset << std::endl;
    return count;
}

off_t my_lseek(storage_handle_t, off_t offset, int whence) {
    // ... implement actual lseek ...
    std::cout << "Storage: lseek to " << offset << " with whence " << whence << std::endl;
    return offset; // Example: simple seek-set
}

int main() {
    storage_operations_t my_storage_ops = {my_pwrite, my_pread, my_lseek};
    
    // Create a conveyor with 1MB write buffer and 512KB read buffer
    conveyor_t* conv = conveyor_create(
        123, // Example storage handle (e.g., a file descriptor)
        O_RDWR, // Open for read/write
        my_storage_ops,
        1024 * 1024, // 1MB write buffer
        512 * 1024   // 512KB read buffer
    );

    if (!conv) {
        std::cerr << "Failed to create conveyor." << std::endl;
        return 1;
    }

    std::string data_to_write = "Hello, buffered world!";
    conveyor_write(conv, data_to_write.c_str(), data_to_write.length());
    
    // Data is now in the buffer, writeWorker will flush it asynchronously.
    // Application thread continues immediately.

    // Force flush to ensure data is written before destroy
    conveyor_flush(conv);

    // Get and print some stats
    conveyor_stats_t stats;
    if (conveyor_get_stats(conv, &stats) == 0) {
        std::cout << "Bytes written in window: " << stats.bytes_written << std::endl;
        std::cout << "Avg write latency: " << stats.avg_write_latency_ms << "ms" << std::endl;
    }

    conveyor_destroy(conv);
    return 0;
}
```

## Contributing

Contributions are welcome! Please ensure that any new features or bug fixes include corresponding test cases and maintain the existing coding style.

## License

`libconveyor` is distributed under the **BSD 3-Clause License**. See the [LICENSE.md](./LICENSE.md) file for full details.
