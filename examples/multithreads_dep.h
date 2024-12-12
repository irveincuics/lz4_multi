//
// Created by Administrator on 2024-11-06.
//
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
    // 构造函数，初始化输出缓冲区和上下文大小
    Consumer(size_t initial_output_size)
        : output_buffer_(initial_output_size), output_size_(0), previous_size_(0) {}
    // 获取完整的解压缩数据
    const std::vector<char>& get_full_output() const {
        return output_buffer_;
    }

    // 获取输出缓冲区指针，用于解压线程填充数据
    char* get_output_buffer() {
        return output_buffer_.data() + output_size_;
    }

    // 获取当前可用的输出缓冲区大小
    size_t get_output_size() const {
        return output_buffer_.size() - output_size_;
    }

    // 写入解压缩后的数据，更新输出缓冲区大小
    void write_output(const char* output, size_t size) {
        std::lock_guard<std::mutex> lock(mtx_);

        // 如果空间不足，扩展缓冲区
        if (output_size_ + size > output_buffer_.size()) {
            expand_buffer(output_size_ + size - output_buffer_.size());
        }

        // 拷贝解压缩后的数据到输出缓冲区
        std::memcpy(output_buffer_.data() + output_size_, output, size);
        output_size_ += size;
    }

    // 获取最近一次解压缩的输出数据指针（用于上下文共享）
    const byte* const get_previous_output() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return reinterpret_cast<const byte* const>(output_buffer_.data() + output_size_ - previous_size_);
    }

    // 获取最近一次解压缩的数据大小
    size_t get_previous_size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return previous_size_;
    }

    // 保存上下文信息（最近一次解压缩的输出数据）
    void save_context(const char* output, size_t size) {
        std::lock_guard<std::mutex> lock(mtx_);

        // 只保留最后 64 KB 作为字典上下文（LZ4 的限制）
        size_t dict_size = std::min(size, static_cast<size_t>(64 * 1024));
        previous_size_ = dict_size;

        // 将最后 64 KB 的数据保存到缓冲区开头，作为字典
        std::memmove(output_buffer_.data(), output + size - dict_size, dict_size);
    }

    // 清空输出缓冲区和上下文信息
    void reset() {
        std::lock_guard<std::mutex> lock(mtx_);
        output_size_ = 0;
        previous_size_ = 0;
    }

  private:
    // 扩展输出缓冲区的大小
    void expand_buffer(size_t additional_size) {
        size_t new_size = output_buffer_.size() + std::max(additional_size, output_buffer_.size() / 2);
        output_buffer_.resize(new_size);
    }

    std::vector<char> output_buffer_; // 输出缓冲区
    size_t output_size_;              // 当前输出数据大小
    size_t previous_size_;            // 最近一次解压缩的数据大小（用于上下文）
    mutable std::mutex mtx_;          // 线程安全锁
};



