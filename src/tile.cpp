// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include "tile.h"

#include "combat.h"
#include "configmanager.h"
#include "creature.h"
#include "game.h"
#include "housetile.h"
#include "mailbox.h"
#include "monster.h"
#include "movement.h"
#include "spectators.h"
#include "teleport.h"
#include "trashholder.h"

extern Game g_game;
extern MoveEvents* g_moveEvents;

std::shared_ptr<Tile> Tile::nullptr_tile = std::make_shared<StaticTile>(0xFFFF, 0xFFFF, 0xFF);

bool Tile::hasProperty(ITEMPROPERTY prop) const
{
	if (ground && ground->hasProperty(prop)) {
		return true;
	}

	if (const TileItemVector* items = getItemList()) {
		for (const auto& item : *items) {
			if (item && item->hasProperty(prop)) {
				return true;
			}
		}
	}
	return false;
}

bool Tile::hasProperty(std::shared_ptr<const Item> exclude, ITEMPROPERTY prop) const
{
	assert(exclude);

	if (ground && exclude != ground && ground->hasProperty(prop)) {
		return true;
	}

	if (const TileItemVector* items = getItemList()) {
		for (auto item : *items) {
			if (item && item != exclude && item->hasProperty(prop)) {
				return true;
			}
		}
	}

	return false;
}

bool Tile::hasHeight(uint32_t n) const
{
	uint32_t height = 0;

	if (ground) {
		if (ground->hasProperty(CONST_PROP_HASHEIGHT)) {
			++height;
		}

		if (n == height) {
			return true;
		}
	}

	if (const TileItemVector* items = getItemList()) {
		for (const auto& item : *items) {
			if (item && item->hasProperty(CONST_PROP_HASHEIGHT)) {
				++height;
			}

			if (n == height) {
				return true;
			}
		}
	}
	return false;
}

size_t Tile::getCreatureCount() const
{
	if (const CreatureVector* creatures = getCreatures()) {
		return creatures->size();
	}
	return 0;
}

size_t Tile::getItemCount() const
{
	if (const TileItemVector* items = getItemList()) {
		return items->size();
	}
	return 0;
}

uint32_t Tile::getTopItemCount() const
{
	if (const TileItemVector* items = getItemList()) {
		return items->getTopItemCount();
	}
	return 0;
}

uint32_t Tile::getDownItemCount() const
{
	if (const TileItemVector* items = getItemList()) {
		return items->getDownItemCount();
	}
	return 0;
}

std::string Tile::getDescription(int32_t) const { return "You dont know why, but you cant see anything!"; }

std::shared_ptr<Teleport> Tile::getTeleportItem() const
{
	if (!hasFlag(TILESTATE_TELEPORT)) {
		return nullptr;
	}

	if (const TileItemVector* items = getItemList()) {
		for (auto it = items->rbegin(), end = items->rend(); it != end; ++it) {
			if ((*it) && (*it)->getTeleport()) {
				return (*it)->getTeleport();
			}
		}
	}
	return nullptr;
}

std::shared_ptr<MagicField> Tile::getFieldItem() const
{
	if (!hasFlag(TILESTATE_MAGICFIELD)) {
		return nullptr;
	}

	if (ground && ground->getMagicField()) {
		return ground->getMagicField();
	}

	if (const TileItemVector* items = getItemList()) {
		for (auto it = items->rbegin(), end = items->rend(); it != end; ++it) {
			if ((*it) && (*it)->getMagicField()) {
				return (*it)->getMagicField();
			}
		}
	}
	return nullptr;
}

std::shared_ptr<TrashHolder> Tile::getTrashHolder() const
{
	if (!hasFlag(TILESTATE_TRASHHOLDER)) {
		return nullptr;
	}

	if (ground && ground->getTrashHolder()) {
		return ground->getTrashHolder();
	}

	if (const TileItemVector* items = getItemList()) {
		for (auto it = items->rbegin(), end = items->rend(); it != end; ++it) {
			if ((*it) && (*it)->getTrashHolder()) {
				return (*it)->getTrashHolder();
			}
		}
	}
	return nullptr;
}

std::shared_ptr<Mailbox> Tile::getMailbox() const
{
	if (!hasFlag(TILESTATE_MAILBOX)) {
		return nullptr;
	}

	if (ground && ground->getMailbox()) {
		return ground->getMailbox();
	}

	if (const TileItemVector* items = getItemList()) {
		for (auto it = items->rbegin(), end = items->rend(); it != end; ++it) {
			if ((*it) && (*it)->getMailbox()) {
				return (*it)->getMailbox();
			}
		}
	}
	return nullptr;
}

std::shared_ptr<BedItem> Tile::getBedItem() const
{
	if (!hasFlag(TILESTATE_BED)) {
		return nullptr;
	}

	if (ground && ground->getBed()) {
		return ground->getBed();
	}

	if (const TileItemVector* items = getItemList()) {
		for (auto it = items->rbegin(), end = items->rend(); it != end; ++it) {
			if ((*it) && (*it)->getBed()) {
				return (*it)->getBed();
			}
		}
	}
	return nullptr;
}

std::shared_ptr<Creature> Tile::getTopCreature() const
{
	if (const CreatureVector* creatures = getCreatures()) {
		if (!creatures->empty()) {
			return *creatures->begin();
		}
	}
	return nullptr;
}

std::shared_ptr<const Creature> Tile::getBottomCreature() const
{
	if (const CreatureVector* creatures = getCreatures()) {
		if (!creatures->empty()) {
			return *creatures->rbegin();
		}
	}
	return nullptr;
}

std::shared_ptr<Creature> Tile::getTopVisibleCreature(std::shared_ptr<const Creature> creature) const
{
	if (const CreatureVector* creatures = getCreatures()) {
		if (creature) {
			for (auto tileCreature : *creatures) {
				if (creature->canSeeCreature(tileCreature)) {
					return tileCreature;
				}
			}
		} else {
			for (auto tileCreature : *creatures) {
				if (!tileCreature->isInvisible()) {
					auto player = tileCreature->getPlayer();
					if (!player || !player->isInGhostMode()) {
						return tileCreature;
					}
				}
			}
		}
	}
	return nullptr;
}

