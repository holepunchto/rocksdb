//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#ifndef ROCKSDB_LITE
#ifndef GFLAGS
#include <cstdio>
int main() {
  fprintf(stderr,
          "Please install gflags to run block_cache_trace_analyzer_test\n");
  return 1;
}
#else

#include <fstream>
#include <iostream>
#include <map>
#include <vector>

#include "rocksdb/env.h"
#include "rocksdb/status.h"
#include "rocksdb/trace_reader_writer.h"
#include "test_util/testharness.h"
#include "test_util/testutil.h"
#include "tools/block_cache_trace_analyzer.h"
#include "trace_replay/block_cache_tracer.h"

namespace rocksdb {

namespace {
const uint64_t kBlockSize = 1024;
const std::string kBlockKeyPrefix = "test-block-";
const uint32_t kCFId = 0;
const uint32_t kLevel = 1;
const uint64_t kSSTStoringEvenKeys = 100;
const uint64_t kSSTStoringOddKeys = 101;
const std::string kRefKeyPrefix = "test-get-";
const uint64_t kNumKeysInBlock = 1024;
const int kMaxArgCount = 100;
const size_t kArgBufferSize = 100000;
}  // namespace

class BlockCacheTracerTest : public testing::Test {
 public:
  BlockCacheTracerTest() {
    test_path_ = test::PerThreadDBPath("block_cache_tracer_test");
    env_ = rocksdb::Env::Default();
    EXPECT_OK(env_->CreateDir(test_path_));
    trace_file_path_ = test_path_ + "/block_cache_trace";
    block_cache_sim_config_path_ = test_path_ + "/block_cache_sim_config";
    timeline_labels_ =
        "block,all,cf,sst,level,bt,caller,cf_sst,cf_level,cf_bt,cf_caller";
    reuse_distance_labels_ =
        "block,all,cf,sst,level,bt,caller,cf_sst,cf_level,cf_bt,cf_caller";
    reuse_distance_buckets_ = "1,1K,1M,1G";
    reuse_interval_labels_ = "block,all,cf,sst,level,bt,cf_sst,cf_level,cf_bt";
    reuse_interval_buckets_ = "1,10,100,1000";
  }

  ~BlockCacheTracerTest() override {
    if (getenv("KEEP_DB")) {
      printf("The trace file is still at %s\n", trace_file_path_.c_str());
      return;
    }
    EXPECT_OK(env_->DeleteFile(trace_file_path_));
    EXPECT_OK(env_->DeleteDir(test_path_));
  }

  TableReaderCaller GetCaller(uint32_t key_id) {
    uint32_t n = key_id % 5;
    switch (n) {
      case 0:
        return TableReaderCaller::kPrefetch;
      case 1:
        return TableReaderCaller::kCompaction;
      case 2:
        return TableReaderCaller::kUserGet;
      case 3:
        return TableReaderCaller::kUserMultiGet;
      case 4:
        return TableReaderCaller::kUserIterator;
    }
    // This cannot happend.
    assert(false);
    return TableReaderCaller::kMaxBlockCacheLookupCaller;
  }

  void WriteBlockAccess(BlockCacheTraceWriter* writer, uint32_t from_key_id,
                        TraceType block_type, uint32_t nblocks) {
    assert(writer);
    for (uint32_t i = 0; i < nblocks; i++) {
      uint32_t key_id = from_key_id + i;
      uint64_t timestamp = (key_id + 1) * kMicrosInSecond;
      BlockCacheTraceRecord record;
      record.block_type = block_type;
      record.block_size = kBlockSize + key_id;
      record.block_key = kBlockKeyPrefix + std::to_string(key_id);
      record.access_timestamp = timestamp;
      record.cf_id = kCFId;
      record.cf_name = kDefaultColumnFamilyName;
      record.caller = GetCaller(key_id);
      record.level = kLevel;
      if (key_id % 2 == 0) {
        record.sst_fd_number = kSSTStoringEvenKeys;
      } else {
        record.sst_fd_number = kSSTStoringOddKeys;
      }
      record.is_cache_hit = Boolean::kFalse;
      record.no_insert = Boolean::kFalse;
      // Provide these fields for all block types.
      // The writer should only write these fields for data blocks and the
      // caller is either GET or MGET.
      record.referenced_key = kRefKeyPrefix + std::to_string(key_id);
      record.referenced_key_exist_in_block = Boolean::kTrue;
      record.num_keys_in_block = kNumKeysInBlock;
      ASSERT_OK(writer->WriteBlockAccess(
          record, record.block_key, record.cf_name, record.referenced_key));
    }
  }

