#include "multithreads_dep.h"
#include <iostream>
#include <vector>
#include <fstream>
#include "lz4.h"
#include <lz4frame.h>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <input_file> <output_file> [num_threads]" << std::endl;
        return 1;
    }

    const char* input_filename = argv[1];
    const char* output_filename = argv[2];
    unsigned nthreads = (argc > 3) ? std::stoi(argv[3]) : std::thread::hardware_concurrency();

    // 读取输入文件
    std::ifstream input_file(input_filename, std::ios::binary | std::ios::ate);
    if (!input_file.is_open()) {
        std::cerr << "Failed to open input file." << std::endl;
        return 1;
    }

    size_t input_size = input_file.tellg();
    input_file.seekg(0, std::ios::beg);

    std::vector<char> input_data(input_size);
    input_file.read(input_data.data(), input_size);
    input_file.close();

    // 压缩数据到 Frame 格式
    size_t max_compressed_size = LZ4F_compressFrameBound(input_size, nullptr);
    std::vector<char> compressed_data(max_compressed_size);

    size_t compressed_size = LZ4F_compressFrame(
        compressed_data.data(),
        max_compressed_size,
        input_data.data(),
        input_size,
        nullptr);

    if (LZ4F_isError(compressed_size)) {
        std::cerr << "Compression failed: " << LZ4F_getErrorName(compressed_size) << std::endl;
        return 1;
    }

    compressed_data.resize(compressed_size);

    // 将压缩数据写入文件
    std::ofstream compressed_file(output_filename, std::ios::binary);
    if (!compressed_file.is_open()) {
        std::cerr << "Failed to open compressed output file." << std::endl;
        return 1;
    }
    compressed_file.write(compressed_data.data(), compressed_size);
    compressed_file.close();

    std::cout << "Compressed data saved to " << output_filename << std::endl;

    // 初始化 Consumer
    size_t max_output_size = 128 * 1024 * 1024; // 假设解压后的最大大小为 128 MB
    Consumer consumer(max_output_size);

    try {
        // 调用解压缩函数
        // 转换 compressed_data.data() 为 const std::byte*
        const std::byte* byte_data = reinterpret_cast<const std::byte*>(compressed_data.data());
        lz4_decompress_with_context(byte_data, input_size, nthreads, consumer);
    } catch (const std::exception& e) {
        std::cerr << "Decompression failed: " << e.what() << std::endl;
        return 1;
    }

// 解压后保存到文件
    std::ofstream output_file(output_filename, std::ios::binary);
    if (!output_file.is_open()) {
        std::cerr << "Failed to open output file." << std::endl;
        return 1;
    }

// 获取完整解压数据并写入
    const auto& decompressed_data = consumer.get_full_output();
    output_file.write(decompressed_data.data(), decompressed_data.size());
    output_file.close();

    std::cout << "Decompressed data written to file: " << output_filename << std::endl;


    std::cout << "Decompression completed successfully!" << std::endl;
    return 0;
}

