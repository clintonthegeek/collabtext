#include "Adapter.h"
#include <collabtext/AnchorPins.h>
#include <csignal>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <vector>
using namespace Consumer;
namespace {
std::string quote(std::string_view value) {
    constexpr char hex[]="0123456789abcdef";
    std::string out="\"";
    for(unsigned char c:value) {
        if(c=='"' || c=='\\') { out+='\\'; out+=char(c); }
        else if(c<32) { out+="\\u00"; out+=hex[c>>4]; out+=hex[c&15]; }
        else out+=char(c);
    }
    return out+'"';
}
std::string position(const Resolution& p) {
    return "{\"state\":"+quote(p.state)+",\"offset\":"+(p.offset?std::to_string(*p.offset):"null")+
        ",\"error\":"+quote(p.error)+",\"lost_left\":"+(p.lost_left?"true":"false")+
        ",\"lost_right\":"+(p.lost_right?"true":"false")+"}";
}
uint32_t usable_offset(const Resolution& p) {
    require((p.state=="Resolved" || p.state=="Collapsed") && p.offset.has_value(),"no usable consumer offset");
    return *p.offset;
}
std::string token(Block& block, uint32_t offset, Affinity affinity=Affinity::left, bool strict=false) {
    if(strict) return checked(encode_position(checked(block.engine->attachment_at(offset,affinity,ReferentSide::following))));
    return checked(encode_position(checked(block.engine->boundary_at(offset,affinity))));
}
// Consumer-owned persistence for WP5's reopen step: length-prefixed records
// under a magic header. Nothing here is engine API and nothing is a store --
// the only claim is that bytes written by one process are readable by another.
constexpr std::string_view kBundleMagic = "MKB1";
void write_bundle(const std::string& path, const std::vector<std::string>& records) {
    std::ofstream file(path,std::ios::binary);
    file.write(kBundleMagic.data(),static_cast<std::streamsize>(kBundleMagic.size()));
    for(const auto& record : records) {
        const auto size=static_cast<uint32_t>(record.size());
        for(int n=3;n>=0;--n) file.put(static_cast<char>(size>>(n*8)));
        file.write(record.data(),static_cast<std::streamsize>(record.size()));
    }
    file.flush();
    require(static_cast<bool>(file),"bundle write failed");
}
std::vector<std::string> read_bundle(const std::string& path) {
    std::ifstream file(path,std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(file)),{});
    require(std::string_view(bytes).starts_with(kBundleMagic),"bundle magic missing");
    std::vector<std::string> out; size_t at=kBundleMagic.size();
    while(at<bytes.size()) {
        require(bytes.size()-at>=4 && out.size()<16,"truncated bundle record header");
        uint32_t size=0; for(int n=0;n<4;++n) size=(size<<8)|static_cast<unsigned char>(bytes[at++]);
        require(size<=(8u<<20) && size<=bytes.size()-at,"truncated bundle record");
        out.emplace_back(bytes.substr(at,size)); at+=size;
    }
    return out;
}
// Phase one. Mints in this process and persists only bytes; nothing about the
// live engines crosses into the replay process but the file.
std::string mint_replay(const std::string& path) {
    Document d("doc",1); auto& b=d.add("body");
    b.engine->insert(0,"ab");
    require(b.emitted.size()==1,"expected exactly one seeded text operation");
    auto a=list(); auto inserted=checked(a->try_insert_after(Crdt::Anchor::min(),UINT64_MAX));
    const std::vector<std::string> records{
        b.emitted[0],
        token(b,1), token(b,1,Affinity::right), token(b,1,Affinity::right,true),
        checked(Crdt::encode_idlist_operation(inserted)),
        checked(encode_position(checked(a->element_of(UINT64_MAX))))};
    write_bundle(path,records);
    return "{\"records\":"+std::to_string(records.size())+",\"tokens\":3}";
}
// Phase two, in a fresh process. Builds its own engines, replays the recorded
// operations, takes a remote edit, and resolves the *original* token bytes in
// two physically independent text engines plus one list engine.
std::string replay(const std::string& path) {
    auto records=read_bundle(path);
    require(records.size()==6,"unexpected bundle record count");
    auto replica=checked(CrdtEngine::create(std::make_shared<Counter>(9),{"doc","body",Primitive::text,1}));
    auto peer=checked(CrdtEngine::create(std::make_shared<Counter>(2),{"doc","body",Primitive::text,1}));
    auto base=checked(Crdt::decode_operation(records[0]));
    require(replica->applyRemoteOp(base),"seed operation refused by replica");
    require(peer->applyRemoteOp(base),"seed operation refused by peer");
    Crdt::Operation edit; peer->setOnLocalOp([&](const auto& op){edit=op;});
    peer->insert(1,"X");
    const auto wire=checked(Crdt::encode_operation(edit));
    require(replica->applyRemoteOp(checked(Crdt::decode_operation(wire))),"remote edit refused");
    auto elements=checked(IdListEngine::create(std::make_shared<Counter>(9),{"doc","structure",Primitive::list,1}));
    require(elements->apply_ops({checked(Crdt::decode_idlist_operation(records[4]))})[0].kind()==ApplyOutcomeKind::applied,"list operation refused");
    const auto left=checked(decode_position_as<TextBoundary>(records[1]));
    const auto right=checked(decode_position_as<TextBoundary>(records[2]));
    const auto strict=checked(decode_position_as<TextAttachment>(records[3]));
    const auto element=checked(decode_position_as<ListElement>(records[5]));
    // Every input frame is destroyed before the first resolve: decoded values
    // own their data, so a token that still worked cannot have been borrowed.
    for(auto& bytes : records) bytes.assign(bytes.size(),'!');
    const auto a=checked(replica->resolve(left)),b=checked(replica->resolve(right)),c=checked(replica->resolve(strict));
    const auto p=checked(peer->resolve(left)),q=checked(peer->resolve(right)),r=checked(peer->resolve(strict));
    const auto index=checked(elements->resolve(element));
    return std::string("{\"text\":")+quote(replica->text())+",\"peer_text\":"+quote(peer->text())+
        ",\"replicas\":[["+std::to_string(a.byte_offset)+","+std::to_string(b.byte_offset)+","+std::to_string(c.byte_offset)+"],["+
        std::to_string(p.byte_offset)+","+std::to_string(q.byte_offset)+","+std::to_string(r.byte_offset)+"]]"+
        ",\"list_index\":"+std::to_string(index.element_index)+
        ",\"operations_replayed\":2,\"original_bytes\":true}";
}
std::string run(const std::string& name) {
    Document d("doc",1); auto& b=d.add("body"); auto& e=*b.engine;
    if(name=="affinity") {
        e.insert(0,"ab");auto l=token(b,1),r=token(b,1,Affinity::right);e.insert(1,"X");
        return "{\"text\":"+quote(e.text())+",\"left\":"+position(d.resolve("body",l))+",\"right\":"+position(d.resolve("body",r))+"}";
    }
    if(name=="scalar_sweep" || name=="scalar_boundaries") {
        e.insert(0,"a\xf0\x9d\x84\x9e" "b");std::string out="{\"offsets\":[";bool first=true;
        for(uint32_t at:{0u,1u,5u,6u}) for(Affinity a:{Affinity::left,Affinity::right}) {
            if(!first) out+=','; first=false; out+=std::to_string(checked(e.resolve(checked(e.boundary_at(at,a)))).byte_offset);
        }
        auto end=checked(e.boundary_at(6,Affinity::left)); auto previous=checked(e.boundary_at(5,Affinity::left));
        auto cmp=checked(e.compare(end,previous));auto saved_counter=b.allocator->peek();auto saved_changes=b.changes;auto saved_ops=b.emitted.size();
        out+="],\"comparison\":"+std::to_string(cmp);
        if(name=="scalar_boundaries") {
            out+=",\"off_end\":"+quote(refusal(e.boundary_at(7,Affinity::left)))+",\"interiors\":[";
            for(unsigned at=2;at<=4;++at) { if(at!=2) out+=',';out+=quote(refusal(e.boundary_at(at,Affinity::right))); }
            out+=']';
        }
        require(saved_counter==b.allocator->peek() && saved_changes==b.changes && saved_ops==b.emitted.size(),"query changed observable state");
        return out+",\"pure\":true}";
    }
    if(name=="shared_token") {
        e.insert(0,"a");e.insert(1,"b");auto l=token(b,1),r=token(b,1,Affinity::right);
        Document peer("doc",2),observer("doc",3);auto& p=peer.add("body");auto& o=observer.add("body");
        for(const auto& op:b.emitted) { require(peer.receive("body",op),"seed transfer");require(observer.receive("body",op),"seed transfer"); }
        p.engine->insert(1,"X");p.engine->insert(3,"!");
        // Opposite delivery order plus duplicates, using the same original tokens.
        for(const auto& op:p.emitted) require(d.receive("body",op),"edit transfer");
        observer.receive("body",p.emitted[1]);observer.receive("body",p.emitted[0]);observer.receive("body",p.emitted[0]);
        require(o.engine->text()==e.text(),"out-of-order replay divergence");
        return "{\"text\":"+quote(e.text())+",\"replicas\":[["+
            std::to_string(usable_offset(d.resolve("body",l)))+","+std::to_string(usable_offset(d.resolve("body",r)))+"],["+
            std::to_string(usable_offset(observer.resolve("body",l)))+","+std::to_string(usable_offset(observer.resolve("body",r)))+"]]}";
    }
    if(name=="caret_neighbour") {
        e.insert(0,"a");e.insert(1,"b");e.insert(2,"c");auto bytes=token(b,2);
        e.remove(0,1);
        return "{\"text\":"+quote(e.text())+",\"result\":"+position(d.resolve("body",bytes))+"}";
    }
    if(name=="caret_delete" || name=="strict_delete") {
        e.insert(0,"a");e.insert(1,"b");e.insert(2,"c"); auto bytes=token(b,1,Affinity::left,name=="strict_delete");
        e.remove(1,1);auto deleted=d.resolve("body",bytes);require(e.undo(),"delete undo");auto restored=d.resolve("body",bytes);
        require(e.redo(),"delete redo");auto redone=d.resolve("body",bytes);
        return "{\"deleted\":"+position(deleted)+",\"restored\":"+position(restored)+",\"redone\":"+position(redone)+",\"original_bytes\":true}";
    }
    if(name=="block_gone") {
        e.insert(0,"ab");auto bytes=token(b,1);d.erase("body");
        // Invalid bytes must still be Gone: consumer ownership precedes decode.
        return "{\"valid\":"+position(d.resolve("body",bytes))+",\"invalid\":"+position(d.resolve("body","invalid"))+",\"retained\":"+(bytes.empty()?"false":"true")+"}";
    }
    if(name=="empty_edges") {
        auto l=token(b,0),r=token(b,0,Affinity::right);e.insert(0,"ab");
        auto end=token(b,2),end_r=token(b,2,Affinity::right);e.insert(2,"X");
        return "{\"empty\":["+std::to_string(usable_offset(d.resolve("body",l)))+","+std::to_string(usable_offset(d.resolve("body",r)))+
          "],\"end\":["+std::to_string(usable_offset(d.resolve("body",end)))+","+std::to_string(usable_offset(d.resolve("body",end_r)))+"]}";
    }
    if(name=="list_indices") {
        auto a=list(),peer=list(2);auto op=checked(a->try_insert_after(Crdt::Anchor::min(),10));
        auto after=checked(a->try_anchor_of(10));auto op2=checked(a->try_insert_after(after,20));
        require(peer->apply_ops({checked(Crdt::decode_idlist_operation(checked(Crdt::encode_idlist_operation(op)))),op2})[1].kind()==ApplyOutcomeKind::applied,"list transfer");
        auto element=checked(a->element_of(20));auto bytes=checked(encode_position(element));
        auto retained=checked(decode_position_as<ListElement>(bytes));
        std::string out="{\"indices\":[";
        for(unsigned at=0;at<=2;++at) { if(at) out+=',';out+=std::to_string(checked(a->resolve(checked(a->boundary_at(at,Affinity::left)))).element_index); }
        require(checked(peer->resolve(retained)).element_index==1,"same list token peer");
        return out+"],\"element\":1,\"peer\":1,\"off_end\":"+quote(refusal(a->boundary_at(3,Affinity::left)))+"}";
    }
    if(name=="codec") {
        e.insert(0,"ab");auto bytes=token(b,1);auto saved_changes=b.changes;auto saved_counter=b.allocator->peek();auto saved_ops=b.emitted.size();
        require(checked(encode_position(checked(decode_position(bytes))))==bytes,"canonical bytes");
        unsigned prefixes=0,suffixes=0;
        for(size_t n=0;n<bytes.size();++n) { require(!decode_position(std::string_view(bytes).substr(0,n)).ok(),"accepted prefix");++prefixes; }
        for(unsigned n=0;n<256;++n) { require(!decode_position(bytes+char(n)).ok(),"accepted trailing byte");++suffixes; }
        auto version=bytes;version[5]=2;auto enum_bytes=bytes;enum_bytes[7]=2;
        auto newer=refusal(decode_position(version)),bad=refusal(decode_position(enum_bytes));
        auto copied=checked(decode_position_as<TextBoundary>(bytes));bytes.assign(bytes.size(),'!');
        require(checked(e.resolve(copied)).byte_offset==1,"decoder input lifetime");
        require(saved_changes==b.changes && saved_counter==b.allocator->peek() && saved_ops==b.emitted.size(),"codec changed engine");
        return "{\"roundtrip\":true,\"prefixes\":"+std::to_string(prefixes)+",\"suffixes\":"+std::to_string(suffixes)+",\"version\":"+quote(newer)+",\"enum\":"+quote(bad)+",\"pure\":true,\"owned\":true}";
    }
    if(name=="malformed") {
        e.insert(0,"ab");auto bytes=token(b,1);auto count=b.changes;auto ops=b.emitted.size();auto counter=b.allocator->peek();
        auto p=d.resolve("body",bytes+'!');require(count==b.changes && ops==b.emitted.size() && counter==b.allocator->peek() && e.text()=="ab","malformed changed engine");
        return "{\"result\":"+position(p)+",\"pure\":true}";
    }
    if(name=="split_paste") {
        Document peer("doc",2);auto& p=peer.add("body");e.insert(0,"abc");p.engine->insert(0,"XYZ");
        require(peer.receive("body",b.emitted[0]),"paste transfer");require(d.receive("body",p.emitted[0]),"paste transfer");
        require(e.text()==p.engine->text(),"paste convergence");
        bool contiguous=e.text().find("abc")!=std::string::npos && e.text().find("XYZ")!=std::string::npos;
        return "{\"text\":"+quote(e.text())+",\"contiguous\":"+(contiguous?"true":"false")+",\"converged\":true}";
    }
    if(name=="wrong_context") {
        e.insert(0,"ab");auto bytes=token(b,1);auto typed=checked(decode_position_as<TextBoundary>(bytes));
        Document doc("other",2),stream("doc",2),epoch("doc",2,2);auto& x=doc.add("body");auto& y=stream.add("other");auto& z=epoch.add("body");
        auto wrong_doc=doc.resolve("body",bytes),wrong_stream=stream.resolve("other",bytes),wrong_epoch=epoch.resolve("body",bytes);
        auto list_kind=refusal(decode_position_as<ListBoundary>(bytes));auto wrong_factory=refusal(IdListEngine::create(std::make_shared<Counter>(2),{"doc","body",Primitive::text,1}));
        // Empty foreign engines establish scope errors precede missing mint state.
        require(x.engine->text().empty()&&y.engine->text().empty()&&z.engine->text().empty() && !x.changes && !y.changes && !z.changes,"foreign query mutation");
        return "{\"document\":"+position(wrong_doc)+",\"stream\":"+position(wrong_stream)+",\"epoch\":"+position(wrong_epoch)+",\"primitive\":"+quote(list_kind)+",\"factory\":"+quote(wrong_factory)+",\"pure\":true}";
    }
    if(name=="missing_list") {
        auto a=list();checked(a->try_insert_after(Crdt::Anchor::min(),10));auto before=a->ids();auto version=a->version();auto depth=a->undo_depth();
        auto missing=refusal(a->element_of(999));auto old=refusal(a->try_anchor_of(999));
        auto insert=refusal(a->try_insert_after(Crdt::Anchor::end(),30));auto remove=refusal(a->try_remove_at(Crdt::Anchor::end()));
        require(before==a->ids() && version==a->version() && depth==a->undo_depth(),"missing list mutation");
        return "{\"lookup\":"+quote(missing)+",\"legacy_lookup\":"+quote(old)+",\"insert\":"+quote(insert)+",\"remove\":"+quote(remove)+",\"pure\":true}";
    }
    if(name=="no_gc") {
        e.insert(0,"a");e.insert(1,"b");auto bytes=token(b,1,Affinity::left,true);e.remove(1,1);
        auto detached=d.resolve("body",bytes);require(e.undo(),"retained text undo");auto restored=d.resolve("body",bytes);
        auto a=list();checked(a->try_insert_after(Crdt::Anchor::min(),10));auto element=checked(a->element_of(10));auto anchor=checked(a->try_anchor_of(10));
        checked(a->try_remove_at(anchor));require(a->entry_count()==1 && a->tombstone_count()==1,"no-GC list retained item");
        require(a->undo().has_value() && checked(a->resolve(element)).element_index==0,"no-GC retained list undo");
        return "{\"deleted\":"+position(detached)+",\"restored\":"+position(restored)+",\"retained_entries\":1,\"gc_enabled\":false}";
    }
    if(name=="pins") {
        const PositionScope scope{"doc", "body", Primitive::text, 1};
        auto boundary=checked(e.boundary_at(0,Affinity::left));
        auto bytes=checked(encode_position(PositionToken{boundary}));
        std::vector<AnchorPinEvent> canonical;
        auto registry=checked(AnchorPinRegistry::create(scope,[&](const AnchorPinEvent& event) {
            canonical.push_back(event); return Result<void>{};
        }));
        require(registry->register_pin(1,"event-1","reply-1",bytes).ok(),"pin register");
        require(registry->live_pins().size()==1 && !registry->recovery_complete(2),"pin register/frontier");
        require(checked(registry->lookup_pin("reply-1")).token_bytes==bytes,"pin lookup");
        auto recovered=checked(AnchorPinRegistry::create(scope,[](const AnchorPinEvent&) {
            return Result<void>(Error(ErrorCode::io_error,"replay must not append"));
        }));
        require(recovered->replay(canonical.front()).ok(),"pin replay");
        require(recovered->recovery_complete(1) && recovered->live_pins().front().token_bytes==bytes,
                "pin recovery before collection");
        require(registry->release_pin(2,"event-2","reply-1").ok(),"pin release event");
        require(registry->live_pins().empty(),"pin release");
        require(refusal(registry->lookup_pin("reply-1"))=="not_found","released pin lookup");
        return "{\"capability\":\"durable_pins\",\"available\":true}";
    }
    throw std::runtime_error("unknown probe case");
}
}
int main(int argc,char** argv) {
    if(argc<2 || argc>3) return 1;
    const std::string name=argv[1];
    const bool phased=name=="mint_replay" || name=="replay";
    if(phased && argc!=3) { std::cerr<<"a bundle path is required for "<<name<<'\n';return 1; }
    if(!phased && argc!=2) { std::cerr<<name<<" takes no bundle path\n";return 1; }
    if(name=="abnormal") { std::raise(SIGABRT);return 99; }
    try {
        const std::string observed=phased ? (name=="mint_replay" ? mint_replay(argv[2]) : replay(argv[2])) : run(name);
        std::cout<<"{\"schema\":\"collabtext-installed-worker/1\",\"case\":"<<quote(name)<<",\"observed\":"<<observed<<"}\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 2; }
}