std::shared_ptr<const Creature> Tile::getBottomVisibleCreature(std::shared_ptr<const Creature> creature) const
{
	if (const CreatureVector* creatures = getCreatures()) {
		if (creature) {
			for (auto it = creatures->rbegin(), end = creatures->rend(); it != end; ++it) {
				if (creature->canSeeCreature(*it)) {
					return *it;
				}
			}
		} else {
			for (auto it = creatures->rbegin(), end = creatures->rend(); it != end; ++it) {
				if (!(*it)->isInvisible()) {
					auto player = (*it)->getPlayer();
					if (!player || !player->isInGhostMode()) {
						return *it;
					}
				}
			}
		}
	}
	return nullptr;
}

std::shared_ptr<Item> Tile::getTopDownItem() const
{
	if (const TileItemVector* items = getItemList()) {
		return items->getTopDownItem();
	}
	return nullptr;
}

std::shared_ptr<Item> Tile::getTopTopItem() const
{
	if (const TileItemVector* items = getItemList()) {
		return items->getTopTopItem();
	}
	return nullptr;
}

std::shared_ptr<Item> Tile::getItemByTopOrder(int32_t topOrder)
{
	// topOrder:
	// 1: borders
	// 2: ladders, signs, splashes
	// 3: doors etc
	// 4: creatures
	if (TileItemVector* items = getItemList()) {
		for (auto it = ItemVector::const_reverse_iterator(items->getEndTopItem()),
		          end = ItemVector::const_reverse_iterator(items->getBeginTopItem());
		     it != end; ++it) {
			if (Item::items[(*it)->getID()].alwaysOnTopOrder == topOrder) {
				return *it;
			}
		}
	}
	return nullptr;
}

std::shared_ptr<Thing> Tile::getTopVisibleThing(std::shared_ptr<const Creature> creature)
{
	auto thing = getTopVisibleCreature(creature);
	if (thing) {
		return thing;
	}

	TileItemVector* items = getItemList();
	if (items) {
		for (ItemVector::const_iterator it = items->getBeginDownItem(), end = items->getEndDownItem(); it != end;
		     ++it) {
			auto item = *it;
			const ItemType& iit = Item::items[item->getID()];
			if (!iit.lookThrough) {
				return item;
			}
		}

		for (auto it = ItemVector::const_reverse_iterator(items->getEndTopItem()),
		          end = ItemVector::const_reverse_iterator(items->getBeginTopItem());
		     it != end; ++it) {
			auto item = *it;
			const ItemType& iit = Item::items[item->getID()];
			if (!iit.lookThrough) {
				return item;
			}
		}
	}

	return ground;
}

void Tile::onAddTileItem(std::shared_ptr<Item> item)
{
	if (item->hasProperty(CONST_PROP_MOVEABLE) || item->getContainer()) {
		auto it = g_game.browseFields.find(shared_from_this());
		if (it != g_game.browseFields.end()) {
			it->second->addItemBack(item);
			item->setParent(it->second);
		}
	}

	setTileFlags(item);

	const Position& cylinderMapPos = getPosition();

	SpectatorVec spectators;
	g_game.map.getSpectators(spectators, cylinderMapPos, true);

	// send to client
	for (auto spectator : spectators) {
		if (auto spectatorPlayer = spectator->getPlayer()) {
			spectatorPlayer->sendAddTileItem(shared_from_this(), cylinderMapPos, item);
		}
	}

	// event methods
	for (auto spectator : spectators) {
		spectator->onAddTileItem(shared_from_this(), cylinderMapPos);
	}

	if ((!hasFlag(TILESTATE_PROTECTIONZONE) || getBoolean(ConfigManager::CLEAN_PROTECTION_ZONES)) &&
	    item->isCleanable()) {
		if (!std::dynamic_pointer_cast<HouseTile>(shared_from_this())) {
			g_game.addTileToClean(shared_from_this());
		}
	}
}

void Tile::onUpdateTileItem(std::shared_ptr<Item> oldItem, const ItemType& oldType, std::shared_ptr<Item> newItem,
                            const ItemType& newType)
{
	if (newItem->hasProperty(CONST_PROP_MOVEABLE) || newItem->getContainer()) {
		auto it = g_game.browseFields.find(shared_from_this());
		if (it != g_game.browseFields.end()) {
			int32_t index = it->second->getThingIndex(oldItem);
			if (index != -1) {
				it->second->replaceThing(index, newItem);
				newItem->setParent(it->second);
			}
		}
	} else if (oldItem->hasProperty(CONST_PROP_MOVEABLE) || oldItem->getContainer()) {
		auto it = g_game.browseFields.find(shared_from_this());
		if (it != g_game.browseFields.end()) {
			auto oldParent = oldItem->getParent();
			it->second->removeThing(oldItem, oldItem->getItemCount());
			oldItem->setParent(oldParent);
		}
	}

	const Position& cylinderMapPos = getPosition();

	SpectatorVec spectators;
	g_game.map.getSpectators(spectators, cylinderMapPos, true);

	// send to client
	for (auto spectator : spectators) {
		if (auto spectatorPlayer = spectator->getPlayer()) {
			spectatorPlayer->sendUpdateTileItem(shared_from_this(), cylinderMapPos, newItem);
		}
	}

	// event methods
	for (auto spectator : spectators) {
		spectator->onUpdateTileItem(shared_from_this(), cylinderMapPos, oldItem, oldType, newItem, newType);
	}
}

void Tile::onRemoveTileItem(const SpectatorVec& spectators, const std::vector<int32_t>& oldStackPosVector,
                            std::shared_ptr<Item> item)
{
	if (item->hasProperty(CONST_PROP_MOVEABLE) || item->getContainer()) {
		auto it = g_game.browseFields.find(shared_from_this());
		if (it != g_game.browseFields.end()) {
			it->second->removeThing(item, item->getItemCount());
		}
	}

	resetTileFlags(item);

	const Position& cylinderMapPos = getPosition();
	const ItemType& iType = Item::items[item->getID()];

	// send to client
	size_t i = 0;
	for (auto spectator : spectators) {
		if (auto tmpPlayer = spectator->getPlayer()) {
			tmpPlayer->sendRemoveTileThing(cylinderMapPos, oldStackPosVector[i++]);
		}
	}

	// event methods
	for (auto spectator : spectators) {
		spectator->onRemoveTileItem(shared_from_this(), cylinderMapPos, iType, item);
	}

	if (!hasFlag(TILESTATE_PROTECTIONZONE) || getBoolean(ConfigManager::CLEAN_PROTECTION_ZONES)) {
		auto items = getItemList();
		if (!items || items->empty()) {
			g_game.removeTileToClean(shared_from_this());
			return;
		}

		bool ret = false;
		for (auto toCheck : *items) {
			if (toCheck->isCleanable()) {
				ret = true;
				break;
			}
		}

		if (!ret) {
			g_game.removeTileToClean(shared_from_this());
		}
	}
}

