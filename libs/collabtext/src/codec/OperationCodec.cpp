#include "crdt/Serialization.h"
#include "crdt/OperationPayload.h"
#include "crdt/OperationValidation.h"
#include "codec/JsonReader.h"
#include <algorithm>
#include <initializer_list>
#include <limits>

namespace CollabText::Crdt {
namespace {
using Codec::JsonValue;
constexpr size_t frame_limit = 8u << 20;
constexpr size_t string_limit = 4u << 20;
constexpr size_t actor_limit = 512;
constexpr uint64_t last_counter = UINT64_MAX - 1;

Error malformed(std::string message) { return Error(ErrorCode::malformed, std::move(message), "operation codec v1"); }
Error invalid_identity(std::string message) { return Error(ErrorCode::invalid_identity, std::move(message), "operation codec v1"); }
bool usable(Identity id) { return id.actor.valid() && id.counter && id.counter <= last_counter; }
std::string hex(uint64_t value) {
    std::string out(16, '0');
    for (size_t i = 16; i; value >>= 4) out[--i] = "0123456789abcdef"[value & 15];
    return out;
}
Error parse_error(const Codec::JsonError& e) {
    using K = Codec::JsonErrorKind;
    const bool unicode = e.kind == K::invalid_utf8 || e.kind == K::lone_surrogate_escape || e.kind == K::invalid_unicode_escape;
    return Error(unicode ? ErrorCode::invalid_unicode : ErrorCode::malformed,
                 Codec::to_string(e.kind), "byte " + std::to_string(e.offset) + ": " + e.context);
}
// The operation families carry their own version: the frontier families (CTGV,
// CTGP) did not change when S03 moved placement from fractional locators to tree
// links, so they stay at 0001 and a v1 operation is refused as an unsupported
// version rather than misread as a malformed v2 one.
Result<JsonValue> read_frame(std::string_view bytes, std::string_view magic,
                             std::string_view version = "0001") {
    if (bytes.size() > frame_limit) return malformed("frame exceeds 8 MiB");
    if (bytes.size() < 4) {
        if (!bytes.empty() && magic.substr(0, bytes.size()) == bytes)
            return malformed("truncated format identifier");
        return Error(ErrorCode::unknown_format, "unknown operation codec format");
    }
    if (bytes.substr(0, 4) != magic) return Error(ErrorCode::unknown_format, "wrong operation codec family");
    if (bytes.size() < 9) return malformed("truncated version header");
    if (bytes[8] != ':') return malformed("missing header delimiter");
    for (char c : bytes.substr(4, 4))
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return malformed("version must be four lowercase hex digits");
    if (bytes.substr(4, 4) != version) return Error(ErrorCode::unsupported_version, "unsupported operation codec version");
    JsonValue root;
    Codec::JsonError error;
    Codec::JsonReader reader;
    if (!reader.parse_syntax(bytes.substr(9), root, error)) return parse_error(error);
    if (!root.is_object()) return malformed("payload must be an object");
    return root;
}

// The writer stops before exceeding a byte limit. Final checked parsing applies
// the same JSON element/depth budgets as the decoder; no unchecked wire escapes.
class Writer {
public:
    std::string bytes;
    Error error;
    void raw(std::string_view text) {
        if (error) return;
        if (text.size() > frame_limit - bytes.size()) { error = malformed("encoded frame exceeds 8 MiB"); return; }
        bytes.append(text);
    }
    void string(std::string_view value) {
        if (error) return;
        if (value.size() > string_limit) { error = malformed("string exceeds 4 MiB"); return; }
        if (const auto e = Codec::validate_utf8(value); !e.ok()) {
            error = Error(ErrorCode::invalid_unicode, Codec::describe(e)); return;
        }
        raw("\"");
        for (unsigned char c : value) {
            if (error) break;
            switch(c) {
                case '"': raw("\\\""); break;
                case '\\': raw("\\\\"); break;
                case '\n': raw("\\n"); break;
                case '\r': raw("\\r"); break;
                case '\t': raw("\\t"); break;
                case '\b': raw("\\b"); break;
                case '\f': raw("\\f"); break;
                default:
                    if (c < 32) {
                        char escaped[] = {'\\','u','0','0',"0123456789abcdef"[c >> 4],"0123456789abcdef"[c & 15]};
                        raw(std::string_view(escaped, 6));
                    } else { char byte = static_cast<char>(c); raw(std::string_view(&byte, 1)); }
            }
        }
        raw("\"");
    }
    void number(uint64_t value) { string(hex(value)); }
    void identity(Identity id) {
        if (error) return;
        if (!usable(id)) { error = invalid_identity("invalid identity"); return; }
        raw("["); string(id.actor.to_string()); raw(","); number(id.counter); raw("]");
    }
    void global(const Global& g) {
        if (error) return;
        if (g.pair_count() > actor_limit) { error = malformed("frontier exceeds 512 actors"); return; }
        raw("{");
        for (size_t i=0; i<g.pair_count(); ++i) {
            const auto [actor, counter] = g.pair(i);
            if (!usable({actor,counter})) { error = invalid_identity("invalid frontier component"); return; }
            if (i) raw(",");
            string(actor.to_string()); raw(":"); number(counter);
        }
        raw("}");
    }
    // An absent identity is an empty array; a present one is the usual [actor,counter].
    void optional_identity(const std::optional<Identity>& id) {
        if (!id) { raw("[]"); return; }
        identity(*id);
    }
    void link(const Link& l) {
        raw("{\"p\":"); optional_identity(l.parent);
        raw(",\"s\":"); number(l.side == Side::Left ? 0 : 1);
        raw(",\"r\":"); optional_identity(l.right_origin); raw("}");
    }
    void counts(const std::vector<std::pair<Identity,uint32_t>>& values) {
        raw("[");
        for (size_t i=0;i<values.size();++i) {
            if(i) raw(","); raw("["); identity(values[i].first); raw(","); number(values[i].second); raw("]");
        }
        raw("]");
    }
    Result<std::string> finish(std::string_view magic, std::string_view version = "0001") {
        if(error) return error;
        auto parsed = read_frame(bytes, magic, version);
        if(!parsed) return parsed.error();
        return std::move(bytes);
    }
};

// Checked tree readers retain the first error and never dereference an absent
// field. Each nested object has an exact supported schema.
class Reader {
public:
    Error error;
    void fail(std::string message) { if(!error) error = malformed(std::move(message)); }
    bool schema(const JsonValue& v, std::initializer_list<std::string_view> fields) {
        if(!v.is_object() || v.members().size() != fields.size()) { fail("missing or unknown object field"); return false; }
        for(auto name:fields) if(!v.find(name)) { fail("missing field: " + std::string(name)); return false; }
        return true;
    }
    const JsonValue& field(const JsonValue& v, std::string_view key) {
        if(const auto* value = v.find(key)) return *value;
        fail("missing field: " + std::string(key));
        static const JsonValue missing;
        return missing;
    }
    bool array(const JsonValue& v, size_t count) {
        if(!v.is_array() || v.elements().size()!=count) { fail("wrong array shape"); return false; }
        return true;
    }
    const std::vector<JsonValue>& array(const JsonValue& v) {
        if(!v.is_array()) fail("expected array");
        return v.elements();
    }
    std::string string(const JsonValue& v) {
        if(!v.is_string()) { fail("expected string"); return {}; }
        return v.string_value();
    }
    uint64_t number(const JsonValue& v) {
        if(!v.is_string() || v.string_value().size()!=16) { fail("expected 16-digit hex integer string"); return 0; }
        uint64_t out=0;
        for(char c:v.string_value()) {
            uint64_t digit;
            if(c>='0' && c<='9') digit=c-'0';
            else if(c>='a' && c<='f') digit=c-'a'+10;
            else { fail("noncanonical integer"); return 0; }
            out=(out<<4)|digit;
        }
        return out;
    }
    uint32_t narrow(const JsonValue& v) {
        const auto n=number(v);
        if(n>UINT32_MAX) { fail("integer exceeds uint32 domain"); return 0; }
        return static_cast<uint32_t>(n);
    }
    ActorId actor(const JsonValue& v) {
        if(!v.is_string()) { fail("actor must be a string"); return {}; }
        auto a=ActorId::parse(v.string_value());
        if(!a || a.value().to_string()!=v.string_value()) {
            if(!error) error=invalid_identity("invalid or noncanonical actor"); return {};
        }
        return a.value();
    }
    Identity identity(const JsonValue& v) {
        if(!array(v,2)) return {};
        Identity id(actor(v.elements()[0]),number(v.elements()[1]));
        if(!error && !usable(id)) error=invalid_identity("invalid identity counter");
        return id;
    }
    Global global(const JsonValue& v) {
        Global g;
        if(!v.is_object() || v.members().size()>actor_limit) { fail("invalid or oversized frontier"); return g; }
        for(const auto& [key,value]:v.members()) {
            auto who=ActorId::parse(key);
            if(!who || who.value().to_string()!=key) { if(!error) error=invalid_identity("invalid or noncanonical frontier actor"); break; }
            const auto n=number(value);
            if(!error && !usable({who.value(),n})) error=invalid_identity("invalid frontier head");
            if(error) break;
            g.observe({who.value(),n});
        }
        return g;
    }
    std::optional<Identity> optional_identity(const JsonValue& v) {
        if(v.is_array() && v.elements().empty()) return std::nullopt;
        return identity(v);
    }
    Link link(const JsonValue& v) {
        Link out;
        if(!schema(v,{"p","s","r"})) return out;
        out.parent=optional_identity(field(v,"p"));
        const auto side=number(field(v,"s"));
        if(side>1) { fail("link side must be 0 or 1"); return out; }
        out.side = side==0 ? Side::Left : Side::Right;
        out.right_origin=optional_identity(field(v,"r"));
        return out;
    }
    std::vector<std::pair<Identity,uint32_t>> counts(const JsonValue& v, Identity timestamp) {
        std::vector<std::pair<Identity,uint32_t>> out;
        if(array(v).size()>65536) { fail("too many undo targets"); return out; }
        for(const auto& entry:array(v)) {
            if(!array(entry,2)) break;
            const auto id=identity(entry.elements()[0]);
            const auto count=narrow(entry.elements()[1]);
            if(id>=timestamp) fail("undo target is not older than operation");
            if(error) break;
            out.emplace_back(id,count);
        }
        return out;
    }
    void allocation(const JsonValue& v, Identity ts, const Global& prior, uint64_t count) {
        if(!array(v,2)) return;
        const auto first=number(v.elements()[0]);
        const auto declared=number(v.elements()[1]);
        if(!usable(ts)) { if(!error) error=invalid_identity("invalid allocation actor or timestamp"); return; }
        if(!count || count>ts.counter || declared!=count || first!=ts.counter-count+1 || prior.get(ts.actor)>=first)
            fail("allocated interval disagrees with payload or causal prior");
    }
};

Result<uint64_t> edit_allocation(const EditOperation& edit) {
    if(!usable(edit.timestamp) || edit.deletion_id!=edit.timestamp)
        return invalid_identity("edit deletion identity must equal operation identity");
    auto validated=validate_edit_operation(edit,edit.version);
    if(!validated.ok()) {
        if(validated.error.code()==ErrorCode::counter_exhausted) return malformed("counter interval overflow");
        return validated.error;
    }
    uint64_t scalars=0;
    for(const auto& f:edit.inserted_fragments) {
        if(f.origin.actor!=edit.timestamp.actor || !f.length || scalars>last_counter-f.length)
            return malformed("invalid inserted allocation");
        scalars+=f.length;
    }
    if(scalars>=edit.timestamp.counter) return malformed("inserted allocation underflows first counter");
    const uint64_t first=edit.timestamp.counter-scalars;
    if(edit.version.get(edit.timestamp.actor)>=first) return malformed("allocation aliases causal prior");
    std::vector<std::pair<uint64_t,uint32_t>> intervals;
    for(const auto& f:edit.inserted_fragments) intervals.emplace_back(f.origin.counter,f.length);
    std::sort(intervals.begin(),intervals.end());
    uint64_t next=first;
    for(auto [start,count]:intervals) {
        if(start!=next || count>edit.timestamp.counter-next) return malformed("item intervals have a gap or overlap");
        next+=count;
    }
    if(next!=edit.timestamp.counter) return malformed("items do not cover allocation");
    return scalars+1;
}
void write_allocation(Writer& w, Identity ts, const Global& prior, uint64_t count) {
    if(!usable(ts)) { w.error=invalid_identity("invalid operation identity"); return; }
    if(!count || count>ts.counter || prior.get(ts.actor)>=ts.counter-count+1) { w.error=malformed("invalid allocation interval"); return; }
    w.raw(",\"ts\":"); w.identity(ts); w.raw(",\"v\":"); w.global(prior);
    w.raw(",\"a\":["); w.number(ts.counter-count+1); w.raw(","); w.number(count); w.raw("]");
}
Error check_counts(const std::vector<std::pair<Identity,uint32_t>>& counts, Identity ts) {
    if(counts.size()>65536) return malformed("too many undo targets");
    for(const auto& [target,count]:counts) {
        (void)count;
        if(!usable(target)) return invalid_identity("invalid undo target");
        if(target>=ts) return malformed("undo target is not older than operation");
    }
    return {};
}
Error check_gap(const CounterGap& gap) {
    if(!gap.actor.valid() || !gap.first || gap.first>last_counter) return invalid_identity("invalid counter gap actor/first");
    if(!gap.count || gap.count>UINT64_MAX-gap.first || gap.prior.get(gap.actor)>=gap.first)
        return malformed("counter gap wraps or aliases prior state");
    return {};
}
} // namespace

Result<std::string> encode_global(const Global& global) {
    Writer w; w.raw("CTGV0001:"); w.global(global); return w.finish("CTGV");
}
Result<Global> decode_global(std::string_view bytes) {
    auto parsed=read_frame(bytes,"CTGV"); if(!parsed) return parsed.error();
    Reader r; auto global=r.global(parsed.value()); if(r.error) return r.error; return global;
}
Result<std::string> encode_counter_gap(const CounterGap& gap) {
    if(auto e=check_gap(gap)) return e;
    Writer w; w.raw("CTGP0001:{\"actor\":"); w.string(gap.actor.to_string());
    w.raw(",\"first\":"); w.number(gap.first); w.raw(",\"count\":"); w.number(gap.count);
    w.raw(",\"v\":"); w.global(gap.prior); w.raw("}"); return w.finish("CTGP");
}
Result<CounterGap> decode_counter_gap(std::string_view bytes) {
    auto parsed=read_frame(bytes,"CTGP"); if(!parsed) return parsed.error();
    Reader r; const auto& root=parsed.value();
    if(!r.schema(root,{"actor","first","count","v"})) return r.error;
    CounterGap gap;
    gap.actor=r.actor(r.field(root,"actor")); gap.first=r.number(r.field(root,"first"));
    gap.count=r.number(r.field(root,"count")); gap.prior=r.global(r.field(root,"v"));
    if(r.error) return r.error;
    if(auto e=check_gap(gap)) return e;
    return gap;
}
Result<std::string> encode_operation(const Operation& op) {
    if (!usable(op_identity(op))) return invalid_identity("invalid or empty text operation");
    const auto* edit = operation_get_if<EditOperation>(&op);
    const bool marker = edit && edit->marker_edit;
    const auto format = marker ? "0003" : "0002";
    Writer w; w.raw(marker ? "CTBO0003:" : "CTBO0002:");
    return visit_operation([&](const auto& value)->Result<std::string> {
        using T=std::decay_t<decltype(value)>;
        if constexpr(std::is_same_v<T,EditOperation>) {
            auto allocation=edit_allocation(value); if(!allocation) return allocation.error();
            w.raw("{\"t\":\"e\""); write_allocation(w,value.timestamp,value.version,allocation.value());
            if (marker) { w.raw(",\"marker\":"); w.number(1); }
            w.raw(",\"did\":"); w.identity(value.deletion_id);
            w.raw(",\"r\":[");
            for(size_t i=0;i<value.ranges.size();++i) {
                if(i) w.raw(","); w.raw("["); w.number(value.ranges[i].first); w.raw(","); w.number(value.ranges[i].second); w.raw("]");
            }
            w.raw("],\"nt\":[");
            for(size_t i=0;i<value.new_text.size();++i) { if(i) w.raw(","); w.string(value.new_text[i]); }
            w.raw("],\"if\":[");
            for(size_t i=0;i<value.inserted_fragments.size();++i) {
                const auto& f=value.inserted_fragments[i]; if(i) w.raw(",");
                w.raw("{\"o\":"); w.identity(f.origin); w.raw(",\"k\":"); w.link(f.link);
                w.raw(",\"c\":"); w.string(f.content); w.raw(",\"n\":"); w.number(f.length); if (marker) { w.raw(",\"m\":"); w.number(1); } w.raw("}");
            }
            w.raw("],\"dr\":[");
            for(size_t i=0;i<value.deletion_runs.size();++i) {
                const auto& d=value.deletion_runs[i]; if(i) w.raw(","); w.raw("["); w.identity(d.first); w.raw(","); w.number(d.count); w.raw("]");
            }
            w.raw("]}");
        } else {
            if(auto e=check_counts(value.counts,value.timestamp)) return e;
            w.raw("{\"t\":\"u\""); write_allocation(w,value.timestamp,value.version,1);
            w.raw(",\"c\":"); w.counts(value.counts); w.raw("}");
        }
        return w.finish("CTBO",format);
    },op);
}
Result<Operation> decode_operation(std::string_view bytes) {
    const bool marker = bytes.size() >= 8 && bytes.substr(4,4) == "0003";
    auto parsed=read_frame(bytes,"CTBO",marker ? "0003" : "0002"); if(!parsed) return parsed.error();
    Reader r; const auto& root=parsed.value(); const auto tag=r.string(r.field(root,"t"));
    if(r.error) return r.error;
    if(tag!="e" && tag!="u") return Error(ErrorCode::unsupported_operation,"unknown text operation tag");
    if(tag=="u" && marker) return malformed("marker format requires a marker edit");
    if(tag=="u") {
        if(!r.schema(root,{"t","ts","v","a","c"})) return r.error;
        UndoOperation op; op.timestamp=r.identity(r.field(root,"ts")); op.version=r.global(r.field(root,"v"));
        op.counts=r.counts(r.field(root,"c"),op.timestamp); r.allocation(r.field(root,"a"),op.timestamp,op.version,1);
        if(r.error) return r.error; return Operation(std::move(op));
    }
    if (marker ? !r.schema(root,{"t","ts","v","a","did","r","nt","if","dr","marker"})
               : !r.schema(root,{"t","ts","v","a","did","r","nt","if","dr"})) return r.error;
    if (marker && r.number(r.field(root,"marker")) != 1) return malformed("invalid marker edit tag");
    EditOperation op; op.marker_edit = marker; op.timestamp=r.identity(r.field(root,"ts")); op.version=r.global(r.field(root,"v"));
    op.deletion_id=r.identity(r.field(root,"did"));
    for(const auto& item:r.array(r.field(root,"r"))) {
        if(!r.array(item,2)) break;
        const auto start=r.narrow(item.elements()[0]); const auto end=r.narrow(item.elements()[1]);
        op.ranges.emplace_back(start,end);
    }
    for(const auto& item:r.array(r.field(root,"nt"))) op.new_text.push_back(r.string(item));
    for(const auto& item:r.array(r.field(root,"if"))) {
        if (marker ? !r.schema(item,{"o","k","c","n","m"}) : !r.schema(item,{"o","k","c","n"})) break;
        if (marker && r.number(r.field(item,"m")) != 1) return malformed("invalid marker fragment tag");
        EditOperation::InsertedFragment f; f.marker = marker;
        f.origin=r.identity(r.field(item,"o")); f.link=r.link(r.field(item,"k"));
        f.content=r.string(r.field(item,"c")); f.length=r.narrow(r.field(item,"n"));
        op.inserted_fragments.push_back(std::move(f));
    }
    for(const auto& item:r.array(r.field(root,"dr"))) {
        if(!r.array(item,2)) break;
        const auto first=r.identity(item.elements()[0]); const auto count=r.narrow(item.elements()[1]);
        op.deletion_runs.push_back({first,count});
    }
    if(r.error) return r.error;
    auto count=edit_allocation(op); if(!count) return count.error();
    r.allocation(r.field(root,"a"),op.timestamp,op.version,count.value());
    if(r.error) return r.error;
    return Operation(std::move(op));
}
Result<std::string> encode_idlist_operation(const IdListOperation& op) {
    if (!usable(idlist_op_identity(op))) return invalid_identity("invalid or empty list operation");
    Writer w; w.raw("CTLO0002:");
    return visit_operation([&](const auto& value)->Result<std::string> {
        using T=std::decay_t<decltype(value)>;
        if constexpr(std::is_same_v<T,IdListInsertOp>) {
            w.raw("{\"t\":\"il-i\""); write_allocation(w,value.timestamp,value.version,1);
            w.raw(",\"id\":"); w.number(value.id); w.raw(",\"k\":"); w.link(value.link);
        } else if constexpr(std::is_same_v<T,IdListRemoveOp>) {
            w.raw("{\"t\":\"il-r\""); write_allocation(w,value.timestamp,value.version,1);
            w.raw(",\"to\":"); w.identity(value.target_origin);
        } else {
            if(auto e=check_counts(value.counts,value.timestamp)) return e;
            w.raw("{\"t\":\"il-u\""); write_allocation(w,value.timestamp,value.version,1);
            w.raw(",\"c\":"); w.counts(value.counts);
        }
        w.raw("}"); return w.finish("CTLO","0002");
    },op);
}
Result<IdListOperation> decode_idlist_operation(std::string_view bytes) {
    auto parsed=read_frame(bytes,"CTLO","0002"); if(!parsed) return parsed.error();
    Reader r; const auto& root=parsed.value(); const auto tag=r.string(r.field(root,"t"));
    if(r.error) return r.error;
    if(tag!="il-i" && tag!="il-r" && tag!="il-u") return Error(ErrorCode::unsupported_operation,"unknown list operation tag");
    if(tag=="il-i") {
        if(!r.schema(root,{"t","ts","v","a","id","k"})) return r.error;
        IdListInsertOp op; op.timestamp=r.identity(r.field(root,"ts")); op.version=r.global(r.field(root,"v"));
        op.id=r.number(r.field(root,"id")); op.link=r.link(r.field(root,"k"));
        r.allocation(r.field(root,"a"),op.timestamp,op.version,1);
        if(r.error) return r.error; return IdListOperation(std::move(op));
    }
    if(tag=="il-r") {
        if(!r.schema(root,{"t","ts","v","a","to"})) return r.error;
        IdListRemoveOp op; op.timestamp=r.identity(r.field(root,"ts")); op.version=r.global(r.field(root,"v"));
        op.target_origin=r.identity(r.field(root,"to")); r.allocation(r.field(root,"a"),op.timestamp,op.version,1);
        if(r.error) return r.error; return IdListOperation(std::move(op));
    }
    if(!r.schema(root,{"t","ts","v","a","c"})) return r.error;
    IdListUndoOpVariant op; op.timestamp=r.identity(r.field(root,"ts")); op.version=r.global(r.field(root,"v"));
    op.counts=r.counts(r.field(root,"c"),op.timestamp); r.allocation(r.field(root,"a"),op.timestamp,op.version,1);
    if(r.error) return r.error; return IdListOperation(std::move(op));
}
} // namespace CollabText::Crdt