  void AssertBlockAccessInfo(
      uint32_t key_id, TraceType type,
      const std::map<std::string, BlockAccessInfo>& block_access_info_map) {
    auto key_id_str = kBlockKeyPrefix + std::to_string(key_id);
    ASSERT_TRUE(block_access_info_map.find(key_id_str) !=
                block_access_info_map.end());
    auto& block_access_info = block_access_info_map.find(key_id_str)->second;
    ASSERT_EQ(1, block_access_info.num_accesses);
    ASSERT_EQ(kBlockSize + key_id, block_access_info.block_size);
    ASSERT_GT(block_access_info.first_access_time, 0);
    ASSERT_GT(block_access_info.last_access_time, 0);
    ASSERT_EQ(1, block_access_info.caller_num_access_map.size());
    TableReaderCaller expected_caller = GetCaller(key_id);
    ASSERT_TRUE(block_access_info.caller_num_access_map.find(expected_caller) !=
                block_access_info.caller_num_access_map.end());
    ASSERT_EQ(
        1,
        block_access_info.caller_num_access_map.find(expected_caller)->second);

    if ((expected_caller == TableReaderCaller::kUserGet ||
         expected_caller == TableReaderCaller::kUserMultiGet) &&
        type == TraceType::kBlockTraceDataBlock) {
      ASSERT_EQ(kNumKeysInBlock, block_access_info.num_keys);
      ASSERT_EQ(1, block_access_info.key_num_access_map.size());
      ASSERT_EQ(0, block_access_info.non_exist_key_num_access_map.size());
      ASSERT_EQ(1, block_access_info.num_referenced_key_exist_in_block);
    }
  }

  void RunBlockCacheTraceAnalyzer() {
    std::vector<std::string> params = {
        "./block_cache_trace_analyzer",
        "-block_cache_trace_path=" + trace_file_path_,
        "-block_cache_sim_config_path=" + block_cache_sim_config_path_,
        "-block_cache_analysis_result_dir=" + test_path_,
        "-print_block_size_stats",
        "-print_access_count_stats",
        "-print_data_block_access_count_stats",
        "-cache_sim_warmup_seconds=0",
        "-timeline_labels=" + timeline_labels_,
        "-reuse_distance_labels=" + reuse_distance_labels_,
        "-reuse_distance_buckets=" + reuse_distance_buckets_,
        "-reuse_interval_labels=" + reuse_interval_labels_,
        "-reuse_interval_buckets=" + reuse_interval_buckets_,
    };
    char arg_buffer[kArgBufferSize];
    char* argv[kMaxArgCount];
    int argc = 0;
    int cursor = 0;
    for (const auto& arg : params) {
      ASSERT_LE(cursor + arg.size() + 1, kArgBufferSize);
      ASSERT_LE(argc + 1, kMaxArgCount);
      snprintf(arg_buffer + cursor, arg.size() + 1, "%s", arg.c_str());

      argv[argc++] = arg_buffer + cursor;
      cursor += static_cast<int>(arg.size()) + 1;
    }
    ASSERT_EQ(0, rocksdb::block_cache_trace_analyzer_tool(argc, argv));
  }

