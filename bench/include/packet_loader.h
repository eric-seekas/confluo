#ifndef PACKETLOADER_H_
#define PACKETLOADER_H_

#include <chrono>
#include <sys/time.h>
#include <random>
#include <fstream>
#include <sstream>
#include <cstring>
#include <vector>
#include <string>
#include <atomic>
#include <thread>

#include "logstore.h"

using namespace ::slog;
using namespace ::std::chrono;

class packet_loader {
 public:
  typedef unsigned long long int timestamp_t;
  static const uint64_t kReportRecordInterval = 1000000;

  int64_t insert_packet() {
    uint64_t cur_id = id.fetch_add(1L);
    if (cur_id >= timestamps_.size()) {
      return -1L;
    }

    tokens tkns;
    tkns.time = (unsigned char*) (&timestamps_[cur_id]);
    tkns.src_ip = (unsigned char*) (&srcips_[cur_id]);
    tkns.dst_ip = (unsigned char*) (&dstips_[cur_id]);
    tkns.src_prt = (unsigned char*) (&sports_[cur_id]);
    tkns.dst_prt = (unsigned char*) (&dports_[cur_id]);

    return logstore_->insert(datas_[cur_id], datalens_[cur_id], tkns) + 1;
  }

  uint32_t parse_ip(std::string ip) {
    char ip_raw[4];
    sscanf(ip.c_str(), "%uhh.%uhh.%uhh.%uhh", &ip_raw[3], &ip_raw[2],
           &ip_raw[1], &ip_raw[0]);
    return ip_raw[0] | ip_raw[1] << 8 | ip_raw[2] << 16 | ip_raw[3] << 24;
  }

  packet_loader(std::string& data_path, std::string& attr_path,
                std::string& hostname);

  void load_data();

  // Throughput benchmarks
  void load_packets(const uint32_t num_clients, const uint64_t timebound);

 private:
  static timestamp_t get_timestamp() {
    struct timeval now;
    gettimeofday(&now, NULL);

    return now.tv_usec + (timestamp_t) now.tv_sec * 1000000;
  }

  std::string data_path_;
  std::string attr_path_;
  std::string hostname_;

  std::vector<uint32_t> timestamps_;
  std::vector<uint32_t> srcips_;
  std::vector<uint32_t> dstips_;
  std::vector<uint16_t> sports_;
  std::vector<uint16_t> dports_;
  std::vector<unsigned char*> datas_;
  std::vector<uint16_t> datalens_;

  log_store<UINT_MAX> *logstore_;
  std::atomic<uint64_t> id;
};

#endif /* RAMCLOUDBENCHMARK_H_ */