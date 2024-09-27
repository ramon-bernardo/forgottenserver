// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#ifndef FS_GUILD_H
#define FS_GUILD_H

class Player;

using GuildWarVector = std::vector<uint32_t>;

struct GuildRank
{
	uint32_t id;
	std::string name;
	uint8_t level;

	GuildRank(uint32_t id, std::string_view name, uint8_t level) : id{id}, name{name}, level{level} {}
};

using GuildRank_ptr = std::shared_ptr<GuildRank>;

static constexpr uint8_t GUILD_MEMBER_RANK_LEVEL_DEFAULT = 1;

class Guild
{
public:
	Guild(uint32_t id, std::string_view name) : name{name}, id{id} {}

	auto getId() const { return id; }
	const auto& getName() const { return name; }

	const auto& getMotd() const { return motd; }
	void setMotd(const std::string& motd) { this->motd = motd; }

	void addMember(Player* player);
	void removeMember(Player* player);

	const auto& getMembersOnline() const { return membersOnline; }
	auto getMemberCount() const { return memberCount; }
	void setMemberCount(uint32_t count) { memberCount = count; }

	void addRank(uint32_t rankId, std::string_view rankName, uint8_t level);
	const auto& getRanks() const { return ranks; }

	GuildRank_ptr getRankById(uint32_t rankId);
	GuildRank_ptr getRankByName(const std::string& name) const;
	GuildRank_ptr getRankByLevel(uint8_t level) const;

private:
	uint32_t id;
	std::string name;
	std::string motd;

	std::list<Player*> membersOnline;
	uint32_t memberCount = 0;

	std::vector<GuildRank_ptr> ranks;
};

using Guild_ptr = std::shared_ptr<Guild>;

namespace tfs::io::guild {
Guild_ptr load(uint32_t guildId);
uint32_t getIdByName(const std::string& name);
}; // namespace tfs::io::guild

#endif // FS_GUILD_H
