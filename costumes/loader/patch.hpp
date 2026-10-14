#pragma once
#include <cstdint>
#include <cstddef>
#include <string_view>
#include <vector>

// Binary search-and-replace of folder references inside file data.
// Replaces  fighter_dir/old_folder/  ->  fighter_dir/new_folder/
//       and fighter_dir_old_folder_  ->  fighter_dir_new_folder_
// in both UTF-16LE and UTF-8, plus uppercase variants of each.
// old_folder and new_folder must be the same length (3-digit strings).
// Returns the total number of individual replacements made.
size_t patch_paths(std::vector<uint8_t>& data,
                   std::string_view fighter_dir,
                   std::string_view old_folder,
                   std::string_view new_folder);

#include <string>
#include <unordered_set>
#include <unordered_map>

// Repoints whole UTF-16LE references (scene, user files) to the slot's copy of a resource.
// relocated_resources: original -> copy, lowercase, without natives/stm/ and version, same length.
// Every occurrence; a leading '@', the case and the slashes of the reference are kept.
// Returns the number of references changed.
size_t patch_resource_references(std::vector<uint8_t>& data,
    const std::unordered_map<std::string, std::string>& relocated_resources);

// Minimal scene patching: only mesh/mdf2/CCVD references in UTF-16LE.
// CCVD: patched only at 2nd occurrence (instance data, not userdata_infos).
// relocated_paths: set of pak paths in output (for existence check). May be null.
// suffixes: extension -> version string (e.g. "tex" -> "241101895"). May be null.
size_t patch_scene_minimal(std::vector<uint8_t>& data,
                           std::string_view fighter_dir,
                           std::string_view old_folder,
                           std::string_view new_folder,
                           const std::unordered_set<std::string>* relocated_paths,
                           const std::unordered_map<std::string, std::string>* suffixes);

// Minimal mdf2 patching: repoints the texture references that name a texture the mod ships,
// from fighter_dir/old_folder/ to fighter_dir/new_folder/, and renames the file name the same way
// relocate_path() renames the file itself (first "<fighter>_<old>_" becomes "<fighter>_<new>_").
// Rewriting only the folder left references to files that were never written.
// mod_tex_keys: "<part>/<file>.tex" relative to fighter_dir/old_folder/, lowercase.
size_t patch_mdf2_minimal(std::vector<uint8_t>& data,
                          std::string_view fighter_dir,
                          std::string_view old_folder,
                          std::string_view new_folder,
                          const std::unordered_set<std::string>& mod_tex_keys);

// Costume table reduction (fightercostumeuserdata.user.2): keeps every vanilla record and, among the
// slot records the static table reserves (100 per character, after the root instance), only those
// whose id is in keep_ids. A character the player does not own lists every record of the table,
// owned or not, so the reserved ones showed up as empty outfits whose missing scene stalled loading.
// Returns false and leaves data untouched when the layout is not the expected one.
// message_for_record (may be null): record id -> id of the message (outfit name) the record must show,
// so that names follow each other per character whatever slot numbers are in use.
bool trim_costume_table(std::vector<uint8_t>& data,
                        const std::unordered_set<uint32_t>& keep_ids,
                        size_t* kept_records, size_t* removed_records,
                        const std::unordered_map<uint32_t, uint32_t>* message_for_record = nullptr);

// The game's costume table as it ships: (fighter id, costume number) of every record. False when data is
// not a costume table, or already carries slot records (instances after the root).
bool costume_table_records(const std::vector<uint8_t>& data, std::vector<std::pair<int, int>>& out);

// Slot records of a fighter: index of its block of 100 ids in the table. Fixed for good (saved choices and
// replays name slots by record): 1-22 -> 0-21, 25 and up -> 22 and up (Arjun, 34 -> 31). 23 and 24 are no
// fighter today; they get blocks past every other one.
int costume_slot_index(int fighter);

struct CostumeSlotRecord {
    uint32_t id;           // record id and ManageId
    uint32_t fighter;
    uint32_t costume_no;
    uint32_t sort_no;
    uint32_t message;      // index into the messages added with it (outfit name)
};