void Tile::onUpdateTile(const SpectatorVec& spectators)
{
	const Position& cylinderMapPos = getPosition();

	// send to clients
	for (auto spectator : spectators) {
		assert(spectator->getPlayer() != nullptr);
		std::static_pointer_cast<Player>(spectator)->sendUpdateTile(shared_from_this(), cylinderMapPos);
	}
}

ReturnValue Tile::queryAdd(int32_t, std::shared_ptr<const Thing> thing, uint32_t, uint32_t flags,
                           std::shared_ptr<Creature>) const
{
	if (auto creature = thing->getCreature()) {
		if (hasBitSet(FLAG_NOLIMIT, flags)) {
			return RETURNVALUE_NOERROR;
		}

		if (hasBitSet(FLAG_PATHFINDING, flags) && hasFlag(TILESTATE_FLOORCHANGE | TILESTATE_TELEPORT)) {
			return RETURNVALUE_NOTPOSSIBLE;
		}

		if (!ground) {
			return RETURNVALUE_NOTPOSSIBLE;
		}

		if (auto monster = creature->getMonster()) {
			if (hasFlag(TILESTATE_PROTECTIONZONE | TILESTATE_FLOORCHANGE | TILESTATE_TELEPORT)) {
				return RETURNVALUE_NOTPOSSIBLE;
			}

			const CreatureVector* creatures = getCreatures();
			if (monster->canPushCreatures() && !monster->isSummon()) {
				if (creatures) {
					for (auto tileCreature : *creatures) {
						if (tileCreature->getPlayer() && tileCreature->getPlayer()->isInGhostMode()) {
							continue;
						}

						auto creatureMonster = tileCreature->getMonster();
						if (!creatureMonster || !tileCreature->isPushable() ||
						    (creatureMonster->isSummon() && creatureMonster->getMaster()->getPlayer())) {
							return RETURNVALUE_NOTPOSSIBLE;
						}
					}
				}
			} else if (creatures && !creatures->empty()) {
				for (auto tileCreature : *creatures) {
					if (!tileCreature->isInGhostMode()) {
						return RETURNVALUE_NOTENOUGHROOM;
					}
				}
			}

			if (hasFlag(TILESTATE_IMMOVABLEBLOCKSOLID)) {
				return RETURNVALUE_NOTPOSSIBLE;
			}

			if (hasBitSet(FLAG_PATHFINDING, flags) && hasFlag(TILESTATE_IMMOVABLENOFIELDBLOCKPATH)) {
				return RETURNVALUE_NOTPOSSIBLE;
			}

			if (hasFlag(TILESTATE_BLOCKSOLID) ||
			    (hasBitSet(FLAG_PATHFINDING, flags) && hasFlag(TILESTATE_NOFIELDBLOCKPATH))) {
				if (!(monster->canPushItems() || hasBitSet(FLAG_IGNOREBLOCKITEM, flags))) {
					return RETURNVALUE_NOTPOSSIBLE;
				}
			}

			auto field = getFieldItem();
			if (!field || field->isBlocking() || field->getDamage() == 0) {
				return RETURNVALUE_NOERROR;
			}

			CombatType_t combatType = field->getCombatType();

			// There is 3 options for a monster to enter a magic field
			// 1) Monster is immune
			if (!monster->isImmune(combatType)) {
				// 1) Monster is able to walk over field type
				// 2) Being attacked while random stepping will make it ignore
				// field damages
				if (hasBitSet(FLAG_IGNOREFIELDDAMAGE, flags)) {
					if (!(monster->canWalkOnFieldType(combatType) || monster->isIgnoringFieldDamage())) {
						return RETURNVALUE_NOTPOSSIBLE;
					}
				} else {
					return RETURNVALUE_NOTPOSSIBLE;
				}
			}

			return RETURNVALUE_NOERROR;
		}

		const CreatureVector* creatures = getCreatures();
		if (auto player = creature->getPlayer()) {
			if (creatures && !creatures->empty() && !hasBitSet(FLAG_IGNOREBLOCKCREATURE, flags) &&
			    !player->isAccessPlayer()) {
				for (auto tileCreature : *creatures) {
					if (!player->canWalkthrough(tileCreature)) {
						return RETURNVALUE_NOTPOSSIBLE;
					}
				}
			}

			if (auto field = getFieldItem()) {
				if (field->getDamage() != 0 && hasBitSet(FLAG_PATHFINDING, flags) &&
				    !hasBitSet(FLAG_IGNOREFIELDDAMAGE, flags)) {
					return RETURNVALUE_NOTPOSSIBLE;
				}
			}

			if (!player->hasParent() && hasFlag(TILESTATE_NOLOGOUT)) {
				// player is trying to login to a "no logout" tile
				return RETURNVALUE_NOTPOSSIBLE;
			}

			auto playerTile = player->getTile();
			if (playerTile && player->isPzLocked()) {
				if (!playerTile->hasFlag(TILESTATE_PVPZONE)) {
					// player is trying to enter a pvp zone while being pz-locked
					if (hasFlag(TILESTATE_PVPZONE)) {
						return RETURNVALUE_PLAYERISPZLOCKEDENTERPVPZONE;
					}
				} else if (!hasFlag(TILESTATE_PVPZONE)) {
					// player is trying to leave a pvp zone while being pz-locked
					return RETURNVALUE_PLAYERISPZLOCKEDLEAVEPVPZONE;
				}

				if ((!playerTile->hasFlag(TILESTATE_NOPVPZONE) && hasFlag(TILESTATE_NOPVPZONE)) ||
				    (!playerTile->hasFlag(TILESTATE_PROTECTIONZONE) && hasFlag(TILESTATE_PROTECTIONZONE))) {
					// player is trying to enter a non-pvp/protection zone while being pz-locked
					return RETURNVALUE_PLAYERISPZLOCKED;
				}
			}
		} else if (creatures && !creatures->empty() && !hasBitSet(FLAG_IGNOREBLOCKCREATURE, flags)) {
			for (auto tileCreature : *creatures) {
				if (!tileCreature->isInGhostMode()) {
					return RETURNVALUE_NOTENOUGHROOM;
				}
			}
		}

		if (!hasBitSet(FLAG_IGNOREBLOCKITEM, flags)) {
			// If the FLAG_IGNOREBLOCKITEM bit isn't set we dont have to iterate every single item
			if (hasFlag(TILESTATE_BLOCKSOLID)) {
				return RETURNVALUE_NOTENOUGHROOM;
			}
		} else {
			// FLAG_IGNOREBLOCKITEM is set
			if (ground) {
				const ItemType& iiType = Item::items[ground->getID()];
				if (iiType.blockSolid && (!iiType.moveable || ground->hasAttribute(ITEM_ATTRIBUTE_UNIQUEID))) {
					return RETURNVALUE_NOTPOSSIBLE;
				}
			}

			if (const auto items = getItemList()) {
				for (auto item : *items) {
					const ItemType& iiType = Item::items[item->getID()];
					if (iiType.blockSolid && (!iiType.moveable || item->hasAttribute(ITEM_ATTRIBUTE_UNIQUEID))) {
						return RETURNVALUE_NOTPOSSIBLE;
					}
				}
			}
		}
	} else if (auto item = thing->getItem()) {
		const TileItemVector* items = getItemList();
		if (items && items->size() >= 0xFFFF) {
			return RETURNVALUE_NOTPOSSIBLE;
		}

		if (hasBitSet(FLAG_NOLIMIT, flags)) {
			return RETURNVALUE_NOERROR;
		}

		if (item->isStoreItem()) {
			return RETURNVALUE_ITEMCANNOTBEMOVEDTHERE;
		}

		bool itemIsHangable = item->isHangable();
		if (!ground && !itemIsHangable) {
			return RETURNVALUE_NOTPOSSIBLE;
		}

		const CreatureVector* creatures = getCreatures();
		if (creatures && !creatures->empty() && item->isBlocking() && !hasBitSet(FLAG_IGNOREBLOCKCREATURE, flags)) {
			for (auto tileCreature : *creatures) {
				if (!tileCreature->isInGhostMode()) {
					return RETURNVALUE_NOTENOUGHROOM;
				}
			}
		}

		if (itemIsHangable && hasFlag(TILESTATE_SUPPORTS_HANGABLE)) {
			if (items) {
				for (auto tileItem : *items) {
					if (tileItem->isHangable()) {
						return RETURNVALUE_NEEDEXCHANGE;
					}
				}
			}
		} else {
			if (ground) {
				const ItemType& iiType = Item::items[ground->getID()];
				if (iiType.blockSolid) {
					if (!iiType.allowPickupable || item->isMagicField() || item->isBlocking()) {
						if (!item->isPickupable()) {
							return RETURNVALUE_NOTENOUGHROOM;
						}

						if (!iiType.hasHeight || iiType.pickupable || iiType.isBed()) {
							return RETURNVALUE_NOTENOUGHROOM;
						}
					}
				}
			}

			if (items) {
				for (auto tileItem : *items) {
					const ItemType& iiType = Item::items[tileItem->getID()];
					if (!iiType.blockSolid) {
						continue;
					}

					if (iiType.allowPickupable && !item->isMagicField() && !item->isBlocking()) {
						continue;
					}

					if (!item->isPickupable()) {
						return RETURNVALUE_NOTENOUGHROOM;
					}

					if (!iiType.hasHeight || iiType.pickupable || iiType.isBed()) {
						return RETURNVALUE_NOTENOUGHROOM;
					}
				}
			}
		}
	}
	return RETURNVALUE_NOERROR;
}

