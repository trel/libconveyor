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
#include <random>
#include <iostream>

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

// --- Test Cases ---

void test_create_destroy() {
    std::cout << "Running test_create_destroy..." << std::endl;
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_write, mock_read, mock_lseek};
    conveyor_t* conv = conveyor_create(1, O_RDWR, mock_ops, 1024, 1024);
    assert(conv != nullptr);
    conveyor_destroy(conv);
    std::cout << "test_create_destroy PASSED" << std::endl;
}

void test_write_and_flush() {
    std::cout << "Running test_write_and_flush..." << std::endl;
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
    std::cout << "test_write_and_flush PASSED" << std::endl;
}

void test_buffered_read() {
    std::cout << "Running test_buffered_read..." << std::endl;
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
    std::cout << "test_buffered_read PASSED" << std::endl;
}

void test_fast_write_hiding() {
    std::cout << "Running test_fast_write_hiding..." << std::endl;
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
    std::cout << "test_fast_write_hiding PASSED" << std::endl;
}

void test_fast_read_hiding() {
    std::cout << "Running test_fast_read_hiding..." << std::endl;
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
    std::cout << "test_fast_read_hiding PASSED" << std::endl;
}


void test_zero_byte_operations() {
    std::cout << "Running test_zero_byte_operations..." << std::endl;
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_write, mock_read, mock_lseek};
    conveyor_t* conv = conveyor_create(1, O_RDWR, mock_ops, 1024, 1024);
    assert(conv != nullptr);
    
    ssize_t bytes_written = conveyor_write(conv, "should not be written", 0);
    assert(bytes_written == 0);

    ssize_t bytes_read = conveyor_read(conv, nullptr, 0);
    assert(bytes_read == 0);

    conveyor_destroy(conv);
    assert(g_mock_storage_data.empty());
    std::cout << "test_zero_byte_operations PASSED" << std::endl;
}

void test_small_buffer_fragmentation() {
    std::cout << "Running test_small_buffer_fragmentation..." << std::endl;
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_write, mock_read, mock_lseek};
    
    std::string test_data(200, 'x');
    
    conveyor_t* write_conv = conveyor_create(1, O_WRONLY, mock_ops, 20, 0);
    assert(write_conv != nullptr);
    ssize_t bytes_written = conveyor_write(write_conv, test_data.c_str(), test_data.length());
    assert(bytes_written == (ssize_t)test_data.length());
    conveyor_destroy(write_conv);
    assert(g_mock_storage_data.size() == test_data.length());
    assert(std::string(g_mock_storage_data.data(), g_mock_storage_data.size()) == test_data);

    mock_lseek(1, 0, SEEK_SET);
    conveyor_t* read_conv = conveyor_create(1, O_RDONLY, mock_ops, 0, 20);
    assert(read_conv != nullptr);
    std::vector<char> read_buffer(test_data.length());
    ssize_t bytes_read = conveyor_read(read_conv, read_buffer.data(), read_buffer.size());
    assert(bytes_read == (ssize_t)test_data.length());
    assert(std::string(read_buffer.data(), read_buffer.size()) == test_data);
    conveyor_destroy(read_conv);
    std::cout << "test_small_buffer_fragmentation PASSED" << std::endl;
}

void test_multithreaded_writes() {
    std::cout << "Running test_multithreaded_writes..." << std::endl;
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_write, mock_read, mock_lseek};
    conveyor_t* conv = conveyor_create(1, O_WRONLY, mock_ops, 1024, 0);
    assert(conv != nullptr);

    std::vector<std::thread> threads;
    const int num_threads = 4;
    const int writes_per_thread = 100;
    const std::string data_to_write = "abcdefgh";

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([=]() {
            for (int j = 0; j < writes_per_thread; ++j) {
                conveyor_write(conv, data_to_write.c_str(), data_to_write.length());
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    conveyor_destroy(conv);

    size_t expected_size = num_threads * writes_per_thread * data_to_write.length();
    assert(g_mock_storage_data.size() == expected_size);
    for (size_t i = 0; i < g_mock_storage_data.size(); i += data_to_write.length()) {
        assert(g_mock_storage_data[i] == 'a' && g_mock_storage_data[i + data_to_write.length() - 1] == 'h');
    }
    std::cout << "test_multithreaded_writes PASSED" << std::endl;
}

void test_random_seek_stress() {
    std::cout << "Running test_random_seek_stress..." << std::endl;
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_write, mock_read, mock_lseek};
    conveyor_t* conv = conveyor_create(1, O_RDWR, mock_ops, 256, 256);
    assert(conv != nullptr);

    const int file_size = 4096;
    std::vector<char> local_copy(file_size, 'A');
    mock_write(1, local_copy.data(), local_copy.size());

    std::mt19937 rng(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    std::uniform_int_distribution<int> pos_dist(0, file_size - 1);
    std::uniform_int_distribution<int> len_dist(1, 32);
    std::uniform_int_distribution<int> char_dist('B', 'Z');

    for (int i = 0; i < 200; ++i) {
        off_t seek_pos = pos_dist(rng);
        conveyor_lseek(conv, seek_pos, SEEK_SET);

        int write_len = len_dist(rng);
        std::vector<char> write_data;
        for (int j = 0; j < write_len; ++j) write_data.push_back(static_cast<char>(char_dist(rng)));

        if (seek_pos + write_len > file_size) {
            write_len = file_size - seek_pos;
        }

        if (write_len > 0) {
            conveyor_write(conv, write_data.data(), write_len);
            std::memcpy(local_copy.data() + seek_pos, write_data.data(), write_len);
        }
    }

    conveyor_destroy(conv);

    assert(g_mock_storage_data == local_copy);
    std::cout << "test_random_seek_stress PASSED" << std::endl;
}

int main() {
    std::cout << "--- Running Basic Tests ---" << std::endl;
    test_create_destroy();
    test_write_and_flush();
    test_buffered_read();
    
    std::cout << "\n--- Running Latency-Hiding Tests ---" << std::endl;
    test_fast_write_hiding();
    test_fast_read_hiding();
    
    std::cout << "\n--- Running Corner Case and Stress Tests ---" << std::endl;
    test_zero_byte_operations();
    test_small_buffer_fragmentation();
    test_multithreaded_writes();
    test_random_seek_stress();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}