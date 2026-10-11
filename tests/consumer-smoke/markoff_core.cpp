// Installed public headers only. No assertions disabled by Release builds.
#include <collabtext/IdListEngine.h>
#include <collabtext/CrdtEngine.h>
#include <collabtext/Serialization.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
using namespace CollabText;
using namespace CollabText::Crdt;
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class T> T take(Result<T> result){if(!result)throw std::runtime_error(std::string(result.error().message()));return std::move(result).value();}
void done(Result<void> result){if(!result)throw std::runtime_error(std::string(result.error().message()));}
ActorId actor(unsigned x){std::array<uint8_t,16> b{};b[15]=x;return ActorId::from_bytes(b);}
class Counter final:public IdentityAllocator {
 ActorId id;CheckedCounter counter;
public:
 Counter(unsigned x,uint64_t start=1):id(::actor(x)),counter(start){}
 Result<uint64_t> reserve_run(uint64_t n) override{return counter.reserve_run(n);}
 void observe(uint64_t n)override{done(counter.observe(n));}
 uint64_t peek()const noexcept override{return counter.peek();}
 const ActorId& actor()const noexcept override{return id;}
};
PositionScope list_scope{"markoff-doc","blocks",Primitive::list,1};
PositionScope text_scope{"markoff-doc","block-text",Primitive::text,1};
auto list(unsigned x,uint64_t start=1){return take(IdListEngine::create(std::make_shared<Counter>(x,start),list_scope));}
auto text(unsigned x,uint64_t start=1){return take(CrdtEngine::create(std::make_shared<Counter>(x,start),text_scope));}
IdListOperation wire(const IdListOperation& op){return take(decode_idlist_operation(take(encode_idlist_operation(op))));}
Operation wire(const Operation& op){return take(decode_operation(take(encode_operation(op))));}
void identities(){
 auto a=list(1),b=list(2);std::vector<IdListOperation> log;
 auto insert=[&](auto& e){auto op=take(e.try_insert_after(Anchor::min(),7));log.push_back(wire(op));return op;};
 auto x=insert(*a),y=insert(*b);
 require(a->apply_ops({wire(y)})[0].kind()==ApplyOutcomeKind::applied,"a concurrent insert");
 require(b->apply_ops({wire(x)})[0].kind()==ApplyOutcomeKind::applied,"b concurrent insert");
 auto z=insert(*a);require(b->apply_ops({wire(z)})[0].kind()==ApplyOutcomeKind::applied,"third insert");
 auto entries=take(a->visible_entries());require(entries==take(b->visible_entries()) && entries.size()==3,"ordered entries diverge");
 auto ids=a->ids();for(size_t i=0;i<entries.size();++i){require(entries[i].application_value==ids[i] && ids[i]==7,"values/order differ");for(size_t j=0;j<i;++j)require(entries[i].identity!=entries[j].identity,"duplicate identity");}
 auto identity=entries[1].identity;auto token=take(a->element_of(identity));
 require(take(a->resolve(token)).element_index==1,"identity token wrong entry");
 auto removal=take(a->try_remove_at(take(a->try_anchor_of(identity))));log.push_back(wire(removal));
 require(take(a->visible_entries()).size()==2,"remove not projected");
 auto undo=take(a->try_undo());require(undo.has_value(),"missing undo");log.push_back(wire(*undo));
 require(take(a->visible_entries())==entries,"undo changed insertion identities");
 auto reopened=list(3);for(const auto& op:log)require(reopened->apply_ops({op})[0].kind()==ApplyOutcomeKind::applied,"fresh replay");
 require(take(reopened->visible_entries())==entries,"replay changed identity/order");
 reopened->discard_local_view();require(!reopened->visible_entries() && reopened->visible_entries().error().code()==ErrorCode::history_unavailable,"discarded view reported valid empty list");
}
void archives(){
 // Independent literal CTLO/0002 insert, remove and compensation frames.
 const std::string insertion=R"(CTLO0002:{"t":"il-i","ts":["00000000000000000000000000000001","0000000000000001"],"v":{},"a":["0000000000000001","0000000000000001"],"id":"0000000000000007","k":{"p":[],"s":"0000000000000001","r":[]}})";
 const std::string removal=R"(CTLO0002:{"t":"il-r","ts":["00000000000000000000000000000001","0000000000000002"],"v":{"00000000000000000000000000000001":"0000000000000001"},"a":["0000000000000002","0000000000000001"],"to":["00000000000000000000000000000001","0000000000000001"]})";
 const std::string undo=R"(CTLO0002:{"t":"il-u","ts":["00000000000000000000000000000001","0000000000000003"],"v":{"00000000000000000000000000000001":"0000000000000002"},"a":["0000000000000003","0000000000000001"],"c":[[["00000000000000000000000000000001","0000000000000002"],"0000000000000001"]]})";
 auto e=list(9);std::vector<VisibleIdListEntry> original;
 for(const auto& bytes:{insertion,removal,undo}){
  auto op=take(decode_idlist_operation(bytes));require(take(encode_idlist_operation(op))==bytes,"literal archive canonical bytes changed");
  require(e->apply_ops({op})[0].kind()==ApplyOutcomeKind::applied,"archive not applied");
  if(bytes==insertion)original=take(e->visible_entries());
  if(bytes==removal)require(take(e->visible_entries()).empty(),"archive remove failed");
  for(size_t n=0;n<bytes.size();++n)require(!decode_idlist_operation(std::string_view(bytes).substr(0,n)),"truncated archive accepted");
  require(!decode_idlist_operation(bytes+"x"),"trailing archive accepted");
 }
 require(take(e->visible_entries())==original && original[0].identity==Identity(actor(1),1),"archive undo changed identity");
 auto future=insertion;future.replace(4,4,"000f");auto refused=decode_idlist_operation(future);require(!refused && refused.error().code()==ErrorCode::unsupported_version,"future CTLO not typed refusal");
}
void gap_semantics(){
 for(bool is_text:{false,true}){
  auto scope=is_text?text_scope:list_scope;
  CounterGap gap{actor(1),1,4,{}};
  const std::string literal=R"(CTGP0001:{"actor":"00000000000000000000000000000001","first":"0000000000000001","count":"0000000000000004","v":{}})";
  require(take(encode_counter_gap(gap))==literal,"terminal archive bytes changed");
  for(size_t n=0;n<literal.size();++n)require(!decode_counter_gap(std::string_view(literal).substr(0,n)),"truncated gap accepted");
  require(!decode_counter_gap(literal+"x"),"trailing gap accepted");
  auto decoded=take(decode_counter_gap(take(encode_counter_gap(gap))));
  auto writer_text=text(1,5),peer_text=text(2),fresh_text=text(3);
  auto writer_list=list(1,5),peer_list=list(2),fresh_list=list(3);
  auto apply=[&](auto& t,auto& l,const CounterGap& g){return is_text?t->apply_counter_gap(scope,g):l->apply_counter_gap(scope,g);};
  int changes=0,published=0;
  writer_text->setOnChange([&]{++changes;});writer_text->setOnLocalOp([&](const auto&){++published;});
  writer_list->set_on_change([&]{++changes;});writer_list->set_on_local_op([&](const auto&){++published;});
  require(apply(writer_text,writer_list,decoded).kind()==ApplyOutcomeKind::applied,"writer gap");
  require(changes==0 && published==0,"gap fabricated document callback");
  auto next_text=is_text?std::optional<Operation>(take(writer_text->try_insert(0,"😀x"))):std::nullopt;
  auto next_list=!is_text?std::optional<IdListOperation>(take(writer_list->try_insert_after(Anchor::min(),7))):std::nullopt;
  require((is_text?peer_text->apply_remote(wire(*next_text)):peer_list->apply_ops({wire(*next_list)})[0]).kind()==ApplyOutcomeKind::deferred,"later op should await gap");
  bool guarded=false;
  auto callback=[&]{auto nested=apply(peer_text,peer_list,decoded); guarded=nested.kind()==ApplyOutcomeKind::rejected && nested.error()->code()==ErrorCode::reentrant_mutation;
   if(is_text){auto edit=peer_text->try_insert(0,"bad");guarded=guarded && !edit && edit.error().code()==ErrorCode::reentrant_mutation;}
   else{auto edit=peer_list->try_insert_after(Anchor::min(),9);guarded=guarded && !edit && edit.error().code()==ErrorCode::reentrant_mutation;}};
  peer_text->setOnChange(callback);peer_list->set_on_change(callback);
  require(apply(peer_text,peer_list,decoded).kind()==ApplyOutcomeKind::applied,"peer gap");
  require(guarded,"gap callback mutation was not guarded");
  require(is_text?peer_text->text()=="😀x":peer_list->ids()==std::vector<uint64_t>{7},"gap did not release deferred operation");
  require(apply(peer_text,peer_list,decoded).kind()==ApplyOutcomeKind::duplicate,"gap retry not idempotent");
  require(apply(fresh_text,fresh_list,decoded).kind()==ApplyOutcomeKind::applied,"fresh gap replay");
  require((is_text?fresh_text->apply_remote(wire(*next_text)):fresh_list->apply_ops({wire(*next_list)})[0]).kind()==ApplyOutcomeKind::applied,"fresh next replay");
  auto frontier = [&] { return is_text ? take(encode_position(PositionToken{take(fresh_text->boundary_at(0,Affinity::left))})) : take(encode_global(fresh_list->version())); };
  auto version=frontier();
  auto conflict=decoded;conflict.count=3;
  auto rejected=apply(fresh_text,fresh_list,conflict);require(rejected.kind()==ApplyOutcomeKind::rejected && rejected.error()->code()==ErrorCode::actor_conflict,"overlap not conflict");
  require(frontier()==version,"conflict advanced coverage");
  auto other_prior=decoded;other_prior.prior.observe({actor(8),1});
  require(apply(fresh_text,fresh_list,other_prior).kind()==ApplyOutcomeKind::rejected,"same interval changed prior accepted");
  auto missing=decoded;missing.actor=actor(8);missing.first=2;missing.prior.observe({actor(8),1});
  require(apply(fresh_text,fresh_list,missing).kind()==ApplyOutcomeKind::deferred,"missing prior should defer gap");
  auto old_text=text(1);auto old_list=list(1);
  auto bad=is_text?fresh_text->apply_remote(take(old_text->try_insert(0,"z"))):fresh_list->apply_ops({take(old_list->try_insert_after(Anchor::min(),9))})[0];
  require(bad.kind()==ApplyOutcomeKind::rejected && bad.error()->code()==ErrorCode::actor_conflict,"operation reused terminal reservation");
  auto bytes=take(encode_counter_gap(decoded)); bytes.insert(9," ");
  require((is_text?fresh_text->apply_counter_gap(scope,bytes):fresh_list->apply_counter_gap(scope,bytes)).kind()==ApplyOutcomeKind::rejected,"changed gap bytes accepted");
  for(unsigned mismatch=0;mismatch<4;++mismatch){
   auto wrong=scope;ErrorCode expected=ErrorCode::wrong_epoch;
   if(mismatch==0){wrong.document="another-doc";expected=ErrorCode::wrong_document;}
   else if(mismatch==1){wrong.stream="another-stream";expected=ErrorCode::wrong_stream;}
   else if(mismatch==2){wrong.primitive=is_text?Primitive::list:Primitive::text;expected=ErrorCode::wrong_primitive;}
   else wrong.epoch++;
   auto bad_scope=is_text?fresh_text->apply_counter_gap(wrong,decoded):fresh_list->apply_counter_gap(wrong,decoded);
   require(bad_scope.kind()==ApplyOutcomeKind::rejected && bad_scope.error()->code()==expected,"gap scope mismatch");
  }
 }
}
class DurableCounter final:public IdentityAllocator {
 std::shared_ptr<ReplicaState> state;PositionScope scope;
public:
 DurableCounter(std::shared_ptr<ReplicaState> s,PositionScope p):state(std::move(s)),scope(std::move(p)){}
 Result<uint64_t> reserve_run(uint64_t n) override{return state->reserve_run(scope.stream,scope.document,scope.epoch,n);}
 void observe(uint64_t n)override{done(state->observe_remote(scope.stream,n));}
 uint64_t peek()const noexcept override{auto value=state->peek_stream(scope.stream);return value?value.value():1;}
 const ActorId& actor()const noexcept override{return state->actor();}
};
void append(const std::filesystem::path& path,const std::string& bytes){
 auto* file=std::fopen(path.string().c_str(),"ab");require(file,"host log open failed");
 bool ok=std::fwrite(bytes.data(),1,bytes.size(),file)==bytes.size() && std::fputc('\n',file)!=EOF && std::fflush(file)==0;
#ifdef _WIN32
 ok=ok && _commit(_fileno(file))==0;
#else
 ok=ok && fsync(fileno(file))==0;
#endif
 ok=std::fclose(file)==0 && ok;require(ok,"host log durability failed");
#ifndef _WIN32
 int directory=open(path.parent_path().c_str(),O_RDONLY|O_DIRECTORY);require(directory>=0,"host directory open failed");
 int synced=fsync(directory);close(directory);require(synced==0,"host directory durability failed");
#endif
}
std::vector<std::string> read(const std::filesystem::path& path){std::ifstream file(path);require(bool(file),"host replay open");std::vector<std::string> rows;std::string row;while(std::getline(file,row))rows.push_back(row);return rows;}
void durable_restart(){
 auto directory=std::filesystem::temp_directory_path()/("collabtext-markoff-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 require(std::filesystem::create_directory(directory),"test directory collision");
 struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code ignored;std::filesystem::remove_all(path,ignored);}} cleanup{directory};
 for(bool is_text:{false,true}){
  auto scope=is_text?text_scope:list_scope;auto root=directory/(is_text?"text":"list");std::filesystem::create_directory(root);
  auto log=root/"host.log";std::string envelope="markoff-doc|"+scope.stream+"|1|"+(is_text?"text":"list");append(log,envelope);
  ActorId owner;uint64_t first=0,count=0;
  {
   auto state=std::make_shared<ReplicaState>(take(ReplicaState::open(root/"private")));owner=state->actor();take(state->acquire_writer(scope.document,scope.stream));
   auto allocator=std::make_shared<DurableCounter>(state,scope);first=allocator->peek();
   // Host rejects before publication. No transaction bytes ever enter the log.
   if(is_text){auto e=take(CrdtEngine::create(allocator,scope));auto absent=take(e->try_insert(0,"abc"));require(op_identity(absent).actor==owner,"text reservation owner");}
   else{auto e=take(IdListEngine::create(allocator,scope));auto absent=take(e->try_insert_after(Anchor::min(),7));require(idlist_op_identity(absent).actor==owner,"list reservation owner");}
   count=allocator->peek()-first;require(read(log)==std::vector<std::string>{envelope},"failed bundle not definitively absent");
  } // Reopen private reservation and canonical host log; discard tentative view.
  {
   auto state=std::make_shared<ReplicaState>(take(ReplicaState::open(root/"private")));require(state->actor()==owner,"restart changed actor");take(state->acquire_writer(scope.document,scope.stream));
   auto allocator=std::make_shared<DurableCounter>(state,scope);require(allocator->peek()==first+count,"reservation was reused after restart");
   CounterGap gap{owner,first,count,{}};auto bytes=take(encode_counter_gap(gap));append(log,bytes); // Barrier before next operation.
   auto decoded=take(decode_counter_gap(read(log)[1]));
   if(is_text){auto writer=take(CrdtEngine::create(allocator,scope)),peer=text(22);require(writer->apply_counter_gap(scope,take(encode_counter_gap(decoded))).kind()==ApplyOutcomeKind::applied,"restart writer text gap");require(peer->apply_counter_gap(scope,take(encode_counter_gap(decoded))).kind()==ApplyOutcomeKind::applied,"peer text gap");auto next=take(writer->try_insert(0,"😀"));append(log,take(encode_operation(next)));require(peer->apply_remote(wire(next)).kind()==ApplyOutcomeKind::applied,"post-gap text peer");}
   else{auto writer=take(IdListEngine::create(allocator,scope)),peer=list(22);require(writer->apply_counter_gap(scope,take(encode_counter_gap(decoded))).kind()==ApplyOutcomeKind::applied,"restart writer list gap");require(peer->apply_counter_gap(scope,take(encode_counter_gap(decoded))).kind()==ApplyOutcomeKind::applied,"peer list gap");auto next=take(writer->try_insert_after(Anchor::min(),7));append(log,take(encode_idlist_operation(next)));require(peer->apply_ops({wire(next)})[0].kind()==ApplyOutcomeKind::applied,"post-gap list peer");}
  }
  auto records=read(log);require(records.size()==3 && records[0]==envelope,"canonical host scope/replay rows changed");auto gap=take(decode_counter_gap(records[1]));
  for(unsigned peer_id:{23,24}){
   if(is_text){auto peer=text(peer_id);require(peer->apply_counter_gap(scope,records[1]).kind()==ApplyOutcomeKind::applied,"reopen text gap");require(peer->apply_remote(take(decode_operation(records[2]))).kind()==ApplyOutcomeKind::applied,"reopen text op");require(peer->text()=="😀","reopen text differs");require(peer->apply_counter_gap(scope,records[1]).kind()==ApplyOutcomeKind::duplicate,"reopen text gap retry");}
   else{auto peer=list(peer_id);require(peer->apply_counter_gap(scope,records[1]).kind()==ApplyOutcomeKind::applied,"reopen list gap");require(peer->apply_ops({take(decode_idlist_operation(records[2]))})[0].kind()==ApplyOutcomeKind::applied,"reopen list op");require(peer->ids()==std::vector<uint64_t>{7},"reopen list differs");require(peer->apply_counter_gap(scope,records[1]).kind()==ApplyOutcomeKind::duplicate,"reopen list gap retry");}
  }
 }
}
}
int main(){try{identities();archives();gap_semantics();durable_restart();std::cout<<"Markoff Core trace passed\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