ReturnValue Tile::queryMaxCount(int32_t, std::shared_ptr<const Thing>, uint32_t count, uint32_t& maxQueryCount,
                                uint32_t) const
{
	maxQueryCount = std::max<uint32_t>(1, count);
	return RETURNVALUE_NOERROR;
}

ReturnValue Tile::queryRemove(std::shared_ptr<const Thing> thing, uint32_t count, uint32_t flags,
                              std::shared_ptr<Creature> /*= nullptr */) const
{
	int32_t index = getThingIndex(thing);
	if (index == -1) {
		return RETURNVALUE_NOTPOSSIBLE;
	}

	std::shared_ptr<const Item> item = thing->getItem();
	if (!item) {
		return RETURNVALUE_NOTPOSSIBLE;
	}

	if (count == 0 || (item->isStackable() && count > item->getItemCount())) {
		return RETURNVALUE_NOTPOSSIBLE;
	}

	if (!item->isMoveable() && !hasBitSet(FLAG_IGNORENOTMOVEABLE, flags)) {
		return RETURNVALUE_NOTMOVEABLE;
	}

	return RETURNVALUE_NOERROR;
}

std::shared_ptr<Cylinder> Tile::queryDestination(int32_t&, std::shared_ptr<const Thing>,
                                                 std::shared_ptr<Item>& destItem, uint32_t& flags)
{
	std::shared_ptr<Tile> destTile = nullptr;
	destItem = nullptr;

	if (hasFlag(TILESTATE_FLOORCHANGE_DOWN)) {
		uint16_t dx = tilePos.x;
		uint16_t dy = tilePos.y;
		uint8_t dz = tilePos.z + 1;

		auto southDownTile = g_game.map.getTile(dx, dy - 1, dz);
		if (southDownTile && southDownTile->hasFlag(TILESTATE_FLOORCHANGE_SOUTH_ALT)) {
			dy -= 2;
			destTile = g_game.map.getTile(dx, dy, dz);
		} else {
			auto eastDownTile = g_game.map.getTile(dx - 1, dy, dz);
			if (eastDownTile && eastDownTile->hasFlag(TILESTATE_FLOORCHANGE_EAST_ALT)) {
				dx -= 2;
				destTile = g_game.map.getTile(dx, dy, dz);
			} else {
				auto downTile = g_game.map.getTile(dx, dy, dz);
				if (downTile) {
					if (downTile->hasFlag(TILESTATE_FLOORCHANGE_NORTH)) {
						++dy;
					}

					if (downTile->hasFlag(TILESTATE_FLOORCHANGE_SOUTH)) {
						--dy;
					}

					if (downTile->hasFlag(TILESTATE_FLOORCHANGE_SOUTH_ALT)) {
						dy -= 2;
					}

					if (downTile->hasFlag(TILESTATE_FLOORCHANGE_EAST)) {
						--dx;
					}

					if (downTile->hasFlag(TILESTATE_FLOORCHANGE_EAST_ALT)) {
						dx -= 2;
					}

					if (downTile->hasFlag(TILESTATE_FLOORCHANGE_WEST)) {
						++dx;
					}

					destTile = g_game.map.getTile(dx, dy, dz);
				}
			}
		}
	} else if (hasFlag(TILESTATE_FLOORCHANGE)) {
		uint16_t dx = tilePos.x;
		uint16_t dy = tilePos.y;
		uint8_t dz = tilePos.z - 1;

		if (hasFlag(TILESTATE_FLOORCHANGE_NORTH)) {
			--dy;
		}

		if (hasFlag(TILESTATE_FLOORCHANGE_SOUTH)) {
			++dy;
		}

		if (hasFlag(TILESTATE_FLOORCHANGE_EAST)) {
			++dx;
		}

		if (hasFlag(TILESTATE_FLOORCHANGE_WEST)) {
			--dx;
		}

		if (hasFlag(TILESTATE_FLOORCHANGE_SOUTH_ALT)) {
			dy += 2;
		}

		if (hasFlag(TILESTATE_FLOORCHANGE_EAST_ALT)) {
			dx += 2;
		}

		destTile = g_game.map.getTile(dx, dy, dz);
	}

	if (!destTile) {
		destTile = shared_from_this();
	} else {
		flags |= FLAG_NOLIMIT; // Will ignore that there is blocking items/creatures
	}

	if (destTile) {
		auto destThing = destTile->getTopDownItem();
		if (destThing) {
			destItem = destThing->getItem();
		}
	}
	return destTile;
}

