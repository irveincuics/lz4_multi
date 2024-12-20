#include "lz4.h"
#include <lz4frame.h>
#include <iostream>
#include <exception>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstring>
#include <stdexcept>

using namespace std;

class Consumer {
  public:
    Consumer(size_t initial_output_size)
        : output_buffer_(initial_output_size), output_size_(0), previous_size_(0) {}

    const vector<char>& get_full_output() const {
        return output_buffer_;
    }

    char* get_output_buffer() {
        return output_buffer_.data() + output_size_;
    }

    size_t get_output_size() const {
        return output_buffer_.size() - output_size_;
    }

    void write_output(const char* output, size_t size) {
        lock_guard<mutex> lock(mtx_);
        if (output_size_ + size > output_buffer_.size()) {
            expand_buffer(output_size_ + size - output_buffer_.size());
        }
        memcpy(output_buffer_.data() + output_size_, output, size);
        output_size_ += size;
    }

    const char* get_previous_output() const {
        lock_guard<mutex> lock(mtx_);
        return output_buffer_.data() + output_size_ - previous_size_;
    }

    size_t get_previous_size() const {
        lock_guard<mutex> lock(mtx_);
        return previous_size_;
    }

    void save_context(const char* output, size_t size) {
        lock_guard<mutex> lock(mtx_);
        size_t dict_size = min(size, static_cast<size_t>(64 * 1024));
        previous_size_ = dict_size;
        memmove(output_buffer_.data(), output + size - dict_size, dict_size);
    }

    void reset() {
        lock_guard<mutex> lock(mtx_);
        output_size_ = 0;
        previous_size_ = 0;
    }

  private:
    void expand_buffer(size_t additional_size) {
        size_t new_size = output_buffer_.size() + max(additional_size, output_buffer_.size() / 2);
        cout << "Expanding buffer to " << new_size << " bytes." << endl;
        output_buffer_.resize(new_size);
    }

    vector<char> output_buffer_;
    size_t output_size_;
    size_t previous_size_;
    mutable mutex mtx_;
};

void lz4_decompress_with_context(const std::byte* in, size_t in_size, unsigned nthreads, Consumer& consumer) {
    vector<thread> threads;
    vector<LZ4F_decompressionContext_t> lz4_contexts(nthreads);
    vector<mutex> context_mutexes(nthreads);
    vector<condition_variable> context_ready(nthreads);
    vector<bool> context_done(nthreads, false);
    atomic<size_t> nready{0};
    exception_ptr exception;
    mutex exception_mutex;

    for (unsigned i = 0; i < nthreads; ++i) {
        size_t result = LZ4F_createDecompressionContext(&lz4_contexts[i], LZ4F_VERSION);
        if (LZ4F_isError(result)) {
            throw runtime_error("Failed to create LZ4 decompression context: " + string(LZ4F_getErrorName(result)));
        }
    }

    size_t chunk_size = in_size / nthreads;
    const size_t first_chunk_size = min(chunk_size + (4UL << 20), in_size);
    chunk_size = (in_size - first_chunk_size) / (nthreads > 1 ? nthreads - 1 : 1);

    for (unsigned thread_id = 0; thread_id < nthreads; ++thread_id) {
        threads.emplace_back([&, thread_id]() {
            try {
                const byte* start = in + (thread_id == 0 ? 0 : first_chunk_size + chunk_size * (thread_id - 1));
                const byte* stop = start + (thread_id == 0 ? first_chunk_size : chunk_size);
                if (stop > in + in_size) stop = in + in_size;

                auto& dctx = lz4_contexts[thread_id];
                size_t src_size = stop - start;
                const byte* src = start;

                if (thread_id > 0) {
                    unique_lock<mutex> lock(context_mutexes[thread_id - 1]);
                    context_ready[thread_id - 1].wait(lock, [&] { return context_done[thread_id - 1]; });

                    const char* dict = consumer.get_previous_output();
                    size_t dict_size = consumer.get_previous_size();
                    if (dict_size > 0) {
                        size_t result = LZ4F_decompress(dctx, nullptr, nullptr, dict, &dict_size, nullptr);
                        if (LZ4F_isError(result)) {
                            throw runtime_error("Failed to load dictionary: " + string(LZ4F_getErrorName(result)));
                        }
                    }
                }

                while (src_size > 0) {
                    char* output = consumer.get_output_buffer();
                    size_t dst_size = consumer.get_output_size();
                    size_t src_consumed = src_size;

                    size_t result = LZ4F_decompress(dctx, output, &dst_size, src, &src_consumed, nullptr);
                    if (LZ4F_isError(result)) {
                        throw runtime_error("LZ4 decompression failed: " + string(LZ4F_getErrorName(result)));
                    }

                    consumer.write_output(output, dst_size);
                    src += src_consumed;
                    src_size -= src_consumed;

                    if (result == 0) break;
                }

                if (src_size == 0 && thread_id < nthreads - 1) {
                    lock_guard<mutex> lock(exception_mutex);
                    consumer.save_context(consumer.get_previous_output(), consumer.get_previous_size());
                }

                {
                    lock_guard<mutex> lock(context_mutexes[thread_id]);
                    context_done[thread_id] = true;
                    context_ready[thread_id].notify_one();
                }
            } catch (...) {
                lock_guard<mutex> lock(exception_mutex);
                if (!exception) {
                    exception = current_exception();
                }
            }
        });
    }

    for (auto& thread : threads) {
        if (thread.joinable()) thread.join();
    }

    if (exception) rethrow_exception(exception);

    for (auto& dctx : lz4_contexts) {
        LZ4F_freeDecompressionContext(dctx);
    }
}
