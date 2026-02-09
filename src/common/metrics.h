#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace anycache {

// Simple metrics collection (Phase 6 extends to Prometheus)
class Metrics {
public:
  static Metrics &Instance() {
    static Metrics instance;
    return instance;
  }

  void IncrCounter(const std::string &name, int64_t delta = 1) {
    std::lock_guard<std::mutex> lock(mu_);
    counters_[name] += delta;
  }

  int64_t GetCounter(const std::string &name) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = counters_.find(name);
    return it != counters_.end() ? it->second : 0;
  }

  void SetGauge(const std::string &name, double value) {
    std::lock_guard<std::mutex> lock(mu_);
    gauges_[name] = value;
  }

  double GetGauge(const std::string &name) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = gauges_.find(name);
    return it != gauges_.end() ? it->second : 0.0;
  }

  void RecordLatency(const std::string &name, double ms) {
    std::lock_guard<std::mutex> lock(mu_);
    auto &h = histograms_[name];
    h.count++;
    h.sum += ms;
    if (ms < h.min)
      h.min = ms;
    if (ms > h.max)
      h.max = ms;
  }

  // Prometheus-compatible text format
  std::string ExportText() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::ostringstream oss;
    for (auto &[name, val] : counters_) {
      oss << "# TYPE " << name << " counter\n";
      oss << name << " " << val << "\n";
    }
    for (auto &[name, val] : gauges_) {
      oss << "# TYPE " << name << " gauge\n";
      oss << name << " " << val << "\n";
    }
    for (auto &[name, h] : histograms_) {
      double avg = h.count > 0 ? h.sum / h.count : 0;
      oss << "# TYPE " << name << " summary\n";
      oss << name << "_count " << h.count << "\n";
      oss << name << "_sum " << h.sum << "\n";
      oss << name << "_avg " << avg << "\n";
      oss << name << "_min " << h.min << "\n";
      oss << name << "_max " << h.max << "\n";
    }
    return oss.str();
  }

  void Reset() {
    std::lock_guard<std::mutex> lock(mu_);
    counters_.clear();
    gauges_.clear();
    histograms_.clear();
  }

private:
  Metrics() = default;

  struct HistogramData {
    int64_t count = 0;
    double sum = 0;
    double min = 1e18;
    double max = 0;
  };

  mutable std::mutex mu_;
  std::unordered_map<std::string, int64_t> counters_;
  std::unordered_map<std::string, double> gauges_;
  std::unordered_map<std::string, HistogramData> histograms_;
};

// RAII latency timer
class ScopedLatency {
public:
  ScopedLatency(const std::string &name)
      : name_(name), start_(std::chrono::steady_clock::now()) {}
  ~ScopedLatency() {
    auto end = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start_).count();
    Metrics::Instance().RecordLatency(name_, ms);
  }

private:
  std::string name_;
  std::chrono::steady_clock::time_point start_;
};

} // namespace anycache
