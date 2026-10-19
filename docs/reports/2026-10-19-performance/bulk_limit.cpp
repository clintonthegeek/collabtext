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
int main(){try{for(int n:{262144,524288,1000000}){auto allocator=std::make_shared<Counter>(1);auto e=take(CrdtEngine::create(allocator,{"perf","body",Primitive::text,1}));auto before=allocator->peek();auto result=e->try_insert(0,std::string(n,'a'));if(!result){check(e->text().empty());check(before==allocator->peek());}else check(e->text()==std::string(n,'a'));std::cout<<"bytes="<<n<<" accepted="<<result.ok();if(!result)std::cout<<" error="<<result.error().message();std::cout<<"\n";}}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
