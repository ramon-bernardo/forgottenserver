// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include "iomapserialize.h"

#include "bed.h"
#include "game.h"
#include "housetile.h"

extern Game g_game;

void IOMapSerialize::loadHouseItems(Map* map)
{
	int64_t start = OTSYS_TIME();

	auto result = tfs::db::store_query("SELECT `data` FROM `tile_store`");
	if (!result) {
		return;
	}

	do {
		auto attr = result->getString("data");
		PropStream propStream;
		propStream.init(attr.data(), attr.size());

		uint16_t x, y;
		uint8_t z;
		if (!propStream.read<uint16_t>(x) || !propStream.read<uint16_t>(y) || !propStream.read<uint8_t>(z)) {
			continue;
		}

		Tile* tile = map->getTile(x, y, z);
		if (!tile) {
			continue;
		}

		uint32_t item_count;
		if (!propStream.read<uint32_t>(item_count)) {
			continue;
		}

		while (item_count--) {
			loadItem(propStream, tile);
		}
	} while (result->next());
	std::cout << "> Loaded house items in: " << (OTSYS_TIME() - start) / (1000.) << " s" << std::endl;
}

bool IOMapSerialize::saveHouseItems()
{
	int64_t start = OTSYS_TIME();

	// Start the transaction
	DBTransaction transaction;
	if (!transaction.begin()) {
		return false;
	}

	// clear old tile data
	if (!tfs::db::execute_query("DELETE FROM `tile_store`")) {
		return false;
	}

	DBInsert stmt("INSERT INTO `tile_store` (`house_id`, `data`) VALUES ");

	PropWriteStream stream;
	for (const auto& it : g_game.map.houses.getHouses()) {
		// save house items
		House* house = it.second;
		for (HouseTile* tile : house->getTiles()) {
			saveTile(stream, tile);

			if (auto attributes = stream.getStream(); !attributes.empty()) {
				if (!stmt.addRow(fmt::format("{:d}, {:s}", house->getId(), tfs::db::escape_string(attributes)))) {
					return false;
				}
				stream.clear();
			}
		}
	}

	if (!stmt.execute()) {
		return false;
	}

	// End the transaction
	bool success = transaction.commit();
	std::cout << "> Saved house items in: " << (OTSYS_TIME() - start) / (1000.) << " s" << std::endl;
	return success;
}

bool IOMapSerialize::loadContainer(PropStream& propStream, Container* container)
{
	while (container->serializationCount > 0) {
		if (!loadItem(propStream, container)) {
			std::cout << "[Warning - IOMapSerialize::loadContainer] Unserialization error for container item: "
			          << container->getID() << std::endl;
			return false;
		}
		container->serializationCount--;
	}

	uint8_t endAttr;
	if (!propStream.read<uint8_t>(endAttr) || endAttr != 0) {
		std::cout << "[Warning - IOMapSerialize::loadContainer] Unserialization error for container item: "
		          << container->getID() << std::endl;
		return false;
	}
	return true;
}

