// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "mounts.h"

#include "pugicast.h"
#include "tools.h"

namespace {

std::set<Mount_ptr> loaded_mounts;

}

bool tfs::game::mounts::reload()
{
	loaded_mounts.clear();
	return tfs::game::mounts::load_from_xml();
}

bool tfs::game::mounts::load_from_xml()
{
	pugi::xml_document doc;
	pugi::xml_parse_result result = doc.load_file("data/XML/mounts.xml");
	if (!result) {
		printXMLError("Error - tfs::game::mount::load_from_xml", "data/XML/mounts.xml", result);
		return false;
	}

	for (auto& node : doc.child("mounts").children()) {
		auto node_id = pugi::cast<uint32_t>(node.attribute("id").value());
		if (node_id == 0 || node_id > std::numeric_limits<uint16_t>::max()) {
			std::cout << "[Notice - tfs::game::mounts::load_from_xml] Mount id \"" << node_id
			          << "\" is not within 1 and 65535 range" << std::endl;
			continue;
		}

		if (tfs::game::mounts::get_mount_by_id(node_id)) {
			std::cout << "[Notice - tfs::game::mounts::load_from_xml] Duplicate mount with id: " << node_id
			          << std::endl;
			continue;
		}

		auto mount = std::make_shared<Mount>(
		    static_cast<uint16_t>(node_id), pugi::cast<uint16_t>(node.attribute("clientid").value()),
		    node.attribute("name").as_string(), pugi::cast<int32_t>(node.attribute("speed").value()),
		    node.attribute("premium").as_bool());

		loaded_mounts.insert(mount);
	}

	return true;
}

Mount_ptr tfs::game::mounts::get_mount_by_id(uint16_t id)
{
	auto it = std::find_if(loaded_mounts.begin(), loaded_mounts.end(),
	                       [id](const Mount_ptr& mount) { return mount->id == id; });
	return (it != loaded_mounts.end()) ? *it : nullptr;
}

Mount_ptr tfs::game::mounts::get_mount_by_name(std::string_view name)
{
	for (const auto& mount : loaded_mounts) {
		if (caseInsensitiveEqual(name, mount->name)) {
			return mount;
		}
	}
	return nullptr;
}

Mount_ptr tfs::game::mounts::get_mount_by_client_id(uint16_t clientId)
{
	auto it = std::find_if(loaded_mounts.begin(), loaded_mounts.end(),
	                       [clientId](const Mount_ptr& mount) { return mount->client_id == clientId; });
	return (it != loaded_mounts.end()) ? *it : nullptr;
}

std::set<Mount_ptr>& tfs::game::mounts::get_mounts() { return loaded_mounts; }
