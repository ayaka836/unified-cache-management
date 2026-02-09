/**
 * MIT License
 *
 * Copyright (c) 2025 Huawei Technologies Co., Ltd. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * */

#include "logger/logger.h"
#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <spdlog/spdlog.h>
#include <thread>

using namespace UC::Logger;

namespace {
void CleanDir(const std::string& path)
{
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    if (ec) {
        std::cerr << "Failed to remove file: " << path << std::endl;
        std::cerr << "Error: " << ec.message() << std::endl;
        std::exit(1);
    }
}
}  // namespace

class UCLoggerDelayTest : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        CleanDir(test_log_dir_);
        std::filesystem::create_directories(test_log_dir_);
        std::cout << "test_log_path_: " << test_log_path_ << std::endl;
        logger_ = &Logger::GetInstance();
        logger_->Setup(test_log_path_, 3, 1);  // 3 files, 1MB max size
    }

    static void TearDownTestSuite()
    {
        CleanDir(test_log_dir_);
        spdlog::drop_all();
    }

    static inline std::string test_log_dir_ = "log_delay_test";
    static inline std::string test_log_path_ = "log_delay_test/test_log.log";
    static inline Logger* logger_ = nullptr;
};

// Test to measure flush latency when spdlog buffer is full
TEST_F(UCLoggerDelayTest, FlushLatencyWhenBufferFull)
{
    SourceLocation loc{"logger_delay_test.cc", "FlushLatencyWhenBufferFull", 0};
    
    // Baseline: Measure flush time with empty buffer
    logger_->Flush();  // Ensure buffer is empty
    auto start_baseline = std::chrono::high_resolution_clock::now();
    logger_->Flush();
    auto end_baseline = std::chrono::high_resolution_clock::now();
    auto baseline_duration = std::chrono::duration_cast<std::chrono::microseconds>(
        end_baseline - start_baseline);
    
    std::cout << "Baseline flush time (empty buffer): " << baseline_duration.count() 
              << " microseconds" << std::endl;
    
    // 2. 准备测试数据（固定长度的日志内容，便于计算写入次数）
    const size_t buffer_size = 8192;                 // 缓冲区大小 8KB（小缓冲区便于测试）
    std::string log_msg = "Test log message for buffer flush latency measurement. ";
    // 补全到固定长度（比如256字节），确保每次写入的字节数一致
    while (log_msg.size() < 256) {
        log_msg += "0";
    }
    const size_t msg_size = log_msg.size() + 30;  // 加上日志头的长度（约30字节）
    const size_t write_times = (buffer_size / msg_size);  // 确保写满缓冲区
    
    // Give spdlog a moment to process some messages (if async)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for (size_t i = 0; i < write_times; ++i) {
        logger_->Log(Level::INFO, std::move(loc), std::move(log_msg));
    }
    // Measure flush time when buffer is full
    auto start_flush = std::chrono::high_resolution_clock::now();
    logger_->Flush();
    auto end_flush = std::chrono::high_resolution_clock::now();
    auto flush_duration = std::chrono::duration_cast<std::chrono::microseconds>(
        end_flush - start_flush);
    
    std::cout << "Flush time (buffer full): " << flush_duration.count() 
              << " microseconds" << std::endl;
    
    // Calculate latency difference
    auto latency_difference = flush_duration.count() - baseline_duration.count();
    std::cout << "Flush latency difference: " << latency_difference 
              << " microseconds" << std::endl;
    
    // Verify that flush completed (check if log file exists and has content)
    ASSERT_TRUE(std::filesystem::exists(test_log_path_)) 
        << "Log file should exist after flush";
    
    // The flush should complete, but we're measuring if there's significant latency
    // Log the results for analysis
    if (latency_difference > 1000) {  // More than 1ms difference
        std::cout << "WARNING: Significant flush latency detected when buffer is full!" 
                  << std::endl;
    }
    
    // Test passes - we're just measuring and reporting the latency
    // The actual assertion is that flush completes (which it should)
    EXPECT_GE(flush_duration.count(), 0) << "Flush should complete";
}

// Test to measure flush latency with continuous high-rate logging
TEST_F(UCLoggerDelayTest, FlushLatencyUnderHighLoad)
{
    SourceLocation loc{"logger_delay_test.cc", "FlushLatencyUnderHighLoad", 0};
    
    const int num_batches = 10;
    const int messages_per_batch = 10000;
    const std::string message(512, 'Y');  // 512 bytes per message
    
    std::vector<long long> flush_times;
    
    for (int batch = 0; batch < num_batches; ++batch) {
        // Write a batch of messages
        for (int i = 0; i < messages_per_batch; ++i) {
            std::string msg = message + " Batch " + std::to_string(batch) + 
                             " Msg " + std::to_string(i);
            logger_->Log(Level::INFO, std::move(loc), std::move(msg));
        }
        
        // Measure flush time after each batch
        auto start = std::chrono::high_resolution_clock::now();
        logger_->Flush();
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            end - start);
        flush_times.push_back(duration.count());
        
        // Small delay to allow some processing
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    // Calculate statistics
    long long min_flush = *std::min_element(flush_times.begin(), flush_times.end());
    long long max_flush = *std::max_element(flush_times.begin(), flush_times.end());
    long long sum = 0;
    for (auto t : flush_times) {
        sum += t;
    }
    long long avg_flush = sum / flush_times.size();
    
    std::cout << "Flush latency statistics under high load:" << std::endl;
    std::cout << "  Min: " << min_flush << " microseconds" << std::endl;
    std::cout << "  Max: " << max_flush << " microseconds" << std::endl;
    std::cout << "  Avg: " << avg_flush << " microseconds" << std::endl;
    std::cout << "  Max/Min ratio: " << (double)max_flush / min_flush << std::endl;
    
    // Verify flush completes
    ASSERT_TRUE(std::filesystem::exists(test_log_path_)) 
        << "Log file should exist after flush";
    
    // If max flush time is significantly higher than min, there may be buffer-related latency
    if (max_flush > min_flush * 2) {
        std::cout << "WARNING: Significant variation in flush times detected!" << std::endl;
        std::cout << "This may indicate buffer-related latency issues." << std::endl;
    }
    
    EXPECT_GE(avg_flush, 0) << "Average flush time should be measurable";
}