bool IOMapSerialize::loadItem(PropStream& propStream, Cylinder* parent)
{
	uint16_t id;
	if (!propStream.read<uint16_t>(id)) {
		return false;
	}

	Tile* tile = nullptr;
	if (!parent->hasParent()) {
		tile = parent->getTile();
	}

	const ItemType& iType = Item::items[id];
	if (iType.moveable || iType.forceSerialize || !tile) {
		// create a new item
		Item* item = Item::CreateItem(id);
		if (item) {
			if (item->unserializeAttr(propStream)) {
				Container* container = item->getContainer();
				if (container && !loadContainer(propStream, container)) {
					delete item;
					return false;
				}

				parent->internalAddThing(item);
				item->startDecaying();
			} else {
				std::cout << "WARNING: Unserialization error in IOMapSerialize::loadItem()" << id << std::endl;
				delete item;
				return false;
			}
		}
	} else {
		// Stationary items like doors/beds/blackboards/bookcases
		Item* item = nullptr;
		if (const TileItemVector* items = tile->getItemList()) {
			for (Item* findItem : *items) {
				if (findItem->getID() == id) {
					item = findItem;
					break;
				} else if (iType.isDoor() && findItem->getDoor()) {
					item = findItem;
					break;
				} else if (iType.isBed() && findItem->getBed()) {
					item = findItem;
					break;
				}
			}
		}

		if (item) {
			if (item->unserializeAttr(propStream)) {
				Container* container = item->getContainer();
				if (container && !loadContainer(propStream, container)) {
					return false;
				}

				g_game.transformItem(item, id);
			} else {
				std::cout << "WARNING: Unserialization error in IOMapSerialize::loadItem()" << id << std::endl;
			}
		} else {
			// The map changed since the last save, just read the attributes
			std::unique_ptr<Item> dummy(Item::CreateItem(id));
			if (dummy) {
				dummy->unserializeAttr(propStream);
				Container* container = dummy->getContainer();
				if (container) {
					if (!loadContainer(propStream, container)) {
						return false;
					}
				} else if (BedItem* bedItem = dynamic_cast<BedItem*>(dummy.get())) {
					uint32_t sleeperGUID = bedItem->getSleeper();
					if (sleeperGUID != 0) {
						g_game.removeBedSleeper(sleeperGUID);
					}
				}
			}
		}
	}
	return true;
}

void IOMapSerialize::saveItem(PropWriteStream& stream, const Item* item)
{
	const Container* container = item->getContainer();

	// Write ID & props
	stream.write<uint16_t>(item->getID());
	item->serializeAttr(stream);

	if (container) {
		// Hack our way into the attributes
		stream.write<uint8_t>(ATTR_CONTAINER_ITEMS);
		stream.write<uint32_t>(container->size());
		for (auto it = container->getReversedItems(), end = container->getReversedEnd(); it != end; ++it) {
			saveItem(stream, *it);
		}
	}

	stream.write<uint8_t>(0x00); // attr end
}

void IOMapSerialize::saveTile(PropWriteStream& stream, const Tile* tile)
{
	const TileItemVector* tileItems = tile->getItemList();
	if (!tileItems) {
		return;
	}

	std::forward_list<Item*> items;
	uint16_t count = 0;
	for (Item* item : *tileItems) {
		const ItemType& it = Item::items[item->getID()];

		// Note that these are NEGATED, ie. these are the items that will be saved.
		if (!(it.moveable || it.forceSerialize || item->getDoor() ||
		      (item->getContainer() && !item->getContainer()->empty()) || it.canWriteText || item->getBed())) {
			continue;
		}

		items.push_front(item);
		++count;
	}

	if (!items.empty()) {
		const Position& tilePosition = tile->getPosition();
		stream.write<uint16_t>(tilePosition.x);
		stream.write<uint16_t>(tilePosition.y);
		stream.write<uint8_t>(tilePosition.z);

		stream.write<uint32_t>(count);
		for (const Item* item : items) {
			saveItem(stream, item);
		}
	}
}

bool IOMapSerialize::loadHouseInfo()
{
	auto result = tfs::db::store_query("SELECT `id`, `owner`, `paid`, `warnings` FROM `houses`");
	if (!result) {
		return false;
	}

	do {
		if (auto house = g_game.map.houses.getHouse(result->getNumber<uint32_t>("id"))) {
			house->setOwner(result->getNumber<uint32_t>("owner"), false);
			house->setPaidUntil(result->getNumber<time_t>("paid"));
			house->setPayRentWarnings(result->getNumber<uint32_t>("warnings"));
		}
	} while (result->next());

	if ((result = tfs::db::store_query("SELECT `house_id`, `listid`, `list` FROM `house_lists`"))) {
		do {
			if (auto house = g_game.map.houses.getHouse(result->getNumber<uint32_t>("house_id"))) {
				house->setAccessList(result->getNumber<uint32_t>("listid"), result->getString("list"));
			}
		} while (result->next());
	}
	return true;
}

