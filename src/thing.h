// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#ifndef FS_THING_H
#define FS_THING_H

struct Position;
class Tile;
class DynamicTile;
class StaticTile;
class HouseTile;
class Cylinder;
class Container;
class DepotChest;
class DepotLocker;
class Inbox;
class StoreInbox;
class Creature;
class Monster;
class Npc;
class Player;
class Item;
class BedItem;
class HouseTransferItem;
class MagicField;
class Podium;
class Teleport;
class TrashHolder;
class Mailbox;
class Door;

class Thing
{
public:
	constexpr Thing() = default;
	virtual ~Thing() = default;

	// non-copyable
	Thing(const Thing&) = delete;
	Thing& operator=(const Thing&) = delete;

	virtual int32_t getThrowRange() const = 0;
	virtual std::string getDescription(int32_t lookDistance) const = 0;
	virtual bool isRemoved() const { return true; }
	virtual bool isPushable() const = 0;

	virtual Cylinder* getParent() const { return nullptr; }
	virtual Cylinder* getRealParent() const { return getParent(); }
	virtual void setParent(Cylinder*) {}

	virtual const Position& getPosition() const;

	virtual Tile* getTile();
	virtual const Tile* getTile() const;

	virtual DynamicTile* getDynamicTile() { return nullptr; }
	virtual const DynamicTile* getDynamicTile() const { return nullptr; }

	virtual StaticTile* getStaticTile() { return nullptr; }
	virtual const StaticTile* getStaticTile() const { return nullptr; }

	virtual HouseTile* getHouseTile() { return nullptr; }
	virtual const HouseTile* getHouseTile() const { return nullptr; }

	virtual Cylinder* getCylinder() { return nullptr; }
	virtual const Cylinder* getCylinder() const { return nullptr; }

	virtual Container* getContainer() { return nullptr; }
	virtual const Container* getContainer() const { return nullptr; }

	virtual DepotChest* getDepotChest() { return nullptr; }
	virtual const DepotChest* getDepotChest() const { return nullptr; }

	virtual DepotLocker* getDepotLocker() { return nullptr; }
	virtual const DepotLocker* getDepotLocker() const { return nullptr; }

	virtual Inbox* getInbox() { return nullptr; }
	virtual const Inbox* getInbox() const { return nullptr; }

	virtual StoreInbox* getStoreInbox() { return nullptr; }
	virtual const StoreInbox* getStoreInbox() const { return nullptr; }

	virtual Creature* getCreature() { return nullptr; }
	virtual const Creature* getCreature() const { return nullptr; }

	virtual Monster* getMonster() { return nullptr; }
	virtual const Monster* getMonster() const { return nullptr; }

	virtual Npc* getNpc() { return nullptr; }
	virtual const Npc* getNpc() const { return nullptr; }

	virtual Player* getPlayer() { return nullptr; }
	virtual const Player* getPlayer() const { return nullptr; }

	virtual Item* getItem() { return nullptr; }
	virtual const Item* getItem() const { return nullptr; }

	virtual BedItem* getBed() { return nullptr; }
	virtual const BedItem* getBed() const { return nullptr; }

	virtual HouseTransferItem* getHouseTransferItem() { return nullptr; }
	virtual const HouseTransferItem* getHouseTransferItem() const { return nullptr; }

	virtual MagicField* getMagicField() { return nullptr; }
	virtual const MagicField* getMagicField() const { return nullptr; }

	virtual Podium* getPodium() { return nullptr; }
	virtual const Podium* getPodium() const { return nullptr; }

	virtual Teleport* getTeleport() { return nullptr; }
	virtual const Teleport* getTeleport() const { return nullptr; }

	virtual TrashHolder* getTrashHolder() { return nullptr; }
	virtual const TrashHolder* getTrashHolder() const { return nullptr; }

	virtual Mailbox* getMailbox() { return nullptr; }
	virtual const Mailbox* getMailbox() const { return nullptr; }

	virtual Door* getDoor() { return nullptr; }
	virtual const Door* getDoor() const { return nullptr; }
};

#endif // FS_THING_H
