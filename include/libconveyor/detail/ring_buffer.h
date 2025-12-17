#ifndef LIBCONVEYOR_DETAIL_RING_BUFFER_H
#define LIBCONVEYOR_DETAIL_RING_BUFFER_H

#include <vector>
#include <cstddef> // For size_t
#include <algorithm> // For std::min
#include <cstring> // For std::memcpy

namespace libconveyor {

struct RingBuffer { // Still needed for read buffer
    std::vector<char> buffer;
    size_t capacity = 0;
    size_t head = 0;
    size_t tail = 0;
    size_t size = 0;

    RingBuffer(size_t cap) : capacity(cap), buffer(cap) {}

    size_t write(const char* data, size_t len) {
        if (len == 0) return 0;
        
        size_t actual_len = len;
        if (len > capacity) { // If writing more than capacity, only the last 'capacity' bytes are kept
            data += (len - capacity);
            actual_len = capacity;
        }

        size_t bytes_to_overwrite = 0;
        if (size + actual_len > capacity) {
            bytes_to_overwrite = (size + actual_len) - capacity;
        }

        // Advance tail if overwriting old data
        if (bytes_to_overwrite > 0) {
            tail = (tail + bytes_to_overwrite) % capacity;
            size -= bytes_to_overwrite; // Reduce size by overwritten bytes
        }
        
        size_t first_chunk_len = std::min(actual_len, capacity - head);
        std::memcpy(buffer.data() + head, data, first_chunk_len);

        if (actual_len > first_chunk_len) {
            std::memcpy(buffer.data(), data + first_chunk_len, actual_len - first_chunk_len);
        }
        head = (head + actual_len) % capacity;
        size += actual_len;

        return len; // Return original length written
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

    void clear() { size = 0; head = 0; tail = 0; }
    bool empty() const { return size == 0; }
    bool full() const { return size == capacity; }
    size_t available_space() const { return capacity - size; }
    size_t available_data() const { return size; }
};

} // namespace libconveyor

#endif // LIBCONVEYOR_DETAIL_RING_BUFFER_H