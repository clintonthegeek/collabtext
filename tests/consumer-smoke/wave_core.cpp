// Public-header-only Wave acceptance trace, also built in the producer tests.
#include <collabtext/CrdtEngine.h>
#include <collabtext/Serialization.h>
#include <collabtext/Projection.h>
#include <array>
#include <iostream>
#include <stdexcept>
#include <utility>
using namespace CollabText;
using namespace CollabText::Crdt;
namespace {
void require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
template<class T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(std::string(result.error().message()));
    return std::move(result).value();
}
void done(Result<void> result) { if (!result) throw std::runtime_error(std::string(result.error().message())); }
class Counter final : public IdentityAllocator {
    ActorId id; uint64_t next;
public:
    Counter(unsigned actor, uint64_t start=1) : next(start) {
        std::array<uint8_t,16> bytes{}; bytes[15]=static_cast<uint8_t>(actor); id=ActorId::from_bytes(bytes);
    }
    Result<uint64_t> reserve_run(uint64_t n) override {
        if (!n || n > UINT64_MAX-next) return Error(ErrorCode::counter_exhausted,"counter exhausted");
        auto first=next; next+=n; return first;
    }
    void observe(uint64_t n) override { if(n>=next) next=n==UINT64_MAX ? n : n+1; }
    uint64_t peek() const noexcept override { return next; }
    const ActorId& actor() const noexcept override {return id;}
};
auto engine(unsigned actor, uint64_t start=1) {
    return take(CrdtEngine::create(std::make_shared<Counter>(actor,start), {"conversation","block",Primitive::text,1}));
}
void applied(CrdtEngine& e,const Operation& op) { require(e.apply_remote(op).kind()==ApplyOutcomeKind::applied,"operation not applied"); }
Operation wire(const Operation& op) { return take(decode_operation(take(encode_operation(op)))); }
void markers() {
    auto a=engine(1),b=engine(2);
    int changes=0,publications=0;
    a->setOnChange([&]{++changes;}); a->setOnLocalOp([&](const auto&){++publications;});
    auto initial=take(a->try_insert(0,"a😀éб")); applied(*b,wire(initial));
    auto minted=take(a->try_insert_marker(5));
    require(a->length()==5 && a->text()=="a😀éб","marker changes visible text");
    require(take(encode_operation(minted.operation)).starts_with("CTBO0003:"),"marker format");
    auto token_bytes=take(encode_position(PositionToken{minted.marker}));
    require(static_cast<unsigned char>(token_bytes[5])==2,"marker token version");
    auto token=take(decode_position_as<TextMarker>(token_bytes));
    applied(*b,wire(minted.operation));
    require(take(b->resolve(token)).byte_offset==5,"replayed marker offset");
    auto attachment=take(a->attachment_at(5,Affinity::left,ReferentSide::following));
    auto right=take(a->try_remove(3,2));
    auto left=take(a->try_remove(0,3));
    require(take(a->resolve(token)).byte_offset==0 && a->text().empty(),"text deletion removes marker");
    require(!a->resolve(attachment) && a->resolve(attachment).error().code()==ErrorCode::detached,"attachment selects marker");
    applied(*b,wire(right)); applied(*b,wire(left));
    auto inserted=take(b->try_insert_at_marker(token,MarkerSide::before,"😀é"));
    applied(*a,wire(inserted));
    require(take(a->resolve(token)).byte_offset==6,"peer insertion does not shift marker");
    auto second=take(a->try_insert_marker(token,MarkerSide::after));
    auto between=take(a->try_insert_at_marker(second.marker,MarkerSide::before,"x"));
    require(take(a->resolve(token)).byte_offset==6 && take(a->resolve(second.marker)).byte_offset==7,"explicit marker gap");
    auto removed=take(a->try_remove_marker(token));
    require(!a->resolve(token) && a->resolve(token).error().code()==ErrorCode::detached,"marker removal doesn't detach");
    require(a->apply_remote(minted.operation).kind()==ApplyOutcomeKind::duplicate,"duplicate insert");
    require(!a->resolve(token),"duplicate insertion revives marker");
    auto snapshot=a->local_history_snapshot();
    auto reopened=engine(1);
    std::vector<Operation> history{initial,minted.operation,right,left,inserted,second.operation,between,removed};
    for(const auto& op:history) applied(*reopened,wire(op));
    require(reopened->undo_depth()==0,"remote replay creates local history");
    done(reopened->restore_local_history(snapshot));
    auto restored=take(reopened->try_undo());require(restored.has_value(),"restored undo missing");
    applied(*a,wire(*restored));
    require(take(a->resolve(token)).byte_offset==6,"undo does not restore original marker");
    auto redone=take(reopened->try_redo());require(redone.has_value(),"restored redo missing");
    require(op_identity(*redone)!=op_identity(*restored),"compensation identity reused");
    applied(*a,wire(*redone));require(!a->resolve(token),"redo doesn't detach");
    auto fresh=engine(4);for(const auto&op:history)applied(*fresh,wire(op));
    applied(*fresh,wire(*restored));applied(*fresh,wire(*redone));
    require(fresh->text()==a->text() && !fresh->resolve(token),"fresh replay mismatch");
    require(changes==publications+3,"notification/publication counts");
    auto wrong=take(CrdtEngine::create(std::make_shared<Counter>(30), {"other-conversation","block",Primitive::text,1}));
    require(!wrong->resolve(token) && wrong->resolve(token).error().code()==ErrorCode::wrong_document,"marker document scope check");
    auto invalid=engine(5);
    require(!invalid->resolve(token) && invalid->resolve(token).error().code()==ErrorCode::awaiting_state,"token causal check");
}
void concurrent() {
    auto seed=engine(1);auto initial=take(seed->try_insert(0,"ab"));auto marker=take(seed->try_insert_marker(1));
    auto left=engine(2),right=engine(3);for(auto*e:{left.get(),right.get()}){applied(*e,initial);applied(*e,marker.operation);}
    auto x=take(left->try_insert_at_marker(marker.marker,MarkerSide::before,"😀"));
    auto y=take(right->try_insert_at_marker(marker.marker,MarkerSide::after,"é"));
    auto first=engine(4),second=engine(5);
    for(auto*e:{first.get(),second.get()}){applied(*e,initial);applied(*e,marker.operation);}
    applied(*first,x);applied(*first,y);applied(*second,y);applied(*second,x);
    require(first->text()=="a😀éb" && first->text()==second->text(),"concurrent order");
    require(take(first->resolve(marker.marker)).byte_offset==5 && take(second->resolve(marker.marker)).byte_offset==5,"concurrent marker position");
    auto deferred=engine(6);require(deferred->apply_remote(marker.operation).kind()==ApplyOutcomeKind::deferred,"marker causal deferral");
    applied(*deferred,initial);require(take(deferred->resolve(marker.marker)).byte_offset==1,"deferred marker not drained");
}
void projection() {
    // Stable actor per stream, >512 canonical transitions without actor growth.
    PositionScope scope{"conversation","block",Primitive::text,1};
    auto allocator=take(ProjectionAllocator::create(scope,"wave/legacy-body",1));
    require(allocator->actor().to_string()=="eadfa202d98ae7e49be8ed2c9515fbb8","projection actor derivation changed");
    require(!ProjectionAllocator::create(scope,"",1) && !ProjectionAllocator::create(scope,"wave/legacy-body",0),"invalid projection adapter accepted");
    auto other=take(ProjectionAllocator::create(scope,"wave/legacy-body",2));
    require(other->actor()!=allocator->actor(),"adapter version namespace collision");
    auto first=take(CrdtEngine::create(allocator,scope));
    auto second=take(CrdtEngine::create(take(ProjectionAllocator::create(scope,"wave/legacy-body",1)),scope));
    auto replay=engine(12);
    for(int n=0;n<600;++n) {
        const auto body=std::to_string(n)+"😀é";
        auto a=take(first->try_replace(0,first->length(),body));
        auto b=take(second->try_replace(0,second->length(),body));
        require(take(encode_operation(a))==take(encode_operation(b)),"projection bytes differ");
        applied(*replay,wire(a));require(replay->text()==body,"projection replay differs");
    }
    allocator->observe(UINT64_MAX);
    auto exhausted=allocator->reserve_run(1);
    require(!exhausted && exhausted.error().code()==ErrorCode::counter_exhausted,"projection counter wraps");
}
void marker_archival() {
    const std::string insertion = R"archive(CTBO0003:{"t":"e","ts":["00000000000000000000000000000015","0000000000000002"],"v":{},"a":["0000000000000001","0000000000000002"],"marker":"0000000000000001","did":["00000000000000000000000000000015","0000000000000002"],"r":[],"nt":[],"if":[{"o":["00000000000000000000000000000015","0000000000000001"],"k":{"p":[],"s":"0000000000000001","r":[]},"c":"","n":"0000000000000001","m":"0000000000000001"}],"dr":[]})archive";
    const std::string removal = R"archive(CTBO0003:{"t":"e","ts":["00000000000000000000000000000015","0000000000000003"],"v":{"00000000000000000000000000000015":"0000000000000002"},"a":["0000000000000003","0000000000000001"],"marker":"0000000000000001","did":["00000000000000000000000000000015","0000000000000003"],"r":[],"nt":[],"if":[],"dr":[[["00000000000000000000000000000015","0000000000000001"],"0000000000000001"]]})archive";
    const std::string token_hex = "4354505400020500000c636f6e766572736174696f6e0005626c6f636b00000000000000010000000000000000000000000000001500000000000000010001000000000000000000000000000000150000000000000002";
    std::string token_bytes;
    for(size_t n=0;n<token_hex.size();n+=2) token_bytes.push_back(static_cast<char>(std::stoul(token_hex.substr(n,2),nullptr,16)));
    auto token=take(decode_position_as<TextMarker>(token_bytes));
    require(take(encode_position(PositionToken{token}))==token_bytes,"marker token golden changed");
    auto author=engine(21);auto minted=take(author->try_insert_marker(0));
    require(take(encode_operation(minted.operation))==insertion,"marker insertion golden changed");
    require(take(encode_position(PositionToken{minted.marker}))==token_bytes,"minted marker token golden changed");
    auto deleted=take(author->try_remove_marker(minted.marker));
    require(take(encode_operation(deleted))==removal,"marker removal golden changed");
    auto replay=engine(22);applied(*replay,take(decode_operation(insertion)));
    require(take(replay->resolve(token)).byte_offset==0,"marker archive referent changed");
    applied(*replay,take(decode_operation(removal)));
    require(!replay->resolve(token) && replay->resolve(token).error().code()==ErrorCode::detached,"marker archive removal changed");
    auto undo=take(author->try_undo());require(undo.has_value(),"marker archive undo missing");
    applied(*replay,wire(*undo));require(take(replay->resolve(token)).byte_offset==0,"marker archive compensation changed");
    auto malformed=removal;malformed.replace(4,4,"0002");require(!decode_operation(malformed),"marker schema accepted as text v2");
    for(size_t n=0;n<token_bytes.size();++n)require(!decode_position(std::string_view(token_bytes).substr(0,n)),"truncated marker token accepted");
    require(!decode_position(token_bytes+"x"),"marker token trailing byte accepted");
}