void Tile::addThing(std::shared_ptr<Thing> thing) { addThing(0, thing); }

void Tile::addThing(int32_t, std::shared_ptr<Thing> thing)
{
	auto creature = thing->getCreature();
	if (creature) {
		g_game.map.clearSpectatorCache();
		if (creature->getPlayer()) {
			g_game.map.clearPlayersSpectatorCache();
		}

		creature->setParent(shared_from_this());
		CreatureVector* creatures = makeCreatures();
		creatures->insert(creatures->begin(), creature);
	} else {
		auto item = thing->getItem();
		if (!item) {
			return /*RETURNVALUE_NOTPOSSIBLE*/;
		}

		TileItemVector* items = getItemList();
		if (items && items->size() >= 0xFFFF) {
			return /*RETURNVALUE_NOTPOSSIBLE*/;
		}

		item->setParent(shared_from_this());

		const ItemType& itemType = Item::items[item->getID()];
		if (itemType.isGroundTile()) {
			if (!ground) {
				ground = item->shared_from_this();
				onAddTileItem(item);
			} else {
				const ItemType& oldType = Item::items[ground->getID()];

				auto oldGround = ground;
				ground->setParent(nullptr);
				ground = item->shared_from_this();
				resetTileFlags(oldGround);
				setTileFlags(item);
				onUpdateTileItem(oldGround, oldType, item, itemType);
				postRemoveNotification(oldGround, nullptr, 0);
			}
		} else if (itemType.alwaysOnTop) {
			if (itemType.isSplash() && items) {
				// remove old splash if exists
				for (ItemVector::const_iterator it = items->getBeginTopItem(), end = items->getEndTopItem(); it != end;
				     ++it) {
					auto oldSplash = *it;
					if (!Item::items[oldSplash->getID()].isSplash()) {
						continue;
					}

					removeThing(oldSplash, 1);
					oldSplash->setParent(nullptr);
					postRemoveNotification(oldSplash, nullptr, 0);
					break;
				}
			}

			bool isInserted = false;

			if (items) {
				for (auto it = items->getBeginTopItem(), end = items->getEndTopItem(); it != end; ++it) {
					// Note: this is different from internalAddThing
					if (itemType.alwaysOnTopOrder <= Item::items[(*it)->getID()].alwaysOnTopOrder) {
						items->insert(it, item->shared_from_this());
						isInserted = true;
						break;
					}
				}
			} else {
				items = makeItemList();
			}

			if (!isInserted) {
				items->push_back(item->shared_from_this());
			}

			onAddTileItem(item);
		} else {
			if (itemType.isMagicField()) {
				// remove old field item if exists
				if (items) {
					for (ItemVector::const_iterator it = items->getBeginDownItem(), end = items->getEndDownItem();
					     it != end; ++it) {
						auto oldField = (*it)->getMagicField();
						if (oldField) {
							if (oldField->isReplaceable()) {
								removeThing(oldField, 1);

								oldField->setParent(nullptr);
								postRemoveNotification(oldField, nullptr, 0);
								break;
							} else {
								item->setParent(nullptr);
								return;
							}
						}
					}
				}
			}

			items = makeItemList();
			items->insert(items->getBeginDownItem(), item->shared_from_this());
			items->addDownItemCount(1);
			onAddTileItem(item);
		}
	}
}

void Tile::updateThing(std::shared_ptr<Thing> thing, uint16_t itemId, uint32_t count)
{
	int32_t index = getThingIndex(thing);
	if (index == -1) {
		return /*RETURNVALUE_NOTPOSSIBLE*/;
	}

	auto item = thing->getItem();
	if (!item) {
		return /*RETURNVALUE_NOTPOSSIBLE*/;
	}

	const ItemType& oldType = Item::items[item->getID()];
	const ItemType& newType = Item::items[itemId];
	resetTileFlags(item);
	item->setID(itemId);
	item->setSubType(count);
	setTileFlags(item);
	onUpdateTileItem(item, oldType, item, newType);
}

