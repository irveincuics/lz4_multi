#include "lz4.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <algorithm>

using namespace std;

const size_t BLOCK_SIZE = 4 * 1024 * 1024; // 每块 4MB

// 将整个源数据压缩为自定义格式
vector<char> compress_data(const vector<char>& input) {
    vector<char> compressed;
    size_t pos = 0;
    while (pos < input.size()) {
        size_t bytes_to_read = min(BLOCK_SIZE, input.size() - pos);
        int max_comp_size = LZ4_compressBound(bytes_to_read);
        vector<char> comp_buffer(max_comp_size);
        int comp_size = LZ4_compress_default(input.data() + pos, comp_buffer.data(), bytes_to_read, max_comp_size);
        if (comp_size <= 0) {
            throw runtime_error("压缩失败");
        }
        // 写入块头：先写入 4 字节压缩后数据大小，再写入 4 字节原始数据大小
        uint32_t comp_size_u = static_cast<uint32_t>(comp_size);
        uint32_t orig_size_u = static_cast<uint32_t>(bytes_to_read);
        compressed.insert(compressed.end(), reinterpret_cast<char*>(&comp_size_u), reinterpret_cast<char*>(&comp_size_u) + sizeof(comp_size_u));
        compressed.insert(compressed.end(), reinterpret_cast<char*>(&orig_size_u), reinterpret_cast<char*>(&orig_size_u) + sizeof(orig_size_u));
        // 写入压缩数据
        compressed.insert(compressed.end(), comp_buffer.begin(), comp_buffer.begin() + comp_size);
        pos += bytes_to_read;
    }
    return compressed;
}

// Consumer 用于收集各块解压后的数据
class Consumer {
  public:
    Consumer() : output_size_(0) {}

    // 预先分配足够大的缓冲区
    void preallocate(size_t size) {
        lock_guard<mutex> lock(mtx_);
        output_buffer_.resize(size);
    }

    // 写入数据到指定位置（假设写入区间不重叠）
    void write_output(const char* data, size_t size, size_t position) {
        lock_guard<mutex> lock(mtx_);
        memcpy(output_buffer_.data() + position, data, size);
        size_t end = position + size;
        if (end > output_size_) {
            output_size_ = end;
        }
    }

    // 获取最终数据
    vector<char> get_output() {
        lock_guard<mutex> lock(mtx_);
        return vector<char>(output_buffer_.begin(), output_buffer_.begin() + output_size_);
    }

  private:
    vector<char> output_buffer_;
    size_t output_size_;
    mutex mtx_;
};

// 每个压缩块的信息
struct BlockInfo {
    const char* compressed_data;
    size_t compressed_size;
    size_t decompressed_size; // 原始数据大小
    size_t output_position;   // 在最终输出中的起始位置
};

// 解析自定义格式压缩数据
vector<BlockInfo> parse_blocks(const char* data, size_t size) {
    vector<BlockInfo> blocks;
    size_t pos = 0;
    size_t current_output = 0;
    while (pos + 8 <= size) { // 至少需要8字节块头
        uint32_t comp_size, orig_size;
        memcpy(&comp_size, data + pos, 4);
        pos += 4;
        memcpy(&orig_size, data + pos, 4);
        pos += 4;
        if (pos + comp_size > size) break;  // 文件损坏或格式错误
        blocks.push_back({data + pos, comp_size, orig_size, current_output});
        current_output += orig_size;
        pos += comp_size;
    }
    return blocks;
}

// 解压单个块，并将结果写入 Consumer 的指定位置
void decompress_block(const BlockInfo& block, Consumer& consumer) {
    vector<char> out_buffer(block.decompressed_size);
    int dec_bytes = LZ4_decompress_safe(
        block.compressed_data,
        out_buffer.data(),
        block.compressed_size,
        block.decompressed_size
    );
    if (dec_bytes < 0) {
        throw runtime_error("LZ4 解压失败");
    }
    consumer.write_output(out_buffer.data(), dec_bytes, block.output_position);
}

// 利用多线程并行解压
void lz4_parallel_decompress(const char* in, size_t in_size, unsigned nthreads, Consumer& consumer) {
    vector<BlockInfo> blocks = parse_blocks(in, in_size);
    size_t total_output_size = 0;
    for (const auto& block : blocks) {
        total_output_size += block.decompressed_size;
    }
    consumer.preallocate(total_output_size);

    atomic<size_t> current_block(0);
    vector<thread> threads;
    for (unsigned i = 0; i < nthreads; i++) {
        threads.emplace_back([&]() {
            while (true) {
                size_t idx = current_block.fetch_add(1);
                if (idx >= blocks.size()) break;
                decompress_block(blocks[idx], consumer);
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
}

// 解压数据，返回解压后的结果
vector<char> decompress_data(const vector<char>& compressed_data, unsigned nthreads) {
    Consumer consumer;
    lz4_parallel_decompress(compressed_data.data(), compressed_data.size(), nthreads, consumer);
    return consumer.get_output();
}


int main(int argc, char* argv[]) {
    if (argc != 2) {
        cout << "用法: " << argv[0] << " 源文件路径" << endl;
        return 1;
    }
    string input_path = argv[1];

    // 读取源文件内容
    ifstream fin(input_path, ios::binary);
    if (!fin) {
        cerr << "无法打开源文件: " << input_path << endl;
        return 1;
    }
    vector<char> source_data((istreambuf_iterator<char>(fin)), istreambuf_iterator<char>());
    fin.close();
    cout << "读取源文件成功，大小: " << source_data.size() << " 字节" << endl;

    // 压缩
    vector<char> compressed_data = compress_data(source_data);
    cout << "压缩后数据大小: " << compressed_data.size() << " 字节" << endl;

    // 解压缩（并行）
    unsigned nthreads = 2;
    vector<char> decompressed_data = decompress_data(compressed_data, nthreads);
    cout << "解压后数据大小: " << decompressed_data.size() << " 字节" << endl;

    // 验证解压后的数据是否与原始数据一致
    if (decompressed_data == source_data) {
        cout << "验证成功：解压数据与原始数据一致！" << endl;
    } else {
        cerr << "验证失败：解压数据与原始数据不一致！" << endl;
        return 1;
    }
    return 0;
}
