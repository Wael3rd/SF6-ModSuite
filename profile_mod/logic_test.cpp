// The pure part of profile_mod. No real id or name here: made-up values only.
#include "steamname_logic.hpp"
#include "swap_logic.hpp"

#include <cstdio>

using namespace psn;
static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++fails;
}

int main() {
    const std::string id = "76561190000000000";
    const std::string line = "<PLATMSG Arg0 = \"94\"> : " + id;
    // the rule's id
    check(find_steam_id(line) == 24, "the id in the game's line, not the 94 of its tag");
    check(find_steam_id(id) == 0, "the id alone");
    check(find_steam_id("<PLATMSG Arg0 = \"94\"> : somebody_psn") == std::string::npos, "a PlayStation name: none");
    check(find_steam_id("<PLATMSG Arg0 = \"94\"> : 2535411111111111") == std::string::npos, "a Xbox number: none");
    check(find_steam_id("7656119000000000") == std::string::npos, "16 digits: none");
    check(find_steam_id("765611900000000000") == std::string::npos, "18 digits: none");
    check(find_steam_id("12345678901234567") == std::string::npos, "17 digits not starting 7656: none");
    check(find_steam_id(std::string()) == std::string::npos, "an empty line: none");
    check(find_steam_id("<PLATMSG> : Bob") == std::string::npos, "a shown name: none");
    // the XML
    check(xml_name("<profile><steamID64>1</steamID64><steamID><![CDATA[Some Name]]></steamID><x/></profile>") == "Some Name", "the name in its CDATA");
    check(xml_name("<response><error><![CDATA[The specified profile could not be found.]]></error></response>").empty(), "an error page: none");
    check(xml_name("<profile><steamID><![CDATA[cut").empty(), "a cut page: none");
    check(xml_name("<html>not xml</html>").empty(), "not XML: none");
    check(xml_name("").empty(), "nothing: none");
    check(xml_name("<steamID><![CDATA[]]></steamID>").empty(), "an empty name: none");
    // a stranger's text
    check(sanitize("Bob") == "Bob", "a plain name stays");
    check(sanitize("<ICON KeyR>Bob") == "ICON KeyRBob", "no message tag can be formed");
    check(sanitize("a<b>c") == "abc", "< and > gone");
    check(sanitize("a\nb\tc\x01" "d\x7F" "e") == "abcde", "control characters gone");
    check(sanitize("  Bob  ") == "Bob", "no space at its ends");
    check(sanitize("Jos\xC3\xA9") == "Jos\xC3\xA9", "accents stay");
    check(sanitize("\xE6\x97\xA5\xE6\x9C\xAC") == "\xE6\x97\xA5\xE6\x9C\xAC", "Japanese stays");
    check(sanitize("\xF0\x9F\x98\x80" "x") == "\xF0\x9F\x98\x80" "x", "an emoji stays");
    check(sanitize("a\xE2\x80\xAE" "b") == "ab", "a bidirectional override gone");
    check(sanitize("a\xE2\x80\xA8" "b") == "ab", "a line separator gone");
    check(sanitize("a\xC2\x85" "b") == "ab", "a C1 control gone");
    check(sanitize("a\xFF" "b\xC3") == "ab", "broken UTF-8 gone");
    check(sanitize("\xC0\xBC" "x") == "x", "an overlong < gone");
    check(sanitize("\xE0\x80\xBC" "x") == "x", "another overlong < gone");
    check(sanitize("<<>>").empty(), "nothing left: empty, so a failure");
    check(sanitize(std::string(300, 'x')).size() == kNameMaxBytes, "bounded");
    // the cut
    check(cut_name("Bobby", 10) == "Bobby", "it fits: whole");
    check(cut_name("Bobby", 2) == "Bo...", "cut with ...");
    check(cut_name("\xC3\xA9\xC3\xA9", 3) == "\xC3\xA9...", "never inside a UTF-8 sequence");
    check(cut_name("\xE6\x97\xA5\xE6\x9C\xAC", 4) == "\xE6\x97\xA5...", "never inside a longer one");
    // the line
    check(line_with_name(line, find_steam_id(line), "Bob") == "<PLATMSG Arg0 = \"94\"> : Bob", "the prefix kept, the name in place of the id");
    check(line_with_name(line + " x", 24, "Bob") == "<PLATMSG Arg0 = \"94\"> : Bob x", "what follows kept");
    check(shown_bytes("<PLATMSG Arg0 = \"94\"> : Bob") == 6, "the bytes that take room");
    // the fit
    check(next_keep(20, 26, 300, 400) == 20, "it fits: as it is");
    check(next_keep(20, 26, 500, 400) < 20, "too wide: fewer");
    check(next_keep(5, 26, 2000, 100) == 4, "never under 4");
    check(next_keep(4, 26, 2000, 100) == 4, "at 4: stays, no endless loop");
    check(next_keep(20, 26, 0, 100) == 20, "not laid out: as it is");
    // the key, the words
    check(qwerty_scan("<ICON KeyR>") == 0x13, "R's scan code");
    check(qwerty_scan("<ICON BtnL>") == 0, "a pad icon: no key");
    check(std::string(word_name("fr")) == "Pseudo Steam" && std::string(word_id("fr")) == "ID Steam", "French");
    check(std::string(word_name("en")) == "Steam Name" && std::string(word_name("nl")) == "Steam Name", "English, and anything else");
    {
        const char* langs[] = { "en", "fr", "ja", "zh-Hans", "it", "de", "es", "es-419", "ru", "pl", "pt", "ko", "zh-Hant", "ar" };
        bool all = true;
        for (const char* l : langs) all = all && std::string(words_of(l).lang) == l && *word_name(l) && *word_id(l) && *word_unavailable(l);
        check(all, "the 14 languages have the 3 words");
        // the source is read as UTF-8 (/utf-8): the German word's u with diaeresis is these two bytes
        check(std::string(word_unavailable("de")).find("\xC3\xBC") != std::string::npos, "the words are UTF-8");
    }

    // the HTML page, made-up pages and names
    {
        const std::string head = "<html><head><title>Steam Community :: Made Up</title></head><body>";
        const std::string unset = head + "<div>This user has not yet set up their Steam Community profile.</div>";
        check(name_from_page(head + "<span class=\"actual_persona_name\">Span Name</span></body>") == "Span Name", "page: the persona span first");
        check(name_from_page(unset) == "Made Up", "page: else the tab's title");
        check(name_from_page("<title>Steam Community :: Error</title><div class=\"error_ctn\"><h3>The specified profile could not be found.</h3></div>").empty(), "page: Steam's error page: none");
        check(name_from_page("<title>Steam Community :: Error</title>").empty(), "page: its title alone: never the name Error");
        check(name_from_page("<title>Steam Community :: Made Up</title><h3>The specified profile could not be found.</h3>").empty(), "page: the error's text wins over a title");
        check(name_from_page("<title>Steam Community</title>").empty(), "page: a title with no name: none");
        check(name_from_page("<title>Steam Community :: </title>").empty(), "page: an empty title: none");
        check(name_from_page("<html>something else</html>").empty() && name_from_page("").empty(), "page: anything else: none");
        check(name_from_page("<span class=\"actual_persona_name\">cut").empty(), "page: a cut span: none");
        check(name_from_page("<title>Steam Community :: cut").empty(), "page: a cut title: none");
        check(name_from_page("<span class=\"actual_persona_name\">Error</span>") == "Error", "page: a player really named so, in its span");
        // the entities
        check(decode_entities("a &amp; b &quot;c&quot; d&#39;e &apos;f") == "a & b \"c\" d'e 'f", "entities: the named ones");
        check(decode_entities("&#233;&#xE9;&#XE9;") == "\xC3\xA9\xC3\xA9\xC3\xA9", "entities: numeric, decimal and hex");
        check(decode_entities("&#128512;") == "\xF0\x9F\x98\x80", "entities: numeric, 4 bytes");
        check(decode_entities("&#26085;") == "\xE6\x97\xA5", "entities: numeric, 3 bytes");
        check(decode_entities("&#0;x&#xD800;y&#1114112;z&#99999999999999;w") == "xyzw", "entities: a number that is no character: dropped");
        check(decode_entities("&amp;lt;") == "&lt;", "entities: one pass only");
        check(decode_entities("&foo; &amp &#; &#x; &#12a; a&b") == "&foo; &amp &#; &#x; &#12a; a&b", "entities: anything else stays");
        check(decode_entities("&") == "&" && decode_entities("") == "", "entities: the ends");
        // no message tag through the entities
        const auto named = [](const std::string& raw) { return name_from_page("<span class=\"actual_persona_name\">" + raw + "</span>"); };
        check(named("&lt;ICON KeyR&gt;Bob") == "ICON KeyRBob", "page: a tag in entities: gone");
        check(named("&#60;PLATMSG&#62;Bob") == "PLATMSGBob", "page: a tag in numeric entities: gone");
        check(named("&#x3C;b&#x3E;") == "b", "page: a tag in hex entities: gone");
        check(named("a&#10;b&#x202E;c") == "abc", "page: a control or a bidi override in entities: gone");
        check(named("Tom &amp; Jerry") == "Tom & Jerry", "page: an ampersand stays");
        check(named("  &#32; ") == "", "page: only spaces: none");
        // what the two requests give
        const std::string xml_named = "<profile><steamID><![CDATA[Xml Name]]></steamID></profile>", xml_none = "<profile><privacyMessage/></profile>";
        check(!needs_page(true, xml_named), "requests: a name in the XML: no second request");
        check(needs_page(true, xml_none), "requests: none in the XML: the page");
        check(!needs_page(false, ""), "requests: no answer: no second request");
        NameAnswer ans = name_answer(true, xml_named, false, "");
        check(ans.result == NameResult::Name && ans.name == "Xml Name", "requests: from the XML");
        ans = name_answer(true, xml_none, true, unset);
        check(ans.result == NameResult::Name && ans.name == "Made Up", "requests: from the page");
        ans = name_answer(true, xml_none, true, "<title>Steam Community :: Error</title>");
        check(ans.result == NameResult::NoName && ans.name.empty(), "requests: an error page: no name, not asked again");
        ans = name_answer(true, xml_none, false, "");
        check(ans.result == NameResult::NetFail, "requests: the page did not answer: may retry");
        ans = name_answer(false, "", false, "");
        check(ans.result == NameResult::NetFail, "requests: the XML did not answer: may retry");
        check(std::string(word_unavailable("fr")) == "Pseudo indisponible" && std::string(word_unavailable("en")) == "Name unavailable", "the guide's words");
    }
    // on screen means on screen
    {
        check(!profile_drawn(false, true), "screen: Battle Settings' hidden SimpleTop (not drawn, controls ActualVisible): no");
        check(!profile_drawn(true, false), "screen: drawn, its main control hidden: no");
        check(profile_drawn(true, true), "screen: drawn and visible: yes");
        const char* full = "app.UICFNFightersProfileTop.FlowParam";
        const char* limited = "app.UICFNFightersProfileSimpleTop.FlowParam";
        // real profiles: post-game, replay details, the hub (the limited one, its Details popup under it, or the full one in its place)
        check(profile_allowed({ limited, "app.UICFNDetailedMenu.FlowParam", "app.UIWidget.BaseParam_CreateByPrefab" }, false), "screen: the limited profile, its flow there: yes");
        check(profile_allowed({ "app.UICFNFightersProfileTabPlayChildTab1.FlowParam", full, "app.UICFNDetailedMenu.FlowParam" }, true), "screen: the full one after the swap: yes");
        check(profile_allowed({ full, "app.menu.UIFlowFGMultiMenu.Param" }, true), "screen: the full one from the Multi Menu: yes");
        // each screen its own flow
        check(!profile_allowed({ limited }, true), "screen: the full profile's GUI with only the limited one's flow: no");
        check(!profile_allowed({ full }, false), "screen: the limited profile's GUI with only the full one's flow: no");
        check(!profile_allowed({}, false) && !profile_allowed({ "app.UIWidget.BaseParam_CreateByPrefab" }, true), "screen: no flow of it: no");
        // Battle Settings, as read on its tabs (g62, g67): never, whatever else is there
        check(!profile_allowed({ "app.UICFNFightersProfileChildTabAvatarEquip.FlowParam", "app.UIMatchingSettingFightersProfileSimpleTop.FlowParam", "app.UICFNFightersProfileTabPlayChildTab1.FlowParam",
                                 "app.UIMatchingSettingFightersProfileSimpleTop.FlowParam", "app.UIFlowMatchingSetting.Param", "app.menu.UIFlowFGMainMenuList.Param" }, false), "screen: Battle Settings: no");
        check(!profile_allowed({ "app.UIMatchingSettingFightersProfileSimpleTop.FlowParam" }, false), "screen: its profile tab's flow is no profile, even alone: no");
        check(!profile_allowed({ "app.UIFlowMatchingFighterSetting.Param", "app.UIFlowMatchingSetting.Param", limited }, false), "screen: Fighter Settings, a limited flow there too: no");
        check(!profile_allowed({ limited, "app.UIFlowMatchingSetting.Param" }, false) && !profile_allowed({ full, "app.UIFlowMatchingFighterSetting.Param" }, true), "screen: Battle Settings' flow anywhere in the list: no");
        check(flow_is_battle_settings("app.UIFlowMatchingSetting.Param") && flow_is_battle_settings("app.UIFlowMatchingFighterSetting.Param"), "screen: its two flows");
        check(!flow_is_battle_settings(limited) && !flow_is_battle_settings(full) && !flow_is_battle_settings("app.UIMatchingSettingFightersProfileSimpleTop.FlowParam"), "screen: nothing else counts as them");
    }
    // the swap's decisions
    {
        using D = SwapTracker::Do;
        constexpr uintptr_t H = 0x1000, H2 = 0x2000;
        SwapTracker s;
        uint32_t f = 100;
        check(s.step(0, false, f) == D::Nothing, "swap: no simple profile: nothing");
        check(s.step(H, false, f) == D::Nothing, "swap: it appears: it waits");
        check(s.step(H, false, f + 4) == D::Nothing, "swap: 4 frames: still waiting");
        check(s.step(H, false, f + 5) == D::Swap, "swap: 5 frames: swap");
        s.swapped(f + 5);
        check(s.step(H, false, f + 6) == D::Nothing, "swap: its handle still there: never twice");
        check(s.step(0, true, f + 7) == D::Nothing, "swap: its handle gone, the full one up");
        check(s.step(0, true, f + 3000) == D::Nothing, "swap: the full one stays up");
        // nested: a simple profile opened from inside a full one stays
        check(s.step(H2, true, f + 3001) == D::LeaveAlone, "swap: from inside a full profile: left alone");
        check(s.step(H2, true, f + 3002) == D::Nothing, "swap: ...and said once");
        check(s.step(0, true, f + 3003) == D::Nothing, "swap: that one closed");
        // no loop: right after ours closed
        check(s.step(0, false, f + 3010) == D::Nothing, "swap: the full one closed");
        check(s.step(H, false, f + 3011) == D::LeaveAlone, "swap: a simple one right after it: left alone, no loop");
        check(s.step(H, false, f + 3100) == D::Nothing, "swap: left alone for as long as it is there");
        check(s.step(0, false, f + 3101) == D::Nothing, "swap: it closed");
        // later: the same address again is a new flow
        const uint32_t later = f + 3003 + SwapTracker::kLoopFrames + 200;
        check(s.step(0, false, later) == D::Nothing && !s.ours, "swap: some time later, ours is over");
        check(s.step(H, false, later + 1) == D::Nothing, "swap: the same address again: a new flow, it waits");
        check(s.step(H, false, later + 6) == D::Swap, "swap: ...and is swapped (the Lua's swapped[addr] never was)");
        // another handle replaces the awaited one: its own wait
        SwapTracker t;
        t.step(H, false, 10);
        check(t.step(H2, false, 13) == D::Nothing && t.step(H2, false, 17) == D::Nothing && t.step(H2, false, 18) == D::Swap, "swap: another flow: its own 5 frames");
        // a failed swap: not tried again on that flow, and no loop guard
        SwapTracker u;
        u.step(H, false, 10);
        check(u.step(H, false, 15) == D::Swap && !u.ours && u.step(H, false, 16) == D::Nothing, "swap: failed: not tried again on that flow");
        // a full profile not of ours, closing: a simple one just after is swapped
        SwapTracker v;
        check(v.step(0, true, 10) == D::Nothing && v.step(H, false, 11) == D::Nothing && v.step(H, false, 16) == D::Swap, "swap: after his own profile closed: swapped");
        // the frame counter wrapping
        SwapTracker w;
        w.step(H, false, 0xFFFFFFFE);
        check(w.step(H, false, 3) == D::Swap, "swap: across the counter's wrap");
    }
    // the settings
    {
        Settings a = parse_settings("");
        check(a.full_profile && a.steam_name, "settings: nothing: both on");
        a = parse_settings("full_profile=0\r\nsteam_name=1\r\n");
        check(!a.full_profile && a.steam_name, "settings: the swap off");
        a = parse_settings("; a comment\n  Steam_Name = 0  ; off\n");
        check(a.full_profile && !a.steam_name, "settings: spaces, case, comments");
        a = parse_settings("full_profile=maybe\nsteam_name\nother=0\n");
        check(a.full_profile && a.steam_name, "settings: anything else: ignored");
    }

    std::printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