template<typename Consumer>
static void lz4_decompress_with_context(const byte* in, size_t in_size, unsigned nthreads, Consumer& consumer) {
    std::vector<std::thread> threads;
    std::vector<LZ4F_decompressionContext_t> lz4_contexts(nthreads);
    std::atomic<size_t> nready = {0};
    std::condition_variable ready;
    std::mutex ready_mtx;
    std::exception_ptr exception;

    // 初始化 LZ4F 解码上下文
    for (unsigned i = 0; i < nthreads; i++) {
        size_t result = LZ4F_createDecompressionContext(&lz4_contexts[i], LZ4F_VERSION);
        if (LZ4F_isError(result)) {
            throw std::runtime_error("Failed to create LZ4F decompression context: " + std::string(LZ4F_getErrorName(result)));
        }
    }

    // 分块大小设置
    size_t chunk_size = in_size / nthreads;
    size_t first_chunk_size = std::min(chunk_size + (4UL << 20), in_size); // 第一个块最多为输入大小
    chunk_size = (in_size - first_chunk_size) / (nthreads > 1 ? (nthreads - 1) : 1);

    std::cout << "Input size: " << in_size
              << ", Threads: " << nthreads
              << ", First chunk size: " << first_chunk_size
              << ", Chunk size: " << chunk_size << std::endl;

    for (unsigned chunk_idx = 0; chunk_idx < nthreads; chunk_idx++) {
        threads.emplace_back([&, chunk_idx]() {
            try {
                const byte* start = in + (chunk_idx == 0 ? 0 : first_chunk_size + chunk_size * (chunk_idx - 1));
                const byte* stop = start + (chunk_idx == 0 ? first_chunk_size : chunk_size);

            // 防止 stop 超出 in_size
                stop = (stop > in + in_size) ? in + in_size : stop;

                std::cout << "Thread " << chunk_idx
                          << ": start=" << (start - in)
                          << ", stop=" << (stop - in)
                          << ", size=" << (stop - start) << std::endl;

            // 获取当前线程的解压上下文
                LZ4F_decompressionContext_t& dctx = lz4_contexts[chunk_idx];
                size_t src_size = stop - start;
                const char* src = (const char*)start;

            // 使用上一个线程的后64KB作为字典
                if (chunk_idx > 0) {
                    const byte* prev_output = static_cast<const byte*>(consumer.get_previous_output());
                    size_t prev_size = consumer.get_previous_size();
                    size_t dict_size = std::min(prev_size, static_cast<size_t>(64 * 1024));
                // 直接将字典作为输入
                    const char* prev_output_char = reinterpret_cast<const char*>(prev_output);

                // LZ4F解压时不需要显式的字典加载，只需在每个块解压前提供字典
                    size_t result = LZ4F_decompress(dctx, nullptr, nullptr, prev_output_char + prev_size - dict_size, &dict_size, nullptr);
                    if (LZ4F_isError(result)) {
                        throw std::runtime_error("Failed to use dictionary: " + std::string(LZ4F_getErrorName(result)));
                    }
                }

            // 逐块解压
                while (src_size > 0) {
                    char* output = (char*)consumer.get_output_buffer();
                    size_t dst_size = consumer.get_output_size();

                    size_t src_consumed = src_size;

                    size_t result = LZ4F_decompress(dctx, output, &dst_size, src, &src_consumed, nullptr);
                    if (LZ4F_isError(result)) {
                        throw std::runtime_error("LZ4F decompression failed: " + std::string(LZ4F_getErrorName(result)));
                    }

                    std::cout << "Thread " << chunk_idx
                              << " decompressed " << dst_size
                              << " bytes, consumed " << src_consumed
                              << " bytes of input." << std::endl;

                    consumer.write_output(output, dst_size);

                    src += src_consumed;
                    src_size -= src_consumed;

                    if (result == 0) {
                    // 解压完成
                        break;
                    }
                }

            // 保存当前线程的后64KB作为字典供下一个线程使用
                if (src_size == 0) {
                    const byte* last_output = static_cast<const byte*>(consumer.get_previous_output());
                    size_t last_size = consumer.get_previous_size();
                    size_t dict_size = std::min(last_size, static_cast<size_t>(64 * 1024));
                    // 显式转换 std::byte* 到 const char*
                    const char* prev_output_char = reinterpret_cast<const char*>(last_output);
                    consumer.save_context(prev_output_char + last_size - dict_size, dict_size);
                }

                {
                    std::unique_lock<std::mutex> lock{ready_mtx};
                    nready++;
                    ready.notify_all();
                }
            } catch (...) {
                std::unique_lock<std::mutex> lock{ready_mtx};
                if (!exception) {
                    exception = std::current_exception();
                    nready = 0; // 停止线程池
                }
                return;
            }
        });
    }

    // 等待所有线程完成
    for (auto& thread : threads)
        thread.join();

    // 处理异常
    if (exception) { std::rethrow_exception(exception); }

    // 释放 LZ4F 解码上下文
    for (auto& dctx : lz4_contexts)
        LZ4F_freeDecompressionContext(dctx);
}