#include "libconveyor/conveyor.h"
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <cerrno>
#include <algorithm>
#include <iostream>

#ifndef O_ACCMODE
#define O_ACCMODE (O_RDONLY | O_WRONLY | O_RDWR)
#endif

namespace libconveyor {

struct RingBuffer {
    std::vector<char> buffer;
    size_t capacity = 0;
    size_t head = 0;
    size_t tail = 0;
    size_t size = 0;

    RingBuffer(size_t cap) : capacity(cap), buffer(cap) {}

    size_t write(const char* data, size_t len) {
        if (len == 0) return 0;
        size_t bytes_to_write = std::min(len, capacity - size);
        if (bytes_to_write == 0) return 0;
        size_t first_chunk_len = std::min(bytes_to_write, capacity - head);
        std::memcpy(buffer.data() + head, data, first_chunk_len);
        if (bytes_to_write > first_chunk_len) {
            std::memcpy(buffer.data(), data + first_chunk_len, bytes_to_write - first_chunk_len);
        }
        head = (head + bytes_to_write) % capacity;
        size += bytes_to_write;
        return bytes_to_write;
    }

    size_t read(char* data, size_t len) {
        if (len == 0) return 0;
        size_t bytes_to_read = std::min(len, size);
        if (bytes_to_read == 0) return 0;
        size_t first_chunk_len = std::min(bytes_to_read, capacity - tail);
        std::memcpy(data, buffer.data() + tail, first_chunk_len);
        if (bytes_to_read > first_chunk_len) {
            std::memcpy(data + first_chunk_len, buffer.data(), bytes_to_read - first_chunk_len);
        }
        tail = (tail + bytes_to_read) % capacity;
        size -= bytes_to_read;
        return bytes_to_read;
    }

    void clear() { head = 0; tail = 0; size = 0; }
    bool empty() const { return size == 0; }
    bool full() const { return size == capacity; }
    size_t available_space() const { return capacity - size; }
    size_t available_data() const { return size; }
};

struct ConveyorImpl {
    storage_handle_t handle;
    int flags;
    storage_operations_t ops;

    bool write_buffer_enabled = false;
    RingBuffer write_buffer;
    std::thread write_worker_thread;
    std::mutex write_mutex;
    std::condition_variable write_cv_producer;
    std::condition_variable write_cv_consumer;
    std::atomic<bool> write_worker_stop_flag{false};
    std::atomic<bool> write_buffer_needs_flush{false};
    std::atomic<off_t> current_pos_in_storage{0};

    bool read_buffer_enabled = false;
    RingBuffer read_buffer;
    std::thread read_worker_thread;
    std::mutex read_mutex;
    std::condition_variable read_cv_producer;
    std::condition_variable read_cv_consumer;
    std::atomic<bool> read_worker_stop_flag{false};
    std::atomic<bool> read_buffer_stale{false};
    std::atomic<bool> read_worker_needs_fill{false};
    std::atomic<bool> read_eof_flag{false};

    off_t current_file_offset = 0;
    
    ConveyorImpl(size_t w_cap, size_t r_cap) : write_buffer(w_cap), read_buffer(r_cap) {}

    void writeWorker() {
        std::vector<char> temp_flush_buffer;
        temp_flush_buffer.reserve(write_buffer.capacity);
        while (true) {
            std::unique_lock<std::mutex> lock(write_mutex);
            write_cv_consumer.wait(lock, [&] { return write_buffer.available_data() > 0 || write_buffer_needs_flush || write_worker_stop_flag; });
            if (write_worker_stop_flag && write_buffer.empty()) break;

            size_t bytes_to_flush = write_buffer.available_data();
            if (bytes_to_flush > 0) {
                temp_flush_buffer.resize(bytes_to_flush);
                write_buffer.read(temp_flush_buffer.data(), bytes_to_flush);
            }

            bool do_flush_now = write_buffer_needs_flush.load();
            if (do_flush_now) write_buffer_needs_flush = false;
            
            write_cv_producer.notify_all();

            if (bytes_to_flush > 0) {
                lock.unlock();
                if (flags & O_APPEND) ops.lseek(handle, 0, SEEK_END);
                ssize_t written = ops.write(handle, temp_flush_buffer.data(), bytes_to_flush);
                lock.lock();
                if (written > 0) current_pos_in_storage += static_cast<off_t>(written);
            } else if (do_flush_now) {
                write_cv_producer.notify_all();
            }
        }
    }

