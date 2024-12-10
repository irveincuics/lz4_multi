//
// Created by Administrator on 2024-11-06.
//
#include "lz4.h"
#include <lz4frame.h>
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
    Consumer(size_t max_output_size)
        : output_buffer_(max_output_size), output_size_(0), previous_size_(0) {}

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

        // 检查是否有足够的空间
        if (output_size_ + size > output_buffer_.size()) {
            throw std::runtime_error("Output buffer overflow");
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
    std::vector<char> output_buffer_; // 输出缓冲区
    size_t output_size_;              // 当前输出数据大小
    size_t previous_size_;            // 最近一次解压缩的数据大小（用于上下文）
    mutable std::mutex mtx_;          // 线程安全锁
};



template<typename Consumer>
static void lz4_decompress_with_context(const byte* in, size_t in_size, unsigned nthreads, Consumer& consumer) {
    std::vector<std::thread> threads;
    std::vector<LZ4_streamDecode_t*> lz4_contexts(nthreads);
    std::atomic<size_t> nready = {0};
    std::condition_variable ready;
    std::mutex ready_mtx;
    std::exception_ptr exception;

    // 初始化 LZ4 解码上下文
    for (unsigned i = 0; i < nthreads; i++) {
        lz4_contexts[i] = LZ4_createStreamDecode();
        if (!lz4_contexts[i]) throw std::runtime_error("Failed to create LZ4 decode context");
    }

    // 分块大小设置
    size_t chunk_size = in_size / nthreads;
    size_t first_chunk_size = chunk_size + (4UL << 20); // 前 4MB 为第一个线程分配更多的数据
    chunk_size = (in_size - first_chunk_size) / (nthreads - 1);

    for (unsigned chunk_idx = 0; chunk_idx < nthreads; chunk_idx++) {
        threads.emplace_back([&, chunk_idx]() {
            try {
                const byte* start = in + (chunk_idx == 0 ? 0 : first_chunk_size + chunk_size * (chunk_idx - 1));
                const byte* stop = start + (chunk_idx == 0 ? first_chunk_size : chunk_size);

                // 初始化上下文
                if (chunk_idx > 0) {
                    // 使用前一个线程的上下文作为初始字典
                    const byte* prev_output = static_cast<const byte*>(consumer.get_previous_output());
                    size_t prev_size = consumer.get_previous_size();
                    LZ4_setStreamDecode(lz4_contexts[chunk_idx], (const char*)prev_output, prev_size);
                }

                // 解压缩数据块
                char* output = (char*)consumer.get_output_buffer();
                int decompressed_size = LZ4_decompress_safe_continue(
                    lz4_contexts[chunk_idx],
                    (const char*)start,
                    output,
                    stop - start,
                    consumer.get_output_size());

                if (decompressed_size < 0) {
                    throw std::runtime_error("LZ4 decompression failed");
                }

                // 直接调用 consumer 处理输出数据
                consumer.write_output(output, decompressed_size);

                // 保存上下文信息供下一个线程使用
                consumer.save_context(output, decompressed_size);
            } catch (...) {
                std::unique_lock<std::mutex> lock{ready_mtx};
                if (!exception) {
                    exception = std::current_exception();
                    nready = 0; // 停止线程池
                }
                return;
            }

            {
                std::unique_lock<std::mutex> lock{ready_mtx};
                nready++;
                ready.notify_all();
            }
        });
    }

    // 等待所有线程完成
    for (auto& thread : threads)
        thread.join();

    // 处理异常
    if (exception) { std::rethrow_exception(exception); }

    // 释放 LZ4 解码上下文
    for (auto ctx : lz4_contexts)
        LZ4_freeStreamDecode(ctx);
}



