#include "storage/RetentionState.h"
#include "core/Sha256.h"

#include <charconv>
#include <fstream>
#include <sstream>

namespace CollabText::Storage {
namespace {
bool number(std::string_view s, uint64_t &n) {
  auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
  return ec == std::errc{} && end == s.data() + s.size();
}
std::string hex(std::string_view s) {
  constexpr char d[] = "0123456789abcdef";
  std::string out; out.reserve(s.size() * 2);
  for (unsigned char c : s) { out.push_back(d[c >> 4]); out.push_back(d[c & 15]); }
  return out;
}
bool unhex(std::string_view s, std::string &out) {
  if (s.size() % 2) return false;
  out.clear(); out.reserve(s.size() / 2);
  auto n = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
  for (size_t i=0; i<s.size(); i+=2) { int a=n(s[i]), b=n(s[i+1]); if(a<0||b<0)return false; out.push_back(char((a<<4)|b)); }
  return true;
}
std::string body(uint64_t seq, std::string_view stream, const Crdt::Identity &id,
                 const ActorId &producer) {
  return "CRS1 " + producer.to_string() + " " + std::to_string(seq) + " " +
      hex(stream) + " " + id.actor.to_string() + " " + std::to_string(id.counter);
}
} // namespace

Result<RetentionState> RetentionState::open(std::filesystem::path path,
                                             ActorId producer,
                                             const StepHook *hook) {
  if (!producer.valid()) return Error(ErrorCode::invalid_identity, "retention producer is invalid");
  RetentionState state(std::move(path), producer, hook);
  auto loaded = state.recover();
  if (!loaded.ok()) return loaded.error();
  return state;
}

Result<void> RetentionState::recover() {
  m_retired.clear();
  m_sequence = 0;
  std::ifstream in(m_path, std::ios::binary);
  if (!in) {
    if (!std::filesystem::exists(m_path)) return {};
    return Error(ErrorCode::io_error, "cannot read retention journal");
  }
  in.seekg(0, std::ios::end); auto length=in.tellg();
  if (length < 0 || length > 64 * 1024 * 1024) return Error(ErrorCode::corrupt_record, "retention journal exceeds size limit");
  in.seekg(0); std::string bytes(static_cast<size_t>(length), '\0');
  if (!bytes.empty() && !in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) return Error(ErrorCode::io_error, "cannot read retention journal contents");
  uint64_t expected=1; size_t start=0;
  while(start<bytes.size()) {
    if(expected==UINT64_MAX) return Error(ErrorCode::corrupt_record, "retention sequence overflow");
    auto end=bytes.find('\n', start);
    if(end==std::string::npos) return Error(ErrorCode::corrupt_record, "retention journal ends with an incomplete record");
    std::string_view line(bytes.data()+start,end-start); auto cut=line.rfind(' ');
    if(cut==std::string_view::npos || sha256_hex(line.substr(0,cut))!=line.substr(cut+1)) return Error(ErrorCode::corrupt_record, "retention record checksum mismatch");
    std::istringstream row{std::string(line.substr(0,cut))};
    std::string magic, producer_s, seq_s, stream_s, actor_s, counter_s, extra;
    if(!(row>>magic>>producer_s>>seq_s>>stream_s>>actor_s>>counter_s) || (row>>extra) || magic!="CRS1") return Error(ErrorCode::corrupt_record, "retention record fields are invalid");
    auto producer=ActorId::parse(producer_s); uint64_t seq=0,counter=0; std::string stream;
    auto actor=ActorId::parse(actor_s);
    if(!producer.ok() || producer.value()!=m_producer || !number(seq_s,seq) || seq!=expected || !number(counter_s,counter) || counter==0 || !actor.ok() || !unhex(stream_s,stream) || stream.empty() || stream.size()>4096) return Error(ErrorCode::corrupt_record, "retention record identity or sequence is invalid");
    auto &set=m_retired[stream];
    if(!set.insert(Crdt::Identity{actor.value(),counter}).second) return Error(ErrorCode::corrupt_record, "retention journal repeats a group");
    ++expected; start=end+1;
  }
  m_sequence=expected-1;
  return {};
}

Result<void> RetentionState::publish_after_durable_trim(
    std::string_view stream, const std::vector<Crdt::Identity> &groups,
    const std::function<Result<void>()> &commit_trim) {
  if(stream.empty() || stream.size()>4096 || !commit_trim || m_sequence==UINT64_MAX) return Error(ErrorCode::malformed, "retention publication arguments are invalid");
  std::set<Crdt::Identity> unique;
  for(const auto &id:groups) if(!id.actor.valid() || id.counter==0 || !unique.insert(id).second) return Error(ErrorCode::malformed, "retention group list has invalid or repeated identity");
  auto committed=commit_trim(); if(!committed.ok()) return committed.error();
  auto &retired=m_retired[std::string(stream)];
  for(const auto &id:unique) {
    if(retired.contains(id)) continue;
    if(m_sequence==UINT64_MAX) return Error(ErrorCode::limit_exceeded, "retention sequence is exhausted");
    auto seq=m_sequence+1;
    auto record=body(seq,stream,id,m_producer); record += " "+sha256_hex(record)+"\n";
    auto log=DurableAppendFile::open(m_path,m_hook); if(!log.ok()) return log.error();
    auto written=log.value().append(record);
    if(!written.ok()) {
      // A failed durability barrier can follow a successful append. Reconcile
      // from disk before another retry can reuse the same sequence number.
      auto reconciled=recover();
      if(!reconciled.ok()) return reconciled.error();
      return written.error();
    }
    retired.insert(id); m_sequence=seq;
  }
  return {};
}

bool RetentionState::is_retired(std::string_view stream,const Crdt::Identity &group) const {
  auto found=m_retired.find(std::string(stream)); return found!=m_retired.end() && found->second.contains(group);
}
const std::set<Crdt::Identity> *RetentionState::retired_groups(std::string_view stream) const {
  auto found=m_retired.find(std::string(stream)); return found==m_retired.end()?nullptr:&found->second;
}
} // namespace CollabText::Storage
