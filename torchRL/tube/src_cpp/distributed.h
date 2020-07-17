/**
 * Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "rpc.h"

#define ZSTD_STATIC_LINKING_ONLY
#include "zstd/lib/zstd.h"

#include <fmt/printf.h>
#include <torch/torch.h>

#include <cstring>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <type_traits>
#include <unordered_set>

namespace rpc {

template <typename X, typename A, typename B>
void serialize(X& x, const std::pair<A, B>& v) {
  x(v.first, v.second);
}

template <typename X, typename A, typename B>
void serialize(X& x, std::pair<A, B>& v) {
  x(v.first, v.second);
}

template <typename X, typename T>
void serialize(X& x, const std::optional<T>& v) {
  x(v.has_value());
  if (v.has_value()) {
    x(v.value());
  }
}

template <typename X, typename T> void serialize(X& x, std::optional<T>& v) {
  if (x.template read<bool>()) {
    v.emplace();
    x(v.value());
  } else {
    v.reset();
  }
}

template <typename X, typename T>
void serialize(X& x, const std::vector<T>& v) {
  x(v.size());
  for (auto& v2 : v) {
    x(v2);
  }
}

template <typename X, typename T> void serialize(X& x, std::vector<T>& v) {
  size_t n = x.template read<size_t>();
  v.resize(n);
  for (size_t i = 0; i != n; ++i) {
    x(v[i]);
  }
}

template <typename X, typename Key, typename Value>
void serialize(X& x, const std::unordered_map<Key, Value>& v) {
  x(v.size());
  for (auto& v2 : v) {
    x(v2.first, v2.second);
  }
}

template <typename X, typename Key, typename Value>
void serialize(X& x, std::unordered_map<Key, Value>& v) {
  v.clear();
  size_t n = x.template read<size_t>();
  for (; n; --n) {
    auto k = x.template read<Key>();
    v.emplace(std::move(k), x.template read<Value>());
  }
}

template <typename X> void serialize(X& x, const torch::Tensor& v) {
  std::ostringstream os;
  torch::save(v, os);
  x(os.str());
}

template <typename X> void serialize(X& x, torch::Tensor& v) {
  std::string_view s;
  x(s);
  std::string str(s.data(), s.size());
  std::istringstream is(str);
  torch::load(v, is);
}

}  // namespace rpc

namespace tube {

struct NetStats {
  bool hasData = false;
  double sent = 0.0;
  double received = 0.0;
  double rpcCalls = 0.0;
  double latency = 0.0;
  std::chrono::steady_clock::time_point lastprint{};
  std::mutex m;
};
inline NetStats netstats;

struct NetStatsCounter {
  std::chrono::steady_clock::time_point timestamp{};
  size_t sent = 0;
  size_t received = 0;
  size_t rpcCalls = 0;
};

template <typename T>
void addnetworkstats(const T& obj, NetStatsCounter& counter) {
  auto now = std::chrono::steady_clock::now();
  auto elapsed = now - counter.timestamp;
  if (elapsed <
      (netstats.hasData ? std::chrono::seconds(1) : std::chrono::seconds(10))) {
    return;
  }
  std::unique_lock l(netstats.m, std::try_to_lock);
  if (!l.owns_lock()) {
    return;
  }
  counter.timestamp = now;
  double t = std::chrono::duration_cast<
                 std::chrono::duration<double, std::ratio<1, 1>>>(elapsed)
                 .count();
  size_t newSent = obj.bytesSent();
  size_t newReceived = obj.bytesReceived();
  size_t newCalls = obj.numRpcCalls();
  double sent = (newSent - std::exchange(counter.sent, newSent)) / t;
  double recv =
      (newReceived - std::exchange(counter.received, newReceived)) / t;
  double calls = (newCalls - std::exchange(counter.rpcCalls, newCalls)) / t;

  double alpha = std::pow(0.99, t);
  if (!netstats.hasData) {
    alpha = 0.0;
    netstats.hasData = true;
  }
  netstats.sent = netstats.sent * alpha + sent * (1.0 - alpha);
  netstats.received = netstats.received * alpha + recv * (1.0 - alpha);
  netstats.rpcCalls = netstats.rpcCalls * alpha + calls * (1.0 - alpha);

  constexpr bool haslatency = std::is_same_v<T, rpc::Client>;
  if constexpr (haslatency) {
    double ll = std::chrono::duration_cast<
                    std::chrono::duration<double, std::ratio<1, 1000>>>(
                    obj.lastLatency())
                    .count();
    netstats.latency = netstats.latency * alpha + ll * (1.0 - alpha);
  }

  if (now - netstats.lastprint >= std::chrono::seconds(60)) {
    netstats.lastprint = now;
    if (haslatency) {
      printf("Network stats: in: %.02fM/s out: %.02fM/s  RPC calls: %.02f/s "
             "latency: %.02fms\n",
             netstats.received / 1024 / 1024,
             netstats.sent / 1024 / 1024,
             netstats.rpcCalls,
             netstats.latency);
    } else {
      printf("Network stats: in: %.02fM/s out: %.02fM/s  RPC calls: %.02f/s\n",
             netstats.received / 1024 / 1024,
             netstats.sent / 1024 / 1024,
             netstats.rpcCalls);
    }
  }
}

inline rpc::Rpc& getRpc() {
  static std::unique_ptr<rpc::Rpc> rpc = []() {
    auto rpc = std::make_unique<rpc::Rpc>();
    rpc->asyncRun(40);
    return rpc;
  }();
  return *rpc;
}

class DistributedServer {

  std::shared_ptr<rpc::Server> server;

  std::minstd_rand rng{std::random_device()()};

  float rollChance(std::string_view id) {
    auto i = models.find(id);
    if (i == models.end()) {
      return 0.0f;
    }
    float rating = i->second.rating;
    float max = 0.0f;
    std::vector<std::pair<float, std::string_view>> sorted;
    for (auto& [id, m] : models) {
      sorted.emplace_back(m.rating, id);
      max = std::max(max, m.rating);
    }
    std::sort(sorted.begin(), sorted.end(), std::greater<>());
    float lo = 1.0f;
    float ret = 0.0f;
    for (size_t i = 0; i != sorted.size(); ++i) {
      auto [r, n] = sorted[i];
      float x = r - max;
      float o =
          x == 0 ? 1.0f : std::min(std::log(1 - (2.0f * 200) / x) / 4, 1.0f);
      if (r < rating) {
        ret += (lo - o) / i;
      }
      lo = o;
    }
    ret += lo / sorted.size();
    return ret;
  }

  std::string_view sampleModelId() {
    if (models.empty() ||
        std::uniform_real_distribution<double>(0.0, 1.0)(rng) < 0.5) {
      return "dev";
    }
    if (std::uniform_real_distribution<double>(0.0, 1.0)(rng) < 0.01) {
      auto it = models.begin();
      std::advance(
          it, std::uniform_int_distribution<size_t>(0, models.size() - 1)(rng));
      return it->first;
    }

    float max = 0.0f;
    for (auto& [id, m] : models) {
      max = std::max(max, m.rating);
    }
    double x = std::uniform_real_distribution<double>(0.0, 1.0)(rng);
    double target = -(2.0f / (std::exp(x * 4) - 1)) * 200;
    std::vector<std::string_view> pool;
    for (auto& [id, m] : models) {
      double diff = m.rating - max;
      if (diff >= target) {
        pool.push_back(id);
      }
    }
    if (!pool.empty()) {
      return pool.at(
          std::uniform_int_distribution<size_t>(0, pool.size() - 1)(rng));
    }
    return "dev";
  }

  std::chrono::steady_clock::time_point lastRatingPrint =
      std::chrono::steady_clock::now();

  void addResult(std::string_view id, float ratio, float reward) {
    if (ratio < 0.9f) {
      return;
    }
    auto i = models.find(id);
    if (i == models.end()) {
      return;
    }
    auto di = models.find("dev");
    if (di == models.end()) {
      return;
    }

    if (i == di) {
      return;
    }

    float rating = i->second.rating;
    float devrating = di->second.rating;

    auto calc = [&](float reward, float diff) {
      float k = 6;
      float scale = 400;
      float offset = 0.5f;
      if (reward > 0) {
        offset = 1.0f;
      } else if (reward < 0) {
        offset = 0.0f;
      }
      return k * (offset - 1.0 / (1.0 + std::pow(10.0f, diff / scale)));
    };

    float delta = calc(reward, devrating - rating) * ratio;
    float delta2 = calc(-reward, rating - devrating) * ratio;

    rating += delta;
    devrating += delta2;

    i->second.rating = rating;
    di->second.rating = devrating;

    ++i->second.ngames;
    ++di->second.ngames;

    i->second.rewardsum += reward;
    di->second.rewardsum -= reward;

    i->second.avgreward = i->second.rewardsum / i->second.ngames;
    di->second.avgreward = di->second.rewardsum / di->second.ngames;

    auto now = std::chrono::steady_clock::now();
    if (now - lastRatingPrint >= std::chrono::seconds(120)) {
      lastRatingPrint = now;
      std::vector<std::pair<float, std::string_view>> sorted;
      for (auto& [id, m] : models) {
        sorted.emplace_back(m.rating, id);

        m.curgames = m.ngames - m.prevngames;
        m.curreward = (m.rewardsum - m.prevrewardsum) / m.curgames;

        m.prevngames = m.ngames;
        m.prevrewardsum = m.rewardsum;
      }
      std::sort(sorted.begin(), sorted.end(), std::greater<>());
      int devrank = 0;
      float devrating = 0;
      for (size_t i = 0; i != sorted.size(); ++i) {
        if (sorted[i].second == "dev") {
          devrank = (int)i + 1;
          devrating = sorted[i].first;
          break;
        }
      }
      if (sorted.size() > 20) {
        sorted.resize(20);
      }
      std::string str;
      int rank = 1;
      auto stringify = [&](int rank, float rating, std::string_view id) {
        return fmt::sprintf("%d. %g %s (roll chance %f) (total %d games, %f "
                            "avg reward) (diff %d games, %f avg reward)\n",
                            rank,
                            rating,
                            id,
                            rollChance(id),
                            models[id].ngames,
                            models[id].avgreward,
                            models[id].curgames,
                            models[id].curreward);
      };
      for (auto& [rating, id] : sorted) {
        str += stringify(rank, rating, id);
        ++rank;
      }
      if (devrank > 20) {
        str += stringify(devrank, devrating, "dev");
      }
      fmt::printf("Top 20:\n%s", str);
    }
  }

  std::pair<std::string_view, int> requestModel(bool wantsNewModelId,
                                                std::string_view modelId) {
    std::unique_lock l(mut);
    if (wantsNewModelId) {
      modelId = sampleModelId();
    }
    int version = -1;
    auto i = models.find(modelId);
    if (i == models.end()) {
      modelId = "dev";
      i = models.find(modelId);
    }
    if (i != models.end()) {
      version = i->second.version;
    } else {
      version = -1;
    }
    addnetworkstats(*server, netstatsCounter);
    return {modelId, version};
  }

  std::optional<std::unordered_map<std::string, torch::Tensor>>
  requestStateDict(std::string_view modelId) {
    std::unique_lock l(mut);
    auto i = models.find(modelId);
    if (i == models.end()) {
      return {};
    } else {
      return i->second.stateDict;
    }
  }

  std::optional<std::vector<char>>
  requestCompressedStateDict(std::string_view modelId) {
    std::unique_lock l(mut);
    auto i = models.find(modelId);
    if (i == models.end()) {
      return {};
    } else {
      if (i->second.compressedStateDict.empty()) {
        for (int n = 0; n != 500 && i->second.compressing.exchange(true); ++n) {
          l.unlock();
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
          l.lock();
          i = models.find(modelId);
          if (i == models.end()) {
            return {};
          }
          if (!i->second.compressedStateDict.empty()) {
            return i->second.compressedStateDict;
          }
        }
        auto copy = i->second.stateDict;
        l.unlock();
        auto start = std::chrono::steady_clock::now();
        rpc::Serializer s;
        rpc::Serialize ser(s);
        ser(i->second.stateDict);
        auto now = std::chrono::steady_clock::now();
        double t1 = std::chrono::duration_cast<
                       std::chrono::duration<double, std::ratio<1, 1000>>>(now - start)
                       .count();
        start = now;
        size_t oldsize = s.size();
        s.compress(15);
        size_t newsize = s.size();
        s.buf.shrink_to_fit();
        now = std::chrono::steady_clock::now();
        double t2 = std::chrono::duration_cast<
                       std::chrono::duration<double, std::ratio<1, 1000>>>(now - start)
                       .count();
        start = now;

        fmt::printf("State dict serialized in %gms, compressed (from %gM to %gM) in %gms\n", t1, oldsize / 1024.0 / 1024.0, newsize / 1024.0 / 1024.0, t2);

        l.lock();
        i = models.find(modelId);
        if (i == models.end()) {
          return {};
        }
        i->second.compressedStateDict = std::move(s.buf);
        i->second.compressing = false;
      }
      return i->second.compressedStateDict;
    }
  }

  void trainData(std::string_view data) {
    onTrainData(data.data(), data.size());
  }

  void gameResult(
      std::vector<std::pair<float, std::unordered_map<std::string_view, float>>>
          result) {
    std::lock_guard l(mut);
    for (auto& [reward, models] : result) {
      for (auto& [id, ratio] : models) {
        addResult(id, ratio, reward);
      }
    }
  }

  struct ModelInfo {
    std::string id;
    int version = 0;
    float rating = 0.0f;
    std::unordered_map<std::string, torch::Tensor> stateDict;
    std::vector<char> compressedStateDict;
    std::atomic<bool> compressing{false};
    uint64_t ngames = 0;
    double rewardsum = 0.0;
    float avgreward = 0.0f;

    uint64_t prevngames = 0;
    double prevrewardsum = 0.0;

    uint64_t curgames = 0;
    float curreward = 0.0f;

    double rollChance = 0.0;
  };

  std::mutex mut;
  std::unordered_map<std::string_view, ModelInfo> models;

  std::mutex timemut;
  std::unordered_map<std::string, float> calltimes;
  std::chrono::steady_clock::time_point lasttimereport;

 public:
  std::function<void(const void* data, size_t len)> onTrainData;
  NetStatsCounter netstatsCounter;

  template<typename R, typename... Args>
  auto define(std::string name, R (DistributedServer::*f)(Args...)) {
    server->define(name, std::function<R(Args...)>([this, f, name](Args&&... args) {
      auto begin = std::chrono::steady_clock::now();
      auto finish = [&]() {
        auto end = std::chrono::steady_clock::now();
        double t = std::chrono::duration_cast<
                       std::chrono::duration<double, std::ratio<1, 1000>>>(end - begin)
                       .count();
        {
          std::lock_guard l(timemut);
          auto i = calltimes.find(name);
          if (i == calltimes.end()) {
            i = calltimes.emplace(name, t).first;
          }
          float& v = i->second;
          v = v * 0.99 + t * 0.01;

          if (end - lasttimereport >= std::chrono::seconds(60)) {
            lasttimereport = end;
            std::string s = "RPC call times (running average):\n";
            for (auto& v : calltimes) {
              s += fmt::sprintf("  %s  %fms\n", v.first, v.second);
            }
            fmt::printf("%s", s);
          }
        }
      };
      if constexpr (std::is_same_v<R, void>) {
        (this->*f)(std::forward<Args>(args)...);
        finish();
      } else {
        auto rv = (this->*f)(std::forward<Args>(args)...);
        finish();
        return rv;
      }
    }));
  }

  void start(std::string_view endpoint) {
    if (endpoint.substr(0, 6) == "tcp://") {
      endpoint.remove_prefix(6);
    }
    printf("actual listen endpoint is %s\n", std::string(endpoint).c_str());
    server = getRpc().listen("");

    define("requestModel", &DistributedServer::requestModel);
    define("requestStateDict", &DistributedServer::requestStateDict);
    define("requestCompressedStateDict", &DistributedServer::requestCompressedStateDict);
    define("trainData", &DistributedServer::trainData);
    define("gameResult", &DistributedServer::gameResult);

    server->listen(endpoint);
  }

  void updateModel(const std::string& id,
                   std::unordered_map<std::string, torch::Tensor> stateDict) {
    std::unique_lock l(mut);
    auto i = models.try_emplace(id);
    if (i.second) {
      i.first->second.id = id;
      (std::string_view&)i.first->first = i.first->second.id;
      auto idev = models.find("dev");
      if (idev != models.end()) {
        i.first->second.rating = idev->second.rating;
      }
    }
    auto& m = i.first->second;
    m.stateDict = std::move(stateDict);
    ++m.version;
    m.compressedStateDict.clear();
  }
};

class DistributedClient {

  std::shared_ptr<rpc::Client> client;

  mutable std::mutex mut;
  std::unordered_set<std::string> allModelIds;
  std::string_view currentModelId = *allModelIds.emplace("dev").first;
  int currentModelVersion = -1;
  int gamesDoneWithCurrentModel = 0;
  bool wantsNewModelId = false;
  bool wantsTournamentResult_ = false;

  std::chrono::steady_clock::time_point lastCheckTournamentResult = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point lastTournamentResult = std::chrono::steady_clock::now();

  std::vector<std::pair<float, std::unordered_map<std::string_view, float>>>
      resultQueue;

  NetStatsCounter netstatsCounter;

  void requestModelStateDict() {
    try {
      std::unique_lock l(mut);
      auto modelId = currentModelId;
      auto result = client->async<
          std::optional<std::vector<char>>>(
          "requestCompressedStateDict", modelId);
      l.unlock();
      auto compressed = result.get();
      addnetworkstats(*client, netstatsCounter);
      if (!compressed) {
        l.lock();
        currentModelId = "dev";
        currentModelVersion = -1;
      } else {
        std::unordered_map<std::string, torch::Tensor> stateDict;
        rpc::Deserializer d(compressed->data(), compressed->size());
        d.decompress();
        rpc::Deserialize des(d);
        des(stateDict);
        onUpdateModel(modelId, stateDict);
      }
    } catch (const rpc::RPCException& e) {
      fmt::printf("RPC exception: %s\n", e.what());
    }
  }

 public:
  std::function<void(
      std::string_view, std::unordered_map<std::string, torch::Tensor>)>
      onUpdateModel;

  void requestModel(bool isTournamentOpponent) {
    try {
      std::unique_lock l(mut);
      if (!resultQueue.empty()) {
        client->async("gameResult", resultQueue);
        resultQueue.clear();
      }

      fmt::printf("Request model, isTournamentOpponent %d, wantsNewModelId %d\n", isTournamentOpponent, wantsNewModelId);

      auto result = client->async<std::pair<std::string, int>>(
          "requestModel",
          isTournamentOpponent ? std::exchange(wantsNewModelId, false) : false,
          currentModelId);
      l.unlock();

      auto [newId, version] = result.get();
      addnetworkstats(*client, netstatsCounter);

      fmt::printf("Got model '%s'\n", newId);

      l.lock();
      auto now = std::chrono::steady_clock::now();
      if (isTournamentOpponent && now - lastCheckTournamentResult >= std::chrono::minutes(2)) {
        lastCheckTournamentResult = now;
        wantsTournamentResult_ = now - lastTournamentResult >= std::chrono::minutes(5);
        if (!wantsTournamentResult_) {
          wantsNewModelId = true;
        }
        fmt::printf("wantsTournamentResult_ is %d, wantsNewModelId is %d\n", wantsTournamentResult_, wantsNewModelId);
      } else if (!isTournamentOpponent) {
        wantsTournamentResult_ = false;
      }
      if (currentModelId != newId) {
        currentModelId = *allModelIds.emplace(newId).first;
        currentModelVersion = -1;
        gamesDoneWithCurrentModel = 0;
      }
      if (version != currentModelVersion) {
        currentModelVersion = version;
        l.unlock();
        requestModelStateDict();
      } else {
        l.unlock();
      }
    } catch (const rpc::RPCException& e) {
      fmt::printf("RPC exception: %s\n", e.what());
    }
  }

  void connect(std::string_view endpoint) {
    if (endpoint.substr(0, 6) == "tcp://") {
      endpoint.remove_prefix(6);
    }
    printf("actual connect endpoint is %s\n", std::string(endpoint).c_str());
    client = getRpc().connect(endpoint);
  }

  void sendTrainData(const void* data, size_t len) {
    try {
      client->sync("trainData", std::string_view((const char*)data, len));
    } catch (const rpc::RPCException& e) {
      fmt::printf("RPC exception: %s\n", e.what());
    }
  }

  void sendResult(float reward,
                  std::unordered_map<std::string_view, float> models) {
    std::unique_lock l(mut);
    fmt::printf("sendResult %g\n", reward);
    for (auto& [k, v] : models) {
      fmt::printf("  model '%s' %g\n", k, v);
    }
    auto i = models.find(currentModelId);
    if (i != models.end()) {
      if (i->second >= 0.9f) {
        ++gamesDoneWithCurrentModel;
        printf("gamesDoneWithCurrentModel is now %d\n", gamesDoneWithCurrentModel);
        if (gamesDoneWithCurrentModel >= 20) {
          lastTournamentResult = std::chrono::steady_clock::now();
          wantsNewModelId = true;
        }
      }
    }
    resultQueue.emplace_back(reward, std::move(models));
  }

  bool wantsTournamentResult() const {
    std::unique_lock l(mut);
    return wantsTournamentResult_;
  }

  std::string_view getModelId() const {
    std::unique_lock l(mut);
    return currentModelId;
  }
};

}  // namespace tube
