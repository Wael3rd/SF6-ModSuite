-- Zen Mode's rules: for a GUI GameObject's name, paths of control names from its view; the control at the end is hidden.
-- It started as ZenMode.lua's `exclude` table. Changed since (11/11, the trees read in the game): the containers that held an
-- icon and points together are split, so that each setting hides what it says; the club of the fight's HUD is added.
-- A path's category is worked out from its names as the Lua did (tools/zen/gen_zen_rules.py); `cat = "..."` in a path names it.
-- After a change: python tools/zen/gen_zen_rules.py
local exclude = {
	-- Fighting ground top right corner
	OnlineBannerUI = {
		{ "c_main", "c_contents", "c_rank", "p_SmallRankIcon_" },
		{ "c_main", "c_contents", "c_rank", "e_text_lp" }, -- LP, or MR for a Master
	},

	-- Battle settings matchmaking/character tab (bottom right, on top of character render)
	MatchingSetting = {
		{ "c_main", "c_RankIcon", "c_inner", "p_LargeRankIcon_" },
		{ "c_main", "c_RankIcon", "c_inner", "c_CertifiedMatch" }, -- placement matches
		{ "c_main", "c_RankIcon", "c_inner", "c_Point", "e_txt_lp", cat = "League points" },
		{ "c_main", "c_RankIcon", "c_inner", "c_Point", "e_txt_mr", cat = "Master rate" },
	},
	-- Battle settings fighter profile (banner)
	CFNFighterProfileSimpleTop = { { "c_main", "p_FightersProfileBanner_", "p_SmallRankIcon_" } },
	-- Battle settings fighter profile league points
	CFNFighterProfileChildPlayTab1 = {
		{ "c_main", "c_user_point", "e_text_lp_num" },
		{ "c_main", "c_user_point", "e_text_leaguepoint" },
        { "c_main", "c_user_point", "c_master", "e_text_master" },
        { "c_main", "c_user_point", "c_master", "e_text_mr_num" },

	},
	-- Battle settings > character settings > title settings preview
	MatchingFighterSetting = { { "c_main", "p_Group_0_switch_show", "c_list", "p_MatchingTitleSetting_1_h", "p_Group_0_switch", "c_detail", "c_preview", "p_FightersProfileBanner_0", "p_SmallRankIcon_" } },

	CustomRoomRoom = {
		-- Custom room participant rows
		{ "c_main", "p_Group_base_h", "p_Group_communication_v", "p_ScrollList_member_v", "p_CustomRoomMemberBanner_", "p_LargeRankIcon_0" },
		{ "c_main", "p_Group_base_h", "p_Group_communication_v", "p_ScrollList_member_v", "item%d+", "p_LargeRankIcon_0" },
		-- Custom room singles slot
		{ "c_main", "p_Group_base_h", "p_Group_table_v", "p_SimpleList_SingleTable_v", "p_CustomRoomSimpleTable_%d", "p_CustomRoomMemberBanner_player%d", "p_LargeRankIcon_0" },
		-- Custom room teams slot
		{ "c_main", "p_Group_base_h", "p_Group_table_v", "p_SimpleList_TeamTable_v", "p_CustomRoomMultiTable_0", "p_CustomRoomMemberBanner_blue%d", "p_LargeRankIcon_0" },
		{ "c_main", "p_Group_base_h", "p_Group_table_v", "p_SimpleList_TeamTable_v", "p_CustomRoomMultiTable_0", "p_CustomRoomMemberBanner_red%d", "p_LargeRankIcon_0" },
		-- Custom room tag slot (the same table as the teams', read in the room's tree 11/11; not in ZenMode.lua)
		{ "c_main", "p_Group_base_h", "p_Group_table_v", "p_SimpleList_TagTable_v", "p_CustomRoomMultiTable_0", "p_CustomRoomMemberBanner_blue%d", "p_LargeRankIcon_0" },
		{ "c_main", "p_Group_base_h", "p_Group_table_v", "p_SimpleList_TagTable_v", "p_CustomRoomMultiTable_0", "p_CustomRoomMemberBanner_red%d", "p_LargeRankIcon_0" },
	},
	-- Custom room slot popup
	CustomRoomTableDetail = {
		-- Singles
		{ "c_main", "p_Group_all_v", "p_CustomRoomSimpleTable_simpleTable_h", "p_CustomRoomMemberBanner_player%d", "p_LargeRankIcon_0" },
		-- Teams
		{ "c_main", "p_Group_all_v", "p_CustomRoomMultiTableDetail_teamTable_h", "p_Group_blue_v", "p_SimpleList_blue%d_h", "p_CustomRoomMemberBanner_blue%d", "p_LargeRankIcon_0" },
		{ "c_main", "p_Group_all_v", "p_CustomRoomMultiTableDetail_teamTable_h", "p_Group_red_v", "p_SimpleList_red%d_h", "p_CustomRoomMemberBanner_red%d", "p_LargeRankIcon_0" },
	},
	-- Custom room accept match
	CustomRoomReadyConfirm = {
		{ "c_main", "p_CustomRoomSimpleReadyTable_single", "p_CustomRoomMemberBanner_player%d", "p_LargeRankIcon_0" },
		{ "c_main", "c_TeamTable", "c_team_blue", "c_item_%d", "p_LargeRankIcon_0" },
		{ "c_main", "c_TeamTable", "c_team_red", "c_item_%d", "p_LargeRankIcon_0" },
	},
	-- CFN club members
	CFNFighterListChildCircleMemberTab = {
		{ "c_main", "p_FighterThumbnail_", "p_LargeRankIcon_0"},
		{ "c_main", "p_FighterThumbnail_", "c_league_point"},
	},
	-- Custom Room Invites
	CustomRoomInviteFriend = {
		{ "c_main", "p_FighterThumbnail_", "p_LargeRankIcon_0"},
		{ "c_main", "p_FighterThumbnail_", "c_league_point"},
	},
	CustomRoomInviteSearchResult = {
		{ "c_main", "p_FighterThumbnail_", "p_LargeRankIcon_0"},
		{ "c_main", "p_FighterThumbnail_", "c_league_point"},
	},

	-- Search Results
	CFNFighterListSearchResult = {
		{ "c_main", "p_FighterThumbnail_", "p_LargeRankIcon_0"},
		{ "c_main", "p_FighterThumbnail_", "c_league_point"},
	},

	-- CFN > profile > stats/history/clubs banner
	CFNFighterProfileTop = {
		{ "c_main", "p_FightersProfileBanner_", "p_SmallRankIcon_" },
	},
	-- CFN > profile > stats > League Points tab
	CFNFighterProfileChildPlayTab3 = {
		{ "c_main", "p_Group_0_switch", "p_ScrollList_chara_v", "p_CharacterUseBanner", "p_SmallRankIcon_" },
		{ "c_main", "p_Group_0_switch", "p_ScrollList_chara_v", "p_CharacterUseBanner", "e_text_lp_num" },
		{ "c_main", "p_Group_0_switch", "p_ScrollList_chara_v", "item%d", "p_SmallRankIcon_" },
		{ "c_main", "p_Group_0_switch", "p_ScrollList_chara_v", "item%d", "e_text_lp_num" },
	},
	-- CFN > profile > stats > Master Rate tab
	CFNFighterProfileChildPlayTab6 = {
		{ "c_main", "p_Group_0_switch", "p_ScrollList_chara_v", "p_CharacterUseBanner", "p_SmallRankIcon_" },
		{ "c_main", "p_Group_0_switch", "p_ScrollList_chara_v", "item%d", "p_SmallRankIcon_" },
        { "c_main", "p_Group_0_switch", "p_ScrollList_chara_v" },
	},
	-- CFN > profile > history rows
	CFNFighterProfileChildBattleLog = {
		{ "c_main", "p_ScrollList_0_v_noloop", "p_ReplayBannerProfile_", "p_ReplayBannerPlayerInfoShort_%dP", "p_SmallRankIcon_" },
		{ "c_main", "p_ScrollList_0_v_noloop", "p_ReplayBannerProfile_", "p_ReplayBannerPlayerInfoShort_%dP", "e_text_lp_num" },
		{ "c_main", "p_ScrollList_0_v_noloop", "item%d", "p_ReplayBannerPlayerInfoShort_%dP", "p_SmallRankIcon_" },
		{ "c_main", "p_ScrollList_0_v_noloop", "item%d", "p_ReplayBannerPlayerInfoShort_%dP", "e_text_lp_num" },
	},
	-- CFN > profile > history > match popup, CFN > replays > popup with replay details
	CFNReplayInfo = {
		{ "c_main", "c_inner", "p_BattleFighterInfo_%dp_", "p_LargeRankIcon_" },
		{ "c_main", "c_inner", "p_BattleFighterInfo_%dp_", "e_text_lp_num" },
	},
	-- CFN > replays > replay row
	CFNReplayChildList = {
		{ "c_main", "p_Group_root_switch", "p_ReplayList_", "p_ScrollList_multi_v", "p_ReplayBanner_", "p_ReplayBannerPlayerInfo_%dP", "e_text_lp_num" },
		{ "c_main", "p_Group_root_switch", "p_ReplayList_", "p_ScrollList_multi_v", "p_ReplayBanner_", "p_ReplayBannerPlayerInfo_%dP", "c_league", cat = "Rank icon" }, -- holds the rank icon only
		{ "c_main", "p_Group_root_switch", "p_ReplayList_", "p_ScrollList_multi_v", "item%d", "p_ReplayBannerPlayerInfo_%dP", "e_text_lp_num" },
		{ "c_main", "p_Group_root_switch", "p_ReplayList_", "p_ScrollList_multi_v", "item%d", "p_ReplayBannerPlayerInfo_%dP", "c_league", cat = "Rank icon" }, -- holds the rank icon only
	},
	-- CFN > ranking > league points
	CFNRankingLeaguePoint = {
		{ "c_main", "p_RankingThumbnailLeaguePoint_", "p_LargeRankIcon_0" },
		{ "c_main", "p_RankingThumbnailLeaguePoint_", "c_league_point" },
		{ "c_main", "p_RankingThumbnailLeaguePoint_", "e_text_rank" },
		{ "c_main", "p_Group_root_switch", "p_RankingListLeaguePoint", "p_ScrollList_ranking_v_noloop", "p_FighterBannerRankingLeaguePoint_", "p_FighterBannerRankingInfo_", "p_LargeRankIcon_" },
		{ "c_main", "p_Group_root_switch", "p_RankingListLeaguePoint", "p_ScrollList_ranking_v_noloop", "p_FighterBannerRankingLeaguePoint_", "p_FighterBannerRankingInfo_", "e_text_lp" },
		{ "c_main", "p_Group_root_switch", "p_RankingListLeaguePoint", "p_ScrollList_ranking_v_noloop", "p_FighterBannerRankingLeaguePoint_", "p_FighterBannerRankingInfo_", "c_rank" },
		{ "c_main", "p_Group_root_switch", "p_RankingListLeaguePoint", "p_ScrollList_ranking_v_noloop", "item%d", "p_FighterBannerRankingInfo_", "p_LargeRankIcon_" },
		{ "c_main", "p_Group_root_switch", "p_RankingListLeaguePoint", "p_ScrollList_ranking_v_noloop", "item%d", "p_FighterBannerRankingInfo_", "e_text_lp" },
		{ "c_main", "p_Group_root_switch", "p_RankingListLeaguePoint", "p_ScrollList_ranking_v_noloop", "item%d", "p_FighterBannerRankingInfo_", "c_rank" },
	},

    CFNRankingChildMasterRating = { 
        {"c_main", "p_Group_root_switch", "p_RankingListMasterRating_" },
    },

	-- CFN > clubs > clubs joined > management > member list
	CFNCircleMemberList = {
		{ "c_main", "p_FighterThumbnail_", "p_LargeRankIcon_0" },
		{ "c_main", "p_FighterThumbnail_", "c_league_point" },
	},

	-- In-match HUD
	BattleHud_AccountInfo = {
		-- The rank icon. Its container c_rank is shown again by the game all fight long (what is written there is undone at
		-- once), its two icon panels at each round's start; what is inside them is left alone once the icon is set (seen in
		-- ranked matches, 11/11: hidden there, it stays hidden for the whole match).
		{ "c_main", "c_hud", "p_BattleHudAccountInfo_%d", "c_RankInfo", "c_rank", "p_SmallRankIcon_", "e_texture_image", cat = "Rank icon" },
		{ "c_main", "c_hud", "p_BattleHudAccountInfo_%d", "c_RankInfo", "c_rank", "p_SmallRankIcon_", "c_rank_num", cat = "Rank icon" },
		{ "c_main", "c_hud", "p_BattleHudAccountInfo_%d", "c_RankInfo", "c_rank", "p_LargeRankIcon_", "e_texture_image", cat = "Rank icon" },
		{ "c_main", "c_hud", "p_BattleHudAccountInfo_%d", "c_RankInfo", "c_rank", "p_LargeRankIcon_", "c_rank_num", cat = "Rank icon" },
		{ "c_main", "c_hud", "p_BattleHudAccountInfo_%d", "c_RankInfo", "c_rank", "c_cetrified", "e_txt_cetrified", cat = "Rank icon" },
		{ "c_main", "c_hud", "p_BattleHudAccountInfo_%d", "c_RankInfo", "c_point", "e_txt_CM_", cat = "Rank icon" }, -- placement matches
		{ "c_main", "c_hud", "p_BattleHudAccountInfo_%d", "c_RankInfo", "c_point", "e_txt_lp", cat = "League points" },
		{ "c_main", "c_hud", "p_BattleHudAccountInfo_%d", "c_CircleInfo" }, -- club name and emblem
	},
	BattleHud_WinCount = { { "c_main" } }, -- Win streak.
	BattleHud_InputTypeIcon = { { "c_main" } }, -- Control type (Modern/Classic) in match.
	BattleHud_ProfileName = { { "c_main" } }, -- In-match player name / CFN id (the ***** masked name).
	BattleHud_TitlePlate = { { "c_main" } }, -- In-match player title.
	-- The title shown during the fight is not the title plate's (hidden by the game then) but a copy under the round counter
	-- (seen in a ranked match, 11/11); the counter itself stays. The game shows p_TitleIcon_ again at each round's start and
	-- leaves what is inside alone: its elements are hidden ("e_": the plate, the texture, the text).
	BattleHud_MatchWonNumber = { { "c_main", "c_hud", "p_WonNumberPanel_%d", "p_TitleIcon_", "e_", cat = "Player title" } },

	-- Versus screen (also used for ranked online)
	VSInfoOffline = {
		{ "c_main", "p_VsInfoTop_player%d_v", "c_online", "c_RankIcon" },
		{ "c_main", "p_VsInfoTop_player%d_v", "c_online", "c_chance" }, -- Rank up opportunity
		{ "c_main", "p_VsInfoTop_player%d_v", "c_streak" },
		{ "c_main", "p_VsInfoTop_player%d_v", "c_online", "c_fighter", "p_NationalFlag_" }, -- Country flag
		{ "c_main", "p_VsInfoTop_player%d_v", "c_online", "c_fighter", "e_text_name" }, -- Player name
		{ "c_main", "p_VsInfoTop_player%d_v", "c_online", "c_emblem", "p_TitlePlate_0" }, -- Player title
		{ "c_main", "p_VsInfoTop_player%d_v", "c_online", "c_circle" }, -- Team / club
		{ "c_main", "p_VsInfoTop_player%d_v", "c_online", "p_PlatformIcon_" }, -- Platform icon
		{ "c_main", "p_VsInfoTop_player%d_v", "p_TypeIcon_0" }, -- Control type (Modern/Classic)
	},

	-- Unverified. League promotion fullscreen animation.
	RankUp = { { "c_main" } },

	-- End of match
	RankGauge_1P = { { "c_main" } },
	RankGauge_2P = { { "c_main" } },
	ResultCounter = { { "c_main", "c_winning_streak" } },
};
