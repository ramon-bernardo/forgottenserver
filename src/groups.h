// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#ifndef FS_GROUPS_H
#define FS_GROUPS_H

struct Group
{
	uint16_t id;
	std::string name;
	bool access;
	uint32_t maxDepotItems;
	uint32_t maxVipEntries;
	uint64_t flags;
};

namespace Groups {
bool load();
std::shared_ptr<Group> getGroup(uint16_t id);
}; // namespace Groups

#endif // FS_GROUPS_H