    void readWorker() {
        std::vector<char> temp_read_buffer;
        temp_read_buffer.reserve(read_buffer.capacity);
        while (true) {
            std::unique_lock<std::mutex> lock(read_mutex);
            read_cv_producer.wait(lock, [&] { return read_buffer.available_space() > 0 || read_buffer_stale.load() || read_worker_stop_flag.load() || read_worker_needs_fill.load(); });
            if (read_worker_stop_flag.load()) break;
            if (read_buffer_stale.load()) { read_buffer.clear(); read_buffer_stale = false; }

            if (read_buffer.available_space() > 0 && !read_worker_stop_flag.load()) {
                if (current_pos_in_storage != (current_file_offset + read_buffer.available_data())) {
                    ops.lseek(handle, current_pos_in_storage, SEEK_SET);
                }
                size_t bytes_to_read = read_buffer.available_space();
                temp_read_buffer.resize(bytes_to_read);
                lock.unlock();
                ssize_t bytes_read = ops.read(handle, temp_read_buffer.data(), bytes_to_read);
                lock.lock();
                if (bytes_read > 0) {
                    read_buffer.write(temp_read_buffer.data(), bytes_read);
                    current_pos_in_storage += static_cast<off_t>(bytes_read);
                } else if (bytes_read == 0) {
                    read_eof_flag = true;
                }
            }
            if (read_worker_needs_fill.load()) read_worker_needs_fill = false;
            read_cv_consumer.notify_all();
        }
    }
};

}

conveyor_t* conveyor_create(storage_handle_t h, int f, const storage_operations_t& o, size_t w, size_t r) {
    auto* impl = new libconveyor::ConveyorImpl(w, r);
    impl->handle = h; impl->flags = f; impl->ops = o;
    int mode = f & O_ACCMODE;
    if (mode == O_RDONLY || mode == O_RDWR) impl->read_buffer_enabled = true;
    if (mode == O_WRONLY || mode == O_RDWR) impl->write_buffer_enabled = true;
    if (impl->read_buffer_enabled) impl->read_worker_thread = std::thread(&libconveyor::ConveyorImpl::readWorker, impl);
    if (impl->write_buffer_enabled) impl->write_worker_thread = std::thread(&libconveyor::ConveyorImpl::writeWorker, impl);
    return reinterpret_cast<conveyor_t*>(impl);
}

void conveyor_destroy(conveyor_t* conv) {
    if (!conv) return;
    auto* impl = reinterpret_cast<libconveyor::ConveyorImpl*>(conv);
    if (impl->write_buffer_enabled) conveyor_flush(conv);
    if (impl->read_buffer_enabled) {
        impl->read_worker_stop_flag = true;
        impl->read_cv_producer.notify_all();
        impl->read_cv_consumer.notify_all();
        if (impl->read_worker_thread.joinable()) impl->read_worker_thread.join();
    }
    if (impl->write_buffer_enabled) {
        impl->write_worker_stop_flag = true;
        impl->write_cv_producer.notify_all();
        impl->write_cv_consumer.notify_all();
        if (impl->write_worker_thread.joinable()) impl->write_worker_thread.join();
    }
    delete impl;
}

ssize_t conveyor_write(conveyor_t* conv, const void* buf, size_t count) {
    if (!conv) { errno = EBADF; return LIBCONVEYOR_ERROR; }
    auto* impl = reinterpret_cast<libconveyor::ConveyorImpl*>(conv);
    if (!impl->write_buffer_enabled) { errno = EBADF; return LIBCONVEYOR_ERROR; }
    ssize_t total_written = 0;
    const char* ptr = static_cast<const char*>(buf);
    {
        std::unique_lock<std::mutex> lock(impl->write_mutex);
        while (total_written < count && !impl->write_worker_stop_flag) {
            impl->write_cv_producer.wait(lock, [&]{ return impl->write_buffer.available_space() > 0 || impl->write_worker_stop_flag; });
            if (impl->write_worker_stop_flag) break;
            size_t written = impl->write_buffer.write(ptr + total_written, count - total_written);
            total_written += written;
            impl->current_file_offset += written;
            impl->write_cv_consumer.notify_one();
        }
    }
    if (impl->read_buffer_enabled && (impl->flags & O_RDWR)) {
        std::lock_guard<std::mutex> lock(impl->read_mutex);
        impl->read_buffer.clear();
        impl->read_buffer_stale = true;
    }
    return total_written;
}

