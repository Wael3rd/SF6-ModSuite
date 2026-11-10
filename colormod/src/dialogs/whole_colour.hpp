// dialogs::editor::whole - the colour editor's WHOLE colour copy / paste (Wael 10/11): every part of a colour copied
// on the main page goes onto the colour of another outfit, of any character. Pure: trees in, trees out (no game, no
// editor state); the editor keeps the copied tree and reads the keys (editor_core.cpp), tests/whole_colour_test.cpp
// checks the matching offline.
//
// A colour's parts are its clusters (the COLORS page's rows). They are matched by the structure's names, never by
// their places: the colours of one outfit do not all list the same clusters in the same order, and two outfits or
// characters share some of theirs (skin, eyes, hair) under the same names. A cluster's name is
// "<part>/<cluster>#<n>":
//   <part>     its part's Type (0 body, 1 face, 2 hair, 3); the other parts (accessories) are one kind, as the quick
//              edit groups them: the colours of one outfit number theirs 4, 4, 4 or 4, 5, 6, even -1 (Dee Jay's)
//   <cluster>  the cluster's Name, in lower case
//   <n>        the how-manieth cluster of that part and name, in the tree's order (the game repeats names)
// A matched cluster takes every value of the copied one: its colours by their place in the cluster (switch, colour,
// material options), its Emissive and Body data (what the preview writes too). What the target lacks is skipped.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "../engine/rsz.hpp"

namespace dialogs::editor::whole {

namespace rsz = engine::rsz;

// the clusters of a colour, in the tree's order, under their names
std::vector<std::pair<std::string, rsz::NodeRef>> clusters(const rsz::NodeRef& tree);

struct Pasted {
    int pasted = 0;                              // clusters of the copy found in the target, and written
    int total = 0;                               // clusters of the copy
    std::vector<const rsz::Node*> colours;       // the target's colours (CustomizeColorData) that took values
};
// every cluster of `from` found in `to` takes its values; `from` null (nothing copied) = nothing, 0 of 0
Pasted paste(const rsz::NodeRef& to, const rsz::NodeRef& from);

}  // namespace dialogs::editor::whole
