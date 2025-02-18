#include "utils.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace vkcom {
using std::string;
using std::vector;

void SpecialTokens::dump(std::ofstream &fout) {
  fout << unk_id << " " << pad_id << " " << bos_id << " " << eos_id
       << std::endl;
}

void SpecialTokens::load(std::ifstream &fin) {
  fin >> unk_id >> pad_id >> bos_id >> eos_id;
}

uint32_t SpecialTokens::max_id() const {
  int ret = 0;
  ret = std::max(ret, unk_id);
  ret = std::max(ret, pad_id);
  ret = std::max(ret, bos_id);
  ret = std::max(ret, eos_id);
  return ret;
}

bool SpecialTokens::taken_id(int id) const {
  return id == unk_id || id == pad_id || id == bos_id || id == eos_id;
}

size_t SpecialTokens::n_special_tokens() const {
  size_t cnt = 0;
  cnt += (unk_id != -1);
  cnt += (pad_id != -1);
  cnt += (bos_id != -1);
  cnt += (eos_id != -1);
  return cnt;
}

SpecialTokens::SpecialTokens(int pad_id, int unk_id, int bos_id, int eos_id)
    : pad_id(pad_id), unk_id(unk_id), bos_id(bos_id), eos_id(eos_id) {}

bool BPE_Rule::operator==(const BPE_Rule &other) const {
  return x == other.x && y == other.y && z == other.z;
}

BPE_Rule::BPE_Rule(uint32_t x, uint32_t y, uint32_t z) : x(x), y(y), z(z) {}

void BPEState::dump(const string &file_name) {
  std::ofstream fout(file_name, std::ios::out);
  if (fout.fail()) {
    std::cerr << "Can't open file: " << file_name << std::endl;
    assert(false);
  }
  fout << char2id.size() << " " << rules.size() << std::endl;
  for (auto s : char2id) {
    fout << s.first << " " << s.second << std::endl;
  }

  for (auto rule : rules) {
    fout << rule.x << " " << rule.y << " " << rule.z << std::endl;
  }
  special_tokens.dump(fout);
  fout.close();
}

void BPEState::load(const string &file_name) {
  char2id.clear();
  rules.clear();
  std::ifstream fin(file_name, std::ios::in);
  if (fin.fail()) {
    std::cerr << "Error. Can not open file with model: " << file_name
              << std::endl;
    exit(EXIT_FAILURE);
  }
  int n, m;
  fin >> n >> m;
  for (int i = 0; i < n; i++) {
    uint32_t inner_id;
    uint32_t utf32_id;
    fin >> inner_id >> utf32_id;
    char2id[inner_id] = utf32_id;
  }
  for (int i = 0; i < m; i++) {
    uint32_t x, y, z;
    fin >> x >> y >> z;
    rules.emplace_back(x, y, z);
  }
  special_tokens.load(fin);
  fin.close();
}

BpeConfig::BpeConfig(double _character_coverage, int _n_threads,
                     const SpecialTokens &_special_tokens)
    : character_coverage(_character_coverage),
      n_threads(_n_threads),
      special_tokens(_special_tokens) {}

vector<string> read_lines_from_stdin(size_t batch_limit, size_t *processed) {
  vector<string> sentences;
  string s;
  while (*processed < batch_limit && getline(std::cin, s)) {
    *processed += s.size();
    sentences.push_back(std::move(s));
  }
  return sentences;
}

}  // namespace vkcom
