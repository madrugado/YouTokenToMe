#include <utility>

#include "bpe.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "third_party/flat_hash_map.h"
#include "utf8.h"
#include "utils.h"

namespace vkcom {
using std::string;
using std::vector;

struct VectorSegment {
  constexpr static uint64_t MOD = 2032191299;
  constexpr static uint64_t P = 726328703;

  vector<uint32_t>::const_iterator begin, end;
  uint64_t hash;

  VectorSegment(vector<uint32_t>::const_iterator begin,
                vector<uint32_t>::const_iterator end)
      : begin(begin), end(end) {
    hash = 0;
    for (auto it = begin; it != end; it++) {
      hash = (hash * P + *it) % MOD;
    }
  }

  bool operator==(const VectorSegment &other) const {
    if (other.hash != hash || end - begin != other.end - other.begin) {
      return false;
    }
    for (auto it = begin, other_it = other.begin; it != end; it++, other_it++) {
      if (*it != *other_it) {
        return false;
      }
    }
    return true;
  }
};

}  // namespace vkcom

namespace std {
template <>
struct hash<vkcom::VectorSegment> {
  size_t operator()(const vkcom::VectorSegment &x) const { return x.hash; }
};
}  // namespace std

namespace vkcom {

string fast_read_file_utf8(const string &file_name) {
  static const int buf_size = 1000000;
  string res;
  auto fin = fopen(file_name.data(), "rb");
  if (fin == nullptr) {
    std::cerr << "Error. Failed to open file: " + file_name << std::endl;
    exit(EXIT_FAILURE);
  }
  while (true) {
    size_t cur_size = res.size();
    res.resize(cur_size + buf_size);
    int buf_len = fread((void *)(res.data() + cur_size), 1, buf_size, fin);
    if (buf_len < buf_size) {
      res.resize(res.size() - (buf_size - buf_len));
      fclose(fin);
      return res;
    }
  }
}

string token2word(const vector<uint32_t> &source,
                  const ska::flat_hash_map<uint32_t, uint32_t> &id2char) {
  vector<uint32_t> res;
  for (int i : source) {
    assert(id2char.count(i) == 1);
    res.push_back(id2char.at(i));
  }
  return encode_utf8(res);
}

bool is_space(uint32_t ch) {
  return (ch < 256 && isspace(ch)) || (ch == SPACE_TOKEN);
}

uint64_t int2comb(uint32_t a, uint32_t b) {
  return (static_cast<uint64_t>(a) << 32u) + b;
}

struct MergeCandidate {
  size_t count{0};
  uint32_t left_token{0};
  uint32_t right_token{0};

  MergeCandidate() = default;

  MergeCandidate(size_t count, uint32_t left_token, uint32_t right_token)
      : count(count), left_token(left_token), right_token(right_token) {}

  bool operator<(const MergeCandidate &other) const {
    if (count != other.count) {
      return count < other.count;
    }
    auto this_mn = std::min(left_token, right_token);
    auto this_mx = std::max(left_token, right_token);

    auto other_mn = std::min(other.left_token, other.right_token);
    auto other_mx = std::max(other.left_token, other.right_token);
    if (this_mx != other_mx) {
      return this_mx > other_mx;
    }
    if (this_mn != other_mn) {
      return this_mn > other_mn;
    }
    return left_token < other.left_token;
  }
};

struct Position {
  size_t word_id, pos_id;

  Position(size_t word_id, size_t pos_id) : word_id(word_id), pos_id(pos_id) {}

  bool operator<(const Position &other) const {
    return word_id < other.word_id ||
           (word_id == other.word_id && pos_id < other.pos_id);
  }
};

int pairsInSeg(int x) {
  assert(x >= 2);
  return x / 2;
}

struct PositionsCnt {
  vector<Position> positions;
  size_t cnt;
};

bool rule_intersection(BPE_Rule rule, uint32_t new_left, uint32_t new_right) {
  return rule.y == new_left || rule.x == new_right;
}

struct SmallObjectQueue {
  vector<vector<MergeCandidate>> queue;
  bool flag_started{false};
  size_t _size{0};

  SmallObjectQueue() = default;

  void push(const MergeCandidate &event) {
    if (queue.size() <= event.count) {
      queue.resize(event.count + 1);
    }
    if (flag_started) {
      assert(event.count + 1 <= queue.size());
    };
    queue[event.count].push_back(event);
    _size++;
  }

  void process_empty_slots() {
    flag_started = true;

    while (!queue.empty() && queue.back().empty()) {
      queue.pop_back();
    }
  }

  bool empty() {
    process_empty_slots();
    return queue.empty();
  }

  MergeCandidate top() {
    process_empty_slots();
    assert(!queue.empty());
    return queue.back().back();
  }

  void pop() {
    assert(!queue.empty());
    assert(!queue.back().empty());
    queue.back().pop_back();
    _size--;
  }

  size_t size() const { return _size; }
};

struct BigObjectQueue {
  vector<MergeCandidate> big_events;
  size_t big_event_bound;

  BigObjectQueue(size_t big_event_bound) : big_event_bound(big_event_bound) {}

  void push(const MergeCandidate &event) { big_events.push_back(event); }

  bool empty() const { return big_events.empty(); }

  bool top(std::function<size_t(uint64_t)> &check_cnt, MergeCandidate &ret,
           SmallObjectQueue *small_object_queue, BPE_Rule last_rule) {
    for (size_t i = 0; i < big_events.size();) {
      if (!rule_intersection(last_rule, big_events[i].left_token,
                             big_events[i].right_token)) {
        uint64_t comb =
            int2comb(big_events[i].left_token, big_events[i].right_token);
        assert(big_events[i].count >= check_cnt(comb));
        big_events[i].count = check_cnt(comb);
      }

      if (big_events[i].count < big_event_bound) {
        small_object_queue->push(big_events[i]);
        big_events[i] = big_events.back();
        big_events.pop_back();
      } else {
        i++;
      }
    }
    for (auto &big_event : big_events) {
      if (big_event.count > big_events.back().count) {
        std::swap(big_event, big_events.back());
      }
    }

    if (big_events.empty()) {
      return false;
    }
    ret = big_events.back();
    return true;
  }

  void pop() {
    assert(!big_events.empty());
    big_events.pop_back();
  }

  size_t size() const { return big_events.size(); }
};

struct PriorityQueue {
  SmallObjectQueue small_queue;
  BigObjectQueue big_queue;
  size_t big_event_bound;

  explicit PriorityQueue(size_t dataset_size)
      : big_queue(static_cast<size_t>(sqrt(dataset_size))),
        big_event_bound(static_cast<size_t>(sqrt(dataset_size))) {}

  void push(const MergeCandidate &event) {
    if (event.count == 0) {
      return;
    }
    if (event.count < big_event_bound) {
      small_queue.push(event);
    } else {
      big_queue.push(event);
    }
  }

  bool empty() {
    return big_queue.empty() && small_queue.empty();
  }

  MergeCandidate top(std::function<size_t(uint64_t)> &check_cnt,
                     BPE_Rule last_rule) {
    MergeCandidate res;
    bool has_top = big_queue.top(check_cnt, res, &small_queue, last_rule);
    if (has_top) {
      return res;
    }
    return small_queue.top();
  }

  void pop() {
    if (!big_queue.empty()) {
      big_queue.pop();
    } else {
      small_queue.pop();
    }
  }