void archival() {
    // Retained literals: never generate these from the current encoder.
    const std::string inserted = R"archive(CTBO0002:{"t":"e","ts":["00000000000000000000000000000001","0000000000000002"],"v":{},"a":["0000000000000001","0000000000000002"],"did":["00000000000000000000000000000001","0000000000000002"],"r":[["0000000000000000","0000000000000000"]],"nt":["a"],"if":[{"o":["00000000000000000000000000000001","0000000000000001"],"k":{"p":[],"s":"0000000000000001","r":[]},"c":"a","n":"0000000000000001"}],"dr":[]})archive";
    const std::string compensated = R"archive(CTBO0002:{"t":"u","ts":["00000000000000000000000000000001","0000000000000003"],"v":{"00000000000000000000000000000001":"0000000000000002"},"a":["0000000000000003","0000000000000001"],"c":[[["00000000000000000000000000000001","0000000000000001"],"0000000000000001"]]})archive";
    auto e=engine(20);
    auto op=take(decode_operation(inserted));
    require(op_identity(op).counter==2,"archive identity/dependencies changed");
    applied(*e,op);require(e->text()=="a","archive insert changed");
    auto position=take(e->attachment_at(0,Affinity::left,ReferentSide::following));
    applied(*e,take(decode_operation(compensated)));require(e->text().empty(),"archive compensation changed");
    require(!e->resolve(position) && e->resolve(position).error().code()==ErrorCode::detached,"archive item identity changed");
    auto unknown=inserted;unknown.replace(4,4,"000f");
    auto refused=decode_operation(unknown);
    require(!refused && refused.error().code()==ErrorCode::unsupported_version,"unknown archive version accepted");
    require(take(encode_operation(op))==inserted,"archived canonical encoding changed");
}
}
int main() {
    try { markers();concurrent();projection();archival();marker_archival();std::cout<<"Wave Core marker, replay, undo, projection and archive traces passed\n"; }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