void Tile::replaceThing(uint32_t index, std::shared_ptr<Thing> thing)
{
	int32_t pos = index;

	auto item = thing->getItem();
	if (!item) {
		return /*RETURNVALUE_NOTPOSSIBLE*/;
	}

	std::shared_ptr<Item> oldItem = nullptr;
	bool isInserted = false;

	if (ground) {
		if (pos == 0) {
			oldItem = ground;
			ground = item->shared_from_this();
			isInserted = true;
		}

		--pos;
	}

	TileItemVector* items = getItemList();
	if (items && !isInserted) {
		int32_t topItemSize = getTopItemCount();
		if (pos < topItemSize) {
			auto it = items->getBeginTopItem();
			it += pos;

			oldItem = *it;
			it = items->erase(it);
			items->insert(it, item->shared_from_this());
			isInserted = true;
		}

		pos -= topItemSize;
	}

	CreatureVector* creatures = getCreatures();
	if (creatures) {
		if (!isInserted && pos < static_cast<int32_t>(creatures->size())) {
			return /*RETURNVALUE_NOTPOSSIBLE*/;
		}

		pos -= static_cast<uint32_t>(creatures->size());
	}

	if (items && !isInserted) {
		int32_t downItemSize = getDownItemCount();
		if (pos < downItemSize) {
			auto it = items->getBeginDownItem() + pos;
			oldItem = *it;
			it = items->erase(it);
			items->insert(it, item->shared_from_this());
			isInserted = true;
		}
	}

	if (isInserted) {
		item->setParent(shared_from_this());

		resetTileFlags(oldItem);
		setTileFlags(item);
		const ItemType& oldType = Item::items[oldItem->getID()];
		const ItemType& newType = Item::items[item->getID()];
		onUpdateTileItem(oldItem, oldType, item, newType);

		oldItem->setParent(nullptr);
		return /*RETURNVALUE_NOERROR*/;
	}
}

void Tile::removeThing(std::shared_ptr<Thing> thing, uint32_t count)
{
	auto creature = thing->getCreature();
	if (creature) {
		CreatureVector* creatures = getCreatures();
		if (creatures) {
			auto it = std::find(creatures->begin(), creatures->end(), thing);
			if (it != creatures->end()) {
				g_game.map.clearSpectatorCache();
				if (creature->getPlayer()) {
					g_game.map.clearPlayersSpectatorCache();
				}

				creatures->erase(it);
			}
		}
		return;
	}

	auto item = thing->getItem();
	if (!item) {
		return;
	}

	int32_t index = getThingIndex(item);
	if (index == -1) {
		return;
	}

	if (ground && item == ground) {
		ground->setParent(nullptr);
		ground = nullptr;

		SpectatorVec spectators;
		g_game.map.getSpectators(spectators, getPosition(), true);
		onRemoveTileItem(spectators, std::vector<int32_t>(spectators.size(), 0), item);
		return;
	}

	TileItemVector* items = getItemList();
	if (!items) {
		return;
	}

	const ItemType& itemType = Item::items[item->getID()];
	if (itemType.alwaysOnTop) {
		auto it = std::find_if(items->getBeginTopItem(), items->getEndTopItem(),
		                       [&](const std::shared_ptr<Item>& p) { return p == item; });
		if (it == items->getEndTopItem()) {
			return;
		}

		std::vector<int32_t> oldStackPosVector;

		SpectatorVec spectators;
		g_game.map.getSpectators(spectators, getPosition(), true);
		for (auto spectator : spectators) {
			if (auto spectatorPlayer = spectator->getPlayer()) {
				oldStackPosVector.push_back(getStackposOfItem(spectatorPlayer, item));
			}
		}

		item->setParent(nullptr);
		items->erase(it);
		onRemoveTileItem(spectators, oldStackPosVector, item);
	} else {
		auto it = std::find_if(items->getBeginDownItem(), items->getEndDownItem(),
		                       [&](const std::shared_ptr<Item>& p) { return p == item; });
		if (it == items->getEndDownItem()) {
			return;
		}

		if (itemType.stackable && count != item->getItemCount()) {
			uint8_t newCount =
			    static_cast<uint8_t>(std::max<int32_t>(0, static_cast<int32_t>(item->getItemCount() - count)));
			item->setItemCount(newCount);
			onUpdateTileItem(item, itemType, item, itemType);
		} else {
			std::vector<int32_t> oldStackPosVector;

			SpectatorVec spectators;
			g_game.map.getSpectators(spectators, getPosition(), true);
			for (auto spectator : spectators) {
				if (auto spectatorPlayer = spectator->getPlayer()) {
					oldStackPosVector.push_back(getStackposOfItem(spectatorPlayer, item));
				}
			}

			item->setParent(nullptr);
			items->erase(it);
			items->addDownItemCount(-1);
			onRemoveTileItem(spectators, oldStackPosVector, item);
		}
	}
}

bool Tile::hasCreature(std::shared_ptr<Creature> creature) const
{
	if (const CreatureVector* creatures = getCreatures()) {
		return std::find(creatures->begin(), creatures->end(), creature) != creatures->end();
	}
	return false;
}

void Tile::removeCreature(std::shared_ptr<Creature> creature)
{
	g_game.map.getQTNode(tilePos.x, tilePos.y)->removeCreature(creature);
	removeThing(creature, 0);
}

int32_t Tile::getThingIndex(std::shared_ptr<const Thing> thing) const
{
	int32_t n = -1;
	if (ground) {
		if (ground == thing) {
			return 0;
		}
		++n;
	}

	const TileItemVector* items = getItemList();
	if (items) {
		std::shared_ptr<const Item> item = thing->getItem();
		if (item && item->isAlwaysOnTop()) {
			for (auto it = items->getBeginTopItem(), end = items->getEndTopItem(); it != end; ++it) {
				++n;
				if (*it == item) {
					return n;
				}
			}
		} else {
			n += items->getTopItemCount();
		}
	}

	if (const CreatureVector* creatures = getCreatures()) {
		if (thing->getCreature()) {
			for (auto creature : *creatures) {
				++n;
				if (creature == thing) {
					return n;
				}
			}
		} else {
			n += creatures->size();
		}
	}

	if (items) {
		std::shared_ptr<const Item> item = thing->getItem();
		if (item && !item->isAlwaysOnTop()) {
			for (auto it = items->getBeginDownItem(), end = items->getEndDownItem(); it != end; ++it) {
				++n;
				if (*it == item) {
					return n;
				}
			}
		}
	}
	return -1;
}

