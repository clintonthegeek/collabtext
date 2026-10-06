// Standalone fresh-process codec/replay proof. This is not a recovered store.
#include <collabtext/Anchor.h>
#include <collabtext/CrdtEngine.h>
#include <collabtext/IdListEngine.h>
#include <collabtext/Serialization.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>
using namespace CollabText;
namespace {
class Counter final : public Crdt::IdentityAllocator {
    ActorId who;
    CheckedCounter counter;
public:
    explicit Counter(uint8_t n) { std::array<uint8_t,16> bytes{}; bytes[15]=n; who=ActorId::from_bytes(bytes); }
    Result<uint64_t> reserve_run(uint64_t count) override { return counter.reserve_run(count); }
    void observe(uint64_t n) override { (void)counter.observe(n); }
    uint64_t peek() const noexcept override { return counter.peek(); }
    const ActorId& actor() const noexcept override { return who; }
};
template<class T> T checked(Result<T> result) {
    if(!result.ok()) {
        std::fprintf(stderr,"worker: %.*s: %.*s\n",int(to_string(result.error().code()).size()),to_string(result.error().code()).data(),
                     int(result.error().message().size()),result.error().message().data()); std::exit(2);
    }
    return std::move(result).value();
}
PositionScope text_scope() { return {"doc","body",Primitive::text,1}; }
PositionScope list_scope() { return {"doc","structure",Primitive::list,1}; }
void write(const char* path,const std::vector<std::string>& records) {
    std::ofstream file(path,std::ios::binary);
    file.write("SPR1",4);
    for(const auto& record:records) {
        const auto size=uint32_t(record.size());
        for(int n=3;n>=0;--n) file.put(static_cast<char>(size>>(n*8)));
        file.write(record.data(),record.size());
    }
    file.flush(); if(!file) std::exit(3);
}
std::vector<std::string> read(const char* path) {
    std::ifstream file(path,std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(file)),{});
    if(!bytes.starts_with("SPR1")) std::exit(3);
    std::vector<std::string> out; size_t at=4;
    while(at<bytes.size()) {
        if(bytes.size()-at<4 || out.size()>=8) std::exit(3);
        uint32_t size=0; for(int n=0;n<4;++n) size=(size<<8)|static_cast<unsigned char>(bytes[at++]);
        if(size>(8u<<20) || size>bytes.size()-at) std::exit(3);
        out.emplace_back(bytes.substr(at,size)); at+=size;
    }
    return out;
}
}
int main(int argc,char** argv) {
    if(argc!=3) return 1;
    const std::string mode=argv[1];
    if(mode=="mint") {
        auto text=checked(CrdtEngine::create(std::make_shared<Counter>(1),text_scope()));
        std::string operation;
        text->setOnLocalOp([&](const auto& op){operation=checked(Crdt::encode_operation(op));});
        text->insert(0,"ab");
        auto left=checked(text->boundary_at(1,Affinity::left));
        auto right=checked(text->boundary_at(1,Affinity::right));
        auto strict=checked(text->attachment_at(1,Affinity::right,ReferentSide::following));
        auto list=checked(IdListEngine::create(std::make_shared<Counter>(1),list_scope()));
        auto inserted=checked(list->try_insert_after(Crdt::Anchor::min(),UINT64_MAX));
        auto element=checked(list->element_of(UINT64_MAX));
        write(argv[2],{operation,checked(encode_position(left)),checked(encode_position(right)),checked(encode_position(strict)),
                       checked(Crdt::encode_idlist_operation(inserted)),checked(encode_position(element))});
        std::puts("minted"); return 0;
    }
    if(mode!="resolve") return 1;
    auto records=read(argv[2]); if(records.size()!=6) return 3;
    auto text=checked(CrdtEngine::create(std::make_shared<Counter>(9),text_scope()));
    auto remote=checked(CrdtEngine::create(std::make_shared<Counter>(2),text_scope()));
    const auto base=checked(Crdt::decode_operation(records[0]));
    if(!text->applyRemoteOp(base) || !remote->applyRemoteOp(base)) return 4;
    const auto left=checked(decode_position_as<TextBoundary>(records[1]));
    const auto right=checked(decode_position_as<TextBoundary>(records[2]));
    const auto strict=checked(decode_position_as<TextAttachment>(records[3]));
    Crdt::Operation edit;
    remote->setOnLocalOp([&](const auto& op){edit=op;});
    remote->insert(1,"X");
    const auto wire=checked(Crdt::encode_operation(edit));
    if(!text->applyRemoteOp(checked(Crdt::decode_operation(wire)))) return 4;
    auto list=checked(IdListEngine::create(std::make_shared<Counter>(9),list_scope()));
    const auto inserted=checked(Crdt::decode_idlist_operation(records[4]));
    if(list->apply_ops({inserted})[0].kind()!=ApplyOutcomeKind::applied) return 4;
    const auto element=checked(decode_position_as<ListElement>(records[5]));
    // Overwrite every input frame after decoding: values must own their data.
    for(auto& bytes:records) bytes.assign(bytes.size(),'!');
    const auto x=checked(text->resolve(left)),y=checked(text->resolve(right)),z=checked(text->resolve(strict));
    const auto index=checked(list->resolve(element));
    if(text->text()!="aXb" || x.byte_offset!=1 || y.byte_offset!=2 || z.byte_offset!=2 || index.element_index!=0) return 5;
    std::printf("left=%u right=%u strict=%u list=%u\n",x.byte_offset,y.byte_offset,z.byte_offset,index.element_index);
    return 0;
}