  size_t size() const { return big_queue.size() + small_queue.size(); }
};

ska::flat_hash_map<uint32_t, uint32_t> compute_alphabet_helper(
    const ska::flat_hash_map<uint32_t, size_t> &char_cnt, size_t data_len,
    ska::flat_hash_set<uint32_t> &removed_chars, const BpeConfig &bpe_config) {
  vector<std::pair<size_t, uint32_t>> frequencies;

  for (auto x : char_cnt) {
    frequencies.emplace_back(x.second, x.first);
  }
  sort(frequencies.begin(), frequencies.end());

  size_t cur = 0;
  size_t n_removed = 0;
  for (; cur < frequencies.size() &&
         (data_len - n_removed - frequencies[cur].first) >
             data_len * bpe_config.character_coverage;
       cur++) {
    n_removed += frequencies[cur].first;
  }
  std::cerr << "number of unique characters in the training data: "
            << frequencies.size() << std::endl;
  std::cerr << "number of deleted characters: " << cur << std::endl;
  std::cerr << "number of unique characters left: " << frequencies.size() - cur
            << std::endl;

  ska::flat_hash_map<uint32_t, uint32_t> char2id;
  size_t used_ids = bpe_config.special_tokens.n_special_tokens();
  char2id[SPACE_TOKEN] = used_ids++;

  for (size_t i = 0; i < cur; i++) {
    removed_chars.insert(frequencies[i].second);
  }

  for (int i = frequencies.size() - 1; i >= static_cast<int>(cur); i--) {
    if (!is_space(frequencies[i].second)) {
      assert(char2id.count(frequencies[i].second) == 0);
      char2id[frequencies[i].second] = used_ids++;
    }
  }
  return char2id;
}

void remove_rare_chars(vector<uint32_t> &data,
                       const ska::flat_hash_set<uint32_t> &removed_chars) {
  if (removed_chars.empty()) {
    return;
  }
  auto it_first_rare_char =
      std::remove_if(data.begin(), data.end(),
                     [&](uint32_t c) { return removed_chars.count(c) != 0; });
  data.erase(it_first_rare_char, data.end());
}

struct WordCount {
  vector<uint32_t> word;
  size_t cnt;
};

ska::flat_hash_map<VectorSegment, WordCount> compute_word_count_helper(
    const vector<uint32_t> &row_data,
    const ska::flat_hash_map<uint32_t, uint32_t> &char2id) {
  ska::flat_hash_map<VectorSegment, WordCount> hash2wordcnt;
  vector<uint32_t> word;

  for (auto loop_it = row_data.begin(); loop_it != row_data.end();) {
    auto begin_of_word = std::find_if_not(loop_it, row_data.end(), is_space);
    if (begin_of_word == row_data.end()) {
      break;
    }
    auto end_of_word = std::find_if(begin_of_word, row_data.end(), is_space);

    VectorSegment word_hash(begin_of_word, end_of_word);
    auto it = hash2wordcnt.find(word_hash);
    if (it == hash2wordcnt.end()) {
      word.clear();
      word.push_back(char2id.at(SPACE_TOKEN));
      std::transform(begin_of_word, end_of_word, std::back_inserter(word),
                     [&](uint32_t ch) { return char2id.at(ch); });
      hash2wordcnt[word_hash] = {word, 1};
    } else {
      it->second.cnt++;
    }
    loop_it = end_of_word;
  }
  return hash2wordcnt;
}

vector<WordCount> compute_word_count(
    const vector<uint32_t> &row_data,
    const ska::flat_hash_map<uint32_t, uint32_t> &char2id) {
  auto hash2wordcnt = compute_word_count_helper(row_data, char2id);
  vector<WordCount> word_cnt(hash2wordcnt.size());
  std::transform(
      hash2wordcnt.begin(), hash2wordcnt.end(), word_cnt.begin(),
      [](const std::pair<VectorSegment, WordCount> &x) { return x.second; });
  return word_cnt;
}

struct NodeEncoder {
  uint32_t val;
  int prev;
  int next;
  int seg_len;

  NodeEncoder(uint32_t val, int prev, int next, int seg_len)
      : val(val), prev(prev), next(next), seg_len(seg_len) {}