bool IOMapSerialize::saveHouseInfo()
{
	DBTransaction transaction;
	if (!transaction.begin()) {
		return false;
	}

	if (!tfs::db::execute_query("DELETE FROM `house_lists`")) {
		return false;
	}

	for (const auto& it : g_game.map.houses.getHouses()) {
		House* house = it.second;
		if (auto result =
		        tfs::db::store_query(fmt::format("SELECT `id` FROM `houses` WHERE `id` = {:d}", house->getId()))) {
			tfs::db::execute_query(fmt::format(
			    "UPDATE `houses` SET `owner` = {:d}, `paid` = {:d}, `warnings` = {:d}, `name` = {:s}, `town_id` = {:d}, `rent` = {:d}, `size` = {:d}, `beds` = {:d} WHERE `id` = {:d}",
			    house->getOwner(), house->getPaidUntil(), house->getPayRentWarnings(),
			    tfs::db::escape_string(house->getName()), house->getTownId(), house->getRent(),
			    house->getTiles().size(), house->getBedCount(), house->getId()));
		} else {
			tfs::db::execute_query(fmt::format(
			    "INSERT INTO `houses` (`id`, `owner`, `paid`, `warnings`, `name`, `town_id`, `rent`, `size`, `beds`) VALUES ({:d}, {:d}, {:d}, {:d}, {:s}, {:d}, {:d}, {:d}, {:d})",
			    house->getId(), house->getOwner(), house->getPaidUntil(), house->getPayRentWarnings(),
			    tfs::db::escape_string(house->getName()), house->getTownId(), house->getRent(),
			    house->getTiles().size(), house->getBedCount()));
		}
	}

	DBInsert stmt("INSERT INTO `house_lists` (`house_id` , `listid` , `list`) VALUES ");

	for (const auto& it : g_game.map.houses.getHouses()) {
		House* house = it.second;

		std::string listText;
		if (house->getAccessList(GUEST_LIST, listText) && !listText.empty()) {
			if (!stmt.addRow(fmt::format("{:d}, {:d}, {:s}", house->getId(), tfs::to_underlying(GUEST_LIST),
			                             tfs::db::escape_string(listText)))) {
				return false;
			}

			listText.clear();
		}

		if (house->getAccessList(SUBOWNER_LIST, listText) && !listText.empty()) {
			if (!stmt.addRow(fmt::format("{:d}, {:d}, {:s}", house->getId(), tfs::to_underlying(SUBOWNER_LIST),
			                             tfs::db::escape_string(listText)))) {
				return false;
			}

			listText.clear();
		}

		for (auto door : house->getDoors()) {
			if (door->getAccessList(listText) && !listText.empty()) {
				if (!stmt.addRow(fmt::format("{:d}, {:d}, {:s}", house->getId(), door->getDoorId(),
				                             tfs::db::escape_string(listText)))) {
					return false;
				}

				listText.clear();
			}
		}
	}

	if (!stmt.execute()) {
		return false;
	}

	return transaction.commit();
}

bool IOMapSerialize::saveHouse(House* house)
{
	// Start the transaction
	DBTransaction transaction;
	if (!transaction.begin()) {
		return false;
	}

	uint32_t houseId = house->getId();

	// clear old tile data
	if (!tfs::db::execute_query(fmt::format("DELETE FROM `tile_store` WHERE `house_id` = {:d}", houseId))) {
		return false;
	}

	DBInsert stmt("INSERT INTO `tile_store` (`house_id`, `data`) VALUES ");

	PropWriteStream stream;
	for (HouseTile* tile : house->getTiles()) {
		saveTile(stream, tile);

		if (auto attributes = stream.getStream(); attributes.size() > 0) {
			if (!stmt.addRow(fmt::format("{:d}, {:s}", houseId, tfs::db::escape_string(attributes)))) {
				return false;
			}
			stream.clear();
		}
	}

	if (!stmt.execute()) {
		return false;
	}

	// End the transaction
	return transaction.commit();
}
