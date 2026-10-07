// .user.2 colour files (RSZ container) - reader and writer for app.CostumeMaterialData.
// Port of SF6_ColorSpinExtra.lua: parse_user (line ~216) and serialize_user (line ~1958), same schema table (RSZ /
// CRC_BY_CLASS, lines ~149-191), same quirks, so a file written by either build reads the same in both. Equivalence is
// checked offline by tools/rsz_harness (the shipped Lua functions vs this code, tree dump + rewritten bytes).
// No REFramework dependency: plain bytes in, bytes out (the live-object writes are in the engine, not here).
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::rsz {

enum class Kind : uint8_t { Bool, U32, S32, F32, Str, Obj, Arr };

struct FieldSpec {
    const char* name;
    Kind kind;
};

struct ClassSpec {
    uint32_t fqn;           // RSZ type id (the files' instance table)
    uint32_t crc;           // instance crc written next to it
    const char* name;       // app.CostumeMaterialData...
    std::vector<FieldSpec> fields;   // serialized order
};

const ClassSpec* spec_by_fqn(uint32_t fqn);
const ClassSpec* spec_by_name(std::string_view name);
const std::vector<ClassSpec>& all_specs();

struct Node;
using NodeRef = std::shared_ptr<Node>;

// One field value. Which member is meaningful depends on the field's Kind.
//   Bool -> b ; U32 / S32 -> u (S32 kept as its 32-bit pattern, like the Lua reader which reads it unsigned)
//   F32 -> f ; Str -> s (ASCII: the low byte of each UTF-16 unit, as the Lua reader keeps it)
//   Obj -> obj (null allowed) ; Arr -> arr (null entries allowed)
struct Value {
    bool b = false;
    uint32_t u = 0;
    float f = 0.0f;
    std::string s;
    NodeRef obj;
    std::vector<NodeRef> arr;
};

struct Node {
    const ClassSpec* cls = nullptr;
    std::vector<Value> fields;   // same order as cls->fields

    Value* field(std::string_view name);
    const Value* field(std::string_view name) const;
};

NodeRef make_node(const ClassSpec* cls);    // every field at its default (false / 0 / "" / null / empty)

struct ParseResult {
    NodeRef root;                // null on error
    std::string error;
};

// Same checks and messages as the Lua reader: "not a .user file", "no RSZ block", "unknown type id %d at
// instance %d", "size mismatch: consumed %d of %d".
ParseResult parse(std::span<const uint8_t> bytes);

// Same layout as the Lua writer: instances children-first (spec order), shared nodes emitted once, null
// instance 0, data block aligned to 16, USR header pointing at the RSZ block (0x30).
std::vector<uint8_t> serialize(const NodeRef& root);

// The nodes in serialization order (index 0 = the first emitted, i.e. file instance 1). Parsing a file written by
// serialize() gives the nodes in this same order: this is the node numbering both sides of the editor sync share.
std::vector<NodeRef> emit_order(const NodeRef& root);

}  // namespace engine::rsz
