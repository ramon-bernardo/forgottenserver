// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#ifndef FS_PARTY_H
#define FS_PARTY_H

#include "const.h"

class Creature;
class Player;

using PlayerVector = std::vector<std::shared_ptr<Player>>;

static constexpr int32_t EXPERIENCE_SHARE_RANGE = 30;
static constexpr int32_t EXPERIENCE_SHARE_FLOORS = 1;

enum SharedExpStatus_t : uint8_t
{
	SHAREDEXP_OK,
	SHAREDEXP_TOOFARAWAY,
	SHAREDEXP_LEVELDIFFTOOLARGE,
	SHAREDEXP_MEMBERINACTIVE,
	SHAREDEXP_EMPTYPARTY
};

class Party
{
public:
	explicit Party(std::shared_ptr<Player> leader);

	std::shared_ptr<Player> getLeader() const { return leader; }
	PlayerVector& getMembers() { return memberList; }
	const PlayerVector& getInvitees() const { return inviteList; }
	size_t getMemberCount() const { return memberList.size(); }
	size_t getInvitationCount() const { return inviteList.size(); }

	void disband();
	bool invitePlayer(std::shared_ptr<Player> player);
	bool joinParty(std::shared_ptr<Player> player);
	void revokeInvitation(std::shared_ptr<Player> player);
	bool passPartyLeadership(std::shared_ptr<Player> player, bool forceRemove = false);
	bool leaveParty(std::shared_ptr<Player> player, bool forceRemove = false);

	bool removeInvite(std::shared_ptr<Player> player, bool removeFromPlayer = true);

	bool isPlayerInvited(std::shared_ptr<const Player> player) const;
	void updateAllPartyIcons();
	void broadcastPartyMessage(MessageClasses msgClass, const std::string& msg, bool sendToInvitations = false);
	bool empty() const { return memberList.empty() && inviteList.empty(); }
	bool canOpenCorpse(uint32_t ownerId) const;

	void shareExperience(uint64_t experience, std::shared_ptr<Creature> source = nullptr);
	bool setSharedExperience(std::shared_ptr<Player> player, bool sharedExpActive);
	bool isSharedExperienceActive() const { return sharedExpActive; }
	bool isSharedExperienceEnabled() const { return sharedExpEnabled; }
	bool canUseSharedExperience(std::shared_ptr<const Player> player) const;
	SharedExpStatus_t getMemberSharedExperienceStatus(std::shared_ptr<const Player> player) const;
	void updateSharedExperience();

	void updatePlayerTicks(std::shared_ptr<Player> player, uint32_t points);
	void clearPlayerPoints(std::shared_ptr<Player> player);

private:
	SharedExpStatus_t getSharedExperienceStatus();

	std::map<uint32_t, int64_t> ticksMap;

	PlayerVector memberList;
	PlayerVector inviteList;

	std::shared_ptr<Player> leader;

	bool sharedExpActive = false;
	bool sharedExpEnabled = false;
};

#endif // FS_PARTY_H
