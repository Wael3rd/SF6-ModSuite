// The plugin's HTTPS path, on a made-up id (no such profile: Steam answers an error page). Prints no name, counts only.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "steamname_net.hpp"
#include "steamname_logic.hpp"

#include <cstdio>

int main() {
    std::string xml;
    const DWORD t0 = GetTickCount();
    const bool got = psn::http_get(L"/profiles/76561190000000000/?xml=1", xml);
    const std::string name = psn::sanitize(psn::xml_name(xml));
    std::printf("answer %d, %zu bytes, %lu ms, a name found: %d, xml seen: %d\n", got ? 1 : 0, xml.size(), GetTickCount() - t0, name.empty() ? 0 : 1,
                xml.find("<response>") != std::string::npos || xml.find("<profile>") != std::string::npos ? 1 : 0);
    std::string page;
    const bool got2 = psn::http_get(L"/profiles/76561190000000000/?l=english", page);
    std::printf("page answer %d, %zu bytes, a name found: %d (0 expected: Steam's error page)\n", got2 ? 1 : 0, page.size(), psn::name_from_page(page).empty() ? 0 : 1);
    return got ? 0 : 1;
}
