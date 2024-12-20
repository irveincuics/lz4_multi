### ./examples/multithreads_dep.h

- consumer类
	- 实现包括输出缓冲区的各项操作以及上下文保存和读取等操作
- 主要压缩算法实现
	- 使用Lz4 frame格式的库函数，框架仿照pugz实现
	- 初始化并按照线程数进行分块操作，因为采用lz4frame格式，会自己进行块的边界处理
	- 各线程进行相对独立的解压缩，上一个块的解压缩后的后64KB作为下一个线程的字典
### ./examples/multithreads_dep.cpp
- main函数
	- 使用LZ4F进行压缩并调用实现的多线程解压缩
### 当前仍然存在的问题
- 多线程问题应该是得到解决，通过下面的修改，但是出现新的问题，Decompression failed: LZ4 decompression failed: ERROR_frameType_unknown，无法识别的帧类型，可能是由于分块导致的问题，但是查阅得知Frame格式会自动处理块之间的边界，还需要debug寻找问题

	- 确保字典加载同步
		- 在现有实现中，已经使用了 `mutex` 和 `condition_variable` 来同步字典的加载，但可能在处理上存在不一致性。特别是，`thread_id == 0` 的线程没有上下文同步的问题。因此修改为：将等待前一线程解压完成并加载字典”的逻辑放入 `while` 循环中，直到保证当前线程有有效字典数据可用
	- 避免字典更新冲突
		- 在解压过程中，多个线程可能会在同一时刻尝试更新字典。为了避免冲突，加锁保护字典的更新操作，在 `consumer.save_context` 处加锁，确保字典更新时不会被其他线程抢占。
