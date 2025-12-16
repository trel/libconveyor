#include "libconveyor/conveyor.h"
#include <vector>
#include <string>
#include <cstring>
#include <fcntl.h>
#include <cassert>
#include <algorithm>
#include <mutex>
#include <thread>
#include <chrono>
#include <atomic>

// --- Mock Infrastructure ---
static std::vector<char> g_mock_storage_data;
static off_t g_mock_storage_pos = 0;
static std::mutex g_mock_storage_mutex;

static std::atomic<bool> g_simulate_slow_write = false;
static std::atomic<bool> g_simulate_slow_read = false;
static const std::chrono::milliseconds g_simulated_latency(50);

static ssize_t mock_write(storage_handle_t, const void* buf, size_t count) {
    if (g_simulate_slow_write) {
        std::this_thread::sleep_for(g_simulated_latency);
    }
    std::lock_guard<std::mutex> lock(g_mock_storage_mutex);
    if (g_mock_storage_pos + count > g_mock_storage_data.size()) {
        g_mock_storage_data.resize(g_mock_storage_pos + count);
    }
    std::memcpy(g_mock_storage_data.data() + g_mock_storage_pos, buf, count);
    g_mock_storage_pos += static_cast<off_t>(count);
    return count;
}

static ssize_t mock_read(storage_handle_t, void* buf, size_t count) {
    if (g_simulate_slow_read) {
        std::this_thread::sleep_for(g_simulated_latency);
    }
    std::lock_guard<std::mutex> lock(g_mock_storage_mutex);
    size_t bytes_to_read = 0;
    if (g_mock_storage_pos < (off_t)g_mock_storage_data.size()) {
         bytes_to_read = std::min((size_t)count, g_mock_storage_data.size() - g_mock_storage_pos);
    }
    
    if (bytes_to_read > 0) {
        std::memcpy(buf, g_mock_storage_data.data() + g_mock_storage_pos, bytes_to_read);
        g_mock_storage_pos += static_cast<off_t>(bytes_to_read);
    }
    return bytes_to_read;
}

static off_t mock_lseek(storage_handle_t, off_t offset, int whence) {
    std::lock_guard<std::mutex> lock(g_mock_storage_mutex);
    off_t new_pos = LIBCONVEYOR_ERROR;
    if (whence == SEEK_SET) new_pos = offset;
    else if (whence == SEEK_CUR) new_pos = g_mock_storage_pos + offset;
    else if (whence == SEEK_END) new_pos = static_cast<off_t>(g_mock_storage_data.size()) + offset;

    if (new_pos >= 0) g_mock_storage_pos = new_pos;
    else return LIBCONVEYOR_ERROR;
    return new_pos;
}

void reset_mock_storage() {
    std::lock_guard<std::mutex> lock(g_mock_storage_mutex);
    g_mock_storage_data.clear();
    g_mock_storage_pos = 0;
    g_simulate_slow_write = false;
    g_simulate_slow_read = false;
}

void test_create_destroy() {
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_write, mock_read, mock_lseek};
    conveyor_t* conv = conveyor_create(1, O_RDWR, mock_ops, 1024, 1024);
    assert(conv != nullptr);
    conveyor_destroy(conv);
}

void test_write_and_flush() {
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_write, mock_read, mock_lseek};
    conveyor_t* conv = conveyor_create(1, O_WRONLY, mock_ops, 1024, 0);
    assert(conv != nullptr);

    std::string test_data = "Hello, Conveyor!";
    ssize_t bytes_written = conveyor_write(conv, test_data.c_str(), test_data.length());
    assert(bytes_written == (ssize_t)test_data.length());

    conveyor_flush(conv);
    conveyor_destroy(conv);

    assert(g_mock_storage_data.size() == test_data.length());
    assert(std::string(g_mock_storage_data.data(), g_mock_storage_data.size()) == test_data);
}

void test_buffered_read() {
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_write, mock_read, mock_lseek};
    
    std::string test_data = "This is a test of the buffered read functionality.";
    mock_write(1, test_data.c_str(), test_data.length());
    mock_lseek(1, 0, SEEK_SET);

    conveyor_t* conv = conveyor_create(1, O_RDONLY, mock_ops, 0, 1024);
    assert(conv != nullptr);
    
    std::vector<char> read_buffer(test_data.length() + 1, '\0');
    ssize_t bytes_read = conveyor_read(conv, read_buffer.data(), test_data.length());
    
    assert(bytes_read == (ssize_t)test_data.length());
    assert(std::string(read_buffer.data()) == test_data);

    conveyor_destroy(conv);
}

void test_fast_write_hiding() {
    reset_mock_storage();
    g_simulate_slow_write = true;
    storage_operations_t mock_ops = {mock_write, mock_read, mock_lseek};
    
    conveyor_t* conv = conveyor_create(1, O_WRONLY, mock_ops, 1024, 0);
    assert(conv != nullptr);

    std::string test_data = "This should write instantly.";
    
    auto start_time = std::chrono::high_resolution_clock::now();
    ssize_t bytes_written = conveyor_write(conv, test_data.c_str(), test_data.length());
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    assert(bytes_written == (ssize_t)test_data.length());
    assert(duration < g_simulated_latency);

    conveyor_destroy(conv);

    assert(g_mock_storage_data.size() == test_data.length());
    assert(std::string(g_mock_storage_data.data(), g_mock_storage_data.size()) == test_data);
}

void test_fast_read_hiding() {
    reset_mock_storage();
    g_simulate_slow_read = true;
    storage_operations_t mock_ops = {mock_write, mock_read, mock_lseek};

    std::string test_data = "This should be read instantly from cache.";
    mock_write(1, test_data.c_str(), test_data.length());
    mock_lseek(1, 0, SEEK_SET);

    conveyor_t* conv = conveyor_create(1, O_RDONLY, mock_ops, 0, 1024);
    assert(conv != nullptr);
    
    std::this_thread::sleep_for(g_simulated_latency + std::chrono::milliseconds(50));

    std::vector<char> read_buffer(test_data.length() + 1, '\0');
    
    auto start_time = std::chrono::high_resolution_clock::now();
    ssize_t bytes_read = conveyor_read(conv, read_buffer.data(), test_data.length());
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    assert(bytes_read == (ssize_t)test_data.length());
    assert(duration < g_simulated_latency);
    assert(std::string(read_buffer.data()) == test_data);

    conveyor_destroy(conv);
}

int main() {
    test_create_destroy();
    test_write_and_flush();
    test_buffered_read();
    test_fast_write_hiding();
    test_fast_read_hiding();

    return 0;
}
