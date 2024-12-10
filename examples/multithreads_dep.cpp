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

    // 读取压缩文件
    std::ifstream input_file(input_filename, std::ios::binary | std::ios::ate);
    if (!input_file.is_open()) {
        std::cerr << "Failed to open input file." << std::endl;
        return 1;
    }

    size_t input_size = input_file.tellg();
    input_file.seekg(0, std::ios::beg);

    std::vector<byte> compressed_data(input_size);
    input_file.read(reinterpret_cast<char*>(compressed_data.data()), input_size);
    input_file.close();

    // 初始化 Consumer
    size_t max_output_size = 128 * 1024 * 1024; // 假设解压后的最大大小为 128 MB
    Consumer consumer(max_output_size);

    try {
        // 调用解压缩函数
        lz4_decompress_with_context(compressed_data.data(), input_size, nthreads, consumer);
    } catch (const std::exception& e) {
        std::cerr << "Decompression failed: " << e.what() << std::endl;
        return 1;
    }

    // 写入解压缩后的文件
    std::ofstream output_file(output_filename, std::ios::binary);
    if (!output_file.is_open()) {
        std::cerr << "Failed to open output file." << std::endl;
        return 1;
    }

    const auto& decompressed_data = consumer.get_output_buffer();
    output_file.write(decompressed_data, consumer.get_previous_size());
    output_file.close();

    std::cout << "Decompression completed successfully!" << std::endl;
    return 0;
}

