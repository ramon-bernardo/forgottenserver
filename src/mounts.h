// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#ifndef FS_MOUNTS_H
#define FS_MOUNTS_H

#include "otpch.h"

struct Mount
{
	Mount(uint16_t id, uint16_t client_id, std::string name, int32_t speed, bool premium) :
	    id(id), client_id(client_id), name(std::move(name)), speed(speed), premium(premium)
	{}

	uint16_t id;
	uint16_t client_id;
	std::string name;
	int32_t speed;
	bool premium;
};

using Mount_ptr = std::shared_ptr<Mount>;

namespace tfs::game::mounts {

bool reload();
bool load_from_xml();
Mount_ptr get_mount_by_id(uint16_t id);
Mount_ptr get_mount_by_name(std::string_view name);
Mount_ptr get_mount_by_client_id(uint16_t client_id);
std::set<Mount_ptr>& get_mounts();

} // namespace tfs::game::mounts

#endif // FS_MOUNTS_H
