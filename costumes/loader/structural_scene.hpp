#pragma once
// Plain-bytes editing of the game's esf scenes (RE Engine .scn.20, RSZ): lists the costume
// folders (via.Folder named esfNNNvCC) and appends new ones. No REFramework / Windows dependency.
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

// Costume folders of an esf scene: fighter dir ("esf034") -> costume numbers found as esfNNNvCC.
// Empty map if the scene layout is not recognised.
std::map<std::string, std::set<int>> scene_costume_folders(const std::vector<uint8_t>& scn);

// Appends folders esfNNNv<first..last-1> for each fighter dir, copied from its template folder
// (prefer_v04: the version scenes' rule, v04 else the lowest costume; otherwise v00 only) under the
// same parent. A fighter with no template folder in this scene is skipped. False and data untouched
// on any layout it does not recognise; err says why. The output is byte-identical to REasy-parser's.
bool add_scene_folders(std::vector<uint8_t>& scn, const std::vector<std::string>& fighter_dirs,
                       bool prefer_v04, int first, int last, std::string* err);
