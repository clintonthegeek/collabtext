#include "crdt/Buffer.h"
#include <map>
#include <collabtext/CrdtEngine.h>
#include <collabtext/IdListEngine.h>
#include <collabtext/Serialization.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>
#include <unistd.h>
using namespace CollabText;using namespace CollabText::Crdt;using Clock=std::chrono::steady_clock;
template<class T>T take(Result<T> r){if(!r)throw std::runtime_error(std::string(r.error().message()));return std::move(r).value();}
void check(bool b){if(!b)throw std::runtime_error("benchmark correctness check failed");}
class Counter:public IdentityAllocator{ActorId id;CheckedCounter counter;public:explicit Counter(uint64_t n){std::array<uint8_t,16>b{};for(int i=15;i>=8;--i){b[i]=n&255;n>>=8;}id=ActorId::from_bytes(b);}Result<uint64_t>reserve_run(uint64_t n)override{return counter.reserve_run(n);}void observe(uint64_t n)override{check(counter.observe(n).ok());}uint64_t peek()const noexcept override{return counter.peek();}const ActorId&actor()const noexcept override{return id;}};
auto text(uint64_t actor=1){return take(CrdtEngine::create(std::make_shared<Counter>(actor),{"perf","body",Primitive::text,1}));}
std::vector<double> samples;
std::map<std::string,std::string> final_frames;
template<class F>auto timed(F f){auto start=Clock::now();auto result=f();auto stop=Clock::now();samples.push_back(std::chrono::duration<double,std::micro>(stop-start).count());return result;}
void report(const std::string& name,uint64_t bytes=0,uint64_t history=0,uint64_t wire_bytes=0){
 auto sorted=samples;std::sort(sorted.begin(),sorted.end());double total=0;for(auto v:samples)total+=v;
 auto p=[&](double q){return sorted[std::min(sorted.size()-1,size_t(q*(sorted.size()-1)))];};
 std::ifstream stat("/proc/self/statm");long pages=0,resident=0;stat>>pages>>resident;
 std::cout<<"{\"name\":\""<<name<<"\",\"samples\":"<<samples.size()<<",\"mean_us\":"<<total/samples.size()<<",\"p50_us\":"<<p(.5)<<",\"p95_us\":"<<p(.95)<<",\"p99_us\":"<<p(.99)<<",\"max_us\":"<<sorted.back()<<",\"visible_bytes\":"<<bytes<<",\"history_edits\":"<<history<<",\"wire_bytes\":"<<wire_bytes<<",\"process_rss_kib\":"<<resident*sysconf(_SC_PAGESIZE)/1024<<"}\n";samples.clear();
}
void edits(size_t size,int history){
 auto e=text();if(size>=1000000)check(e->set_max_undo_bytes(256u*1024*1024).ok());std::string expected(size,'a');take(e->try_insert(0,expected));std::mt19937 rng(42);
 for(int i=0;i<history;++i){int p=rng()%size;std::string c(1,expected[p]=='x'?'y':'x');take(e->try_replace(p,1,c));expected[p]=c[0];}
 Operation last;for(int i=0;i<200;++i){int p=rng()%size;std::string c(1,expected[p]=='x'?'y':'x');last=take(timed([&]{return e->try_replace(p,1,c);}));expected[p]=c[0];}
 final_frames[std::to_string(size)+":"+std::to_string(history)]=take(encode_operation(last));
 check(e->text()==expected);report("public_replace_"+std::to_string(size)+"B_history_"+std::to_string(history),size,history);
}
void byte_edits(size_t size,int history){
 Buffer e(std::make_shared<Counter>(1));if(size>=1000000)check(e.set_max_undo_bytes(256u*1024*1024).ok());std::string expected(size,'a');take(e.try_apply_local_edit({{0,0}},{expected}));std::mt19937 rng(42);
 for(int i=0;i<history;++i){uint32_t p=rng()%size;std::string c(1,expected[p]=='x'?'y':'x');take(e.try_apply_local_edit({{p,p+1}},{c}));expected[p]=c[0];}
 Operation last;for(int i=0;i<200;++i){uint32_t p=rng()%size;std::string c(1,expected[p]=='x'?'y':'x');last=take(timed([&]{return e.try_apply_local_edit({{p,p+1}},{c});}));expected[p]=c[0];}
 check(e.text()==expected);check(take(encode_operation(last))==final_frames.at(std::to_string(size)+":"+std::to_string(history)));
 report("byte_replace_"+std::to_string(size)+"B_history_"+std::to_string(history),size,history);
}
void queries(){
 for(int history:{0,1000}){
  auto e=text();std::string expected(100000,'a');take(e->try_insert(0,expected));std::mt19937 rng(42);
  for(int i=0;i<history;++i){int p=rng()%expected.size();std::string c(1,expected[p]=='x'?'y':'x');take(e->try_replace(p,1,c));expected[p]=c[0];}
  auto marker=take(e->try_insert_marker(50000)).marker;auto boundary=take(e->boundary_at(50000,Affinity::left));check(e->text()==expected);
  for(int i=0;i<50;++i){auto v=take(timed([&]{return e->boundary_at(50000,Affinity::left);}));check(take(e->resolve(v)).byte_offset==50000);}report("boundary_mint_history_"+std::to_string(history),100000,history);
  for(int i=0;i<50;++i)check(take(timed([&]{return e->resolve(boundary);})).byte_offset==50000);report("boundary_resolve_history_"+std::to_string(history),100000,history);
  for(int i=0;i<50;++i)check(take(timed([&]{return e->resolve(marker);})).byte_offset==50000);report("marker_resolve_history_"+std::to_string(history),100000,history);
 }
}
void lists(){for(int n:{100,1000,5000}){
 auto e=take(IdListEngine::create(std::make_shared<Counter>(1),{"perf","blocks",Primitive::list,1}));Anchor last=Anchor::min();
 for(int i=0;i<n;++i)last=Anchor{idlist_op_identity(take(e->try_insert_after(last,7)))};
 for(int i=0;i<50;++i){auto entries=take(timed([&]{return e->visible_entries();}));check(entries.size()==size_t(n)&&entries.back().identity==last.position&&entries.front().application_value==7);}
 report("visible_entries_"+std::to_string(n),0,n);
}}
void codec(){for(int n:{1,32,128,512}){
 auto receiver=text(999);
 for(int i=1;i<n;++i){auto source=text(i);auto op=take(source->try_insert(0,"s"));check(receiver->apply_remote(op).kind()==ApplyOutcomeKind::applied);}
 take(receiver->try_insert(n-1,"z"));auto op=take(receiver->try_insert(n,"q"));check(receiver->text()==std::string(n-1,'s')+"zq");auto bytes=take(encode_operation(op));
 for(int i=0;i<100;++i)check(take(timed([&]{return encode_operation(op);}))==bytes);report("encode_actors_"+std::to_string(n),n+1,0,bytes.size());
 for(int i=0;i<100;++i)check(op_identity(take(timed([&]{return decode_operation(bytes);})))==op_identity(op));report("decode_actors_"+std::to_string(n),n+1,0,bytes.size());
}}
void unicode(){auto e=text();std::string expected;for(int i=0;i<10000;++i)expected+="a😀é";take(e->try_insert(0,expected));
 for(int i=0;i<200;++i){take(timed([&]{return e->try_insert(40000+i*2,"😀");}));expected+="😀";}check(e->text()==expected);check(e->length()==40400);report("unicode_append_70000B",expected.size());}
int main(){try{for(auto n:{1000,10000,100000,1000000}){edits(n,0);byte_edits(n,0);}for(auto n:{500,3000}){edits(10000,n);byte_edits(10000,n);}unicode();queries();lists();codec();}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