  Env* env_;
  EnvOptions env_options_;
  std::string block_cache_sim_config_path_;
  std::string trace_file_path_;
  std::string test_path_;
  std::string timeline_labels_;
  std::string reuse_distance_labels_;
  std::string reuse_distance_buckets_;
  std::string reuse_interval_labels_;
  std::string reuse_interval_buckets_;
};

TEST_F(BlockCacheTracerTest, BlockCacheAnalyzer) {
  {
    // Generate a trace file.
    TraceOptions trace_opt;
    std::unique_ptr<TraceWriter> trace_writer;
    ASSERT_OK(NewFileTraceWriter(env_, env_options_, trace_file_path_,
                                 &trace_writer));
    BlockCacheTraceWriter writer(env_, trace_opt, std::move(trace_writer));
    ASSERT_OK(writer.WriteHeader());
    WriteBlockAccess(&writer, 0, TraceType::kBlockTraceDataBlock, 50);
    ASSERT_OK(env_->FileExists(trace_file_path_));
  }
  {
    // Generate a cache sim config.
    std::string config = "lru,1,0,1K,1M,1G";
    std::ofstream out(block_cache_sim_config_path_);
    ASSERT_TRUE(out.is_open());
    out << config << std::endl;
    out.close();
  }
  RunBlockCacheTraceAnalyzer();
  {
    // Validate the cache miss ratios.
    const std::vector<uint64_t> expected_capacities{1024, 1024 * 1024,
                                                    1024 * 1024 * 1024};
    const std::string mrc_path = test_path_ + "/mrc";
    std::ifstream infile(mrc_path);
    uint32_t config_index = 0;
    std::string line;
    // Read header.
    ASSERT_TRUE(getline(infile, line));
    while (getline(infile, line)) {
      std::stringstream ss(line);
      std::vector<std::string> result_strs;
      while (ss.good()) {
        std::string substr;
        getline(ss, substr, ',');
        result_strs.push_back(substr);
      }
      ASSERT_EQ(6, result_strs.size());
      ASSERT_LT(config_index, expected_capacities.size());
      ASSERT_EQ("lru", result_strs[0]);  // cache_name
      ASSERT_EQ("1", result_strs[1]);    // num_shard_bits
      ASSERT_EQ("0", result_strs[2]);    // ghost_cache_capacity
      ASSERT_EQ(std::to_string(expected_capacities[config_index]),
                result_strs[3]);              // cache_capacity
      ASSERT_EQ("100.0000", result_strs[4]);  // miss_ratio
      ASSERT_EQ("50", result_strs[5]);        // number of accesses.
      config_index++;
    }
    ASSERT_EQ(expected_capacities.size(), config_index);
    infile.close();
    ASSERT_OK(env_->DeleteFile(mrc_path));
  }
  {
    // Validate the timeline csv files.
    const uint32_t expected_num_lines = 50;
    std::stringstream ss(timeline_labels_);
    while (ss.good()) {
      std::string l;
      ASSERT_TRUE(getline(ss, l, ','));
      const std::string timeline_file =
          test_path_ + "/" + l + "_access_timeline";
      std::ifstream infile(timeline_file);
      std::string line;
      uint32_t nlines = 0;
      ASSERT_TRUE(getline(infile, line));
      uint64_t expected_time = 1;
      while (getline(infile, line)) {
        std::stringstream ss_naccess(line);
        uint32_t naccesses = 0;
        std::string substr;
        uint32_t time = 0;
        while (ss_naccess.good()) {
          ASSERT_TRUE(getline(ss_naccess, substr, ','));
          if (time == 0) {
            time = ParseUint32(substr);
            continue;
          }
          naccesses += ParseUint32(substr);
        }
        nlines++;
        ASSERT_EQ(1, naccesses);
        ASSERT_EQ(expected_time, time);
        expected_time += 1;
      }
      ASSERT_EQ(expected_num_lines, nlines);
      ASSERT_OK(env_->DeleteFile(timeline_file));
    }
  }
  {
    // Validate the reuse_interval and reuse_distance csv files.
    std::map<std::string, std::string> test_reuse_csv_files;
    test_reuse_csv_files["_reuse_interval"] = reuse_interval_labels_;
    test_reuse_csv_files["_reuse_distance"] = reuse_distance_labels_;
    for (auto const& test : test_reuse_csv_files) {
      const std::string& file_suffix = test.first;
      const std::string& labels = test.second;
      const uint32_t expected_num_rows = 10;
      const uint32_t expected_num_rows_absolute_values = 5;
      const uint32_t expected_reused_blocks = 0;
      std::stringstream ss(labels);
      while (ss.good()) {
        std::string l;
        ASSERT_TRUE(getline(ss, l, ','));
        const std::string reuse_csv_file = test_path_ + "/" + l + file_suffix;
        std::ifstream infile(reuse_csv_file);
        std::string line;
        ASSERT_TRUE(getline(infile, line));
        uint32_t nblocks = 0;
        double npercentage = 0;
        uint32_t nrows = 0;
        while (getline(infile, line)) {
          std::stringstream ss_naccess(line);
          bool label_read = false;
          nrows++;
          while (ss_naccess.good()) {
            std::string substr;
            ASSERT_TRUE(getline(ss_naccess, substr, ','));
            if (!label_read) {
              label_read = true;
              continue;
            }
            if (nrows < expected_num_rows_absolute_values) {
              nblocks += ParseUint32(substr);
            } else {
              npercentage += ParseDouble(substr);
            }
          }
        }
        ASSERT_EQ(expected_num_rows, nrows);
        ASSERT_EQ(expected_reused_blocks, nblocks);
        ASSERT_LT(npercentage, 0);
        ASSERT_OK(env_->DeleteFile(reuse_csv_file));
      }
    }
  }
  ASSERT_OK(env_->DeleteFile(block_cache_sim_config_path_));
}

TEST_F(BlockCacheTracerTest, MixedBlocks) {
  {
    // Generate a trace file containing a mix of blocks.
    // It contains two SST files with 25 blocks of odd numbered block_key in
    // kSSTStoringOddKeys and 25 blocks of even numbered blocks_key in
    // kSSTStoringEvenKeys.
    TraceOptions trace_opt;
    std::unique_ptr<TraceWriter> trace_writer;
    ASSERT_OK(NewFileTraceWriter(env_, env_options_, trace_file_path_,
                                 &trace_writer));
    BlockCacheTraceWriter writer(env_, trace_opt, std::move(trace_writer));
    ASSERT_OK(writer.WriteHeader());
    // Write blocks of different types.
    WriteBlockAccess(&writer, 0, TraceType::kBlockTraceUncompressionDictBlock,
                     10);
    WriteBlockAccess(&writer, 10, TraceType::kBlockTraceDataBlock, 10);
    WriteBlockAccess(&writer, 20, TraceType::kBlockTraceFilterBlock, 10);
    WriteBlockAccess(&writer, 30, TraceType::kBlockTraceIndexBlock, 10);
    WriteBlockAccess(&writer, 40, TraceType::kBlockTraceRangeDeletionBlock, 10);
    ASSERT_OK(env_->FileExists(trace_file_path_));
  }

  {
    // Verify trace file is generated correctly.
    std::unique_ptr<TraceReader> trace_reader;
    ASSERT_OK(NewFileTraceReader(env_, env_options_, trace_file_path_,
                                 &trace_reader));
    BlockCacheTraceReader reader(std::move(trace_reader));
    BlockCacheTraceHeader header;
    ASSERT_OK(reader.ReadHeader(&header));
    ASSERT_EQ(kMajorVersion, header.rocksdb_major_version);
    ASSERT_EQ(kMinorVersion, header.rocksdb_minor_version);
    // Read blocks.
    BlockCacheTraceAnalyzer analyzer(trace_file_path_,
                                     /*output_miss_ratio_curve_path=*/"",
                                     /*simulator=*/nullptr);
    // The analyzer ends when it detects an incomplete access record.
    ASSERT_EQ(Status::Incomplete(""), analyzer.Analyze());
    const uint64_t expected_num_cfs = 1;
    std::vector<uint64_t> expected_fds{kSSTStoringOddKeys, kSSTStoringEvenKeys};
    const std::vector<TraceType> expected_types{
        TraceType::kBlockTraceUncompressionDictBlock,
        TraceType::kBlockTraceDataBlock, TraceType::kBlockTraceFilterBlock,
        TraceType::kBlockTraceIndexBlock,
        TraceType::kBlockTraceRangeDeletionBlock};
    const uint64_t expected_num_keys_per_type = 5;

    auto& stats = analyzer.TEST_cf_aggregates_map();
    ASSERT_EQ(expected_num_cfs, stats.size());
    ASSERT_TRUE(stats.find(kDefaultColumnFamilyName) != stats.end());
    auto& cf_stats = stats.find(kDefaultColumnFamilyName)->second;
    ASSERT_EQ(expected_fds.size(), cf_stats.fd_aggregates_map.size());
    for (auto fd_id : expected_fds) {
      ASSERT_TRUE(cf_stats.fd_aggregates_map.find(fd_id) !=
                  cf_stats.fd_aggregates_map.end());
      ASSERT_EQ(kLevel, cf_stats.fd_aggregates_map.find(fd_id)->second.level);
      auto& block_type_aggregates_map = cf_stats.fd_aggregates_map.find(fd_id)
                                            ->second.block_type_aggregates_map;
      ASSERT_EQ(expected_types.size(), block_type_aggregates_map.size());
      uint32_t key_id = 0;
      for (auto type : expected_types) {
        ASSERT_TRUE(block_type_aggregates_map.find(type) !=
                    block_type_aggregates_map.end());
        auto& block_access_info_map =
            block_type_aggregates_map.find(type)->second.block_access_info_map;
        // Each block type has 5 blocks.
        ASSERT_EQ(expected_num_keys_per_type, block_access_info_map.size());
        for (uint32_t i = 0; i < 10; i++) {
          // Verify that odd numbered blocks are stored in kSSTStoringOddKeys
          // and even numbered blocks are stored in kSSTStoringEvenKeys.
          auto key_id_str = kBlockKeyPrefix + std::to_string(key_id);
          if (fd_id == kSSTStoringOddKeys) {
            if (key_id % 2 == 1) {
              AssertBlockAccessInfo(key_id, type, block_access_info_map);
            } else {
              ASSERT_TRUE(block_access_info_map.find(key_id_str) ==
                          block_access_info_map.end());
            }
          } else {
            if (key_id % 2 == 1) {
              ASSERT_TRUE(block_access_info_map.find(key_id_str) ==
                          block_access_info_map.end());
            } else {
              AssertBlockAccessInfo(key_id, type, block_access_info_map);
            }
          }
          key_id++;
        }
      }
    }
  }
}

}  // namespace rocksdb

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif  // GFLAG
#else
#include <stdio.h>
int main(int /*argc*/, char** /*argv*/) {
  fprintf(stderr,
          "block_cache_trace_analyzer_test is not supported in ROCKSDB_LITE\n");
  return 0;
}
#endif  // ROCKSDB_LITE