int32_t Tile::getClientIndexOfCreature(std::shared_ptr<const Player> player,
                                       std::shared_ptr<const Creature> creature) const
{
	int32_t n;
	if (ground) {
		n = 1;
	} else {
		n = 0;
	}

	const TileItemVector* items = getItemList();
	if (items) {
		n += items->getTopItemCount();
	}

	if (const CreatureVector* creatures = getCreatures()) {
		for (auto it = creatures->rbegin(), end = creatures->rend(); it != end; ++it) {
			auto c = *it;
			if (*it == creature) {
				return n;
			} else if (player->canSeeCreature(c)) {
				++n;
			}
		}
	}
	return -1;
}

int32_t Tile::getStackposOfItem(std::shared_ptr<const Player> player, std::shared_ptr<const Item> item) const
{
	int32_t n = 0;
	if (ground) {
		if (ground == item) {
			return n;
		}
		++n;
	}

	const TileItemVector* items = getItemList();
	if (items) {
		if (item->isAlwaysOnTop()) {
			for (auto it = items->getBeginTopItem(), end = items->getEndTopItem(); it != end; ++it) {
				if (*it == item) {
					return n;
				} else if (++n == MAX_STACKPOS) {
					return -1;
				}
			}
		} else {
			n += items->getTopItemCount();
			if (n >= MAX_STACKPOS) {
				return -1;
			}
		}
	}

	if (const CreatureVector* creatures = getCreatures()) {
		for (auto creature : *creatures) {
			if (player->canSeeCreature(creature)) {
				if (++n >= MAX_STACKPOS) {
					return -1;
				}
			}
		}
	}

	if (items && !item->isAlwaysOnTop()) {
		for (auto it = items->getBeginDownItem(), end = items->getEndDownItem(); it != end; ++it) {
			if (*it == item) {
				return n;
			} else if (++n >= MAX_STACKPOS) {
				return -1;
			}
		}
	}
	return -1;
}

size_t Tile::getFirstIndex() const { return 0; }

size_t Tile::getLastIndex() const { return getThingCount(); }

uint32_t Tile::getItemTypeCount(uint16_t itemId, int32_t subType /*= -1*/) const
{
	uint32_t count = 0;
	if (ground && ground->getID() == itemId) {
		count += Item::countByType(ground, subType);
	}

	const TileItemVector* items = getItemList();
	if (items) {
		for (const auto& item : *items) {
			if (item && item->getID() == itemId) {
				count += Item::countByType(item, subType);
			}
		}
	}
	return count;
}

std::shared_ptr<Thing> Tile::getThing(size_t index) const
{
	if (ground) {
		if (index == 0) {
			return ground;
		}

		--index;
	}

	const TileItemVector* items = getItemList();
	if (items) {
		uint32_t topItemSize = items->getTopItemCount();
		if (index < topItemSize) {
			return items->at(items->getDownItemCount() + index);
		}
		index -= topItemSize;
	}

	if (const CreatureVector* creatures = getCreatures()) {
		if (index < creatures->size()) {
			return (*creatures)[index];
		}
		index -= creatures->size();
	}

	if (items && index < items->getDownItemCount()) {
		return items->at(index);
	}
	return nullptr;
}

void Tile::postAddNotification(std::shared_ptr<Thing> thing, std::shared_ptr<const Cylinder> oldParent, int32_t index,
                               cylinderlink_t link /*= LINK_OWNER*/)
{
	SpectatorVec spectators;
	g_game.map.getSpectators(spectators, getPosition(), true, true);
	for (auto spectator : spectators) {
		assert(spectator->getPlayer() != nullptr);
		std::static_pointer_cast<Player>(spectator)->postAddNotification(thing, oldParent, index, LINK_NEAR);
	}

	// Hold a strong reference during callbacks since the item might be moved/deleted (e.g., mailbox)
	auto creature = thing->getCreature();
	std::shared_ptr<Item> item = nullptr;
	std::shared_ptr<Item> holdItem;
	if (!creature) {
		item = thing->getItem();
		if (item) {
			holdItem = item->shared_from_this();
		}
	}

	if (link == LINK_OWNER) {
		if (hasFlag(TILESTATE_TELEPORT)) {
			auto teleport = getTeleportItem();
			if (teleport) {
				teleport->addThing(thing);
			}
		} else if (hasFlag(TILESTATE_TRASHHOLDER)) {
			auto trashholder = getTrashHolder();
			if (trashholder) {
				trashholder->addThing(thing);
			}
		} else if (hasFlag(TILESTATE_MAILBOX)) {
			auto mailbox = getMailbox();
			if (mailbox) {
				mailbox->addThing(thing);
			}
		}

		// calling movement scripts
		if (creature) {
			g_moveEvents->onCreatureMove(creature, shared_from_this(), MOVE_EVENT_STEP_IN);
		} else if (item) {
			g_moveEvents->onItemMove(item, shared_from_this(), true);
		}
	}
}

void Tile::postRemoveNotification(std::shared_ptr<Thing> thing, std::shared_ptr<const Cylinder> newParent,
                                  int32_t index, cylinderlink_t)
{
	SpectatorVec spectators;
	g_game.map.getSpectators(spectators, getPosition(), true, true);

	if (getThingCount() > 8) {
		onUpdateTile(spectators);
	}

	for (auto spectator : spectators) {
		assert(spectator->getPlayer() != nullptr);
		std::static_pointer_cast<Player>(spectator)->postRemoveNotification(thing, newParent, index, LINK_NEAR);
	}

	// calling movement scripts
	auto creature = thing->getCreature();
	if (creature) {
		g_moveEvents->onCreatureMove(creature, shared_from_this(), MOVE_EVENT_STEP_OUT);
	} else {
		auto item = thing->getItem();
		if (item) {
			g_moveEvents->onItemMove(item, shared_from_this(), false);
		}
	}
}

void Tile::internalAddThing(std::shared_ptr<Thing> thing) { internalAddThing(0, thing); }