// Adds slot records to the game's costume table, the way make_static_structural.py did: after the root
// instance, one name message per entry of message_guids (id msg_id_base + n), then the records, each a copy
// of the game's record of fighter 1 / Outfit 1, not owned by default, visualNo = its costume number. The
// records go at the end of the root's DataArray. trim_costume_table then keeps those in use.
// False and data untouched when the layout is not the expected one.
bool add_costume_records(std::vector<uint8_t>& data, const std::vector<CostumeSlotRecord>& records,
                         uint32_t msg_id_base, const std::vector<std::vector<uint8_t>>& message_guids,
                         std::string* err = nullptr);
// "a1b2c3d4-0001-4000-8000-000000001388" -> the 16 bytes of a System.Guid (first three groups little-endian)
bool guid_bytes(std::string_view text, std::vector<uint8_t>& out);

// Colour tints of a costume colour file (cmd_*.user): each material cluster in file order, with its
// customize colours (enabled flag, RGBA packed as the file stores it: R in the low byte).
struct CmdCluster { std::string name; std::vector<std::pair<bool, uint32_t>> colors; };
bool cmd_tints(const std::vector<uint8_t>& data, std::vector<CmdCluster>& out);

// Mean of each channel (0..1, RGBA) of a block-compressed texture over its first mip level whose sides
// are both 64 or less (BC1, BC3, BC4, BC5, BC7). False when that level is missing or smaller than 4x4.
bool texture_channel_means(const std::vector<uint8_t>& tex, float out[4]);

// EXPERIMENTAL. Rebuilds a mod's material file (.mdf2) on the game's own file for the same part: same
// material names required (false otherwise, out untouched); the result is the game's file, with each
// texture binding that differs in the mod's file (same material, same parameter) pointing at the mod's
// texture. For material files made for an older engine, whose parameter blocks are outdated.
bool modernize_mdf2(const std::vector<uint8_t>& mod, const std::vector<uint8_t>& game,
                    std::vector<uint8_t>& out, int* rebound);

// Type signatures of an RSZ user file (.user): the game refuses an instance whose type CRC is not the
// one of its current build. A game update can change a type's CRC without changing its layout (an enum
// that gains values); mod files made before it then load without their data.
// Reads (type id -> crc) of every instance into out. Returns false if data is not a user file.
bool user_type_crcs(const std::vector<uint8_t>& data, std::unordered_map<uint32_t, uint32_t>& out);
// Rewrites to current[type] the CRC of every instance whose type is in `upgradable` and whose CRC
// differs. Types of `current` left with another CRC are added to stale_left (may be null).
// Returns the number of instances rewritten.
size_t upgrade_user_crcs(std::vector<uint8_t>& data,
                         const std::unordered_map<uint32_t, uint32_t>& current,
                         const std::unordered_set<uint32_t>& upgradable,
                         std::unordered_set<uint32_t>* stale_left);

// Costume colour files (cmd_*.user) in the 2023 layout, before app.CostumeMaterialData.Cloth gained
// ClothFur_Color: converted to the current layout. Every garment gets the fur block the game ships
// (ShellFurColor + its five values), copied from `reference`, a current colour file of the game, where
// it is disabled. All instances are then written with the CRCs of `current`.
// Returns false and leaves data untouched when the file is not an old colour file.
bool upgrade_costume_material_layout(std::vector<uint8_t>& data,
                                     const std::unordered_map<uint32_t, uint32_t>& current,
                                     const std::vector<uint8_t>& reference,
                                     size_t* garments_upgraded);

// Test hook: parses a colour file and writes it back unchanged. False if the layout is not understood.
bool costume_material_roundtrip(const std::vector<uint8_t>& data, std::vector<uint8_t>& out);

// Texture mip tables (.tex): every level's row pitch must cover a row of blocks (or pixels) and its
// size must be pitch x rows, as in all the game's costume textures. With repair, a level whose data is
// tightly packed but whose pitch is wrong gets the right pitch (counted in *repaired). Returns the
// number of levels left inconsistent; 0 for a texture of unknown format or not a texture.
int texture_mip_check(std::vector<uint8_t>& tex, bool repair, int* repaired);