ssize_t conveyor_read(conveyor_t* conv, void* buf, size_t count) {
    if (!conv) { errno = EBADF; return LIBCONVEYOR_ERROR; }
    auto* impl = reinterpret_cast<libconveyor::ConveyorImpl*>(conv);
    if (!impl->read_buffer_enabled) { errno = EBADF; return LIBCONVEYOR_ERROR; }
    std::unique_lock<std::mutex> lock(impl->read_mutex);
    ssize_t total_read = 0;
    char* ptr = static_cast<char*>(buf);
    while (total_read < count && !impl->read_worker_stop_flag.load()) {
        if (impl->read_buffer.empty()) {
            if (impl->read_eof_flag.load()) break;
            impl->read_worker_needs_fill = true;
            impl->read_cv_producer.notify_one();
            impl->read_cv_consumer.wait(lock, [&]{ return impl->read_buffer.available_data() > 0 || impl->read_eof_flag.load() || impl->read_worker_stop_flag.load(); });
            if (impl->read_buffer.available_data() == 0) break;
        }
        size_t read_now = impl->read_buffer.read(ptr + total_read, count - total_read);
        total_read += read_now;
        impl->current_file_offset += read_now;
        impl->read_cv_producer.notify_one();
    }
    return total_read;
}

off_t conveyor_lseek(conveyor_t* conv, off_t offset, int whence) {
    if (!conv) { errno = EBADF; return LIBCONVEYOR_ERROR; }
    auto* impl = reinterpret_cast<libconveyor::ConveyorImpl*>(conv);

    // Acquire both locks to make the flush and seek atomic from the user's perspective.
    std::lock(impl->read_mutex, impl->write_mutex);
    std::unique_lock<std::mutex> read_lock(impl->read_mutex, std::adopt_lock);
    std::unique_lock<std::mutex> write_lock(impl->write_mutex, std::adopt_lock);

    // 1. Manually flush the write buffer while holding the locks.
    if (impl->write_buffer_enabled && !impl->write_buffer.empty()) {
        impl->write_buffer_needs_flush = true;
        impl->write_cv_consumer.notify_one();
        // Wait for the worker to empty the buffer.
        impl->write_cv_producer.wait(write_lock, [&] { 
            return impl->write_buffer.empty() || impl->write_worker_stop_flag; 
        });
        impl->write_buffer_needs_flush = false;
    }

    // 2. Now that writes are flushed, perform the actual seek.
    off_t new_pos = impl->ops.lseek(impl->handle, offset, whence);

    // 3. Update internal state and invalidate the read buffer.
    if (new_pos != LIBCONVEYOR_ERROR) {
        if (impl->read_buffer_enabled) {
            impl->read_buffer.clear();
            impl->read_buffer_stale = true;
            impl->read_eof_flag = false;
            impl->read_cv_consumer.notify_all();
            impl->read_cv_producer.notify_all();
        }
        impl->current_file_offset = new_pos;
        impl->current_pos_in_storage = new_pos;
    }

    return new_pos;
}

int conveyor_flush(conveyor_t* conv) {
    std::cout << "FLUSH: Called." << std::endl;
    if (!conv) { errno = EBADF; return LIBCONVEYOR_ERROR; }
    auto* impl = reinterpret_cast<libconveyor::ConveyorImpl*>(conv);
    if (!impl->write_buffer_enabled) return 0;
    std::cout << "FLUSH: Locking write mutex." << std::endl;
    std::unique_lock<std::mutex> lock(impl->write_mutex);
    std::cout << "FLUSH: Locked. available_data=" << impl->write_buffer.available_data() << std::endl;
    if (impl->write_buffer.available_data() > 0) {
        impl->write_buffer_needs_flush = true;
        impl->write_cv_consumer.notify_one();
        std::cout << "FLUSH: Waiting for worker." << std::endl;
        impl->write_cv_producer.wait(lock, [&] { return impl->write_buffer.empty() || impl->write_worker_stop_flag; });
        std::cout << "FLUSH: Worker finished." << std::endl;
    }
    impl->write_buffer_needs_flush = false;
    std::cout << "FLUSH: Exiting." << std::endl;
    return 0;
}