void Tile::internalAddThing(uint32_t, std::shared_ptr<Thing> thing)
{
	thing->setParent(shared_from_this());

	auto creature = thing->getCreature();
	if (creature) {
		g_game.map.clearSpectatorCache();
		if (creature->getPlayer()) {
			g_game.map.clearPlayersSpectatorCache();
		}

		CreatureVector* creatures = makeCreatures();
		creatures->insert(creatures->begin(), creature);
	} else {
		auto item = thing->getItem();
		if (!item) {
			return;
		}

		const ItemType& itemType = Item::items[item->getID()];
		if (itemType.isGroundTile()) {
			if (!ground) {
				ground = item->shared_from_this();
				setTileFlags(item);
			}
			return;
		}

		TileItemVector* items = makeItemList();
		if (items->size() >= 0xFFFF) {
			return /*RETURNVALUE_NOTPOSSIBLE*/;
		}

		if (itemType.alwaysOnTop) {
			bool isInserted = false;
			for (auto it = items->getBeginTopItem(), end = items->getEndTopItem(); it != end; ++it) {
				if (Item::items[(*it)->getID()].alwaysOnTopOrder > itemType.alwaysOnTopOrder) {
					items->insert(it, item->shared_from_this());
					isInserted = true;
					break;
				}
			}

			if (!isInserted) {
				items->push_back(item->shared_from_this());
			}
		} else {
			items->insert(items->getBeginDownItem(), item->shared_from_this());
			items->addDownItemCount(1);
		}

		setTileFlags(item);
	}
}

void Tile::setTileFlags(std::shared_ptr<const Item> item)
{
	if (!hasFlag(TILESTATE_FLOORCHANGE)) {
		const ItemType& it = Item::items[item->getID()];
		if (it.floorChange != 0) {
			setFlag(it.floorChange);
		}
	}

	if (item->hasProperty(CONST_PROP_IMMOVABLEBLOCKSOLID)) {
		setFlag(TILESTATE_IMMOVABLEBLOCKSOLID);
	}

	if (item->hasProperty(CONST_PROP_BLOCKPATH)) {
		setFlag(TILESTATE_BLOCKPATH);
	}

	if (item->hasProperty(CONST_PROP_NOFIELDBLOCKPATH)) {
		setFlag(TILESTATE_NOFIELDBLOCKPATH);
	}

	if (item->hasProperty(CONST_PROP_IMMOVABLENOFIELDBLOCKPATH)) {
		setFlag(TILESTATE_IMMOVABLENOFIELDBLOCKPATH);
	}

	if (item->getTeleport()) {
		setFlag(TILESTATE_TELEPORT);
	}

	if (item->getMagicField()) {
		setFlag(TILESTATE_MAGICFIELD);
	}

	if (item->getMailbox()) {
		setFlag(TILESTATE_MAILBOX);
	}

	if (item->getTrashHolder()) {
		setFlag(TILESTATE_TRASHHOLDER);
	}

	if (item->hasProperty(CONST_PROP_BLOCKSOLID)) {
		setFlag(TILESTATE_BLOCKSOLID);
	}

	if (item->getBed()) {
		setFlag(TILESTATE_BED);
	}

	auto container = item->getContainer();
	if (container && container->getDepotLocker()) {
		setFlag(TILESTATE_DEPOT);
	}

	if (item->hasProperty(CONST_PROP_SUPPORTHANGABLE)) {
		setFlag(TILESTATE_SUPPORTS_HANGABLE);
	}
}

void Tile::resetTileFlags(std::shared_ptr<const Item> item)
{
	const ItemType& it = Item::items[item->getID()];
	if (it.floorChange != 0) {
		resetFlag(TILESTATE_FLOORCHANGE);
	}

	if (item->hasProperty(CONST_PROP_BLOCKSOLID) && !hasProperty(item, CONST_PROP_BLOCKSOLID)) {
		resetFlag(TILESTATE_BLOCKSOLID);
	}

	if (item->hasProperty(CONST_PROP_IMMOVABLEBLOCKSOLID) && !hasProperty(item, CONST_PROP_IMMOVABLEBLOCKSOLID)) {
		resetFlag(TILESTATE_IMMOVABLEBLOCKSOLID);
	}

	if (item->hasProperty(CONST_PROP_BLOCKPATH) && !hasProperty(item, CONST_PROP_BLOCKPATH)) {
		resetFlag(TILESTATE_BLOCKPATH);
	}

	if (item->hasProperty(CONST_PROP_NOFIELDBLOCKPATH) && !hasProperty(item, CONST_PROP_NOFIELDBLOCKPATH)) {
		resetFlag(TILESTATE_NOFIELDBLOCKPATH);
	}

	if (item->hasProperty(CONST_PROP_IMMOVABLEBLOCKPATH) && !hasProperty(item, CONST_PROP_IMMOVABLEBLOCKPATH)) {
		resetFlag(TILESTATE_IMMOVABLEBLOCKPATH);
	}

	if (item->hasProperty(CONST_PROP_IMMOVABLENOFIELDBLOCKPATH) &&
	    !hasProperty(item, CONST_PROP_IMMOVABLENOFIELDBLOCKPATH)) {
		resetFlag(TILESTATE_IMMOVABLENOFIELDBLOCKPATH);
	}

	if (item->getTeleport()) {
		resetFlag(TILESTATE_TELEPORT);
	}

	if (item->getMagicField()) {
		resetFlag(TILESTATE_MAGICFIELD);
	}

	if (item->getMailbox()) {
		resetFlag(TILESTATE_MAILBOX);
	}

	if (item->getTrashHolder()) {
		resetFlag(TILESTATE_TRASHHOLDER);
	}

	if (item->getBed()) {
		resetFlag(TILESTATE_BED);
	}

	auto container = item->getContainer();
	if (container && container->getDepotLocker()) {
		resetFlag(TILESTATE_DEPOT);
	}

	if (item->hasProperty(CONST_PROP_SUPPORTHANGABLE)) {
		resetFlag(TILESTATE_SUPPORTS_HANGABLE);
	}
}

bool Tile::isMoveableBlocking() const { return !ground || hasFlag(TILESTATE_BLOCKSOLID); }

std::shared_ptr<Item> Tile::getUseItem(int32_t index) const
{
	const TileItemVector* items = getItemList();

	// no items, get ground
	if (!items || items->size() == 0) {
		return ground;
	}

	// try getting thing by index
	if (auto thing = getThing(index)) {
		auto thingItem = thing->getItem();
		if (thingItem) {
			return thingItem;
		}
	}

	// try getting top movable item
	auto topDownItem = getTopDownItem();
	if (topDownItem) {
		return topDownItem;
	}

	// try getting door
	for (auto it = items->rbegin(), end = items->rend(); it != end; ++it) {
		if ((*it)->getDoor()) {
			return (*it)->getItem();
		}
	}

	return *items->begin();
}