  bool is_alive() const {
    assert((val == 0) == (seg_len == 0));
    return val != 0;
  }
};

void build_linked_list(const vector<WordCount> &word_cnt,
                       vector<vector<NodeEncoder>> &list,
                       ska::flat_hash_map<uint64_t, vector<Position>> &pair2pos,
                       ska::flat_hash_map<uint64_t, size_t> &pair2cnt) {
  list.resize(word_cnt.size());
  for (size_t i = 0; i < word_cnt.size(); i++) {
    for (uint32_t ch : word_cnt[i].word) {
      if (!list[i].empty() && list[i].back().val == ch) {
        list[i].back().seg_len++;
      } else {
        int list_size = list[i].size();
        list[i].emplace_back(ch, list_size - 1, list_size + 1, 1);
      }
    }

    list[i].back().next = -1;
    for (size_t j = 0; j < list[i].size(); j++) {
      if (j + 1 < list[i].size()) {
        uint64_t comb = int2comb(list[i][j].val, list[i][j + 1].val);
        auto it = pair2pos.find(comb);
        if (it == pair2pos.end()) {
          pair2pos[comb] = {{i, j}};
        } else {
          it->second.emplace_back(i, j);
        }
        pair2cnt[comb] += word_cnt[i].cnt;
      }
      assert(list[i][j].seg_len >= 1);

      if (list[i][j].seg_len > 1) {
        uint64_t comb = int2comb(list[i][j].val, list[i][j].val);
        auto it = pair2pos.find(comb);
        size_t cc = word_cnt[i].cnt * pairsInSeg(list[i][j].seg_len);
        if (it == pair2pos.end()) {
          pair2pos[comb] = {{i, j}};
        } else {
          it->second.emplace_back(i, j);
        }
        pair2cnt[comb] += cc;
      }
    }
  }
}

std::chrono::steady_clock::time_point last_time_stamp;

void time_check(const string &message) {
  auto cur_moment = std::chrono::steady_clock::now();
  if (!message.empty()) {
    std::cerr << "## time " << message << " ... "
              << std::chrono::duration_cast<std::chrono::microseconds>(
                     cur_moment - last_time_stamp)
                         .count() *
                     1.0 / 1e6
              << std::endl;
  }
  last_time_stamp = cur_moment;
}

double time_check_silent() {
  auto cur_moment = std::chrono::steady_clock::now();
  double ret = std::chrono::duration_cast<std::chrono::microseconds>(
                   cur_moment - last_time_stamp)
                   .count() *
               1.0 / 1e6;
  last_time_stamp = cur_moment;
  return ret;
}

int alive_tokens;

void init_recipe(const ska::flat_hash_map<uint32_t, uint32_t> &char2id,
                 ska::flat_hash_map<uint32_t, vector<uint32_t>> &recipe,
                 ska::flat_hash_map<uint32_t, string> &recipe_s) {
  for (auto token_id : char2id) {
    uint32_t ch = token_id.first;
    uint32_t id = token_id.second;
    recipe[id] = {id};
    recipe_s[id] = encode_utf8({ch});
  }
}

void worker_doing_merge(
    size_t thread_id, vector<vector<NodeEncoder>> &lists_of_tokens,
    vector<ska::flat_hash_map<uint64_t, size_t>> &pair2cnt_g,
    ska::flat_hash_map<uint64_t, vector<Position>> &pair2pos,
    vector<size_t> &word_freq, vector<std::mutex> &mt,
    vector<std::condition_variable> &cv, vector<BPE_Rule> &task_order,
    vector<std::atomic_bool> &thread_use_hs,
    ska::flat_hash_map<uint32_t, uint32_t> &char2id,
    vector<vector<ska::flat_hash_map<uint32_t, size_t>>> &left_tokens_submit,
    vector<vector<ska::flat_hash_map<uint32_t, size_t>>> &right_tokens_submit,
    std::atomic<uint32_t> &real_n_tokens,
    vector<std::atomic<uint32_t>> &results_ready, const BpeConfig &bpe_config,
    std::mutex &main_loop_mt, std::condition_variable &main_loop_cv) {
  auto &pair2cnt = pair2cnt_g[thread_id];
  ska::flat_hash_set<uint32_t> left_tokens;
  ska::flat_hash_set<uint32_t> right_tokens;

  uint32_t cur_token_rule =
      char2id.size() + bpe_config.special_tokens.n_special_tokens();
  auto get_pair_code = [&](size_t word_id, size_t p1) {
    int p2 = lists_of_tokens[word_id][p1].next;
    return int2comb(lists_of_tokens[word_id][p1].val,
                    lists_of_tokens[word_id][p2].val);
  };

  auto get_self_code = [&](size_t word_id, size_t p1) {
    return int2comb(lists_of_tokens[word_id][p1].val,
                    lists_of_tokens[word_id][p1].val);
  };

  auto remove_pair = [&](int word_id, int pos_id) {
    pair2cnt[get_pair_code(word_id, pos_id)] -= word_freq[word_id];
  };

  auto add_pair = [&](size_t word_id, size_t pos_id) {
    uint64_t comb = get_pair_code(word_id, pos_id);
    auto it = pair2pos.find(comb);
    if (it == pair2pos.end()) {
      pair2pos[comb] = {{word_id, pos_id}};
    } else {
      it->second.emplace_back(word_id, pos_id);
    }
    pair2cnt[comb] += word_freq[word_id];
  };

  auto add_empty_pair = [&](size_t word_id, size_t pos_id) {
    auto it = pair2pos.find(get_pair_code(word_id, pos_id));
    assert(it != pair2pos.end());
    it->second.emplace_back(word_id, pos_id);
  };

  auto add_self_pair = [&](size_t word_id, size_t pos_id) {
    int seg_len = lists_of_tokens[word_id][pos_id].seg_len;
    assert(seg_len >= 2);
    uint64_t comb = get_self_code(word_id, pos_id);
    auto it = pair2pos.find(comb);
    size_t real_cnt = word_freq[word_id] * pairsInSeg(seg_len);
    if (it == pair2pos.end()) {
      pair2pos[comb] = {{word_id, pos_id}};
      assert(pair2pos[comb].size() == 1);
    } else {
      it->second.emplace_back(word_id, pos_id);
    }
    pair2cnt[comb] += real_cnt;
  };

  auto add_merge_compensation = [&](size_t word_id, size_t pos_id,
                                    int score_diff) {
    assert(score_diff > 0);
    uint64_t comb = get_self_code(word_id, pos_id);
    pair2cnt[comb] -= score_diff * word_freq[word_id];
  };

  auto seg_len_decrement = [&](size_t word_id, size_t pos_id) {
    int seg_len = lists_of_tokens[word_id][pos_id].seg_len;
    assert(seg_len >= 2);
    lists_of_tokens[word_id][pos_id].seg_len--;
    if (seg_len % 2 == 1) {
      return;
    }
    uint64_t comb = get_self_code(word_id, pos_id);
    pair2cnt[comb] -= word_freq[word_id];
  };

  auto self_full_remove = [&](size_t word_id, size_t pos_id) {
    uint64_t comb = get_self_code(word_id, pos_id);
    uint32_t real_cnt = word_freq[word_id] *
                        pairsInSeg(lists_of_tokens[word_id][pos_id].seg_len);
    pair2cnt[comb] -= real_cnt;
  };

  auto try_merge = [&](size_t word_id, size_t pos1, size_t pos2) {
    vector<NodeEncoder> &cur_list = lists_of_tokens[word_id];
    if (cur_list[pos1].val == cur_list[pos2].val) {
      int score_before =
          (cur_list[pos1].seg_len / 2) + (cur_list[pos2].seg_len / 2) + 1;
      cur_list[pos1].seg_len += cur_list[pos2].seg_len;
      int score_after = cur_list[pos1].seg_len / 2;
      if (score_before != score_after) {
        add_merge_compensation(word_id, pos1, score_before - score_after);
      }

      cur_list[pos1].next = cur_list[pos2].next;
      cur_list[pos2] = {0, -1, -1, 0};
      if (cur_list[pos1].next != -1) {
        cur_list[cur_list[pos1].next].prev = pos1;
        add_empty_pair(word_id, pos1);
      }
    }
  };

  while (true) {
    {
      std::unique_lock<std::mutex> ul(mt[thread_id]);
      cv[thread_id].wait(ul, [&] {
        return task_order[cur_token_rule % 2].z == cur_token_rule ||
               cur_token_rule >= real_n_tokens;
      });
      assert(cur_token_rule <= real_n_tokens);
      if (cur_token_rule == real_n_tokens) {
        break;
      }
    }

    uint32_t x = task_order[cur_token_rule % 2].x;
    uint32_t y = task_order[cur_token_rule % 2].y;
    uint32_t z = task_order[cur_token_rule % 2].z;

    left_tokens.clear();
    right_tokens.clear();

    left_tokens.insert(z);
    int real_merge = 0;
    int not_real_merge = 0;

    if (x == y) {
      const vector<Position> &merge_candidates = pair2pos[int2comb(x, y)];
      std::unique_lock<std::mutex> lk(mt[thread_id]);
      for (auto word_pos : merge_candidates) {
        cv[thread_id].wait(lk, [&] { return thread_use_hs[thread_id].load(); });
        not_real_merge++;

        // p0 <-> p1 <-> p3 -- ids of nodes in linked list.
        // merge will happen inside p1

        int word_id = word_pos.word_id;
        vector<NodeEncoder> &cur_list = lists_of_tokens[word_id];
        int p1 = word_pos.pos_id;
        if (cur_list[p1].val != x || cur_list[p1].seg_len < 2) {
          continue;
        }
        real_merge++;

        int p0 = cur_list[p1].prev;
        int p3 = cur_list[p1].next;

        self_full_remove(word_id, p1);
        if (p0 != -1) {
          remove_pair(word_id, p0);
        }
        if (p3 != -1) {
          remove_pair(word_id, p1);
        }
        int seg_len = cur_list[p1].seg_len;
        if (seg_len % 2 == 0) {
          cur_list[p1] = {z, p0, p3, seg_len / 2};

          if (p0 != -1) {
            add_pair(word_id, p0);
            left_tokens.insert(cur_list[p0].val);
          }

          if (p3 != -1) {
            add_pair(word_id, p1);
            right_tokens.insert(cur_list[p3].val);
          }
          if (seg_len / 2 >= 2) {
            add_self_pair(word_id, p1);
          }
        } else {
          cur_list.emplace_back(x, p1, p3, 1);
          int p2 = static_cast<int>(cur_list.size() - 1);
          cur_list[p1] = {z, p0, p2, seg_len / 2};
          if (p0 != -1) {
            add_pair(word_id, p0);
            left_tokens.insert(cur_list[p0].val);
          }

          add_pair(word_id, p1);
          right_tokens.insert(cur_list[p2].val);

          if (p3 != -1) {
            cur_list[p3].prev = p2;
            add_pair(word_id, p2);
          }

          if (seg_len / 2 >= 2) {
            add_self_pair(word_id, p1);
          }
        }
      }
    } else {
      std::unique_lock<std::mutex> lk(mt[thread_id]);
      for (auto word_pos : pair2pos[int2comb(x, y)]) {
        not_real_merge++;
        cv[thread_id].wait(lk, [&] { return thread_use_hs[thread_id].load(); });
        // p0 <-> p1 <-> p2 <-> p3 -- ids of nodes in linked list.
        // merge will happen between p1 p2
        int word_id = word_pos.word_id;

        int p1 = word_pos.pos_id;
        vector<NodeEncoder> &cur_list = lists_of_tokens[word_id];
        int p2 = cur_list[p1].next;
        if (cur_list[p1].val != x || p2 == -1 || cur_list[p2].val != y) {
          continue;
        }
        real_merge++;

        int p0 = cur_list[p1].prev;
        int p3 = cur_list[p2].next;
        remove_pair(word_id, p1);
        if (p0 != -1 && cur_list[p1].seg_len == 1) {
          remove_pair(word_id, p0);
        }
        if (p3 != -1 && cur_list[p2].seg_len == 1) {
          remove_pair(word_id, p2);
        }

        if (cur_list[p1].seg_len > 1 && cur_list[p2].seg_len > 1) {
          cur_list.emplace_back(z, p1, p2, 1);
          int p12 = static_cast<int>(cur_list.size() - 1);

          seg_len_decrement(word_id, p1);
          seg_len_decrement(word_id, p2);

          cur_list[p1].next = p12;
          cur_list[p2].prev = p12;

          add_pair(word_id, p1);
          left_tokens.insert(cur_list[p1].val);

          add_pair(word_id, p12);
          right_tokens.insert(cur_list[p2].val);
        } else if (cur_list[p1].seg_len > 1 && cur_list[p2].seg_len == 1) {
          cur_list[p2] = {z, p1, p3, 1};

          seg_len_decrement(word_id, p1);

          add_pair(word_id, p1);
          left_tokens.insert(cur_list[p1].val);

          if (p3 != -1) {
            add_pair(word_id, p2);
            right_tokens.insert(cur_list[p3].val);
            try_merge(word_id, p2, p3);
          }
        } else if (cur_list[p1].seg_len == 1 && cur_list[p2].seg_len > 1) {
          cur_list[p1] = {z, p0, p2, 1};

          seg_len_decrement(word_id, p2);

          if (p0 != -1) {
            add_pair(word_id, p0);
            left_tokens.insert(cur_list[p0].val);
          }

          add_pair(word_id, p1);
          right_tokens.insert(cur_list[p2].val);
          if (p0 != -1) {
            try_merge(word_id, p0, p1);
          }
        } else {
          assert(cur_list[p1].seg_len == 1 && cur_list[p2].seg_len == 1);

          cur_list[p1] = {z, p0, p3, 1};
          cur_list[p2] = {0, -1, -1, 0};
          if (p3 != -1) {
            cur_list[p3].prev = p1;
          }

          if (p0 != -1) {
            add_pair(word_id, p0);
            left_tokens.insert(cur_list[p0].val);
          }
          if (p3 != -1) {
            add_pair(word_id, p1);
            right_tokens.insert(cur_list[p3].val);
          }
          if (p0 != -1) {
            try_merge(word_id, p0, p1);
          }
          if (p3 != -1) {
            try_merge(word_id, p1, p3);
          }
        }
      }
    }
    {
      std::unique_lock<std::mutex> lk(mt[thread_id]);

      left_tokens_submit[cur_token_rule % 2][thread_id].clear();
      right_tokens_submit[cur_token_rule % 2][thread_id].clear();

      for (auto token : left_tokens) {
        left_tokens_submit[cur_token_rule % 2][thread_id][token] =
            pair2cnt[int2comb(token, z)];
      }

      for (auto token : right_tokens) {
        right_tokens_submit[cur_token_rule % 2][thread_id][token] =
            pair2cnt[int2comb(z, token)];
      }
    }
    {
      std::lock_guard<std::mutex> lg(main_loop_mt);
      results_ready[thread_id] = cur_token_rule;
    }
    main_loop_cv.notify_one();
    cur_token_rule++;
  }
}

void rename_tokens(ska::flat_hash_map<uint32_t, uint32_t> &char2id,
                   vector<BPE_Rule> &rules, const SpecialTokens &special_tokens,
                   uint32_t n_tokens) {
  ska::flat_hash_map<uint32_t, uint32_t> renaming;
  uint32_t cur = special_tokens.n_special_tokens();
  for (uint32_t i = 0; i < n_tokens; i++) {
    if (!special_tokens.taken_id(i)) {
      renaming[cur++] = i;
    }
  }
  for (auto &node : char2id) {
    assert(renaming.count(node.second));
    node.second = renaming[node.second];
  }

  for (auto &rule : rules) {
    assert(renaming.count(rule.x));
    assert(renaming.count(rule.y));
    assert(renaming.count(rule.z));
    rule.x = renaming[rule.x];
    rule.y = renaming[rule.y];
    rule.z = renaming[rule.z];
  }
}

BPEState learn_bpe_from_string(string &text_utf8, int n_tokens,
                               const string &output_file,
                               BpeConfig bpe_config) {
  vector<std::thread> threads;
  assert(bpe_config.n_threads >= 1 || bpe_config.n_threads == -1);
  size_t n_threads = bpe_config.n_threads;

  vector<size_t> split_pos;
  split_pos.push_back(0);
  for (size_t i = 1; i <= n_threads; i++) {
    size_t candidate = text_utf8.size() * i / n_threads;
    for (; candidate < text_utf8.size() && !is_space(text_utf8[candidate]);
         candidate++) {
    }

    split_pos.push_back(candidate);
  }

  vector<ska::flat_hash_map<uint32_t, size_t>> shared_char_cnt(n_threads);

  vector<std::mutex> mt(n_threads);
  vector<std::condition_variable> cv(n_threads);
  vector<char> thread_finished(n_threads, 0);
  vector<char> main_finished(n_threads, 0);
  vector<size_t> text_len(n_threads);

  ska::flat_hash_set<uint32_t> removed_chars;
  ska::flat_hash_map<uint32_t, uint32_t> char2id;

  vector<ska::flat_hash_map<VectorSegment, WordCount>> hash2wordcnt(n_threads);

  ska::flat_hash_map<uint32_t, vector<uint32_t>> recipe;
  ska::flat_hash_map<uint32_t, string> recipe_s;
  vector<ska::flat_hash_map<uint64_t, size_t>> pair2cnt_g(n_threads);
  ska::flat_hash_map<uint64_t, vector<Position>> pair2pos;
  PriorityQueue merge_order(1);
  vector<size_t> split_word_cnt;
  vector<WordCount> word_cnt_global;

  auto comb2int = [](uint64_t a, uint32_t &b, uint32_t &c) {
    b = static_cast<uint32_t>(a >> 32u);
    c = static_cast<uint32_t>(a & UINT32_MAX);
  };

  vector<vector<ska::flat_hash_map<uint32_t, size_t>>> left_tokens_submit(
      2, vector<ska::flat_hash_map<uint32_t, size_t>>(n_threads));
  vector<vector<ska::flat_hash_map<uint32_t, size_t>>> right_tokens_submit(
      2, vector<ska::flat_hash_map<uint32_t, size_t>>(n_threads));
  vector<std::atomic<uint32_t>> results_ready(n_threads);
  for (size_t i = 0; i < n_threads; i++) {
    results_ready[i] = 0;
  }

  vector<char> thread_stopped(n_threads);
  vector<char> thread_ready_to_run(n_threads);
  vector<std::atomic_bool> thread_use_hs(n_threads);
  vector<BPE_Rule> task_order(2);

  std::atomic<uint32_t> real_n_tokens(n_tokens);

  std::mutex main_loop_mt;
  std::condition_variable main_loop_cv;

  for (size_t i = 0; i < n_threads; i++) {
    threads.emplace_back(
        [&](size_t thread_id) {
          // threads are working 1

          vector<vector<NodeEncoder>> lists_of_tokens;
          ska::flat_hash_map<uint64_t, vector<Position>> pair2pos;
          vector<size_t> word_freq;

          auto thread_awake_main = [&]() {
            {
              std::lock_guard<std::mutex> lk(mt[thread_id]);
              thread_finished[thread_id] = 1;
            }
            cv[thread_id].notify_one();
          };

          auto thread_wait_main = [&]() {
            std::unique_lock<std::mutex> lk(mt[thread_id]);
            cv[thread_id].wait(lk, [&] { return main_finished[thread_id]; });
            main_finished[thread_id] = 0;
          };

          vector<uint32_t> utf_code_points;
          ska::flat_hash_map<uint32_t, size_t> char_cnt;
          utf_code_points =
              decode_utf8(text_utf8.data() + split_pos[thread_id],
                          text_utf8.data() + split_pos[thread_id + 1]);

          for (auto ch : utf_code_points) {
            if (!is_space(ch)) {
              char_cnt[ch]++;
            }
          }

          text_len[thread_id] = utf_code_points.size();
          shared_char_cnt[thread_id] = char_cnt;

          thread_awake_main();
          // main is working  1
          thread_wait_main();
          // threads are working 2

          remove_rare_chars(utf_code_points, removed_chars);
          hash2wordcnt[thread_id] =
              compute_word_count_helper(utf_code_points, char2id);

          thread_awake_main();
          // main is working 2
          thread_wait_main();
          // threads are working 3

          build_linked_list(
              {word_cnt_global.begin() + split_word_cnt[thread_id],
               word_cnt_global.begin() + split_word_cnt[thread_id + 1]},
              lists_of_tokens, pair2pos, pair2cnt_g[thread_id]);

          std::transform(
              word_cnt_global.begin() + split_word_cnt[thread_id],
              word_cnt_global.begin() + split_word_cnt[thread_id + 1],
              std::back_inserter(word_freq),
              [](const WordCount &x) { return x.cnt; });

          thread_awake_main();
          // main is working 3
          // threads are working 4

          worker_doing_merge(thread_id, lists_of_tokens, pair2cnt_g, pair2pos,
                             word_freq, mt, cv, task_order, thread_use_hs,
                             char2id, left_tokens_submit, right_tokens_submit,
                             real_n_tokens, results_ready, bpe_config,
                             main_loop_mt, main_loop_cv);
        },
        i);
  }

  auto main_wait_threads = [&]() {
    for (size_t i = 0; i < n_threads; i++) {
      std::unique_lock<std::mutex> lk(mt[i]);
      cv[i].wait(lk, [&] { return thread_finished[i]; });
      thread_finished[i] = 0;
    }
  };

  auto main_awake_threads = [&]() {
    for (size_t i = 0; i < n_threads; i++) {
      std::lock_guard<std::mutex> lk(mt[i]);
      main_finished[i] = 1;
    }
    for (size_t i = 0; i < n_threads; i++) {
      cv[i].notify_one();
    }
  };

  main_wait_threads();

  // main is working  1
  for (size_t i = 1; i < n_threads; i++) {
    for (auto x : shared_char_cnt[i]) {
      shared_char_cnt[0][x.first] += x.second;
    }
    text_len[0] += text_len[i];
  }

  char2id = compute_alphabet_helper(shared_char_cnt[0], text_len[0],
                                    removed_chars, bpe_config);

  main_awake_threads();
  // threads are working 2

  main_wait_threads();
  // main is working 2

  for (size_t i = 1; i < n_threads; i++) {
    for (const auto &x : hash2wordcnt[i]) {
      auto it = hash2wordcnt[0].find(x.first);
      if (it == hash2wordcnt[0].end()) {
        hash2wordcnt[0][x.first] = x.second;
      } else {
        it->second.cnt += x.second.cnt;
      }
    }
  }

  word_cnt_global.resize(hash2wordcnt[0].size());
  std::transform(
      hash2wordcnt[0].begin(), hash2wordcnt[0].end(), word_cnt_global.begin(),
      [](const std::pair<VectorSegment, WordCount> &x) { return x.second; });
  merge_order = PriorityQueue(text_len[0]);

  size_t used_ids =
      char2id.size() + bpe_config.special_tokens.n_special_tokens();
  if (used_ids > (size_t)n_tokens) {
    std::cerr
        << "Incorrect arguments. Vocabulary size too small. Set vocab_size>="
        << used_ids << ".  Current value for vocab_size=" << n_tokens
        << std::endl;
    exit(EXIT_FAILURE);
  }

  init_recipe(char2id, recipe, recipe_s);

  split_word_cnt.push_back(0);
  for (size_t i = 1; i <= n_threads; i++) {
    split_word_cnt.push_back(word_cnt_global.size() * i / n_threads);
  }

  main_awake_threads();
  // threads are working 3

  main_wait_threads();
  // main is working 3

  ska::flat_hash_map<uint64_t, uint64_t> real_pair_cnt;

  for (size_t i = 0; i < n_threads; i++) {
    for (const auto &x : pair2cnt_g[i]) {
      real_pair_cnt[x.first] += x.second;
    }
  }

  for (const auto &x : real_pair_cnt) {
    uint32_t ka, kb;
    comb2int(x.first, ka, kb);
    merge_order.push({x.second, ka, kb});
  }
  vector<BPE_Rule> rules;

  auto get_recipe = [&](uint32_t x, uint32_t y) {
    assert(recipe.count(x));
    assert(recipe.count(y));
    vector<uint32_t> new_recipe = recipe[x];
    new_recipe.insert(new_recipe.end(), recipe[y].begin(), recipe[y].end());
    return new_recipe;
  };

  std::function<size_t(uint64_t)> check_cnt = [&](uint64_t mask) {
    size_t ret = 0;
    for (size_t i = 0; i < n_threads; i++) {
      auto it = pair2cnt_g[i].find(mask);
      if (it != pair2cnt_g[i].end()) {
        ret += it->second;
      }
    }
    return ret;
  };

  size_t finished_cur = used_ids;
  size_t last_failed_try = 0;

  ska::flat_hash_map<uint32_t, size_t> all_res;
  vector<char> local_check_list(n_threads);
  ska::flat_hash_map<uint32_t, size_t> global_ht_update_left;
  ska::flat_hash_map<uint32_t, size_t> global_ht_update_right;

  int inter_fail = 0;
  int equal_fail = 0;
  vector<std::pair<int, double>> progress_debug;
  while (used_ids < (size_t)n_tokens) {
    uint32_t x, y, z;
    assert(finished_cur <= used_ids && used_ids <= finished_cur + 2);
    bool progress = false;

    if (used_ids < (size_t)n_tokens && used_ids - finished_cur < 2 &&
        last_failed_try < finished_cur) {
      progress = true;
      for (size_t i = 0; i < n_threads; i++) {
        thread_use_hs[i] = false;
      }
      {
        vector<std::lock_guard<std::mutex>> lg(mt.begin(), mt.end());

        size_t real_cnt = 0;
        while (true) {
          if (merge_order.empty()) {
            if (finished_cur == used_ids) {
              std::cerr << "WARNING merged only: " << used_ids
                        << " pairs of tokens" << std::endl;
              x = UINT32_MAX;
              y = UINT32_MAX;
              z = UINT32_MAX;
              real_n_tokens = used_ids;
              break;
            } else {
              x = y = z = 0;
              last_failed_try = finished_cur;
              break;
            }
          }
          BPE_Rule last_rule = (used_ids - finished_cur == 1)
                                   ? rules.back()
                                   : BPE_Rule({0, 0, 0});

          auto merge_event = merge_order.top(check_cnt, last_rule);
          if ((used_ids - finished_cur == 1) &&
              (merge_event.left_token == rules.back().y ||
               merge_event.right_token == rules.back().x ||
               (!rules.empty() && rules.back().x == rules.back().y))) {
            inter_fail += merge_event.left_token == rules.back().y ||
                          merge_event.right_token == rules.back().x;
            equal_fail += !rules.empty() && rules.back().x == rules.back().y &&
                          used_ids - finished_cur == 1;

            last_failed_try = finished_cur;
            x = y = z = 0;
            break;
          }

          merge_order.pop();
          real_cnt = check_cnt(
              int2comb(merge_event.left_token, merge_event.right_token));
          assert(real_cnt <= merge_event.count);

          if (real_cnt != merge_event.count) {
            if (real_cnt > 0) {
              merge_event.count = real_cnt;
              merge_order.push(merge_event);
            }
            continue;
          }

          if (real_cnt == 0) {
            continue;
          }

          x = merge_event.left_token;
          y = merge_event.right_token;
          z = used_ids;
          break;
        }
        if (last_failed_try != finished_cur && x != UINT32_MAX) {
          task_order[used_ids % 2] = {x, y, z};
          recipe[z] = get_recipe(x, y);
          recipe_s[z] = recipe_s[x] + recipe_s[y];

          if (used_ids % 1000 == 0) {
            int used_symbols = 0;
            std::cerr << "id: " << z << "=" << x << "+" << y;
            used_symbols += std::to_string(z).size();
            used_symbols += 1;
            used_symbols += std::to_string(x).size();
            used_symbols += 1;
            used_symbols += std::to_string(y).size();
            for (int j = used_symbols; j < 22 + 4; j++) {
              std::cerr << " ";
            }
            used_symbols = 0;
            std::cerr << "freq: " << real_cnt;
            used_symbols += 5;
            used_symbols += std::to_string(real_cnt).size();

            for (int j = used_symbols; j < 15; j++) {
              std::cerr << " ";
            }
            std::cerr << "  subword: " << recipe_s[z] << "="
                      << recipe_s[x] + "+" + recipe_s[y] << std::endl;
          }
          used_ids++;
          rules.emplace_back(x, y, z);
        }

        for (size_t i = 0; i < n_threads; i++) {
          thread_use_hs[i] = true;
        }
      }
      for (auto &cond_value : cv) {
        cond_value.notify_one();
      }
      if (x == UINT32_MAX) {
        break;
      }
    }

    // collect results

    bool full_epoch = true;
    for (size_t i = 0; i < n_threads; i++) {
      if (!local_check_list[i]) {
        if (results_ready[i] >= finished_cur) {
          progress = true;
          local_check_list[i] = 1;

          for (auto token_cnt : left_tokens_submit[finished_cur % 2][i]) {
            global_ht_update_left[token_cnt.first] += token_cnt.second;
          }

          for (auto token_cnt : right_tokens_submit[finished_cur % 2][i]) {
            global_ht_update_right[token_cnt.first] += token_cnt.second;
          }
        } else {
          full_epoch = false;
        }
      }
    }

    if (full_epoch) {
      for (auto left_token : global_ht_update_left) {
        merge_order.push({left_token.second, left_token.first,
                          task_order[finished_cur % 2].z});
      }
      for (auto right_token : global_ht_update_right) {
        merge_order.push({right_token.second, task_order[finished_cur % 2].z,
                          right_token.first});
      }
      local_check_list.assign(n_threads, 0);
      global_ht_update_left.clear();
      global_ht_update_right.clear();
      finished_cur++;
    }
    if (!progress) {
      std::unique_lock<std::mutex> ul(main_loop_mt);
      main_loop_cv.wait(ul, [&] {
        for (size_t i = 0; i < n_threads; i++) {
          if (!local_check_list[i] && results_ready[i] >= finished_cur)
            return true;
        }
        return false;
      });
    }
  }
  for (auto &t : threads) {
    t.join();
  }

  rename_tokens(char2id, rules, bpe_config.special_tokens, n_tokens);

  BPEState bpe_state = {char2id, rules, bpe_config.special_tokens};
  bpe_state.dump(output_file);
  std::cerr << "model saved to: " << output_file << std::endl;
  return bpe_state;
}

void build_linked_list(
    const vector<WordCount> &word_cnt, vector<vector<NodeEncoder>> &list,
    ska::flat_hash_map<uint64_t, PositionsCnt> &pair2poscnt) {
  list.resize(word_cnt.size());
  for (size_t i = 0; i < word_cnt.size(); i++) {
    for (uint32_t ch : word_cnt[i].word) {
      if (!list[i].empty() && list[i].back().val == ch) {
        list[i].back().seg_len++;
      } else {
        int list_size = list[i].size();
        list[i].emplace_back(ch, list_size - 1, list_size + 1, 1);
      }
    }

    list[i].back().next = -1;
    for (size_t j = 0; j < list[i].size(); j++) {
      if (j + 1 < list[i].size()) {
        uint64_t comb = int2comb(list[i][j].val, list[i][j + 1].val);
        auto it = pair2poscnt.find(comb);
        if (it == pair2poscnt.end()) {
          pair2poscnt[comb] = {{{i, j}}, word_cnt[i].cnt};
        } else {
          it->second.positions.emplace_back(i, j);
          it->second.cnt += word_cnt[i].cnt;
        }
      }
      assert(list[i][j].seg_len >= 1);

      if (list[i][j].seg_len > 1) {
        uint64_t comb = int2comb(list[i][j].val, list[i][j].val);
        auto it = pair2poscnt.find(comb);
        size_t cc = word_cnt[i].cnt * pairsInSeg(list[i][j].seg_len);
        if (it == pair2poscnt.end()) {
          pair2poscnt[comb] = {{{i, j}}, cc};
        } else {
          it->second.positions.emplace_back(i, j);
          it->second.cnt += cc;
        }
      }
    }
  }
}

ska::flat_hash_map<uint32_t, uint32_t> compute_alphabet(
    const vector<uint32_t> &data, ska::flat_hash_set<uint32_t> &removed_chars,
    const BpeConfig &bpe_config) {
  ska::flat_hash_map<uint32_t, size_t> char_cnt;
  for (auto ch : data) {
    if (!is_space(ch)) {
      char_cnt[ch]++;
    }
  }
  assert(!char_cnt.empty());
  return compute_alphabet_helper(char_cnt, data.size(), removed_chars,
                                 bpe_config);
}

void check_config(BpeConfig &bpe_config, int vocab_size) {
  if (bpe_config.character_coverage <= 0 || bpe_config.character_coverage > 1) {
    std::cerr << "Invalid value. Character_coverage value must be in the range "
                 "(0, 1]."
              << std::endl;
    exit(EXIT_FAILURE);
  }
  if (bpe_config.special_tokens.unk_id < 0 ||
      bpe_config.special_tokens.unk_id >= vocab_size) {
    std::cerr << "Invalid value. unk_id: must be in the range [0, vocab_size - "
                 "1]. Current value of vocab_size = " +
                     std::to_string(vocab_size) + "."
              << std::endl;
    exit(EXIT_FAILURE);
  }

  if (bpe_config.special_tokens.pad_id < -1 ||
      bpe_config.special_tokens.pad_id >= vocab_size) {
    std::cerr << "Invalid value. pad_id must be in the range [-1, vocab_size - "
                 "1]. Current value of vocab_size = " +
                     std::to_string(vocab_size) + "."
              << std::endl;
    exit(EXIT_FAILURE);
  }

  if (bpe_config.special_tokens.bos_id < -1 ||
      bpe_config.special_tokens.bos_id >= vocab_size) {
    std::cerr << "Invalid value. bos_id must be in the range [-1, vocab_size - "
                 "1]. Current value of vocab_size = " +
                     std::to_string(vocab_size) + "."
              << std::endl;
    exit(EXIT_FAILURE);
  }

  if (bpe_config.special_tokens.eos_id < -1 ||
      bpe_config.special_tokens.eos_id >= vocab_size) {
    std::cerr << "Invalid value. eos_id must be in the range [-1, vocab_size - "
                 "1]. Current value of vocab_size = " +
                     std::to_string(vocab_size) + "."
              << std::endl;
    exit(EXIT_FAILURE);
  }

  ska::flat_hash_set<int> ids;
  size_t cnt_add = 0;
  if (bpe_config.special_tokens.pad_id != -1) {
    ids.insert(bpe_config.special_tokens.pad_id);
    cnt_add++;
  }
  if (bpe_config.special_tokens.bos_id != -1) {
    ids.insert(bpe_config.special_tokens.bos_id);
    cnt_add++;
  }
  if (bpe_config.special_tokens.eos_id != -1) {
    ids.insert(bpe_config.special_tokens.eos_id);
    cnt_add++;
  }
  ids.insert(bpe_config.special_tokens.unk_id);
  cnt_add++;
  if (ids.size() != cnt_add) {
    std::cerr << "Error: All ids of special tokens must be different."
              << std::endl;
    exit(EXIT_FAILURE);
  }

  if (bpe_config.n_threads == -1) {
    bpe_config.n_threads = std::thread::hardware_concurrency();
  }
  bpe_config.n_threads = std::min(8, std::max(1, bpe_config.n_threads));
}

void print_config(const string &input_path, const string &model_path,
                  int vocab_size, BpeConfig bpe_config) {
  std::cerr << "Training parameters" << std::endl;
  std::cerr << "  input: " << input_path << std::endl;
  std::cerr << "  model: " << model_path << std::endl;
  std::cerr << "  vocab_size: " << vocab_size << std::endl;
  std::cerr << "  n_threads: " << bpe_config.n_threads << std::endl;
  std::cerr << "  character_coverage: " << bpe_config.character_coverage
            << std::endl;
  std::cerr << "  pad: " << bpe_config.special_tokens.pad_id << std::endl;
  std::cerr << "  unk: " << bpe_config.special_tokens.unk_id << std::endl;
  std::cerr << "  bos: " << bpe_config.special_tokens.bos_id << std::endl;
  std::cerr << "  eos: " << bpe_config.special_tokens.eos_id << std::endl;
  std::cerr << std::endl;
}

void train_bpe(const string &input_path, const string &model_path,
               int vocab_size, BpeConfig bpe_config) {
  check_config(bpe_config, vocab_size);
  print_config(input_path, model_path, vocab_size, bpe_config);
  std::cerr << "reading file..." << std::endl;
  auto data = fast_read_file_utf8(input_path);
  std::cerr << "learning bpe..." << std::endl;
  learn_bpe_from_string(data, vocab_size, model_path, bpe_config);
}

DecodeResult BaseEncoder::encode_sentence(const std::string &sentence_utf8,
                                          const EncodingConfig &encoding_config,
                                          OutputType output_type) const {
  struct NodeDecoder {
    uint32_t token_id;
    int prev, next;

    NodeDecoder(uint32_t _val, size_t cur_pos)
        : token_id(_val),
          prev(static_cast<int>(cur_pos) - 1),
          next(static_cast<int>(cur_pos) + 1) {}

    NodeDecoder(uint32_t _val, int _prev, int _next)
        : token_id(_val), prev(_prev), next(_next) {}
  };

  struct MergeEvent2 {
    int priority;
    int pos;

    bool operator<(const MergeEvent2 &other) const {
      return priority > other.priority ||
             (priority == other.priority && pos > other.pos);
    }
  };

  vector<int> output_ids;
  vector<string> output_pieces;

  if (encoding_config.bos) {
    if (output_type == ID) {
      output_ids.push_back(bpe_state.special_tokens.bos_id);
    } else {
      output_pieces.push_back(BOS_TOKEN);
    }
  }

  vector<NodeDecoder> list;
  ska::flat_hash_map<uint32_t, string> unrecognized_tokens;

  auto text = decode_utf8(sentence_utf8.data(),
                          sentence_utf8.data() + sentence_utf8.size());

  assert(bpe_state.char2id.count(SPACE_TOKEN));

  for (; !text.empty() && is_space(text.back()); text.pop_back()) {
  }

  const int new_tokens_start = static_cast<int>(
      1e9);  // just some number that bigger than any subword id
  for (auto it_text = text.begin(); it_text != text.end();) {
    list.clear();
    unrecognized_tokens.clear();

    auto begin_of_word = std::find_if_not(it_text, text.end(), is_space);
    auto end_of_word = std::find_if(begin_of_word, text.end(), is_space);
    it_text = end_of_word;

    uint32_t new_token_cur = new_tokens_start;
    list.emplace_back(bpe_state.char2id.at(SPACE_TOKEN), 0);

    for (auto it_char_in_word = begin_of_word; it_char_in_word < end_of_word;) {
      if (bpe_state.char2id.count(*it_char_in_word) == 0) {
        auto it_unrecognized_word = std::find_if(
            it_char_in_word, end_of_word,
            [&](uint32_t ch) { return bpe_state.char2id.count(ch); });

        unrecognized_tokens[new_token_cur] =
            encode_utf8({it_char_in_word, it_unrecognized_word});
        it_char_in_word = it_unrecognized_word;

        list.emplace_back(new_token_cur, list.size());
        new_token_cur++;
      } else {
        list.emplace_back(bpe_state.char2id.at(*it_char_in_word), list.size());
        ++it_char_in_word;
      }
    }
    list.back().next = -1;

    std::priority_queue<MergeEvent2> queue;

    auto pair_code = [&](size_t first_pos) {
      auto second_pos = list[first_pos].next;
      return int2comb(list[first_pos].token_id, list[second_pos].token_id);
    };

    auto push_in_queue_if_rule_exist = [&](size_t pos) {
      auto it = rule2id.find(pair_code(pos));
      if (it != rule2id.end()) {
        queue.push({it->second, static_cast<int>(pos)});
      }
    };

    for (size_t j = 0; j + 1 < list.size(); j++) {
      push_in_queue_if_rule_exist(j);
    }

    while (!queue.empty()) {
      MergeEvent2 event = queue.top();
      queue.pop();
      int rule_id = event.priority;
      int pos_1 = event.pos;
      int pos_2 = list[pos_1].next;
      assert(pos_1 != pos_2);
      if (list[pos_1].token_id != bpe_state.rules[rule_id].x || pos_2 == -1 ||
          list[pos_2].token_id != bpe_state.rules[rule_id].y) {
        continue;
      }

      int pos_0 = list[pos_1].prev;
      int pos_3 = list[pos_2].next;

      list[pos_2] = {0, -1, -1};
      list[pos_1] = {bpe_state.rules[rule_id].z, pos_0, pos_3};
      if (pos_3 != -1) {
        list[pos_3].prev = pos_1;
      }

      if (pos_0 != -1) {
        push_in_queue_if_rule_exist(pos_0);
      }
      if (pos_3 != -1) {
        push_in_queue_if_rule_exist(pos_1);
      }
    }

    auto it_alive_token = std::find_if(
        list.begin(), list.end(),
        [](const NodeDecoder &node) { return node.token_id != 0; });

    assert(it_alive_token != list.end());
    int alive_token = std::distance(list.begin(), it_alive_token);
    for (; alive_token != -1; alive_token = list[alive_token].next) {
      int token_id = list[alive_token].token_id;
      if (token_id >= new_tokens_start) {
        if (output_type == ID) {
          output_ids.push_back(bpe_state.special_tokens.unk_id);
        } else {
          assert(unrecognized_tokens.count(token_id));
          output_pieces.push_back(unrecognized_tokens[token_id]);
        }
      } else {
        if (output_type == ID) {
          output_ids.push_back(token_id);
        } else {
          assert(recipe.count(token_id));
          output_pieces.push_back(token2word(recipe.at(token_id), id2char));
        }
      }
    }
  }
  if (encoding_config.eos) {
    if (output_type == ID) {
      output_ids.push_back(bpe_state.special_tokens.eos_id);
    } else {
      output_pieces.push_back(EOS_TOKEN);
    }
  }

  if (encoding_config.reverse) {
    if (output_type == ID) {
      std::reverse(output_ids.begin(), output_ids.end());
    } else {
      std::reverse(output_pieces.begin(), output_pieces.end());
    }
  }

  return {output_ids, output_pieces};
}

BaseEncoder::BaseEncoder(BPEState _bpe_state, int _n_threads)
    : bpe_state(std::move(_bpe_state)), n_threads(_n_threads) {
  fill_from_state();
  assert(n_threads >= 1 || n_threads == -1);
  if (n_threads == -1) {
    n_threads = std::max(1, int(std::thread::hardware_concurrency()));
  }
}

BaseEncoder::BaseEncoder(const string &model_path, int _n_threads)
    : n_threads(_n_threads) {
  bpe_state.load(model_path);
  fill_from_state();
  assert(n_threads >= 1 || n_threads == -1);
  if (n_threads == -1) {
    n_threads = std::max(1, int(std::thread::hardware_concurrency()));
  }
}

template <typename T>
vector<T> concat_vectors(const vector<T> &a, const vector<T> &b) {
  vector<T> c;
  c.reserve(a.size() + b.size());
  c.insert(c.end(), a.begin(), a.end());
  c.insert(c.end(), b.begin(), b.end());
  return c;
}

void BaseEncoder::fill_from_state() {
  for (auto x : bpe_state.char2id) {
    id2char[x.second] = x.first;
  }

  for (int i = 0; i < (int)bpe_state.rules.size(); i++) {
    rule2id[int2comb(bpe_state.rules[i].x, bpe_state.rules[i].y)] = i;
  }

  for (auto x : id2char) {
    recipe[x.first] = {x.first};
  }

  for (auto rule : bpe_state.rules) {
    recipe[rule.z] = concat_vectors(recipe[rule.x], recipe[rule.y]);
  }

  for (const auto &id_to_recipe : recipe) {
    reversed_recipe[token2word(id_to_recipe.second, id2char)] =
        id_to_recipe.first;
  }
  reversed_recipe[BOS_TOKEN] = bpe_state.special_tokens.bos_id;
  reversed_recipe[EOS_TOKEN] = bpe_state.special_tokens.eos_id;
}

int BaseEncoder::vocab_size() const {
  return bpe_state.rules.size() + bpe_state.char2id.size() +
         bpe_state.special_tokens.n_special_tokens();
}

std::vector<DecodeResult> BaseEncoder::encode_parallel(
    const std::vector<std::string> &sentences,
    const EncodingConfig &encoding_config, OutputType output_type) const {
  if (encoding_config.bos && bpe_state.special_tokens.bos_id == -1) {
    std::cerr << "Can't add <BOS> token. Model was trained without it."
              << std::endl;
    exit(EXIT_FAILURE);
  }
  if (encoding_config.eos && bpe_state.special_tokens.eos_id == -1) {
    std::cerr << "Can't add <EOS> token. Model was trained without it."
              << std::endl;
    exit(EXIT_FAILURE);
  }

  vector<DecodeResult> bpe(sentences.size());
  if (sentences.size() <= static_cast<size_t>(n_threads) * 3 ||
      n_threads == 1) {  // Not too many sentences. It's better to solve it
    // without threads.
    for (size_t i = 0; i < sentences.size(); i++) {
      bpe[i] = encode_sentence(sentences[i], encoding_config, output_type);
    }
    return bpe;
  }
  vector<std::thread> threads;
  for (int i = 0; i < n_threads; i++) {
    threads.emplace_back(
        [&](size_t this_thread) {
          size_t tasks_for_thread =
              (sentences.size() + n_threads - 1) / n_threads;
          size_t first_task = tasks_for_thread * this_thread;
          size_t last_task =
              std::min(tasks_for_thread * (this_thread + 1), sentences.size());
          for (size_t j = first_task; j < last_task; j++) {
            bpe[j] =
                encode_sentence(sentences[j], encoding_config, output_type);
          }
        },
        i);
  }
  for (auto &thread : threads) {
    thread.join();
  }
  return bpe;
}

vector<vector<int>> BaseEncoder::encode_as_ids(const vector<string> &sentences,
                                               bool bos, bool eos,
                                               bool reverse) const {
  EncodingConfig encoding_config = {bos, eos, reverse};
  auto res = encode_parallel(sentences, encoding_config, ID);
  vector<vector<int>> ret(res.size());
  for (size_t i = 0; i < res.size(); i++) {
    ret[i] = move(res[i].ids);
  }
  return ret;
}

vector<vector<string>> BaseEncoder::encode_as_subwords(
    const vector<string> &sentences, bool bos, bool eos, bool reverse) const {
  time_check("");
  EncodingConfig encoding_config = {bos, eos, reverse};
  auto res = encode_parallel(sentences, encoding_config, SUBWORD);
  vector<vector<string>> ret(res.size());
  for (size_t i = 0; i < res.size(); i++) {
    ret[i] = move(res[i].pieces);
  }
  return ret;
}

string BaseEncoder::id_to_subword(int id, bool replace_space) const {
  if (id < 0 || vocab_size() <= id) {
    std::cerr << "Error: Invalid value for id. id must be in the range [0, "
                 "vocab_size - 1]. Current value: vocab_size = " +
                     std::to_string(vocab_size()) +
                     "; id=" + std::to_string(id) + ";"
              << std::endl;
    exit(EXIT_FAILURE);
  }
  if (bpe_state.special_tokens.unk_id == id) {
    return UNK_TOKEN;
  }
  if (bpe_state.special_tokens.pad_id == id) {
    return PAD_TOKEN;
  }
  if (bpe_state.special_tokens.bos_id == id) {
    return BOS_TOKEN;
  }
  if (bpe_state.special_tokens.eos_id == id) {
    return EOS_TOKEN;
  }

  assert(recipe.count(id));
  if (replace_space) {
    auto symbols = recipe.at(id);
    if (id2char.at(symbols[0]) == SPACE_TOKEN) {
      return " " + token2word({symbols.begin() + 1, symbols.end()}, id2char);
    }
  }
  return token2word(recipe.at(id), id2char);
}

int BaseEncoder::subword_to_id(const string &token) const {
  if (UNK_TOKEN == token) {
    return bpe_state.special_tokens.unk_id;
  }
  if (PAD_TOKEN == token) {
    return bpe_state.special_tokens.pad_id;
  }
  if (BOS_TOKEN == token) {
    return bpe_state.special_tokens.bos_id;
  }
  if (EOS_TOKEN == token) {
    return bpe_state.special_tokens.eos_id;
  }
  if (reversed_recipe.count(token)) {
    return reversed_recipe.at(token);
  }
  return bpe_state.special_tokens.unk_id;
}

vector<string> BaseEncoder::decode(const vector<vector<int>> &ids) const {
  vector<string> ret;
  for (auto sentence : ids) {
    ret.push_back(decode(sentence));
  }
  return ret;
}

string BaseEncoder::decode(const vector<int> &ids) const {
  string ret;
  bool first_iter = true;
  for (auto id : ids) {
    ret += id_to_subword(id, true);
    if (first_iter && ret[0] == ' ') {
      ret = ret.substr(1);
    }
    first_iter = false;
  }
  return ret;
}

vector<string> BaseEncoder::decode(const vector<string> &data) const {
  vector<string> ret;
  for (const auto &s : data) {
    std::stringstream stream;
    stream << s;
    vector<int> ids;
    int x;
    while (stream >> x) {
      ids.push_back(x);
    }
    ret.push_back(decode(ids));
  }
  return ret;
}

vector<string> BaseEncoder::vocabulary() const {
  int n = vocab_size();
  vector<string> vocab(n);
  for (int i = 0; i < n; i++) {
    vocab[i] = id_to_subword(i);
  }
  return vocab;
}

void BaseEncoder::vocab_cli(bool verbose) const {
  uint32_t n_tokens = 0;
  for (const auto &entry : recipe) {
    n_tokens = std::max(entry.first, n_tokens);
  }
  n_tokens = std::max(n_tokens, bpe_state.special_tokens.max_id());
  n_tokens++;

  ska::flat_hash_map<uint32_t, std::pair<uint32_t, uint32_t>> reversed_rules;
  if (verbose) {
    for (auto rule : bpe_state.rules) {
      reversed_rules[rule.z] = {rule.x, rule.y};
    }
  }

  for (size_t i = 0; i < n_tokens; i++) {
    string token_z = id_to_subword(i);
    std::cout << i << "\t" << token_z;
    if (verbose) {
      if (reversed_rules.count(i)) {
        int used_symbols = 0;
        auto comb = reversed_rules[i];
        string token_x = id_to_subword(comb.first);
        string token_y = id_to_subword(comb.second);

        used_symbols += decode_utf8(token_z).size() + 1;
        used_symbols +=
            decode_utf8(token_x).size() + 1 + decode_utf8(token_y).size();

        std::cout << "=" << token_x << "+" << token_y;
        for (int t = 0; t < std::max(2, 50 - used_symbols); t++) {
          std::cout << " ";
        }
        std::cout << comb.first << "+" << comb.second;
      }
    }
    std::cout << std::endl;
  }
}

void BaseEncoder::encode_cli(const string &output_type_str, bool stream,
                             bool bos, bool eos, bool reverse) const {
  std::ios_base::sync_with_stdio(false);
  OutputType output_type;
  if (output_type_str == "id") {
    output_type = ID;
  } else {
    assert(output_type_str == "subword");
    output_type = SUBWORD;
  }
  if (stream) {
    if (output_type == SUBWORD) {
      string sentence;
      while (getline(std::cin, sentence)) {
        write_to_stdout(encode_as_subwords({sentence}, bos, eos, reverse),
                        true);
      }
    } else {
      assert(output_type == ID);
      string sentence;
      while (getline(std::cin, sentence)) {
        write_to_stdout(encode_as_ids({sentence}, bos, eos, reverse), true);
      }
    }
  } else {
    time_check("");
    const size_t batch_limit = 10 * 1024 * 1024;
    size_t total_progress = 0;
    size_t processed;
    std::cerr << "n_threads: " << n_threads << std::endl;
    int chars_remove = 0;
    do {
      processed = 0;
      auto sentences = read_lines_from_stdin(batch_limit, &processed);
      if (output_type == SUBWORD) {
        auto ret = encode_as_subwords(sentences, bos, eos, reverse);
        write_to_stdout(ret, false);
      } else {
        assert(output_type == ID);
        auto ret = encode_as_ids(sentences, bos, eos, reverse);
        write_to_stdout(ret, false);
      }
      total_progress += processed;

      for (int i = 0; i < chars_remove; i++) {
        std::cerr << '\b';
      }
      chars_remove = 0;
      string message = "bytes processed: ";
      chars_remove += message.size();
      chars_remove += std::to_string(total_progress).length();
      std::cerr << message << total_progress;
    } while (processed >= batch_limit);
    std::cerr << std::endl;
  }
}

void BaseEncoder::decode_cli() const {
  std::ios_base::sync_with_stdio(false);
  string sentence;
  while (getline(std::cin, sentence)) {
    std::cout << decode({sentence})[0] << "\n";
  }
}

}  // namespace vkcom
