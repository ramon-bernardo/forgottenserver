// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include "luascript.h"

#include "bed.h"
#include "chat.h"
#include "configmanager.h"
#include "databasemanager.h"
#include "databasetasks.h"
#include "depotchest.h"
#include "events.h"
#include "game.h"
#include "globalevent.h"
#include "housetile.h"
#include "inbox.h"
#include "iologindata.h"
#include "iomapserialize.h"
#include "iomarket.h"
#include "luavariant.h"
#include "matrixarea.h"
#include "monster.h"
#include "movement.h"
#include "npc.h"
#include "outfit.h"
#include "party.h"
#include "player.h"
#include "podium.h"
#include "protocolstatus.h"
#include "scheduler.h"
#include "script.h"
#include "spells.h"
#include "storeinbox.h"
#include "teleport.h"
#include "weapons.h"

#include <ranges>

extern Chat* g_chat;
extern Game g_game;
extern GlobalEvents* g_globalEvents;
extern Monsters g_monsters;
extern Vocations g_vocations;
extern Spells* g_spells;
extern Events* g_events;
extern Actions* g_actions;
extern TalkActions* g_talkActions;
extern CreatureEvents* g_creatureEvents;
extern MoveEvents* g_moveEvents;
extern GlobalEvents* g_globalEvents;
extern Scripts* g_scripts;
extern Weapons* g_weapons;

LuaEnvironment g_luaEnvironment;

namespace {

constexpr int32_t EVENT_ID_LOADING = 1;

enum LuaDataType
{
	LuaData_Unknown,

	LuaData_Item,
	LuaData_Container,
	LuaData_Teleport,
	LuaData_Podium,
	LuaData_Player,
	LuaData_Monster,
	LuaData_Npc,
	LuaData_Tile,
};

// temporary item list
std::multimap<ScriptEnvironment*, Item*> tempItems = {};

// result map
uint32_t lastResultId = 0;
std::map<uint32_t, DBResult_ptr> tempResults = {};

bool isNumber(lua_State* L, int32_t arg) { return lua_type(L, arg) == LUA_TNUMBER; }

void setField(lua_State* L, const char* index, lua_Number value)
{
	context.push_number(value);
	lua_setfield(L, -2, index);
}

void setField(lua_State* L, const char* index, std::string_view value)
{
	tfs::lua::pushString(L, value);
	lua_setfield(L, -2, index);
}

void setField(lua_State* L, std::string_view index, std::string_view value)
{
	tfs::lua::pushString(L, value);
	lua_setfield(L, -2, index.data());
}

void registerClass(lua_State* L, std::string_view className, std::string_view baseClass,
                   lua_CFunction newFunction = nullptr)
{
	// className = {}
	lua_newtable(L);
	lua_pushvalue(L, -1);
	lua_setglobal(L, className.data());
	int methods = lua_gettop(L);

	// methodsTable = {}
	lua_newtable(L);
	int methodsTable = lua_gettop(L);

	if (newFunction) {
		// className.__call = newFunction
		lua_pushcfunction(L, newFunction);
		lua_setfield(L, methodsTable, "__call");
	}

	uint32_t parents = 0;
	if (!baseClass.empty()) {
		lua_getglobal(L, baseClass.data());
		context.raw_geti(-1, 'p');
		parents = context.get_number<uint32_t>(-1) + 1;
		lua_pop(L, 1);
		lua_setfield(L, methodsTable, "__index");
	}

	// setmetatable(className, methodsTable)
	lua_setmetatable(L, methods);

	// className.metatable = {}
	luaL_newmetatable(L, className.data());
	int metatable = lua_gettop(L);

	// className.metatable.__metatable = className
	lua_pushvalue(L, methods);
	lua_setfield(L, metatable, "__metatable");

	// className.metatable.__index = className
	lua_pushvalue(L, methods);
	lua_setfield(L, metatable, "__index");

	// className.metatable['h'] = hash
	context.push_number(std::hash<std::string_view>()(className));
	lua_rawseti(L, metatable, 'h');

	// className.metatable['p'] = parents
	context.push_number(parents);
	lua_rawseti(L, metatable, 'p');

	// className.metatable['t'] = type
	if (className == "Item") {
		context.push_number(LuaData_Item);
	} else if (className == "Container") {
		context.push_number(LuaData_Container);
	} else if (className == "Teleport") {
		context.push_number(LuaData_Teleport);
	} else if (className == "Podium") {
		context.push_number(LuaData_Podium);
	} else if (className == "Player") {
		context.push_number(LuaData_Player);
	} else if (className == "Monster") {
		context.push_number(LuaData_Monster);
	} else if (className == "Npc") {
		context.push_number(LuaData_Npc);
	} else if (className == "Tile") {
		context.push_number(LuaData_Tile);
	} else {
		context.push_number(LuaData_Unknown);
	}
	lua_rawseti(L, metatable, 't');

	// pop className, className.metatable
	lua_pop(L, 2);
}

void registerTable(lua_State* L, std::string_view tableName)
{
	// _G[tableName] = {}
	lua_newtable(L);
	lua_setglobal(L, tableName.data());
}

void registerMetaMethod(lua_State* L, std::string_view className, std::string_view methodName, lua_CFunction func)
{
	// className.metatable.methodName = func
	luaL_getmetatable(L, className.data());
	lua_pushcfunction(L, func);
	lua_setfield(L, -2, methodName.data());

	// pop className.metatable
	lua_pop(L, 1);
}

void registerGlobalMethod(lua_State* L, std::string_view functionName, lua_CFunction func)
{
	// _G[functionName] = func
	lua_pushcfunction(L, func);
	lua_setglobal(L, functionName.data());
}

void registerVariable(lua_State* L, std::string_view tableName, std::string_view name, lua_Number value)
{
	// tableName.name = value
	lua_getglobal(L, tableName.data());
	setField(L, name.data(), value);

	// pop tableName
	lua_pop(L, 1);
}

void registerGlobalVariable(lua_State* L, std::string_view name, lua_Number value)
{
	// _G[name] = value
	context.push_number(value);
	lua_setglobal(L, name.data());
}

void registerGlobalBoolean(lua_State* L, std::string_view name, bool value)
{
	// _G[name] = value
	context.push_boolean(value);
	lua_setglobal(L, name.data());
}

std::string getStackTrace(lua_State* L, std::string_view error_desc)
{
	luaL_traceback(L, L, error_desc.data(), 1);
	return tfs::lua::popString(L);
}

int luaErrorHandler()
{
	std::string errorMessage = tfs::lua::popString(L);
	tfs::lua::pushString(L, getStackTrace(L, errorMessage));
	return 1;
}

bool getArea(lua_State* L, std::vector<uint32_t>& vec, uint32_t& rows)
{
	context.push_nil();
	for (rows = 0; lua_next(L, -2) != 0; ++rows) {
		if (!lua_istable(L, -1)) {
			return false;
		}

		context.push_nil();
		while (lua_next(L, -2) != 0) {
			if (!isNumber(L, -1)) {
				return false;
			}
			vec.push_back(context.get_number<uint32_t>(-1));
			lua_pop(L, 1);
		}

		lua_pop(L, 1);
	}

	lua_pop(L, 1);
	return (rows != 0);
}

template <class T>
std::shared_ptr<T>& getSharedPtr(lua_State* L, int32_t arg)
{
	return *static_cast<std::shared_ptr<T>*>(lua_touserdata(L, arg));
}
template <class T>
void pushSharedPtr(lua_State* L, T value)
{
	new (lua_newuserdata(L, sizeof(T))) T(std::move(value));
}

} // namespace

ScriptEnvironment::ScriptEnvironment() { resetEnv(); }

ScriptEnvironment::~ScriptEnvironment() { resetEnv(); }

void ScriptEnvironment::resetEnv()
{
	scriptId = 0;
	callbackId = 0;
	timerEvent = false;
	interface = nullptr;
	localMap.clear();
	tempResults.clear();

	auto pair = tempItems.equal_range(this);
	auto it = pair.first;
	while (it != pair.second) {
		Item* item = it->second;
		if (item && item->getParent() == VirtualCylinder::virtualCylinder) {
			g_game.ReleaseItem(item);
		}
		it = tempItems.erase(it);
	}
}

bool ScriptEnvironment::setCallbackId(int32_t callbackId, LuaScriptInterface* scriptInterface)
{
	if (this->callbackId != 0) {
		// nested callbacks are not allowed
		if (interface) {
			reportErrorFunc(interface->getLuaState(), "Nested callbacks!");
		}
		return false;
	}

	this->callbackId = callbackId;
	interface = scriptInterface;
	return true;
}

uint32_t ScriptEnvironment::addThing(Thing* thing)
{
	if (!thing || thing->isRemoved()) {
		return 0;
	}

	Creature* creature = thing->getCreature();
	if (creature) {
		return creature->getID();
	}

	Item* item = thing->getItem();
	if (item && item->hasAttribute(ITEM_ATTRIBUTE_UNIQUEID)) {
		return item->getUniqueId();
	}

	for (const auto& it : localMap) {
		if (it.second == item) {
			return it.first;
		}
	}

	localMap[++lastUID] = item;
	return lastUID;
}

void ScriptEnvironment::insertItem(uint32_t uid, Item* item)
{
	auto result = localMap.emplace(uid, item);
	if (!result.second) {
		std::cout << "\nLua Script Error: Thing uid already taken.";
	}
}

Thing* ScriptEnvironment::getThingByUID(uint32_t uid)
{
	if (uid >= CREATURE_ID_MIN) {
		return g_game.getCreatureByID(uid);
	}

	if (uid <= std::numeric_limits<uint16_t>::max()) {
		Item* item = g_game.getUniqueItem(uid);
		if (item && !item->isRemoved()) {
			return item;
		}
		return nullptr;
	}

	auto it = localMap.find(uid);
	if (it != localMap.end()) {
		Item* item = it->second;
		if (!item->isRemoved()) {
			return item;
		}
	}
	return nullptr;
}

Item* ScriptEnvironment::getItemByUID(uint32_t uid)
{
	Thing* thing = getThingByUID(uid);
	if (!thing) {
		return nullptr;
	}
	return thing->getItem();
}

Container* ScriptEnvironment::getContainerByUID(uint32_t uid)
{
	Item* item = getItemByUID(uid);
	if (!item) {
		return nullptr;
	}
	return item->getContainer();
}

void ScriptEnvironment::removeItemByUID(uint32_t uid)
{
	if (uid <= std::numeric_limits<uint16_t>::max()) {
		g_game.removeUniqueItem(uid);
		return;
	}

	auto it = localMap.find(uid);
	if (it != localMap.end()) {
		localMap.erase(it);
	}
}

static void addTempItem(Item* item) { tempItems.emplace(tfs::lua::getScriptEnv(), item); }

void tfs::lua::removeTempItem(Item* item)
{
	std::erase_if(tempItems, [item](const auto& pair) { return pair.second == item; });
}

static uint32_t addResult(DBResult_ptr res)
{
	tempResults[++lastResultId] = std::move(res);
	return lastResultId;
}

static bool removeResult(uint32_t id)
{
	auto it = tempResults.find(id);
	if (it == tempResults.end()) {
		return false;
	}

	tempResults.erase(it);
	return true;
}

static DBResult_ptr getResultByID(uint32_t id)
{
	auto it = tempResults.find(id);
	if (it == tempResults.end()) {
		return nullptr;
	}
	return it->second;
}

std::string tfs::lua::getErrorDesc(ErrorCode_t code)
{
	switch (code) {
		case LUA_ERROR_PLAYER_NOT_FOUND:
			return "Player not found";
		case LUA_ERROR_CREATURE_NOT_FOUND:
			return "Creature not found";
		case LUA_ERROR_ITEM_NOT_FOUND:
			return "Item not found";
		case LUA_ERROR_THING_NOT_FOUND:
			return "Thing not found";
		case LUA_ERROR_TILE_NOT_FOUND:
			return "Tile not found";
		case LUA_ERROR_HOUSE_NOT_FOUND:
			return "House not found";
		case LUA_ERROR_COMBAT_NOT_FOUND:
			return "Combat not found";
		case LUA_ERROR_CONDITION_NOT_FOUND:
			return "Condition not found";
		case LUA_ERROR_AREA_NOT_FOUND:
			return "Area not found";
		case LUA_ERROR_CONTAINER_NOT_FOUND:
			return "Container not found";
		case LUA_ERROR_VARIANT_NOT_FOUND:
			return "Variant not found";
		case LUA_ERROR_VARIANT_UNKNOWN:
			return "Unknown variant type";
		case LUA_ERROR_SPELL_NOT_FOUND:
			return "Spell not found";
		default:
			return "Bad error code";
	}
}

static std::array<ScriptEnvironment, 16> scriptEnv = {};
static int32_t scriptEnvIndex = -1;

LuaScriptInterface::LuaScriptInterface(std::string interfaceName) : interfaceName(std::move(interfaceName))
{
	if (!g_luaEnvironment.getLuaState()) {
		g_luaEnvironment.initState();
	}
}

LuaScriptInterface::~LuaScriptInterface() { closeState(); }

bool LuaScriptInterface::reInitState()
{
	g_luaEnvironment.clearCombatObjects(this);
	g_luaEnvironment.clearAreaObjects(this);

	closeState();
	return initState();
}

/// Same as lua_pcall, but adds stack trace to error strings in called function.
int tfs::lua::protectedCall(lua_State* L, int nargs, int nresults)
{
	int error_index = lua_gettop(L) - nargs;
	lua_pushcfunction(L, luaErrorHandler);
	lua_insert(L, error_index);

	int ret = lua_pcall(L, nargs, nresults, error_index);
	lua_remove(L, error_index);
	return ret;
}

int32_t LuaScriptInterface::loadFile(const std::string& file, Npc* npc /* = nullptr*/)
{
	// loads file as a chunk at stack top
	int ret = luaL_loadfile(L, file.data());
	if (ret != 0) {
		lastLuaError = tfs::lua::popString(L);
		return -1;
	}

	// check that it is loaded as a function
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 1);
		return -1;
	}

	loadingFile = file;

	if (!tfs::lua::reserveScriptEnv()) {
		lua_pop(L, 1);
		return -1;
	}

	ScriptEnvironment* env = tfs::lua::getScriptEnv();
	env->setScriptId(EVENT_ID_LOADING, this);
	env->setNpc(npc);

	// execute it
	ret = tfs::lua::protectedCall(L, 0, 0);
	if (ret != 0) {
		reportErrorFunc(nullptr, tfs::lua::popString(L));
		tfs::lua::resetScriptEnv();
		return -1;
	}

	tfs::lua::resetScriptEnv();
	return 0;
}

int32_t LuaScriptInterface::getEvent(std::string_view eventName)
{
	// get our events table
	context.raw_geti(LUA_REGISTRYINDEX, eventTableRef);
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return -1;
	}

	// get current event function pointer
	lua_getglobal(L, eventName.data());
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 2);
		return -1;
	}

	// save in our events table
	lua_pushvalue(L, -1);
	lua_rawseti(L, -3, runningEventId);
	lua_pop(L, 2);

	// reset global value of this event
	context.push_nil();
	lua_setglobal(L, eventName.data());

	cacheFiles[runningEventId] = fmt::format("{:s}:{:s}", loadingFile, eventName);
	return runningEventId++;
}

int32_t LuaScriptInterface::getEvent()
{
	// check if function is on the stack
	if (!lua_isfunction(L, -1)) {
		return -1;
	}

	// get our events table
	context.raw_geti(LUA_REGISTRYINDEX, eventTableRef);
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return -1;
	}

	// save in our events table
	lua_pushvalue(L, -2);
	lua_rawseti(L, -2, runningEventId);
	lua_pop(L, 2);

	cacheFiles[runningEventId] = loadingFile + ":callback";
	return runningEventId++;
}

int32_t LuaScriptInterface::getMetaEvent(const std::string& globalName, const std::string& eventName)
{
	// get our events table
	context.raw_geti(LUA_REGISTRYINDEX, eventTableRef);
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return -1;
	}

	// get current event function pointer
	lua_getglobal(L, globalName.data());
	lua_getfield(L, -1, eventName.data());
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 3);
		return -1;
	}

	// save in our events table
	lua_pushvalue(L, -1);
	lua_rawseti(L, -4, runningEventId);
	lua_pop(L, 1);

	// reset global value of this event
	context.push_nil();
	lua_setfield(L, -2, eventName.data());
	lua_pop(L, 2);

	cacheFiles[runningEventId] = loadingFile + ":" + globalName + "@" + eventName;
	return runningEventId++;
}

void LuaScriptInterface::removeEvent(int32_t scriptId)
{
	if (scriptId == -1) {
		return;
	}

	// get our events table
	context.raw_geti(LUA_REGISTRYINDEX, eventTableRef);
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return;
	}

	// remove event from table
	context.push_nil();
	lua_rawseti(L, -2, scriptId);
	lua_pop(L, 1);

	cacheFiles.erase(scriptId);
}

const std::string& LuaScriptInterface::getFileById(int32_t scriptId)
{
	if (scriptId == EVENT_ID_LOADING) {
		return loadingFile;
	}

	auto it = cacheFiles.find(scriptId);
	if (it == cacheFiles.end()) {
		static const std::string& unk = "(Unknown scriptfile)";
		return unk;
	}
	return it->second;
}

void tfs::lua::reportError(std::string_view function, std::string_view error_desc, lua_State* L /*= nullptr*/,
                           bool stack_trace /*= false*/)
{
	auto [scriptId, scriptInterface, callbackId, timerEvent] = getScriptEnv()->getEventInfo();

	std::cout << "\nLua Script Error: ";

	if (scriptInterface) {
		std::cout << '[' << scriptInterface->getInterfaceName() << "]\n";

		if (timerEvent) {
			std::cout << "in a timer event called from:\n";
		}

		if (callbackId) {
			std::cout << "in callback: " << scriptInterface->getFileById(callbackId) << '\n';
		}

		std::cout << scriptInterface->getFileById(scriptId) << '\n';
	}

	if (!function.empty()) {
		std::cout << function << "(). ";
	}

	if (L && stack_trace) {
		std::cout << getStackTrace(L, error_desc) << '\n';
	} else {
		std::cout << error_desc << '\n';
	}
}

bool LuaScriptInterface::pushFunction(int32_t functionId)
{
	context.raw_geti(LUA_REGISTRYINDEX, eventTableRef);
	if (!lua_istable(L, -1)) {
		return false;
	}

	context.raw_geti(-1, functionId);
	lua_replace(L, -2);
	return lua_isfunction(L, -1);
}

bool LuaScriptInterface::initState()
{
	if (L = g_luaEnvironment.getLuaState(); !L) {
		return false;
	}

	lua_newtable(L);
	eventTableRef = luaL_ref(L, LUA_REGISTRYINDEX);
	runningEventId = EVENT_ID_USER;
	return true;
}

bool LuaScriptInterface::closeState()
{
	if (!g_luaEnvironment.getLuaState() || !L) {
		return false;
	}

	cacheFiles.clear();
	if (eventTableRef != -1) {
		luaL_unref(L, LUA_REGISTRYINDEX, eventTableRef);
		eventTableRef = -1;
	}

	L = nullptr;
	return true;
}

bool LuaScriptInterface::callFunction(int params)
{
	bool result = false;
	int size = lua_gettop(L);
	if (tfs::lua::protectedCall(L, params, 1) != 0) {
		reportErrorFunc(nullptr, tfs::lua::getString(L, -1));
	} else {
		result = tfs::lua::getBoolean(L, -1);
	}

	lua_pop(L, 1);
	if ((lua_gettop(L) + params + 1) != size) {
		reportErrorFunc(nullptr, "Stack size changed!");
	}

	tfs::lua::resetScriptEnv();
	return result;
}

void LuaScriptInterface::callVoidFunction(int params)
{
	int size = lua_gettop(L);
	if (tfs::lua::protectedCall(L, params, 0) != 0) {
		reportErrorFunc(nullptr, tfs::lua::popString(L));
	}

	if ((lua_gettop(L) + params + 1) != size) {
		reportErrorFunc(nullptr, "Stack size changed!");
	}

	tfs::lua::resetScriptEnv();
}

void tfs::lua::pushVariant(lua_State* L, const LuaVariant& var)
{
	lua_createtable(L, 0, 2);
	setField(L, "type", var.type());
	switch (var.type()) {
		case VARIANT_NUMBER:
			setField(L, "number", var.getNumber());
			break;
		case VARIANT_STRING:
			setField(L, "string", var.getString());
			break;
		case VARIANT_TARGETPOSITION:
			pushPosition(L, var.getTargetPosition());
			lua_setfield(L, -2, "pos");
			break;
		case VARIANT_POSITION: {
			pushPosition(L, var.getPosition());
			lua_setfield(L, -2, "pos");
			break;
		}
		default:
			break;
	}
	setMetatable(L, -1, "Variant");
}

void tfs::lua::pushThing(lua_State* L, Thing* thing)
{
	if (!thing) {
		lua_createtable(L, 0, 4);
		setField(L, "uid", 0);
		setField(L, "itemid", 0);
		setField(L, "actionid", 0);
		setField(L, "type", 0);
		return;
	}

	if (Item* item = thing->getItem()) {
		pushUserdata(L, item);
		setItemMetatable(L, -1, item);
	} else if (Creature* creature = thing->getCreature()) {
		pushUserdata(L, creature);
		setCreatureMetatable(L, -1, creature);
	} else {
		context.push_nil();
	}
}

void tfs::lua::pushCylinder(lua_State* L, Cylinder* cylinder)
{
	if (Creature* creature = cylinder->getCreature()) {
		pushUserdata(L, creature);
		setCreatureMetatable(L, -1, creature);
	} else if (Item* parentItem = cylinder->getItem()) {
		pushUserdata(L, parentItem);
		setItemMetatable(L, -1, parentItem);
	} else if (Tile* tile = cylinder->getTile()) {
		pushUserdata(L, tile);
		setMetatable(L, -1, "Tile");
	} else if (cylinder == VirtualCylinder::virtualCylinder) {
		pushBoolean(L, true);
	} else {
		context.push_nil();
	}
}

void tfs::lua::pushString(lua_State* L, std::string_view value) { lua_pushlstring(L, value.data(), value.size()); }
void tfs::lua::pushCallback(lua_State* L, int32_t callback) { context.raw_geti(LUA_REGISTRYINDEX, callback); }

std::string tfs::lua::popString()
{
	if (lua_gettop(L) == 0) {
		return std::string();
	}

	std::string str = getString(L, -1);
	lua_pop(L, 1);
	return str;
}

int32_t tfs::lua::popCallback() { return luaL_ref(L, LUA_REGISTRYINDEX); }

// Metatables
void tfs::lua::setMetatable(lua_State* L, int32_t index, std::string_view name)
{
	luaL_getmetatable(L, name.data());
	lua_setmetatable(L, index - 1);
}

static void setWeakMetatable(lua_State* L, int32_t index, const std::string& name)
{
	static std::set<std::string> weakObjectTypes;
	const std::string& weakName = name + "_weak";

	auto result = weakObjectTypes.emplace(name);
	if (result.second) {
		luaL_getmetatable(L, name.data());
		int childMetatable = lua_gettop(L);

		luaL_newmetatable(L, weakName.data());
		int metatable = lua_gettop(L);

		static const std::vector<std::string> methodKeys = {"__index", "__metatable", "__eq"};
		for (const std::string& metaKey : methodKeys) {
			lua_getfield(L, childMetatable, metaKey.data());
			lua_setfield(L, metatable, metaKey.data());
		}

		static const std::vector<int> methodIndexes = {'h', 'p', 't'};
		for (int metaIndex : methodIndexes) {
			context.raw_geti(childMetatable, metaIndex);
			lua_rawseti(L, metatable, metaIndex);
		}

		context.push_nil();
		lua_setfield(L, metatable, "__gc");

		lua_remove(L, childMetatable);
	} else {
		luaL_getmetatable(L, weakName.data());
	}
	lua_setmetatable(L, index - 1);
}

void tfs::lua::setItemMetatable(lua_State* L, int32_t index, const Item* item)
{
	if (item->getContainer()) {
		luaL_getmetatable(L, "Container");
	} else if (item->getTeleport()) {
		luaL_getmetatable(L, "Teleport");
	} else if (item->getPodium()) {
		luaL_getmetatable(L, "Podium");
	} else {
		luaL_getmetatable(L, "Item");
	}
	lua_setmetatable(L, index - 1);
}

void tfs::lua::setCreatureMetatable(lua_State* L, int32_t index, const Creature* creature)
{
	if (creature->getPlayer()) {
		luaL_getmetatable(L, "Player");
	} else if (creature->getMonster()) {
		luaL_getmetatable(L, "Monster");
	} else {
		luaL_getmetatable(L, "Npc");
	}
	lua_setmetatable(L, index - 1);
}

// Get
std::string tfs::lua::getString(lua_State* L, int32_t arg)
{
	size_t len;
	const char* data = lua_tolstring(L, arg, &len);
	if (!data || len == 0) {
		return {};
	}
	return {data, len};
}

Position tfs::lua::getPosition(lua_State* L, int32_t arg, int32_t& stackpos)
{
	Position position{
	    getField<uint16_t>(L, arg, "x"),
	    getField<uint16_t>(L, arg, "y"),
	    getField<uint8_t>(L, arg, "z"),
	};

	lua_getfield(L, arg, "stackpos");
	if (lua_isnil(L, -1) == 1) {
		stackpos = 0;
	} else {
		stackpos = getNumber<int32_t>(L, -1);
	}

	lua_pop(L, 4);
	return position;
}

Position tfs::lua::getPosition(lua_State* L, int32_t arg)
{
	Position position{
	    getField<uint16_t>(L, arg, "x"),
	    getField<uint16_t>(L, arg, "y"),
	    getField<uint8_t>(L, arg, "z"),
	};

	lua_pop(L, 3);
	return position;
}

static Outfit_t getOutfit(lua_State* L, int32_t arg)
{
	Outfit_t outfit{
	    .lookType = tfs::lua::getField<uint16_t>(L, arg, "lookType"),
	    .lookTypeEx = tfs::lua::getField<uint16_t>(L, arg, "lookTypeEx"),

	    .lookHead = tfs::lua::getField<uint8_t>(L, arg, "lookHead"),
	    .lookBody = tfs::lua::getField<uint8_t>(L, arg, "lookBody"),
	    .lookLegs = tfs::lua::getField<uint8_t>(L, arg, "lookLegs"),
	    .lookFeet = tfs::lua::getField<uint8_t>(L, arg, "lookFeet"),
	    .lookAddons = tfs::lua::getField<uint8_t>(L, arg, "lookAddons"),

	    .lookMount = tfs::lua::getField<uint16_t>(L, arg, "lookMount"),
	    .lookMountHead = tfs::lua::getField<uint8_t>(L, arg, "lookMountHead"),
	    .lookMountBody = tfs::lua::getField<uint8_t>(L, arg, "lookMountBody"),
	    .lookMountLegs = tfs::lua::getField<uint8_t>(L, arg, "lookMountLegs"),
	    .lookMountFeet = tfs::lua::getField<uint8_t>(L, arg, "lookMountFeet"),
	};

	lua_pop(L, 12);
	return outfit;
}

static Outfit getOutfitClass(lua_State* L, int32_t arg)
{
	Outfit outfit{
	    .name = tfs::lua::getFieldString(L, arg, "name"),
	    .lookType = tfs::lua::getField<uint16_t>(L, arg, "lookType"),
	    .premium = tfs::lua::getField<uint8_t>(L, arg, "premium") == 1,
	    .unlocked = tfs::lua::getField<uint8_t>(L, arg, "unlocked") == 1,
	};

	lua_pop(L, 4);
	return outfit;
}

static LuaVariant getVariant(lua_State* L, int32_t arg)
{
	LuaVariant var;
	switch (tfs::lua::getField<LuaVariantType_t>(L, arg, "type")) {
		case VARIANT_NUMBER: {
			var.setNumber(tfs::lua::getField<uint32_t>(L, arg, "number"));
			lua_pop(L, 2);
			break;
		}

		case VARIANT_STRING: {
			var.setString(tfs::lua::getFieldString(L, arg, "string"));
			lua_pop(L, 2);
			break;
		}

		case VARIANT_POSITION:
			lua_getfield(L, arg, "pos");
			var.setPosition(tfs::lua::getPosition(L, lua_gettop(L)));
			lua_pop(L, 2);
			break;

		case VARIANT_TARGETPOSITION: {
			lua_getfield(L, arg, "pos");
			var.setTargetPosition(tfs::lua::getPosition(L, lua_gettop(L)));
			lua_pop(L, 2);
			break;
		}

		default: {
			var = {};
			lua_pop(L, 1);
			break;
		}
	}
	return var;
}

Thing* tfs::lua::getThing(lua_State* L, int32_t arg)
{
	Thing* thing;
	if (lua_getmetatable(L, arg) != 0) {
		context.raw_geti(-1, 't');
		switch (getNumber<uint32_t>(L, -1)) {
			case LuaData_Item:
				thing = getUserdata<Item>(L, arg);
				break;
			case LuaData_Container:
				thing = getUserdata<Container>(L, arg);
				break;
			case LuaData_Teleport:
				thing = getUserdata<Teleport>(L, arg);
				break;
			case LuaData_Podium:
				thing = getUserdata<Podium>(L, arg);
				break;
			case LuaData_Player:
				thing = getUserdata<Player>(L, arg);
				break;
			case LuaData_Monster:
				thing = getUserdata<Monster>(L, arg);
				break;
			case LuaData_Npc:
				thing = getUserdata<Npc>(L, arg);
				break;
			default:
				thing = nullptr;
				break;
		}
		lua_pop(L, 2);
	} else {
		thing = getScriptEnv()->getThingByUID(getNumber<uint32_t>(L, arg));
	}
	return thing;
}

Creature* tfs::lua::getCreature(lua_State* L, int32_t arg)
{
	if (lua_isuserdata(L, arg)) {
		return getUserdata<Creature>(L, arg);
	}
	return g_game.getCreatureByID(getNumber<uint32_t>(L, arg));
}

Player* tfs::lua::getPlayer(lua_State* L, int32_t arg)
{
	if (lua_isuserdata(L, arg)) {
		return getUserdata<Player>(L, arg);
	}
	return g_game.getPlayerByID(getNumber<uint32_t>(L, arg));
}

std::string tfs::lua::getFieldString(lua_State* L, int32_t arg, const std::string_view key)
{
	lua_getfield(L, arg, key.data());
	return getString(L, -1);
}

static LuaDataType getUserdataType(lua_State* L, int32_t arg)
{
	if (lua_getmetatable(L, arg) == 0) {
		return LuaData_Unknown;
	}
	context.raw_geti(-1, 't');

	LuaDataType type = tfs::lua::getNumber<LuaDataType>(L, -1);
	lua_pop(L, 2);

	return type;
}

void tfs::lua::pushBoolean(lua_State* L, bool value) { lua_pushboolean(L, value ? 1 : 0); }

void tfs::lua::pushSpell(lua_State* L, const Spell& spell)
{
	lua_createtable(L, 0, 5);
	setField(L, "name", spell.getName());
	setField(L, "level", spell.getLevel());
	setField(L, "mlevel", spell.getMagicLevel());
	setField(L, "mana", spell.getMana());
	setField(L, "manapercent", spell.getManaPercent());
	setMetatable(L, -1, "Spell");
}

void tfs::lua::pushPosition(lua_State* L, const Position& position, int32_t stackpos /* = 0*/)
{
	lua_createtable(L, 0, 4);
	setField(L, "x", position.x);
	setField(L, "y", position.y);
	setField(L, "z", position.z);
	setField(L, "stackpos", stackpos);
	setMetatable(L, -1, "Position");
}

void tfs::lua::pushOutfit(lua_State* L, const Outfit_t& outfit)
{
	lua_createtable(L, 0, 12);
	setField(L, "lookType", outfit.lookType);
	setField(L, "lookTypeEx", outfit.lookTypeEx);
	setField(L, "lookHead", outfit.lookHead);
	setField(L, "lookBody", outfit.lookBody);
	setField(L, "lookLegs", outfit.lookLegs);
	setField(L, "lookFeet", outfit.lookFeet);
	setField(L, "lookAddons", outfit.lookAddons);
	setField(L, "lookMount", outfit.lookMount);
	setField(L, "lookMountHead", outfit.lookMountHead);
	setField(L, "lookMountBody", outfit.lookMountBody);
	setField(L, "lookMountLegs", outfit.lookMountLegs);
	setField(L, "lookMountFeet", outfit.lookMountFeet);
}

void tfs::lua::pushOutfit(lua_State* L, const Outfit* outfit)
{
	lua_createtable(L, 0, 4);
	setField(L, "lookType", outfit->lookType);
	setField(L, "name", outfit->name);
	setField(L, "premium", outfit->premium);
	setField(L, "unlocked", outfit->unlocked);
	setMetatable(L, -1, "Outfit");
}

static void pushLoot(lua_State* L, const std::vector<LootBlock>& lootList)
{
	lua_createtable(L, lootList.size(), 0);

	int index = 0;
	for (const auto& lootBlock : lootList) {
		lua_createtable(L, 0, 7);

		setField(L, "itemId", lootBlock.id);
		setField(L, "chance", lootBlock.chance);
		setField(L, "subType", lootBlock.subType);
		setField(L, "maxCount", lootBlock.countmax);
		setField(L, "actionId", lootBlock.actionId);
		setField(L, "text", lootBlock.text);

		pushLoot(L, lootBlock.childLoot);
		lua_setfield(L, -2, "childLoot");

		lua_rawseti(L, -2, ++index);
	}
}

#define registerEnum(L, value) \
	{ \
		std::string enumName = #value; \
		registerGlobalVariable(L, enumName.substr(enumName.find_last_of(':') + 1), value); \
	}
#define registerEnumIn(L, tableName, value) \
	{ \
		std::string enumName = #value; \
		registerVariable(L, tableName, enumName.substr(enumName.find_last_of(':') + 1), value); \
	}

void LuaScriptInterface::registerFunctions()
{
	using namespace tfs::lua;

	// doPlayerAddItem(uid, itemid, <optional: default: 1> count/subtype)
	// doPlayerAddItem(cid, itemid, <optional: default: 1> count, <optional: default: 1> canDropOnMap, <optional:
	// default: 1>subtype) Returns uid of the created item
	lua_register(L, "doPlayerAddItem", LuaScriptInterface::luaDoPlayerAddItem);

	// isValidUID(uid)
	lua_register(L, "isValidUID", LuaScriptInterface::luaIsValidUID);

	// isDepot(uid)
	lua_register(L, "isDepot", LuaScriptInterface::luaIsDepot);

	// isMovable(uid)
	lua_register(L, "isMovable", LuaScriptInterface::luaIsMoveable);

	// doAddContainerItem(uid, itemid, <optional> count/subtype)
	// lua_register(L, "doAddContainerItem", LuaScriptInterface::luaDoAddContainerItem);

	// getDepotId(uid)
	lua_register(L, "getDepotId", LuaScriptInterface::luaGetDepotId);

	// getWorldUpTime()
	lua_register(L, "getWorldUpTime", LuaScriptInterface::luaGetWorldUpTime);

	// getSubTypeName(subType)
	lua_register(L, "getSubTypeName", LuaScriptInterface::luaGetSubTypeName);

	// createCombatArea({area}, <optional> {extArea})
	lua_register(L, "createCombatArea", LuaScriptInterface::luaCreateCombatArea);

	// doAreaCombat(cid, type, pos, area, min, max, effect[, origin = ORIGIN_SPELL[, blockArmor = false[, blockShield =
	// false[, ignoreResistances = false]]]])
	lua_register(L, "doAreaCombat", LuaScriptInterface::luaDoAreaCombat);

	// doTargetCombat(cid, target, type, min, max, effect[, origin = ORIGIN_SPELL[, blockArmor = false[, blockShield =
	// false[, ignoreResistances = false]]]])
	lua_register(L, "doTargetCombat", LuaScriptInterface::luaDoTargetCombat);

	// doChallengeCreature(cid, target[, force = false])
	lua_register(L, "doChallengeCreature", LuaScriptInterface::luaDoChallengeCreature);

	// addEvent(callback, delay, ...)
	lua_register(L, "addEvent", LuaScriptInterface::luaAddEvent);

	// stopEvent(eventid)
	lua_register(L, "stopEvent", LuaScriptInterface::luaStopEvent);

	// saveServer()
	lua_register(L, "saveServer", LuaScriptInterface::luaSaveServer);

	// cleanMap()
	lua_register(L, "cleanMap", LuaScriptInterface::luaCleanMap);

	// debugPrint(text)
	lua_register(L, "debugPrint", LuaScriptInterface::luaDebugPrint);

	// isInWar(cid, target)
	lua_register(L, "isInWar", LuaScriptInterface::luaIsInWar);

	// getWaypointPosition(name)
	lua_register(L, "getWaypointPositionByName", LuaScriptInterface::luaGetWaypointPositionByName);

	// sendChannelMessage(channelId, type, message)
	lua_register(L, "sendChannelMessage", LuaScriptInterface::luaSendChannelMessage);

	// sendGuildChannelMessage(guildId, type, message)
	lua_register(L, "sendGuildChannelMessage", LuaScriptInterface::luaSendGuildChannelMessage);

	// isScriptsInterface()
	lua_register(L, "isScriptsInterface", LuaScriptInterface::luaIsScriptsInterface);

#ifndef LUAJIT_VERSION
	// bit operations for Lua, based on bitlib project release 24
	// bit.bnot, bit.band, bit.bor, bit.bxor, bit.lshift, bit.rshift
	luaL_register(L, "bit", LuaScriptInterface::luaBitReg);
	lua_pop(L, 1);
#endif

	// configManager table
	luaL_register(L, "configManager", LuaScriptInterface::luaConfigManagerTable);
	lua_pop(L, 1);

	// db table
	luaL_register(L, "db", LuaScriptInterface::luaDatabaseTable);
	lua_pop(L, 1);

	// result table
	luaL_register(L, "result", LuaScriptInterface::luaResultTable);
	lua_pop(L, 1);

	/* New functions */
	// registerClass(L, className, baseClass, newFunction)
	// registerTable(L, tableName)
	// registerMethod(L, className, functionName, function)
	// registerMetaMethod(L, className, functionName, function)
	// registerGlobalMethod(L, functionName, function)
	// registerVariable(L, tableName, name, value)
	// registerGlobalVariable(L, name, value)
	// registerEnum(L, value)
	// registerEnumIn(L, tableName, value)

	// Enums
	registerEnum(L, ACCOUNT_TYPE_NORMAL);
	registerEnum(L, ACCOUNT_TYPE_TUTOR);
	registerEnum(L, ACCOUNT_TYPE_SENIORTUTOR);
	registerEnum(L, ACCOUNT_TYPE_GAMEMASTER);
	registerEnum(L, ACCOUNT_TYPE_COMMUNITYMANAGER);
	registerEnum(L, ACCOUNT_TYPE_GOD);

	registerEnum(L, AMMO_NONE);
	registerEnum(L, AMMO_BOLT);
	registerEnum(L, AMMO_ARROW);
	registerEnum(L, AMMO_SPEAR);
	registerEnum(L, AMMO_THROWINGSTAR);
	registerEnum(L, AMMO_THROWINGKNIFE);
	registerEnum(L, AMMO_STONE);
	registerEnum(L, AMMO_SNOWBALL);

	registerEnum(L, CALLBACK_PARAM_LEVELMAGICVALUE);
	registerEnum(L, CALLBACK_PARAM_SKILLVALUE);
	registerEnum(L, CALLBACK_PARAM_TARGETTILE);
	registerEnum(L, CALLBACK_PARAM_TARGETCREATURE);

	registerEnum(L, COMBAT_FORMULA_UNDEFINED);
	registerEnum(L, COMBAT_FORMULA_LEVELMAGIC);
	registerEnum(L, COMBAT_FORMULA_SKILL);
	registerEnum(L, COMBAT_FORMULA_DAMAGE);

	registerEnum(L, DIRECTION_NORTH);
	registerEnum(L, DIRECTION_EAST);
	registerEnum(L, DIRECTION_SOUTH);
	registerEnum(L, DIRECTION_WEST);
	registerEnum(L, DIRECTION_SOUTHWEST);
	registerEnum(L, DIRECTION_SOUTHEAST);
	registerEnum(L, DIRECTION_NORTHWEST);
	registerEnum(L, DIRECTION_NORTHEAST);

	registerEnum(L, COMBAT_NONE);
	registerEnum(L, COMBAT_PHYSICALDAMAGE);
	registerEnum(L, COMBAT_ENERGYDAMAGE);
	registerEnum(L, COMBAT_EARTHDAMAGE);
	registerEnum(L, COMBAT_FIREDAMAGE);
	registerEnum(L, COMBAT_UNDEFINEDDAMAGE);
	registerEnum(L, COMBAT_LIFEDRAIN);
	registerEnum(L, COMBAT_MANADRAIN);
	registerEnum(L, COMBAT_HEALING);
	registerEnum(L, COMBAT_DROWNDAMAGE);
	registerEnum(L, COMBAT_ICEDAMAGE);
	registerEnum(L, COMBAT_HOLYDAMAGE);
	registerEnum(L, COMBAT_DEATHDAMAGE);

	registerEnum(L, COMBAT_PARAM_TYPE);
	registerEnum(L, COMBAT_PARAM_EFFECT);
	registerEnum(L, COMBAT_PARAM_DISTANCEEFFECT);
	registerEnum(L, COMBAT_PARAM_BLOCKSHIELD);
	registerEnum(L, COMBAT_PARAM_BLOCKARMOR);
	registerEnum(L, COMBAT_PARAM_TARGETCASTERORTOPMOST);
	registerEnum(L, COMBAT_PARAM_CREATEITEM);
	registerEnum(L, COMBAT_PARAM_AGGRESSIVE);
	registerEnum(L, COMBAT_PARAM_DISPEL);
	registerEnum(L, COMBAT_PARAM_USECHARGES);

	registerEnum(L, CONDITION_NONE);
	registerEnum(L, CONDITION_POISON);
	registerEnum(L, CONDITION_FIRE);
	registerEnum(L, CONDITION_ENERGY);
	registerEnum(L, CONDITION_BLEEDING);
	registerEnum(L, CONDITION_HASTE);
	registerEnum(L, CONDITION_PARALYZE);
	registerEnum(L, CONDITION_OUTFIT);
	registerEnum(L, CONDITION_INVISIBLE);
	registerEnum(L, CONDITION_LIGHT);
	registerEnum(L, CONDITION_MANASHIELD);
	registerEnum(L, CONDITION_MANASHIELD_BREAKABLE);
	registerEnum(L, CONDITION_INFIGHT);
	registerEnum(L, CONDITION_DRUNK);
	registerEnum(L, CONDITION_EXHAUST_WEAPON);
	registerEnum(L, CONDITION_REGENERATION);
	registerEnum(L, CONDITION_SOUL);
	registerEnum(L, CONDITION_DROWN);
	registerEnum(L, CONDITION_MUTED);
	registerEnum(L, CONDITION_CHANNELMUTEDTICKS);
	registerEnum(L, CONDITION_YELLTICKS);
	registerEnum(L, CONDITION_ATTRIBUTES);
	registerEnum(L, CONDITION_FREEZING);
	registerEnum(L, CONDITION_DAZZLED);
	registerEnum(L, CONDITION_CURSED);
	registerEnum(L, CONDITION_EXHAUST_COMBAT);
	registerEnum(L, CONDITION_EXHAUST_HEAL);
	registerEnum(L, CONDITION_PACIFIED);
	registerEnum(L, CONDITION_SPELLCOOLDOWN);
	registerEnum(L, CONDITION_SPELLGROUPCOOLDOWN);
	registerEnum(L, CONDITION_ROOT);

	registerEnum(L, CONDITIONID_DEFAULT);
	registerEnum(L, CONDITIONID_COMBAT);
	registerEnum(L, CONDITIONID_HEAD);
	registerEnum(L, CONDITIONID_NECKLACE);
	registerEnum(L, CONDITIONID_BACKPACK);
	registerEnum(L, CONDITIONID_ARMOR);
	registerEnum(L, CONDITIONID_RIGHT);
	registerEnum(L, CONDITIONID_LEFT);
	registerEnum(L, CONDITIONID_LEGS);
	registerEnum(L, CONDITIONID_FEET);
	registerEnum(L, CONDITIONID_RING);
	registerEnum(L, CONDITIONID_AMMO);

	registerEnum(L, CONDITION_PARAM_OWNER);
	registerEnum(L, CONDITION_PARAM_TICKS);
	registerEnum(L, CONDITION_PARAM_DRUNKENNESS);
	registerEnum(L, CONDITION_PARAM_HEALTHGAIN);
	registerEnum(L, CONDITION_PARAM_HEALTHTICKS);
	registerEnum(L, CONDITION_PARAM_MANAGAIN);
	registerEnum(L, CONDITION_PARAM_MANATICKS);
	registerEnum(L, CONDITION_PARAM_DELAYED);
	registerEnum(L, CONDITION_PARAM_SPEED);
	registerEnum(L, CONDITION_PARAM_LIGHT_LEVEL);
	registerEnum(L, CONDITION_PARAM_LIGHT_COLOR);
	registerEnum(L, CONDITION_PARAM_SOULGAIN);
	registerEnum(L, CONDITION_PARAM_SOULTICKS);
	registerEnum(L, CONDITION_PARAM_MINVALUE);
	registerEnum(L, CONDITION_PARAM_MAXVALUE);
	registerEnum(L, CONDITION_PARAM_STARTVALUE);
	registerEnum(L, CONDITION_PARAM_TICKINTERVAL);
	registerEnum(L, CONDITION_PARAM_FORCEUPDATE);
	registerEnum(L, CONDITION_PARAM_SKILL_MELEE);
	registerEnum(L, CONDITION_PARAM_SKILL_FIST);
	registerEnum(L, CONDITION_PARAM_SKILL_CLUB);
	registerEnum(L, CONDITION_PARAM_SKILL_SWORD);
	registerEnum(L, CONDITION_PARAM_SKILL_AXE);
	registerEnum(L, CONDITION_PARAM_SKILL_DISTANCE);
	registerEnum(L, CONDITION_PARAM_SKILL_SHIELD);
	registerEnum(L, CONDITION_PARAM_SKILL_FISHING);
	registerEnum(L, CONDITION_PARAM_STAT_MAXHITPOINTS);
	registerEnum(L, CONDITION_PARAM_STAT_MAXMANAPOINTS);
	registerEnum(L, CONDITION_PARAM_STAT_MAGICPOINTS);
	registerEnum(L, CONDITION_PARAM_STAT_MAXHITPOINTSPERCENT);
	registerEnum(L, CONDITION_PARAM_STAT_MAXMANAPOINTSPERCENT);
	registerEnum(L, CONDITION_PARAM_STAT_MAGICPOINTSPERCENT);
	registerEnum(L, CONDITION_PARAM_PERIODICDAMAGE);
	registerEnum(L, CONDITION_PARAM_SKILL_MELEEPERCENT);
	registerEnum(L, CONDITION_PARAM_SKILL_FISTPERCENT);
	registerEnum(L, CONDITION_PARAM_SKILL_CLUBPERCENT);
	registerEnum(L, CONDITION_PARAM_SKILL_SWORDPERCENT);
	registerEnum(L, CONDITION_PARAM_SKILL_AXEPERCENT);
	registerEnum(L, CONDITION_PARAM_SKILL_DISTANCEPERCENT);
	registerEnum(L, CONDITION_PARAM_SKILL_SHIELDPERCENT);
	registerEnum(L, CONDITION_PARAM_SKILL_FISHINGPERCENT);
	registerEnum(L, CONDITION_PARAM_BUFF_SPELL);
	registerEnum(L, CONDITION_PARAM_SUBID);
	registerEnum(L, CONDITION_PARAM_FIELD);
	registerEnum(L, CONDITION_PARAM_DISABLE_DEFENSE);
	registerEnum(L, CONDITION_PARAM_MANASHIELD_BREAKABLE);
	registerEnum(L, CONDITION_PARAM_SPECIALSKILL_CRITICALHITCHANCE);
	registerEnum(L, CONDITION_PARAM_SPECIALSKILL_CRITICALHITAMOUNT);
	registerEnum(L, CONDITION_PARAM_SPECIALSKILL_LIFELEECHCHANCE);
	registerEnum(L, CONDITION_PARAM_SPECIALSKILL_LIFELEECHAMOUNT);
	registerEnum(L, CONDITION_PARAM_SPECIALSKILL_MANALEECHCHANCE);
	registerEnum(L, CONDITION_PARAM_SPECIALSKILL_MANALEECHAMOUNT);
	registerEnum(L, CONDITION_PARAM_AGGRESSIVE);

	registerEnum(L, CONST_ME_NONE);
	registerEnum(L, CONST_ME_DRAWBLOOD);
	registerEnum(L, CONST_ME_LOSEENERGY);
	registerEnum(L, CONST_ME_POFF);
	registerEnum(L, CONST_ME_BLOCKHIT);
	registerEnum(L, CONST_ME_EXPLOSIONAREA);
	registerEnum(L, CONST_ME_EXPLOSIONHIT);
	registerEnum(L, CONST_ME_FIREAREA);
	registerEnum(L, CONST_ME_YELLOW_RINGS);
	registerEnum(L, CONST_ME_GREEN_RINGS);
	registerEnum(L, CONST_ME_HITAREA);
	registerEnum(L, CONST_ME_TELEPORT);
	registerEnum(L, CONST_ME_ENERGYHIT);
	registerEnum(L, CONST_ME_MAGIC_BLUE);
	registerEnum(L, CONST_ME_MAGIC_RED);
	registerEnum(L, CONST_ME_MAGIC_GREEN);
	registerEnum(L, CONST_ME_HITBYFIRE);
	registerEnum(L, CONST_ME_HITBYPOISON);
	registerEnum(L, CONST_ME_MORTAREA);
	registerEnum(L, CONST_ME_SOUND_GREEN);
	registerEnum(L, CONST_ME_SOUND_RED);
	registerEnum(L, CONST_ME_POISONAREA);
	registerEnum(L, CONST_ME_SOUND_YELLOW);
	registerEnum(L, CONST_ME_SOUND_PURPLE);
	registerEnum(L, CONST_ME_SOUND_BLUE);
	registerEnum(L, CONST_ME_SOUND_WHITE);
	registerEnum(L, CONST_ME_BUBBLES);
	registerEnum(L, CONST_ME_CRAPS);
	registerEnum(L, CONST_ME_GIFT_WRAPS);
	registerEnum(L, CONST_ME_FIREWORK_YELLOW);
	registerEnum(L, CONST_ME_FIREWORK_RED);
	registerEnum(L, CONST_ME_FIREWORK_BLUE);
	registerEnum(L, CONST_ME_STUN);
	registerEnum(L, CONST_ME_SLEEP);
	registerEnum(L, CONST_ME_WATERCREATURE);
	registerEnum(L, CONST_ME_GROUNDSHAKER);
	registerEnum(L, CONST_ME_HEARTS);
	registerEnum(L, CONST_ME_FIREATTACK);
	registerEnum(L, CONST_ME_ENERGYAREA);
	registerEnum(L, CONST_ME_SMALLCLOUDS);
	registerEnum(L, CONST_ME_HOLYDAMAGE);
	registerEnum(L, CONST_ME_BIGCLOUDS);
	registerEnum(L, CONST_ME_ICEAREA);
	registerEnum(L, CONST_ME_ICETORNADO);
	registerEnum(L, CONST_ME_ICEATTACK);
	registerEnum(L, CONST_ME_STONES);
	registerEnum(L, CONST_ME_SMALLPLANTS);
	registerEnum(L, CONST_ME_CARNIPHILA);
	registerEnum(L, CONST_ME_PURPLEENERGY);
	registerEnum(L, CONST_ME_YELLOWENERGY);
	registerEnum(L, CONST_ME_HOLYAREA);
	registerEnum(L, CONST_ME_BIGPLANTS);
	registerEnum(L, CONST_ME_CAKE);
	registerEnum(L, CONST_ME_GIANTICE);
	registerEnum(L, CONST_ME_WATERSPLASH);
	registerEnum(L, CONST_ME_PLANTATTACK);
	registerEnum(L, CONST_ME_TUTORIALARROW);
	registerEnum(L, CONST_ME_TUTORIALSQUARE);
	registerEnum(L, CONST_ME_MIRRORHORIZONTAL);
	registerEnum(L, CONST_ME_MIRRORVERTICAL);
	registerEnum(L, CONST_ME_SKULLHORIZONTAL);
	registerEnum(L, CONST_ME_SKULLVERTICAL);
	registerEnum(L, CONST_ME_ASSASSIN);
	registerEnum(L, CONST_ME_STEPSHORIZONTAL);
	registerEnum(L, CONST_ME_BLOODYSTEPS);
	registerEnum(L, CONST_ME_STEPSVERTICAL);
	registerEnum(L, CONST_ME_YALAHARIGHOST);
	registerEnum(L, CONST_ME_BATS);
	registerEnum(L, CONST_ME_SMOKE);
	registerEnum(L, CONST_ME_INSECTS);
	registerEnum(L, CONST_ME_DRAGONHEAD);
	registerEnum(L, CONST_ME_ORCSHAMAN);
	registerEnum(L, CONST_ME_ORCSHAMAN_FIRE);
	registerEnum(L, CONST_ME_THUNDER);
	registerEnum(L, CONST_ME_FERUMBRAS);
	registerEnum(L, CONST_ME_CONFETTI_HORIZONTAL);
	registerEnum(L, CONST_ME_CONFETTI_VERTICAL);
	registerEnum(L, CONST_ME_BLACKSMOKE);
	registerEnum(L, CONST_ME_REDSMOKE);
	registerEnum(L, CONST_ME_YELLOWSMOKE);
	registerEnum(L, CONST_ME_GREENSMOKE);
	registerEnum(L, CONST_ME_PURPLESMOKE);
	registerEnum(L, CONST_ME_EARLY_THUNDER);
	registerEnum(L, CONST_ME_RAGIAZ_BONECAPSULE);
	registerEnum(L, CONST_ME_CRITICAL_DAMAGE);
	registerEnum(L, CONST_ME_PLUNGING_FISH);
	registerEnum(L, CONST_ME_BLUECHAIN);
	registerEnum(L, CONST_ME_ORANGECHAIN);
	registerEnum(L, CONST_ME_GREENCHAIN);
	registerEnum(L, CONST_ME_PURPLECHAIN);
	registerEnum(L, CONST_ME_GREYCHAIN);
	registerEnum(L, CONST_ME_YELLOWCHAIN);
	registerEnum(L, CONST_ME_YELLOWSPARKLES);
	registerEnum(L, CONST_ME_FAEEXPLOSION);
	registerEnum(L, CONST_ME_FAECOMING);
	registerEnum(L, CONST_ME_FAEGOING);
	registerEnum(L, CONST_ME_BIGCLOUDSSINGLESPACE);
	registerEnum(L, CONST_ME_STONESSINGLESPACE);
	registerEnum(L, CONST_ME_BLUEGHOST);
	registerEnum(L, CONST_ME_POINTOFINTEREST);
	registerEnum(L, CONST_ME_MAPEFFECT);
	registerEnum(L, CONST_ME_PINKSPARK);
	registerEnum(L, CONST_ME_FIREWORK_GREEN);
	registerEnum(L, CONST_ME_FIREWORK_ORANGE);
	registerEnum(L, CONST_ME_FIREWORK_PURPLE);
	registerEnum(L, CONST_ME_FIREWORK_TURQUOISE);
	registerEnum(L, CONST_ME_THECUBE);
	registerEnum(L, CONST_ME_DRAWINK);
	registerEnum(L, CONST_ME_PRISMATICSPARKLES);
	registerEnum(L, CONST_ME_THAIAN);
	registerEnum(L, CONST_ME_THAIANGHOST);
	registerEnum(L, CONST_ME_GHOSTSMOKE);
	registerEnum(L, CONST_ME_FLOATINGBLOCK);
	registerEnum(L, CONST_ME_BLOCK);
	registerEnum(L, CONST_ME_ROOTING);
	registerEnum(L, CONST_ME_GHOSTLYSCRATCH);
	registerEnum(L, CONST_ME_GHOSTLYBITE);
	registerEnum(L, CONST_ME_BIGSCRATCHING);
	registerEnum(L, CONST_ME_SLASH);
	registerEnum(L, CONST_ME_BITE);
	registerEnum(L, CONST_ME_CHIVALRIOUSCHALLENGE);
	registerEnum(L, CONST_ME_DIVINEDAZZLE);
	registerEnum(L, CONST_ME_ELECTRICALSPARK);
	registerEnum(L, CONST_ME_PURPLETELEPORT);
	registerEnum(L, CONST_ME_REDTELEPORT);
	registerEnum(L, CONST_ME_ORANGETELEPORT);
	registerEnum(L, CONST_ME_GREYTELEPORT);
	registerEnum(L, CONST_ME_LIGHTBLUETELEPORT);
	registerEnum(L, CONST_ME_FATAL);
	registerEnum(L, CONST_ME_DODGE);
	registerEnum(L, CONST_ME_HOURGLASS);
	registerEnum(L, CONST_ME_FIREWORKSSTAR);
	registerEnum(L, CONST_ME_FIREWORKSCIRCLE);
	registerEnum(L, CONST_ME_FERUMBRAS_1);
	registerEnum(L, CONST_ME_GAZHARAGOTH);
	registerEnum(L, CONST_ME_MAD_MAGE);
	registerEnum(L, CONST_ME_HORESTIS);
	registerEnum(L, CONST_ME_DEVOVORGA);
	registerEnum(L, CONST_ME_FERUMBRAS_2);
	registerEnum(L, CONST_ME_FOAM);

	registerEnum(L, CONST_ANI_NONE);
	registerEnum(L, CONST_ANI_SPEAR);
	registerEnum(L, CONST_ANI_BOLT);
	registerEnum(L, CONST_ANI_ARROW);
	registerEnum(L, CONST_ANI_FIRE);
	registerEnum(L, CONST_ANI_ENERGY);
	registerEnum(L, CONST_ANI_POISONARROW);
	registerEnum(L, CONST_ANI_BURSTARROW);
	registerEnum(L, CONST_ANI_THROWINGSTAR);
	registerEnum(L, CONST_ANI_THROWINGKNIFE);
	registerEnum(L, CONST_ANI_SMALLSTONE);
	registerEnum(L, CONST_ANI_DEATH);
	registerEnum(L, CONST_ANI_LARGEROCK);
	registerEnum(L, CONST_ANI_SNOWBALL);
	registerEnum(L, CONST_ANI_POWERBOLT);
	registerEnum(L, CONST_ANI_POISON);
	registerEnum(L, CONST_ANI_INFERNALBOLT);
	registerEnum(L, CONST_ANI_HUNTINGSPEAR);
	registerEnum(L, CONST_ANI_ENCHANTEDSPEAR);
	registerEnum(L, CONST_ANI_REDSTAR);
	registerEnum(L, CONST_ANI_GREENSTAR);
	registerEnum(L, CONST_ANI_ROYALSPEAR);
	registerEnum(L, CONST_ANI_SNIPERARROW);
	registerEnum(L, CONST_ANI_ONYXARROW);
	registerEnum(L, CONST_ANI_PIERCINGBOLT);
	registerEnum(L, CONST_ANI_WHIRLWINDSWORD);
	registerEnum(L, CONST_ANI_WHIRLWINDAXE);
	registerEnum(L, CONST_ANI_WHIRLWINDCLUB);
	registerEnum(L, CONST_ANI_ETHEREALSPEAR);
	registerEnum(L, CONST_ANI_ICE);
	registerEnum(L, CONST_ANI_EARTH);
	registerEnum(L, CONST_ANI_HOLY);
	registerEnum(L, CONST_ANI_SUDDENDEATH);
	registerEnum(L, CONST_ANI_FLASHARROW);
	registerEnum(L, CONST_ANI_FLAMMINGARROW);
	registerEnum(L, CONST_ANI_SHIVERARROW);
	registerEnum(L, CONST_ANI_ENERGYBALL);
	registerEnum(L, CONST_ANI_SMALLICE);
	registerEnum(L, CONST_ANI_SMALLHOLY);
	registerEnum(L, CONST_ANI_SMALLEARTH);
	registerEnum(L, CONST_ANI_EARTHARROW);
	registerEnum(L, CONST_ANI_EXPLOSION);
	registerEnum(L, CONST_ANI_CAKE);
	registerEnum(L, CONST_ANI_TARSALARROW);
	registerEnum(L, CONST_ANI_VORTEXBOLT);
	registerEnum(L, CONST_ANI_PRISMATICBOLT);
	registerEnum(L, CONST_ANI_CRYSTALLINEARROW);
	registerEnum(L, CONST_ANI_DRILLBOLT);
	registerEnum(L, CONST_ANI_ENVENOMEDARROW);
	registerEnum(L, CONST_ANI_GLOOTHSPEAR);
	registerEnum(L, CONST_ANI_SIMPLEARROW);
	registerEnum(L, CONST_ANI_LEAFSTAR);
	registerEnum(L, CONST_ANI_DIAMONDARROW);
	registerEnum(L, CONST_ANI_SPECTRALBOLT);
	registerEnum(L, CONST_ANI_ROYALSTAR);
	registerEnum(L, CONST_ANI_WEAPONTYPE);

	registerEnum(L, CONST_PROP_BLOCKSOLID);
	registerEnum(L, CONST_PROP_HASHEIGHT);
	registerEnum(L, CONST_PROP_BLOCKPROJECTILE);
	registerEnum(L, CONST_PROP_BLOCKPATH);
	registerEnum(L, CONST_PROP_ISVERTICAL);
	registerEnum(L, CONST_PROP_ISHORIZONTAL);
	registerEnum(L, CONST_PROP_MOVEABLE);
	registerEnum(L, CONST_PROP_IMMOVABLEBLOCKSOLID);
	registerEnum(L, CONST_PROP_IMMOVABLEBLOCKPATH);
	registerEnum(L, CONST_PROP_IMMOVABLENOFIELDBLOCKPATH);
	registerEnum(L, CONST_PROP_NOFIELDBLOCKPATH);
	registerEnum(L, CONST_PROP_SUPPORTHANGABLE);

	registerEnum(L, CONST_SLOT_HEAD);
	registerEnum(L, CONST_SLOT_NECKLACE);
	registerEnum(L, CONST_SLOT_BACKPACK);
	registerEnum(L, CONST_SLOT_ARMOR);
	registerEnum(L, CONST_SLOT_RIGHT);
	registerEnum(L, CONST_SLOT_LEFT);
	registerEnum(L, CONST_SLOT_LEGS);
	registerEnum(L, CONST_SLOT_FEET);
	registerEnum(L, CONST_SLOT_RING);
	registerEnum(L, CONST_SLOT_AMMO);

	registerEnum(L, CREATURE_EVENT_NONE);
	registerEnum(L, CREATURE_EVENT_LOGIN);
	registerEnum(L, CREATURE_EVENT_LOGOUT);
	registerEnum(L, CREATURE_EVENT_THINK);
	registerEnum(L, CREATURE_EVENT_PREPAREDEATH);
	registerEnum(L, CREATURE_EVENT_DEATH);
	registerEnum(L, CREATURE_EVENT_KILL);
	registerEnum(L, CREATURE_EVENT_ADVANCE);
	registerEnum(L, CREATURE_EVENT_MODALWINDOW);
	registerEnum(L, CREATURE_EVENT_TEXTEDIT);
	registerEnum(L, CREATURE_EVENT_HEALTHCHANGE);
	registerEnum(L, CREATURE_EVENT_MANACHANGE);
	registerEnum(L, CREATURE_EVENT_EXTENDED_OPCODE);

	registerEnum(L, CREATURE_ID_MIN);
	registerEnum(L, CREATURE_ID_MAX);

	registerEnum(L, GAME_STATE_STARTUP);
	registerEnum(L, GAME_STATE_INIT);
	registerEnum(L, GAME_STATE_NORMAL);
	registerEnum(L, GAME_STATE_CLOSED);
	registerEnum(L, GAME_STATE_SHUTDOWN);
	registerEnum(L, GAME_STATE_CLOSING);
	registerEnum(L, GAME_STATE_MAINTAIN);

	registerEnum(L, ITEM_STACK_SIZE);

	registerEnum(L, MESSAGE_STATUS_DEFAULT);
	registerEnum(L, MESSAGE_STATUS_WARNING);
	registerEnum(L, MESSAGE_EVENT_ADVANCE);
	registerEnum(L, MESSAGE_STATUS_WARNING2);
	registerEnum(L, MESSAGE_STATUS_SMALL);
	registerEnum(L, MESSAGE_INFO_DESCR);
	registerEnum(L, MESSAGE_DAMAGE_DEALT);
	registerEnum(L, MESSAGE_DAMAGE_RECEIVED);
	registerEnum(L, MESSAGE_HEALED);
	registerEnum(L, MESSAGE_EXPERIENCE);
	registerEnum(L, MESSAGE_DAMAGE_OTHERS);
	registerEnum(L, MESSAGE_HEALED_OTHERS);
	registerEnum(L, MESSAGE_EXPERIENCE_OTHERS);
	registerEnum(L, MESSAGE_EVENT_DEFAULT);
	registerEnum(L, MESSAGE_LOOT);
	registerEnum(L, MESSAGE_TRADE);
	registerEnum(L, MESSAGE_GUILD);
	registerEnum(L, MESSAGE_PARTY_MANAGEMENT);
	registerEnum(L, MESSAGE_PARTY);
	registerEnum(L, MESSAGE_REPORT);
	registerEnum(L, MESSAGE_HOTKEY_PRESSED);
	registerEnum(L, MESSAGE_MARKET);
	registerEnum(L, MESSAGE_BEYOND_LAST);
	registerEnum(L, MESSAGE_TOURNAMENT_INFO);
	registerEnum(L, MESSAGE_ATTENTION);
	registerEnum(L, MESSAGE_BOOSTED_CREATURE);
	registerEnum(L, MESSAGE_OFFLINE_TRAINING);
	registerEnum(L, MESSAGE_TRANSACTION);

	registerEnum(L, CREATURETYPE_PLAYER);
	registerEnum(L, CREATURETYPE_MONSTER);
	registerEnum(L, CREATURETYPE_NPC);
	registerEnum(L, CREATURETYPE_SUMMON_OWN);
	registerEnum(L, CREATURETYPE_SUMMON_OTHERS);

	registerEnum(L, CLIENTOS_LINUX);
	registerEnum(L, CLIENTOS_WINDOWS);
	registerEnum(L, CLIENTOS_FLASH);
	registerEnum(L, CLIENTOS_OTCLIENT_LINUX);
	registerEnum(L, CLIENTOS_OTCLIENT_WINDOWS);
	registerEnum(L, CLIENTOS_OTCLIENT_MAC);

	registerEnum(L, FIGHTMODE_ATTACK);
	registerEnum(L, FIGHTMODE_BALANCED);
	registerEnum(L, FIGHTMODE_DEFENSE);

	registerEnum(L, ITEM_ATTRIBUTE_NONE);
	registerEnum(L, ITEM_ATTRIBUTE_ACTIONID);
	registerEnum(L, ITEM_ATTRIBUTE_UNIQUEID);
	registerEnum(L, ITEM_ATTRIBUTE_DESCRIPTION);
	registerEnum(L, ITEM_ATTRIBUTE_TEXT);
	registerEnum(L, ITEM_ATTRIBUTE_DATE);
	registerEnum(L, ITEM_ATTRIBUTE_WRITER);
	registerEnum(L, ITEM_ATTRIBUTE_NAME);
	registerEnum(L, ITEM_ATTRIBUTE_ARTICLE);
	registerEnum(L, ITEM_ATTRIBUTE_PLURALNAME);
	registerEnum(L, ITEM_ATTRIBUTE_WEIGHT);
	registerEnum(L, ITEM_ATTRIBUTE_ATTACK);
	registerEnum(L, ITEM_ATTRIBUTE_DEFENSE);
	registerEnum(L, ITEM_ATTRIBUTE_EXTRADEFENSE);
	registerEnum(L, ITEM_ATTRIBUTE_ARMOR);
	registerEnum(L, ITEM_ATTRIBUTE_HITCHANCE);
	registerEnum(L, ITEM_ATTRIBUTE_SHOOTRANGE);
	registerEnum(L, ITEM_ATTRIBUTE_OWNER);
	registerEnum(L, ITEM_ATTRIBUTE_DURATION);
	registerEnum(L, ITEM_ATTRIBUTE_DECAYSTATE);
	registerEnum(L, ITEM_ATTRIBUTE_CORPSEOWNER);
	registerEnum(L, ITEM_ATTRIBUTE_CHARGES);
	registerEnum(L, ITEM_ATTRIBUTE_FLUIDTYPE);
	registerEnum(L, ITEM_ATTRIBUTE_DOORID);
	registerEnum(L, ITEM_ATTRIBUTE_DECAYTO);
	registerEnum(L, ITEM_ATTRIBUTE_WRAPID);
	registerEnum(L, ITEM_ATTRIBUTE_STOREITEM);
	registerEnum(L, ITEM_ATTRIBUTE_ATTACK_SPEED);
	registerEnum(L, ITEM_ATTRIBUTE_OPENCONTAINER);
	registerEnum(L, ITEM_ATTRIBUTE_DURATION_MIN);
	registerEnum(L, ITEM_ATTRIBUTE_DURATION_MAX);

	registerEnum(L, ITEM_TYPE_DEPOT);
	registerEnum(L, ITEM_TYPE_MAILBOX);
	registerEnum(L, ITEM_TYPE_TRASHHOLDER);
	registerEnum(L, ITEM_TYPE_CONTAINER);
	registerEnum(L, ITEM_TYPE_DOOR);
	registerEnum(L, ITEM_TYPE_MAGICFIELD);
	registerEnum(L, ITEM_TYPE_TELEPORT);
	registerEnum(L, ITEM_TYPE_BED);
	registerEnum(L, ITEM_TYPE_KEY);
	registerEnum(L, ITEM_TYPE_RUNE);
	registerEnum(L, ITEM_TYPE_PODIUM);

	registerEnum(L, ITEM_GROUP_GROUND);
	registerEnum(L, ITEM_GROUP_CONTAINER);
	registerEnum(L, ITEM_GROUP_WEAPON);
	registerEnum(L, ITEM_GROUP_AMMUNITION);
	registerEnum(L, ITEM_GROUP_ARMOR);
	registerEnum(L, ITEM_GROUP_CHARGES);
	registerEnum(L, ITEM_GROUP_TELEPORT);
	registerEnum(L, ITEM_GROUP_MAGICFIELD);
	registerEnum(L, ITEM_GROUP_WRITEABLE);
	registerEnum(L, ITEM_GROUP_KEY);
	registerEnum(L, ITEM_GROUP_SPLASH);
	registerEnum(L, ITEM_GROUP_FLUID);
	registerEnum(L, ITEM_GROUP_DOOR);
	registerEnum(L, ITEM_GROUP_DEPRECATED);
	registerEnum(L, ITEM_GROUP_PODIUM);

	registerEnum(L, ITEM_BROWSEFIELD);
	registerEnum(L, ITEM_BAG);
	registerEnum(L, ITEM_SHOPPING_BAG);
	registerEnum(L, ITEM_GOLD_COIN);
	registerEnum(L, ITEM_PLATINUM_COIN);
	registerEnum(L, ITEM_CRYSTAL_COIN);
	registerEnum(L, ITEM_AMULETOFLOSS);
	registerEnum(L, ITEM_PARCEL);
	registerEnum(L, ITEM_LABEL);
	registerEnum(L, ITEM_FIREFIELD_PVP_FULL);
	registerEnum(L, ITEM_FIREFIELD_PVP_MEDIUM);
	registerEnum(L, ITEM_FIREFIELD_PVP_SMALL);
	registerEnum(L, ITEM_FIREFIELD_PERSISTENT_FULL);
	registerEnum(L, ITEM_FIREFIELD_PERSISTENT_MEDIUM);
	registerEnum(L, ITEM_FIREFIELD_PERSISTENT_SMALL);
	registerEnum(L, ITEM_FIREFIELD_NOPVP);
	registerEnum(L, ITEM_FIREFIELD_NOPVP_MEDIUM);
	registerEnum(L, ITEM_POISONFIELD_PVP);
	registerEnum(L, ITEM_POISONFIELD_PERSISTENT);
	registerEnum(L, ITEM_POISONFIELD_NOPVP);
	registerEnum(L, ITEM_ENERGYFIELD_PVP);
	registerEnum(L, ITEM_ENERGYFIELD_PERSISTENT);
	registerEnum(L, ITEM_ENERGYFIELD_NOPVP);
	registerEnum(L, ITEM_MAGICWALL);
	registerEnum(L, ITEM_MAGICWALL_PERSISTENT);
	registerEnum(L, ITEM_MAGICWALL_SAFE);
	registerEnum(L, ITEM_WILDGROWTH);
	registerEnum(L, ITEM_WILDGROWTH_PERSISTENT);
	registerEnum(L, ITEM_WILDGROWTH_SAFE);
	registerEnum(L, ITEM_DECORATION_KIT);

	registerEnum(L, WIELDINFO_NONE);
	registerEnum(L, WIELDINFO_LEVEL);
	registerEnum(L, WIELDINFO_MAGLV);
	registerEnum(L, WIELDINFO_VOCREQ);
	registerEnum(L, WIELDINFO_PREMIUM);

	registerEnum(L, PlayerFlag_CannotUseCombat);
	registerEnum(L, PlayerFlag_CannotAttackPlayer);
	registerEnum(L, PlayerFlag_CannotAttackMonster);
	registerEnum(L, PlayerFlag_CannotBeAttacked);
	registerEnum(L, PlayerFlag_CanConvinceAll);
	registerEnum(L, PlayerFlag_CanSummonAll);
	registerEnum(L, PlayerFlag_CanIllusionAll);
	registerEnum(L, PlayerFlag_CanSenseInvisibility);
	registerEnum(L, PlayerFlag_IgnoredByMonsters);
	registerEnum(L, PlayerFlag_NotGainInFight);
	registerEnum(L, PlayerFlag_HasInfiniteMana);
	registerEnum(L, PlayerFlag_HasInfiniteSoul);
	registerEnum(L, PlayerFlag_HasNoExhaustion);
	registerEnum(L, PlayerFlag_CannotUseSpells);
	registerEnum(L, PlayerFlag_CannotPickupItem);
	registerEnum(L, PlayerFlag_CanAlwaysLogin);
	registerEnum(L, PlayerFlag_CanBroadcast);
	registerEnum(L, PlayerFlag_CanEditHouses);
	registerEnum(L, PlayerFlag_CannotBeBanned);
	registerEnum(L, PlayerFlag_CannotBePushed);
	registerEnum(L, PlayerFlag_HasInfiniteCapacity);
	registerEnum(L, PlayerFlag_CanPushAllCreatures);
	registerEnum(L, PlayerFlag_CanTalkRedPrivate);
	registerEnum(L, PlayerFlag_CanTalkRedChannel);
	registerEnum(L, PlayerFlag_TalkOrangeHelpChannel);
	registerEnum(L, PlayerFlag_NotGainExperience);
	registerEnum(L, PlayerFlag_NotGainMana);
	registerEnum(L, PlayerFlag_NotGainHealth);
	registerEnum(L, PlayerFlag_NotGainSkill);
	registerEnum(L, PlayerFlag_SetMaxSpeed);
	registerEnum(L, PlayerFlag_SpecialVIP);
	registerEnum(L, PlayerFlag_NotGenerateLoot);
	registerEnum(L, PlayerFlag_IgnoreProtectionZone);
	registerEnum(L, PlayerFlag_IgnoreSpellCheck);
	registerEnum(L, PlayerFlag_IgnoreWeaponCheck);
	registerEnum(L, PlayerFlag_CannotBeMuted);
	registerEnum(L, PlayerFlag_IsAlwaysPremium);
	registerEnum(L, PlayerFlag_IgnoreYellCheck);
	registerEnum(L, PlayerFlag_IgnoreSendPrivateCheck);

	registerEnum(L, PODIUM_SHOW_PLATFORM);
	registerEnum(L, PODIUM_SHOW_OUTFIT);
	registerEnum(L, PODIUM_SHOW_MOUNT);

	registerEnum(L, PLAYERSEX_FEMALE);
	registerEnum(L, PLAYERSEX_MALE);

	registerEnum(L, REPORT_REASON_NAMEINAPPROPRIATE);
	registerEnum(L, REPORT_REASON_NAMEPOORFORMATTED);
	registerEnum(L, REPORT_REASON_NAMEADVERTISING);
	registerEnum(L, REPORT_REASON_NAMEUNFITTING);
	registerEnum(L, REPORT_REASON_NAMERULEVIOLATION);
	registerEnum(L, REPORT_REASON_INSULTINGSTATEMENT);
	registerEnum(L, REPORT_REASON_SPAMMING);
	registerEnum(L, REPORT_REASON_ADVERTISINGSTATEMENT);
	registerEnum(L, REPORT_REASON_UNFITTINGSTATEMENT);
	registerEnum(L, REPORT_REASON_LANGUAGESTATEMENT);
	registerEnum(L, REPORT_REASON_DISCLOSURE);
	registerEnum(L, REPORT_REASON_RULEVIOLATION);
	registerEnum(L, REPORT_REASON_STATEMENT_BUGABUSE);
	registerEnum(L, REPORT_REASON_UNOFFICIALSOFTWARE);
	registerEnum(L, REPORT_REASON_PRETENDING);
	registerEnum(L, REPORT_REASON_HARASSINGOWNERS);
	registerEnum(L, REPORT_REASON_FALSEINFO);
	registerEnum(L, REPORT_REASON_ACCOUNTSHARING);
	registerEnum(L, REPORT_REASON_STEALINGDATA);
	registerEnum(L, REPORT_REASON_SERVICEATTACKING);
	registerEnum(L, REPORT_REASON_SERVICEAGREEMENT);

	registerEnum(L, REPORT_TYPE_NAME);
	registerEnum(L, REPORT_TYPE_STATEMENT);
	registerEnum(L, REPORT_TYPE_BOT);

	registerEnum(L, VOCATION_NONE);

	registerEnum(L, SKILL_FIST);
	registerEnum(L, SKILL_CLUB);
	registerEnum(L, SKILL_SWORD);
	registerEnum(L, SKILL_AXE);
	registerEnum(L, SKILL_DISTANCE);
	registerEnum(L, SKILL_SHIELD);
	registerEnum(L, SKILL_FISHING);
	registerEnum(L, SKILL_MAGLEVEL);
	registerEnum(L, SKILL_LEVEL);

	registerEnum(L, SPECIALSKILL_CRITICALHITCHANCE);
	registerEnum(L, SPECIALSKILL_CRITICALHITAMOUNT);
	registerEnum(L, SPECIALSKILL_LIFELEECHCHANCE);
	registerEnum(L, SPECIALSKILL_LIFELEECHAMOUNT);
	registerEnum(L, SPECIALSKILL_MANALEECHCHANCE);
	registerEnum(L, SPECIALSKILL_MANALEECHAMOUNT);

	registerEnum(L, STAT_MAXHITPOINTS);
	registerEnum(L, STAT_MAXMANAPOINTS);
	registerEnum(L, STAT_SOULPOINTS);
	registerEnum(L, STAT_MAGICPOINTS);

	registerEnum(L, SKULL_NONE);
	registerEnum(L, SKULL_YELLOW);
	registerEnum(L, SKULL_GREEN);
	registerEnum(L, SKULL_WHITE);
	registerEnum(L, SKULL_RED);
	registerEnum(L, SKULL_BLACK);
	registerEnum(L, SKULL_ORANGE);

	registerEnum(L, FLUID_NONE);
	registerEnum(L, FLUID_WATER);
	registerEnum(L, FLUID_BLOOD);
	registerEnum(L, FLUID_BEER);
	registerEnum(L, FLUID_SLIME);
	registerEnum(L, FLUID_LEMONADE);
	registerEnum(L, FLUID_MILK);
	registerEnum(L, FLUID_MANA);
	registerEnum(L, FLUID_LIFE);
	registerEnum(L, FLUID_OIL);
	registerEnum(L, FLUID_URINE);
	registerEnum(L, FLUID_COCONUTMILK);
	registerEnum(L, FLUID_WINE);
	registerEnum(L, FLUID_MUD);
	registerEnum(L, FLUID_FRUITJUICE);
	registerEnum(L, FLUID_LAVA);
	registerEnum(L, FLUID_RUM);
	registerEnum(L, FLUID_SWAMP);
	registerEnum(L, FLUID_TEA);
	registerEnum(L, FLUID_MEAD);

	registerEnum(L, TALKTYPE_SAY);
	registerEnum(L, TALKTYPE_WHISPER);
	registerEnum(L, TALKTYPE_YELL);
	registerEnum(L, TALKTYPE_PRIVATE_FROM);
	registerEnum(L, TALKTYPE_PRIVATE_TO);
	registerEnum(L, TALKTYPE_CHANNEL_Y);
	registerEnum(L, TALKTYPE_CHANNEL_O);
	registerEnum(L, TALKTYPE_SPELL);
	registerEnum(L, TALKTYPE_PRIVATE_NP);
	registerEnum(L, TALKTYPE_PRIVATE_NP_CONSOLE);
	registerEnum(L, TALKTYPE_PRIVATE_PN);
	registerEnum(L, TALKTYPE_BROADCAST);
	registerEnum(L, TALKTYPE_CHANNEL_R1);
	registerEnum(L, TALKTYPE_PRIVATE_RED_FROM);
	registerEnum(L, TALKTYPE_PRIVATE_RED_TO);
	registerEnum(L, TALKTYPE_MONSTER_SAY);
	registerEnum(L, TALKTYPE_MONSTER_YELL);
	registerEnum(L, TALKTYPE_POTION);

	registerEnum(L, TEXTCOLOR_BLUE);
	registerEnum(L, TEXTCOLOR_LIGHTGREEN);
	registerEnum(L, TEXTCOLOR_LIGHTBLUE);
	registerEnum(L, TEXTCOLOR_MAYABLUE);
	registerEnum(L, TEXTCOLOR_DARKRED);
	registerEnum(L, TEXTCOLOR_LIGHTGREY);
	registerEnum(L, TEXTCOLOR_SKYBLUE);
	registerEnum(L, TEXTCOLOR_PURPLE);
	registerEnum(L, TEXTCOLOR_ELECTRICPURPLE);
	registerEnum(L, TEXTCOLOR_RED);
	registerEnum(L, TEXTCOLOR_PASTELRED);
	registerEnum(L, TEXTCOLOR_ORANGE);
	registerEnum(L, TEXTCOLOR_YELLOW);
	registerEnum(L, TEXTCOLOR_WHITE_EXP);
	registerEnum(L, TEXTCOLOR_NONE);

	registerEnum(L, TILESTATE_NONE);
	registerEnum(L, TILESTATE_PROTECTIONZONE);
	registerEnum(L, TILESTATE_NOPVPZONE);
	registerEnum(L, TILESTATE_NOLOGOUT);
	registerEnum(L, TILESTATE_PVPZONE);
	registerEnum(L, TILESTATE_FLOORCHANGE);
	registerEnum(L, TILESTATE_FLOORCHANGE_DOWN);
	registerEnum(L, TILESTATE_FLOORCHANGE_NORTH);
	registerEnum(L, TILESTATE_FLOORCHANGE_SOUTH);
	registerEnum(L, TILESTATE_FLOORCHANGE_EAST);
	registerEnum(L, TILESTATE_FLOORCHANGE_WEST);
	registerEnum(L, TILESTATE_TELEPORT);
	registerEnum(L, TILESTATE_MAGICFIELD);
	registerEnum(L, TILESTATE_MAILBOX);
	registerEnum(L, TILESTATE_TRASHHOLDER);
	registerEnum(L, TILESTATE_BED);
	registerEnum(L, TILESTATE_DEPOT);
	registerEnum(L, TILESTATE_BLOCKSOLID);
	registerEnum(L, TILESTATE_BLOCKPATH);
	registerEnum(L, TILESTATE_IMMOVABLEBLOCKSOLID);
	registerEnum(L, TILESTATE_IMMOVABLEBLOCKPATH);
	registerEnum(L, TILESTATE_IMMOVABLENOFIELDBLOCKPATH);
	registerEnum(L, TILESTATE_NOFIELDBLOCKPATH);
	registerEnum(L, TILESTATE_FLOORCHANGE_SOUTH_ALT);
	registerEnum(L, TILESTATE_FLOORCHANGE_EAST_ALT);
	registerEnum(L, TILESTATE_SUPPORTS_HANGABLE);

	registerEnum(L, WEAPON_NONE);
	registerEnum(L, WEAPON_SWORD);
	registerEnum(L, WEAPON_CLUB);
	registerEnum(L, WEAPON_AXE);
	registerEnum(L, WEAPON_SHIELD);
	registerEnum(L, WEAPON_DISTANCE);
	registerEnum(L, WEAPON_WAND);
	registerEnum(L, WEAPON_AMMO);
	registerEnum(L, WEAPON_QUIVER);

	registerEnum(L, WORLD_TYPE_NO_PVP);
	registerEnum(L, WORLD_TYPE_PVP);
	registerEnum(L, WORLD_TYPE_PVP_ENFORCED);

	// Use with container:addItem, container:addItemEx and possibly other functions.
	registerEnum(L, FLAG_NOLIMIT);
	registerEnum(L, FLAG_IGNOREBLOCKITEM);
	registerEnum(L, FLAG_IGNOREBLOCKCREATURE);
	registerEnum(L, FLAG_CHILDISOWNER);
	registerEnum(L, FLAG_PATHFINDING);
	registerEnum(L, FLAG_IGNOREFIELDDAMAGE);
	registerEnum(L, FLAG_IGNORENOTMOVEABLE);
	registerEnum(L, FLAG_IGNOREAUTOSTACK);

	// Use with itemType:getSlotPosition
	registerEnum(L, SLOTP_WHEREEVER);
	registerEnum(L, SLOTP_HEAD);
	registerEnum(L, SLOTP_NECKLACE);
	registerEnum(L, SLOTP_BACKPACK);
	registerEnum(L, SLOTP_ARMOR);
	registerEnum(L, SLOTP_RIGHT);
	registerEnum(L, SLOTP_LEFT);
	registerEnum(L, SLOTP_LEGS);
	registerEnum(L, SLOTP_FEET);
	registerEnum(L, SLOTP_RING);
	registerEnum(L, SLOTP_AMMO);
	registerEnum(L, SLOTP_DEPOT);
	registerEnum(L, SLOTP_TWO_HAND);

	// Use with combat functions
	registerEnum(L, ORIGIN_NONE);
	registerEnum(L, ORIGIN_CONDITION);
	registerEnum(L, ORIGIN_SPELL);
	registerEnum(L, ORIGIN_MELEE);
	registerEnum(L, ORIGIN_RANGED);
	registerEnum(L, ORIGIN_WAND);

	// Use with house:getAccessList, house:setAccessList
	registerEnum(L, GUEST_LIST);
	registerEnum(L, SUBOWNER_LIST);

	// Use with npc:setSpeechBubble
	registerEnum(L, SPEECHBUBBLE_NONE);
	registerEnum(L, SPEECHBUBBLE_NORMAL);
	registerEnum(L, SPEECHBUBBLE_TRADE);
	registerEnum(L, SPEECHBUBBLE_QUEST);
	registerEnum(L, SPEECHBUBBLE_COMPASS);
	registerEnum(L, SPEECHBUBBLE_NORMAL2);
	registerEnum(L, SPEECHBUBBLE_NORMAL3);
	registerEnum(L, SPEECHBUBBLE_HIRELING);

	// Use with player:addMapMark
	registerEnum(L, MAPMARK_TICK);
	registerEnum(L, MAPMARK_QUESTION);
	registerEnum(L, MAPMARK_EXCLAMATION);
	registerEnum(L, MAPMARK_STAR);
	registerEnum(L, MAPMARK_CROSS);
	registerEnum(L, MAPMARK_TEMPLE);
	registerEnum(L, MAPMARK_KISS);
	registerEnum(L, MAPMARK_SHOVEL);
	registerEnum(L, MAPMARK_SWORD);
	registerEnum(L, MAPMARK_FLAG);
	registerEnum(L, MAPMARK_LOCK);
	registerEnum(L, MAPMARK_BAG);
	registerEnum(L, MAPMARK_SKULL);
	registerEnum(L, MAPMARK_DOLLAR);
	registerEnum(L, MAPMARK_REDNORTH);
	registerEnum(L, MAPMARK_REDSOUTH);
	registerEnum(L, MAPMARK_REDEAST);
	registerEnum(L, MAPMARK_REDWEST);
	registerEnum(L, MAPMARK_GREENNORTH);
	registerEnum(L, MAPMARK_GREENSOUTH);

	// Use with Game.getReturnMessage
	registerEnum(L, RETURNVALUE_NOERROR);
	registerEnum(L, RETURNVALUE_NOTPOSSIBLE);
	registerEnum(L, RETURNVALUE_NOTENOUGHROOM);
	registerEnum(L, RETURNVALUE_PLAYERISPZLOCKED);
	registerEnum(L, RETURNVALUE_PLAYERISNOTINVITED);
	registerEnum(L, RETURNVALUE_CANNOTTHROW);
	registerEnum(L, RETURNVALUE_THEREISNOWAY);
	registerEnum(L, RETURNVALUE_DESTINATIONOUTOFREACH);
	registerEnum(L, RETURNVALUE_CREATUREBLOCK);
	registerEnum(L, RETURNVALUE_NOTMOVEABLE);
	registerEnum(L, RETURNVALUE_DROPTWOHANDEDITEM);
	registerEnum(L, RETURNVALUE_BOTHHANDSNEEDTOBEFREE);
	registerEnum(L, RETURNVALUE_CANONLYUSEONEWEAPON);
	registerEnum(L, RETURNVALUE_NEEDEXCHANGE);
	registerEnum(L, RETURNVALUE_CANNOTBEDRESSED);
	registerEnum(L, RETURNVALUE_PUTTHISOBJECTINYOURHAND);
	registerEnum(L, RETURNVALUE_PUTTHISOBJECTINBOTHHANDS);
	registerEnum(L, RETURNVALUE_TOOFARAWAY);
	registerEnum(L, RETURNVALUE_FIRSTGODOWNSTAIRS);
	registerEnum(L, RETURNVALUE_FIRSTGOUPSTAIRS);
	registerEnum(L, RETURNVALUE_CONTAINERNOTENOUGHROOM);
	registerEnum(L, RETURNVALUE_NOTENOUGHCAPACITY);
	registerEnum(L, RETURNVALUE_CANNOTPICKUP);
	registerEnum(L, RETURNVALUE_THISISIMPOSSIBLE);
	registerEnum(L, RETURNVALUE_DEPOTISFULL);
	registerEnum(L, RETURNVALUE_CREATUREDOESNOTEXIST);
	registerEnum(L, RETURNVALUE_CANNOTUSETHISOBJECT);
	registerEnum(L, RETURNVALUE_PLAYERWITHTHISNAMEISNOTONLINE);
	registerEnum(L, RETURNVALUE_NOTREQUIREDLEVELTOUSERUNE);
	registerEnum(L, RETURNVALUE_YOUAREALREADYTRADING);
	registerEnum(L, RETURNVALUE_THISPLAYERISALREADYTRADING);
	registerEnum(L, RETURNVALUE_YOUMAYNOTLOGOUTDURINGAFIGHT);
	registerEnum(L, RETURNVALUE_DIRECTPLAYERSHOOT);
	registerEnum(L, RETURNVALUE_NOTENOUGHLEVEL);
	registerEnum(L, RETURNVALUE_NOTENOUGHMAGICLEVEL);
	registerEnum(L, RETURNVALUE_NOTENOUGHMANA);
	registerEnum(L, RETURNVALUE_NOTENOUGHSOUL);
	registerEnum(L, RETURNVALUE_YOUAREEXHAUSTED);
	registerEnum(L, RETURNVALUE_YOUCANNOTUSEOBJECTSTHATFAST);
	registerEnum(L, RETURNVALUE_PLAYERISNOTREACHABLE);
	registerEnum(L, RETURNVALUE_CANONLYUSETHISRUNEONCREATURES);
	registerEnum(L, RETURNVALUE_ACTIONNOTPERMITTEDINPROTECTIONZONE);
	registerEnum(L, RETURNVALUE_YOUMAYNOTATTACKTHISPLAYER);
	registerEnum(L, RETURNVALUE_YOUMAYNOTATTACKAPERSONINPROTECTIONZONE);
	registerEnum(L, RETURNVALUE_YOUMAYNOTATTACKAPERSONWHILEINPROTECTIONZONE);
	registerEnum(L, RETURNVALUE_YOUMAYNOTATTACKTHISCREATURE);
	registerEnum(L, RETURNVALUE_YOUCANONLYUSEITONCREATURES);
	registerEnum(L, RETURNVALUE_CREATUREISNOTREACHABLE);
	registerEnum(L, RETURNVALUE_TURNSECUREMODETOATTACKUNMARKEDPLAYERS);
	registerEnum(L, RETURNVALUE_YOUNEEDPREMIUMACCOUNT);
	registerEnum(L, RETURNVALUE_YOUNEEDTOLEARNTHISSPELL);
	registerEnum(L, RETURNVALUE_YOURVOCATIONCANNOTUSETHISSPELL);
	registerEnum(L, RETURNVALUE_YOUNEEDAWEAPONTOUSETHISSPELL);
	registerEnum(L, RETURNVALUE_PLAYERISPZLOCKEDLEAVEPVPZONE);
	registerEnum(L, RETURNVALUE_PLAYERISPZLOCKEDENTERPVPZONE);
	registerEnum(L, RETURNVALUE_ACTIONNOTPERMITTEDINANOPVPZONE);
	registerEnum(L, RETURNVALUE_YOUCANNOTLOGOUTHERE);
	registerEnum(L, RETURNVALUE_YOUNEEDAMAGICITEMTOCASTSPELL);
	registerEnum(L, RETURNVALUE_NAMEISTOOAMBIGUOUS);
	registerEnum(L, RETURNVALUE_CANONLYUSEONESHIELD);
	registerEnum(L, RETURNVALUE_NOPARTYMEMBERSINRANGE);
	registerEnum(L, RETURNVALUE_YOUARENOTTHEOWNER);
	registerEnum(L, RETURNVALUE_TRADEPLAYERFARAWAY);
	registerEnum(L, RETURNVALUE_YOUDONTOWNTHISHOUSE);
	registerEnum(L, RETURNVALUE_TRADEPLAYERALREADYOWNSAHOUSE);
	registerEnum(L, RETURNVALUE_TRADEPLAYERHIGHESTBIDDER);
	registerEnum(L, RETURNVALUE_YOUCANNOTTRADETHISHOUSE);
	registerEnum(L, RETURNVALUE_YOUDONTHAVEREQUIREDPROFESSION);
	registerEnum(L, RETURNVALUE_YOUCANNOTUSETHISBED);

	registerEnum(L, RELOAD_TYPE_ALL);
	registerEnum(L, RELOAD_TYPE_ACTIONS);
	registerEnum(L, RELOAD_TYPE_CHAT);
	registerEnum(L, RELOAD_TYPE_CONFIG);
	registerEnum(L, RELOAD_TYPE_CREATURESCRIPTS);
	registerEnum(L, RELOAD_TYPE_EVENTS);
	registerEnum(L, RELOAD_TYPE_GLOBAL);
	registerEnum(L, RELOAD_TYPE_GLOBALEVENTS);
	registerEnum(L, RELOAD_TYPE_ITEMS);
	registerEnum(L, RELOAD_TYPE_MONSTERS);
	registerEnum(L, RELOAD_TYPE_MOUNTS);
	registerEnum(L, RELOAD_TYPE_MOVEMENTS);
	registerEnum(L, RELOAD_TYPE_NPCS);
	registerEnum(L, RELOAD_TYPE_QUESTS);
	registerEnum(L, RELOAD_TYPE_SCRIPTS);
	registerEnum(L, RELOAD_TYPE_SPELLS);
	registerEnum(L, RELOAD_TYPE_TALKACTIONS);
	registerEnum(L, RELOAD_TYPE_WEAPONS);

	registerEnum(L, ZONE_PROTECTION);
	registerEnum(L, ZONE_NOPVP);
	registerEnum(L, ZONE_PVP);
	registerEnum(L, ZONE_NOLOGOUT);
	registerEnum(L, ZONE_NORMAL);

	registerEnum(L, MAX_LOOTCHANCE);

	registerEnum(L, SPELL_INSTANT);
	registerEnum(L, SPELL_RUNE);

	registerEnum(L, MONSTERS_EVENT_THINK);
	registerEnum(L, MONSTERS_EVENT_APPEAR);
	registerEnum(L, MONSTERS_EVENT_DISAPPEAR);
	registerEnum(L, MONSTERS_EVENT_MOVE);
	registerEnum(L, MONSTERS_EVENT_SAY);

	registerEnum(L, DECAYING_FALSE);
	registerEnum(L, DECAYING_TRUE);
	registerEnum(L, DECAYING_PENDING);

	registerEnum(L, RESOURCE_BANK_BALANCE);
	registerEnum(L, RESOURCE_GOLD_EQUIPPED);
	registerEnum(L, RESOURCE_PREY_WILDCARDS);
	registerEnum(L, RESOURCE_DAILYREWARD_STREAK);
	registerEnum(L, RESOURCE_DAILYREWARD_JOKERS);

	registerEnum(L, CREATURE_ICON_CROSS_WHITE);
	registerEnum(L, CREATURE_ICON_CROSS_WHITE_RED);
	registerEnum(L, CREATURE_ICON_ORB_RED);
	registerEnum(L, CREATURE_ICON_ORB_GREEN);
	registerEnum(L, CREATURE_ICON_ORB_RED_GREEN);
	registerEnum(L, CREATURE_ICON_GEM_GREEN);
	registerEnum(L, CREATURE_ICON_GEM_YELLOW);
	registerEnum(L, CREATURE_ICON_GEM_BLUE);
	registerEnum(L, CREATURE_ICON_GEM_PURPLE);
	registerEnum(L, CREATURE_ICON_GEM_RED);
	registerEnum(L, CREATURE_ICON_PIGEON);
	registerEnum(L, CREATURE_ICON_ENERGY);
	registerEnum(L, CREATURE_ICON_POISON);
	registerEnum(L, CREATURE_ICON_WATER);
	registerEnum(L, CREATURE_ICON_FIRE);
	registerEnum(L, CREATURE_ICON_ICE);
	registerEnum(L, CREATURE_ICON_ARROW_UP);
	registerEnum(L, CREATURE_ICON_ARROW_DOWN);
	registerEnum(L, CREATURE_ICON_WARNING);
	registerEnum(L, CREATURE_ICON_QUESTION);
	registerEnum(L, CREATURE_ICON_CROSS_RED);
	registerEnum(L, CREATURE_ICON_FIRST);
	registerEnum(L, CREATURE_ICON_LAST);

	registerEnum(L, MONSTER_ICON_VULNERABLE);
	registerEnum(L, MONSTER_ICON_WEAKENED);
	registerEnum(L, MONSTER_ICON_MELEE);
	registerEnum(L, MONSTER_ICON_INFLUENCED);
	registerEnum(L, MONSTER_ICON_FIENDISH);
	registerEnum(L, MONSTER_ICON_FIRST);
	registerEnum(L, MONSTER_ICON_LAST);

	// _G
	registerGlobalVariable(L, "INDEX_WHEREEVER", INDEX_WHEREEVER);
	registerGlobalBoolean(L, "VIRTUAL_PARENT", true);

	registerGlobalMethod(L, "isType", LuaScriptInterface::luaIsType);
	registerGlobalMethod(L, "rawgetmetatable", LuaScriptInterface::luaRawGetMetatable);

	// configKeys
	registerTable(L, "configKeys");

	registerEnumIn(L, "configKeys", ConfigManager::ALLOW_CHANGEOUTFIT);
	registerEnumIn(L, "configKeys", ConfigManager::ONE_PLAYER_ON_ACCOUNT);
	registerEnumIn(L, "configKeys", ConfigManager::AIMBOT_HOTKEY_ENABLED);
	registerEnumIn(L, "configKeys", ConfigManager::REMOVE_RUNE_CHARGES);
	registerEnumIn(L, "configKeys", ConfigManager::REMOVE_WEAPON_AMMO);
	registerEnumIn(L, "configKeys", ConfigManager::REMOVE_WEAPON_CHARGES);
	registerEnumIn(L, "configKeys", ConfigManager::REMOVE_POTION_CHARGES);
	registerEnumIn(L, "configKeys", ConfigManager::EXPERIENCE_FROM_PLAYERS);
	registerEnumIn(L, "configKeys", ConfigManager::FREE_PREMIUM);
	registerEnumIn(L, "configKeys", ConfigManager::REPLACE_KICK_ON_LOGIN);
	registerEnumIn(L, "configKeys", ConfigManager::ALLOW_CLONES);
	registerEnumIn(L, "configKeys", ConfigManager::BIND_ONLY_GLOBAL_ADDRESS);
	registerEnumIn(L, "configKeys", ConfigManager::OPTIMIZE_DATABASE);
	registerEnumIn(L, "configKeys", ConfigManager::MARKET_PREMIUM);
	registerEnumIn(L, "configKeys", ConfigManager::EMOTE_SPELLS);
	registerEnumIn(L, "configKeys", ConfigManager::STAMINA_SYSTEM);
	registerEnumIn(L, "configKeys", ConfigManager::WARN_UNSAFE_SCRIPTS);
	registerEnumIn(L, "configKeys", ConfigManager::CONVERT_UNSAFE_SCRIPTS);
	registerEnumIn(L, "configKeys", ConfigManager::CLASSIC_EQUIPMENT_SLOTS);
	registerEnumIn(L, "configKeys", ConfigManager::CLASSIC_ATTACK_SPEED);
	registerEnumIn(L, "configKeys", ConfigManager::SERVER_SAVE_NOTIFY_MESSAGE);
	registerEnumIn(L, "configKeys", ConfigManager::SERVER_SAVE_NOTIFY_DURATION);
	registerEnumIn(L, "configKeys", ConfigManager::SERVER_SAVE_CLEAN_MAP);
	registerEnumIn(L, "configKeys", ConfigManager::SERVER_SAVE_CLOSE);
	registerEnumIn(L, "configKeys", ConfigManager::SERVER_SAVE_SHUTDOWN);
	registerEnumIn(L, "configKeys", ConfigManager::ONLINE_OFFLINE_CHARLIST);
	registerEnumIn(L, "configKeys", ConfigManager::CHECK_DUPLICATE_STORAGE_KEYS);

	registerEnumIn(L, "configKeys", ConfigManager::MAP_NAME);
	registerEnumIn(L, "configKeys", ConfigManager::HOUSE_RENT_PERIOD);
	registerEnumIn(L, "configKeys", ConfigManager::SERVER_NAME);
	registerEnumIn(L, "configKeys", ConfigManager::OWNER_NAME);
	registerEnumIn(L, "configKeys", ConfigManager::OWNER_EMAIL);
	registerEnumIn(L, "configKeys", ConfigManager::URL);
	registerEnumIn(L, "configKeys", ConfigManager::LOCATION);
	registerEnumIn(L, "configKeys", ConfigManager::IP);
	registerEnumIn(L, "configKeys", ConfigManager::WORLD_TYPE);
	registerEnumIn(L, "configKeys", ConfigManager::MYSQL_HOST);
	registerEnumIn(L, "configKeys", ConfigManager::MYSQL_USER);
	registerEnumIn(L, "configKeys", ConfigManager::MYSQL_PASS);
	registerEnumIn(L, "configKeys", ConfigManager::MYSQL_DB);
	registerEnumIn(L, "configKeys", ConfigManager::MYSQL_SOCK);
	registerEnumIn(L, "configKeys", ConfigManager::DEFAULT_PRIORITY);
	registerEnumIn(L, "configKeys", ConfigManager::MAP_AUTHOR);

	registerEnumIn(L, "configKeys", ConfigManager::SQL_PORT);
	registerEnumIn(L, "configKeys", ConfigManager::MAX_PLAYERS);
	registerEnumIn(L, "configKeys", ConfigManager::PZ_LOCKED);
	registerEnumIn(L, "configKeys", ConfigManager::DEFAULT_DESPAWNRANGE);
	registerEnumIn(L, "configKeys", ConfigManager::DEFAULT_DESPAWNRADIUS);
	registerEnumIn(L, "configKeys", ConfigManager::DEFAULT_WALKTOSPAWNRADIUS);
	registerEnumIn(L, "configKeys", ConfigManager::REMOVE_ON_DESPAWN);
	registerEnumIn(L, "configKeys", ConfigManager::RATE_EXPERIENCE);
	registerEnumIn(L, "configKeys", ConfigManager::RATE_SKILL);
	registerEnumIn(L, "configKeys", ConfigManager::RATE_LOOT);
	registerEnumIn(L, "configKeys", ConfigManager::RATE_MAGIC);
	registerEnumIn(L, "configKeys", ConfigManager::RATE_SPAWN);
	registerEnumIn(L, "configKeys", ConfigManager::HOUSE_PRICE);
	registerEnumIn(L, "configKeys", ConfigManager::KILLS_TO_RED);
	registerEnumIn(L, "configKeys", ConfigManager::KILLS_TO_BLACK);
	registerEnumIn(L, "configKeys", ConfigManager::MAX_MESSAGEBUFFER);
	registerEnumIn(L, "configKeys", ConfigManager::ACTIONS_DELAY_INTERVAL);
	registerEnumIn(L, "configKeys", ConfigManager::EX_ACTIONS_DELAY_INTERVAL);
	registerEnumIn(L, "configKeys", ConfigManager::KICK_AFTER_MINUTES);
	registerEnumIn(L, "configKeys", ConfigManager::PROTECTION_LEVEL);
	registerEnumIn(L, "configKeys", ConfigManager::DEATH_LOSE_PERCENT);
	registerEnumIn(L, "configKeys", ConfigManager::STATUSQUERY_TIMEOUT);
	registerEnumIn(L, "configKeys", ConfigManager::FRAG_TIME);
	registerEnumIn(L, "configKeys", ConfigManager::WHITE_SKULL_TIME);
	registerEnumIn(L, "configKeys", ConfigManager::GAME_PORT);
	registerEnumIn(L, "configKeys", ConfigManager::LOGIN_PORT);
	registerEnumIn(L, "configKeys", ConfigManager::STATUS_PORT);
	registerEnumIn(L, "configKeys", ConfigManager::STAIRHOP_DELAY);
	registerEnumIn(L, "configKeys", ConfigManager::MARKET_OFFER_DURATION);
	registerEnumIn(L, "configKeys", ConfigManager::CHECK_EXPIRED_MARKET_OFFERS_EACH_MINUTES);
	registerEnumIn(L, "configKeys", ConfigManager::MAX_MARKET_OFFERS_AT_A_TIME_PER_PLAYER);
	registerEnumIn(L, "configKeys", ConfigManager::EXP_FROM_PLAYERS_LEVEL_RANGE);
	registerEnumIn(L, "configKeys", ConfigManager::MAX_PACKETS_PER_SECOND);
	registerEnumIn(L, "configKeys", ConfigManager::TWO_FACTOR_AUTH);
	registerEnumIn(L, "configKeys", ConfigManager::MANASHIELD_BREAKABLE);
	registerEnumIn(L, "configKeys", ConfigManager::STAMINA_REGEN_MINUTE);
	registerEnumIn(L, "configKeys", ConfigManager::STAMINA_REGEN_PREMIUM);
	registerEnumIn(L, "configKeys", ConfigManager::HOUSE_DOOR_SHOW_PRICE);
	registerEnumIn(L, "configKeys", ConfigManager::MONSTER_OVERSPAWN);

	registerEnumIn(L, "configKeys", ConfigManager::QUEST_TRACKER_FREE_LIMIT);
	registerEnumIn(L, "configKeys", ConfigManager::QUEST_TRACKER_PREMIUM_LIMIT);

	// os
	registerMethod(L, "os", "mtime", LuaScriptInterface::luaSystemTime);

	// table
	registerMethod(L, "table", "create", LuaScriptInterface::luaTableCreate);
	registerMethod(L, "table", "pack", LuaScriptInterface::luaTablePack);

	// DB Insert
	registerClass(L, "DBInsert", "", LuaScriptInterface::luaDBInsertCreate);
	registerMetaMethod(L, "DBInsert", "__gc", LuaScriptInterface::luaDBInsertDelete);

	registerMethod(L, "DBInsert", "addRow", LuaScriptInterface::luaDBInsertAddRow);
	registerMethod(L, "DBInsert", "execute", LuaScriptInterface::luaDBInsertExecute);

	// DB Transaction
	registerClass(L, "DBTransaction", "", LuaScriptInterface::luaDBTransactionCreate);
	registerMetaMethod(L, "DBTransaction", "__eq", LuaScriptInterface::luaUserdataCompare);
	registerMetaMethod(L, "DBTransaction", "__gc", LuaScriptInterface::luaDBTransactionDelete);

	registerMethod(L, "DBTransaction", "begin", LuaScriptInterface::luaDBTransactionBegin);
	registerMethod(L, "DBTransaction", "commit", LuaScriptInterface::luaDBTransactionCommit);
	registerMethod(L, "DBTransaction", "rollback", LuaScriptInterface::luaDBTransactionDelete);

	// Game
	registerTable(L, "Game");

	registerMethod(L, "Game", "getSpectators", LuaScriptInterface::luaGameGetSpectators);
	registerMethod(L, "Game", "getPlayers", LuaScriptInterface::luaGameGetPlayers);
	registerMethod(L, "Game", "getNpcs", LuaScriptInterface::luaGameGetNpcs);
	registerMethod(L, "Game", "getMonsters", LuaScriptInterface::luaGameGetMonsters);
	registerMethod(L, "Game", "loadMap", LuaScriptInterface::luaGameLoadMap);

	registerMethod(L, "Game", "getExperienceStage", LuaScriptInterface::luaGameGetExperienceStage);
	registerMethod(L, "Game", "getExperienceForLevel", LuaScriptInterface::luaGameGetExperienceForLevel);
	registerMethod(L, "Game", "getMonsterCount", LuaScriptInterface::luaGameGetMonsterCount);
	registerMethod(L, "Game", "getPlayerCount", LuaScriptInterface::luaGameGetPlayerCount);
	registerMethod(L, "Game", "getNpcCount", LuaScriptInterface::luaGameGetNpcCount);
	registerMethod(L, "Game", "getMonsterTypes", LuaScriptInterface::luaGameGetMonsterTypes);
	registerMethod(L, "Game", "getBestiary", LuaScriptInterface::luaGameGetBestiary);
	registerMethod(L, "Game", "getCurrencyItems", LuaScriptInterface::luaGameGetCurrencyItems);
	registerMethod(L, "Game", "getItemTypeByClientId", LuaScriptInterface::luaGameGetItemTypeByClientId);
	registerMethod(L, "Game", "getMountIdByLookType", LuaScriptInterface::luaGameGetMountIdByLookType);

	registerMethod(L, "Game", "getTowns", LuaScriptInterface::luaGameGetTowns);
	registerMethod(L, "Game", "getHouses", LuaScriptInterface::luaGameGetHouses);
	registerMethod(L, "Game", "getOutfits", LuaScriptInterface::luaGameGetOutfits);
	registerMethod(L, "Game", "getMounts", LuaScriptInterface::luaGameGetMounts);
	registerMethod(L, "Game", "getVocations", LuaScriptInterface::luaGameGetVocations);

	registerMethod(L, "Game", "getGameState", LuaScriptInterface::luaGameGetGameState);
	registerMethod(L, "Game", "setGameState", LuaScriptInterface::luaGameSetGameState);

	registerMethod(L, "Game", "getWorldType", LuaScriptInterface::luaGameGetWorldType);
	registerMethod(L, "Game", "setWorldType", LuaScriptInterface::luaGameSetWorldType);

	registerMethod(L, "Game", "getItemAttributeByName", LuaScriptInterface::luaGameGetItemAttributeByName);
	registerMethod(L, "Game", "getReturnMessage", LuaScriptInterface::luaGameGetReturnMessage);

	registerMethod(L, "Game", "createItem", LuaScriptInterface::luaGameCreateItem);
	registerMethod(L, "Game", "createContainer", LuaScriptInterface::luaGameCreateContainer);
	registerMethod(L, "Game", "createMonster", LuaScriptInterface::luaGameCreateMonster);
	registerMethod(L, "Game", "createNpc", LuaScriptInterface::luaGameCreateNpc);
	registerMethod(L, "Game", "createTile", LuaScriptInterface::luaGameCreateTile);
	registerMethod(L, "Game", "createMonsterType", LuaScriptInterface::luaGameCreateMonsterType);
	registerMethod(L, "Game", "createNpcType", LuaScriptInterface::luaGameCreateNpcType);

	registerMethod(L, "Game", "startEvent", LuaScriptInterface::luaGameStartEvent);

	registerMethod(L, "Game", "getClientVersion", LuaScriptInterface::luaGameGetClientVersion);

	registerMethod(L, "Game", "reload", LuaScriptInterface::luaGameReload);

	// Variant
	registerClass(L, "Variant", "", LuaScriptInterface::luaVariantCreate);

	registerMethod(L, "Variant", "getNumber", LuaScriptInterface::luaVariantGetNumber);
	registerMethod(L, "Variant", "getString", LuaScriptInterface::luaVariantGetString);
	registerMethod(L, "Variant", "getPosition", LuaScriptInterface::luaVariantGetPosition);

	// Position
	registerClass(L, "Position", "", LuaScriptInterface::luaPositionCreate);

	registerMethod(L, "Position", "isSightClear", LuaScriptInterface::luaPositionIsSightClear);

	registerMethod(L, "Position", "sendMagicEffect", LuaScriptInterface::luaPositionSendMagicEffect);
	registerMethod(L, "Position", "sendDistanceEffect", LuaScriptInterface::luaPositionSendDistanceEffect);

	// Tile
	registerClass(L, "Tile", "", LuaScriptInterface::luaTileCreate);
	registerMetaMethod(L, "Tile", "__eq", LuaScriptInterface::luaUserdataCompare);

	registerMethod(L, "Tile", "remove", LuaScriptInterface::luaTileRemove);

	registerMethod(L, "Tile", "getPosition", LuaScriptInterface::luaTileGetPosition);
	registerMethod(L, "Tile", "getGround", LuaScriptInterface::luaTileGetGround);
	registerMethod(L, "Tile", "getThing", LuaScriptInterface::luaTileGetThing);
	registerMethod(L, "Tile", "getThingCount", LuaScriptInterface::luaTileGetThingCount);
	registerMethod(L, "Tile", "getTopVisibleThing", LuaScriptInterface::luaTileGetTopVisibleThing);

	registerMethod(L, "Tile", "getTopTopItem", LuaScriptInterface::luaTileGetTopTopItem);
	registerMethod(L, "Tile", "getTopDownItem", LuaScriptInterface::luaTileGetTopDownItem);
	registerMethod(L, "Tile", "getFieldItem", LuaScriptInterface::luaTileGetFieldItem);

	registerMethod(L, "Tile", "getItemById", LuaScriptInterface::luaTileGetItemById);
	registerMethod(L, "Tile", "getItemByType", LuaScriptInterface::luaTileGetItemByType);
	registerMethod(L, "Tile", "getItemByTopOrder", LuaScriptInterface::luaTileGetItemByTopOrder);
	registerMethod(L, "Tile", "getItemCountById", LuaScriptInterface::luaTileGetItemCountById);

	registerMethod(L, "Tile", "getBottomCreature", LuaScriptInterface::luaTileGetBottomCreature);
	registerMethod(L, "Tile", "getTopCreature", LuaScriptInterface::luaTileGetTopCreature);
	registerMethod(L, "Tile", "getBottomVisibleCreature", LuaScriptInterface::luaTileGetBottomVisibleCreature);
	registerMethod(L, "Tile", "getTopVisibleCreature", LuaScriptInterface::luaTileGetTopVisibleCreature);

	registerMethod(L, "Tile", "getItems", LuaScriptInterface::luaTileGetItems);
	registerMethod(L, "Tile", "getItemCount", LuaScriptInterface::luaTileGetItemCount);
	registerMethod(L, "Tile", "getDownItemCount", LuaScriptInterface::luaTileGetDownItemCount);
	registerMethod(L, "Tile", "getTopItemCount", LuaScriptInterface::luaTileGetTopItemCount);

	registerMethod(L, "Tile", "getCreatures", LuaScriptInterface::luaTileGetCreatures);
	registerMethod(L, "Tile", "getCreatureCount", LuaScriptInterface::luaTileGetCreatureCount);

	registerMethod(L, "Tile", "getThingIndex", LuaScriptInterface::luaTileGetThingIndex);

	registerMethod(L, "Tile", "hasProperty", LuaScriptInterface::luaTileHasProperty);
	registerMethod(L, "Tile", "hasFlag", LuaScriptInterface::luaTileHasFlag);

	registerMethod(L, "Tile", "queryAdd", LuaScriptInterface::luaTileQueryAdd);
	registerMethod(L, "Tile", "addItem", LuaScriptInterface::luaTileAddItem);
	registerMethod(L, "Tile", "addItemEx", LuaScriptInterface::luaTileAddItemEx);

	registerMethod(L, "Tile", "getHouse", LuaScriptInterface::luaTileGetHouse);

	// NetworkMessage
	registerClass(L, "NetworkMessage", "", LuaScriptInterface::luaNetworkMessageCreate);
	registerMetaMethod(L, "NetworkMessage", "__eq", LuaScriptInterface::luaUserdataCompare);
	registerMetaMethod(L, "NetworkMessage", "__gc", LuaScriptInterface::luaNetworkMessageDelete);
	registerMethod(L, "NetworkMessage", "delete", LuaScriptInterface::luaNetworkMessageDelete);

	registerMethod(L, "NetworkMessage", "getByte", LuaScriptInterface::luaNetworkMessageGetByte);
	registerMethod(L, "NetworkMessage", "getU16", LuaScriptInterface::luaNetworkMessageGetU16);
	registerMethod(L, "NetworkMessage", "getU32", LuaScriptInterface::luaNetworkMessageGetU32);
	registerMethod(L, "NetworkMessage", "getU64", LuaScriptInterface::luaNetworkMessageGetU64);
	registerMethod(L, "NetworkMessage", "getString", LuaScriptInterface::luaNetworkMessageGetString);
	registerMethod(L, "NetworkMessage", "getPosition", LuaScriptInterface::luaNetworkMessageGetPosition);

	registerMethod(L, "NetworkMessage", "addByte", LuaScriptInterface::luaNetworkMessageAddByte);
	registerMethod(L, "NetworkMessage", "addU16", LuaScriptInterface::luaNetworkMessageAddU16);
	registerMethod(L, "NetworkMessage", "addU32", LuaScriptInterface::luaNetworkMessageAddU32);
	registerMethod(L, "NetworkMessage", "addU64", LuaScriptInterface::luaNetworkMessageAddU64);
	registerMethod(L, "NetworkMessage", "addString", LuaScriptInterface::luaNetworkMessageAddString);
	registerMethod(L, "NetworkMessage", "addPosition", LuaScriptInterface::luaNetworkMessageAddPosition);
	registerMethod(L, "NetworkMessage", "addDouble", LuaScriptInterface::luaNetworkMessageAddDouble);
	registerMethod(L, "NetworkMessage", "addItem", LuaScriptInterface::luaNetworkMessageAddItem);
	registerMethod(L, "NetworkMessage", "addItemId", LuaScriptInterface::luaNetworkMessageAddItemId);

	registerMethod(L, "NetworkMessage", "reset", LuaScriptInterface::luaNetworkMessageReset);
	registerMethod(L, "NetworkMessage", "seek", LuaScriptInterface::luaNetworkMessageSeek);
	registerMethod(L, "NetworkMessage", "tell", LuaScriptInterface::luaNetworkMessageTell);
	registerMethod(L, "NetworkMessage", "len", LuaScriptInterface::luaNetworkMessageLength);
	registerMethod(L, "NetworkMessage", "skipBytes", LuaScriptInterface::luaNetworkMessageSkipBytes);
	registerMethod(L, "NetworkMessage", "sendToPlayer", LuaScriptInterface::luaNetworkMessageSendToPlayer);

	// ModalWindow
	registerClass(L, "ModalWindow", "", LuaScriptInterface::luaModalWindowCreate);
	registerMetaMethod(L, "ModalWindow", "__eq", LuaScriptInterface::luaUserdataCompare);
	registerMetaMethod(L, "ModalWindow", "__gc", LuaScriptInterface::luaModalWindowDelete);
	registerMethod(L, "ModalWindow", "delete", LuaScriptInterface::luaModalWindowDelete);

	registerMethod(L, "ModalWindow", "getId", LuaScriptInterface::luaModalWindowGetId);
	registerMethod(L, "ModalWindow", "getTitle", LuaScriptInterface::luaModalWindowGetTitle);
	registerMethod(L, "ModalWindow", "getMessage", LuaScriptInterface::luaModalWindowGetMessage);

	registerMethod(L, "ModalWindow", "setTitle", LuaScriptInterface::luaModalWindowSetTitle);
	registerMethod(L, "ModalWindow", "setMessage", LuaScriptInterface::luaModalWindowSetMessage);

	registerMethod(L, "ModalWindow", "getButtonCount", LuaScriptInterface::luaModalWindowGetButtonCount);
	registerMethod(L, "ModalWindow", "getChoiceCount", LuaScriptInterface::luaModalWindowGetChoiceCount);

	registerMethod(L, "ModalWindow", "addButton", LuaScriptInterface::luaModalWindowAddButton);
	registerMethod(L, "ModalWindow", "addChoice", LuaScriptInterface::luaModalWindowAddChoice);

	registerMethod(L, "ModalWindow", "getDefaultEnterButton", LuaScriptInterface::luaModalWindowGetDefaultEnterButton);
	registerMethod(L, "ModalWindow", "setDefaultEnterButton", LuaScriptInterface::luaModalWindowSetDefaultEnterButton);

	registerMethod(L, "ModalWindow", "getDefaultEscapeButton",
	               LuaScriptInterface::luaModalWindowGetDefaultEscapeButton);
	registerMethod(L, "ModalWindow", "setDefaultEscapeButton",
	               LuaScriptInterface::luaModalWindowSetDefaultEscapeButton);

	registerMethod(L, "ModalWindow", "hasPriority", LuaScriptInterface::luaModalWindowHasPriority);
	registerMethod(L, "ModalWindow", "setPriority", LuaScriptInterface::luaModalWindowSetPriority);

	registerMethod(L, "ModalWindow", "sendToPlayer", LuaScriptInterface::luaModalWindowSendToPlayer);

	// Item
	registerClass(L, "Item", "", LuaScriptInterface::luaItemCreate);
	registerMetaMethod(L, "Item", "__eq", LuaScriptInterface::luaUserdataCompare);

	registerMethod(L, "Item", "isItem", LuaScriptInterface::luaItemIsItem);

	registerMethod(L, "Item", "getParent", LuaScriptInterface::luaItemGetParent);
	registerMethod(L, "Item", "getTopParent", LuaScriptInterface::luaItemGetTopParent);

	registerMethod(L, "Item", "getId", LuaScriptInterface::luaItemGetId);

	registerMethod(L, "Item", "clone", LuaScriptInterface::luaItemClone);
	registerMethod(L, "Item", "split", LuaScriptInterface::luaItemSplit);
	registerMethod(L, "Item", "remove", LuaScriptInterface::luaItemRemove);

	registerMethod(L, "Item", "getUniqueId", LuaScriptInterface::luaItemGetUniqueId);
	registerMethod(L, "Item", "getActionId", LuaScriptInterface::luaItemGetActionId);
	registerMethod(L, "Item", "setActionId", LuaScriptInterface::luaItemSetActionId);

	registerMethod(L, "Item", "getCount", LuaScriptInterface::luaItemGetCount);
	registerMethod(L, "Item", "getCharges", LuaScriptInterface::luaItemGetCharges);
	registerMethod(L, "Item", "getFluidType", LuaScriptInterface::luaItemGetFluidType);
	registerMethod(L, "Item", "getWeight", LuaScriptInterface::luaItemGetWeight);
	registerMethod(L, "Item", "getWorth", LuaScriptInterface::luaItemGetWorth);

	registerMethod(L, "Item", "getSubType", LuaScriptInterface::luaItemGetSubType);

	registerMethod(L, "Item", "getName", LuaScriptInterface::luaItemGetName);
	registerMethod(L, "Item", "getPluralName", LuaScriptInterface::luaItemGetPluralName);
	registerMethod(L, "Item", "getArticle", LuaScriptInterface::luaItemGetArticle);

	registerMethod(L, "Item", "getPosition", LuaScriptInterface::luaItemGetPosition);
	registerMethod(L, "Item", "getTile", LuaScriptInterface::luaItemGetTile);

	registerMethod(L, "Item", "hasAttribute", LuaScriptInterface::luaItemHasAttribute);
	registerMethod(L, "Item", "getAttribute", LuaScriptInterface::luaItemGetAttribute);
	registerMethod(L, "Item", "setAttribute", LuaScriptInterface::luaItemSetAttribute);
	registerMethod(L, "Item", "removeAttribute", LuaScriptInterface::luaItemRemoveAttribute);
	registerMethod(L, "Item", "getCustomAttribute", LuaScriptInterface::luaItemGetCustomAttribute);
	registerMethod(L, "Item", "setCustomAttribute", LuaScriptInterface::luaItemSetCustomAttribute);
	registerMethod(L, "Item", "removeCustomAttribute", LuaScriptInterface::luaItemRemoveCustomAttribute);

	registerMethod(L, "Item", "moveTo", LuaScriptInterface::luaItemMoveTo);
	registerMethod(L, "Item", "transform", LuaScriptInterface::luaItemTransform);
	registerMethod(L, "Item", "decay", LuaScriptInterface::luaItemDecay);

	registerMethod(L, "Item", "getSpecialDescription", LuaScriptInterface::luaItemGetSpecialDescription);

	registerMethod(L, "Item", "hasProperty", LuaScriptInterface::luaItemHasProperty);
	registerMethod(L, "Item", "isLoadedFromMap", LuaScriptInterface::luaItemIsLoadedFromMap);

	registerMethod(L, "Item", "setStoreItem", LuaScriptInterface::luaItemSetStoreItem);
	registerMethod(L, "Item", "isStoreItem", LuaScriptInterface::luaItemIsStoreItem);

	registerMethod(L, "Item", "setReflect", LuaScriptInterface::luaItemSetReflect);
	registerMethod(L, "Item", "getReflect", LuaScriptInterface::luaItemGetReflect);

	registerMethod(L, "Item", "setBoostPercent", LuaScriptInterface::luaItemSetBoostPercent);
	registerMethod(L, "Item", "getBoostPercent", LuaScriptInterface::luaItemGetBoostPercent);

	// Container
	registerClass(L, "Container", "Item", LuaScriptInterface::luaContainerCreate);
	registerMetaMethod(L, "Container", "__eq", LuaScriptInterface::luaUserdataCompare);

	registerMethod(L, "Container", "getSize", LuaScriptInterface::luaContainerGetSize);
	registerMethod(L, "Container", "getCapacity", LuaScriptInterface::luaContainerGetCapacity);
	registerMethod(L, "Container", "getEmptySlots", LuaScriptInterface::luaContainerGetEmptySlots);
	registerMethod(L, "Container", "getItems", LuaScriptInterface::luaContainerGetItems);
	registerMethod(L, "Container", "getItemHoldingCount", LuaScriptInterface::luaContainerGetItemHoldingCount);
	registerMethod(L, "Container", "getItemCountById", LuaScriptInterface::luaContainerGetItemCountById);

	registerMethod(L, "Container", "getItem", LuaScriptInterface::luaContainerGetItem);
	registerMethod(L, "Container", "hasItem", LuaScriptInterface::luaContainerHasItem);
	registerMethod(L, "Container", "addItem", LuaScriptInterface::luaContainerAddItem);
	registerMethod(L, "Container", "addItemEx", LuaScriptInterface::luaContainerAddItemEx);
	registerMethod(L, "Container", "getCorpseOwner", LuaScriptInterface::luaContainerGetCorpseOwner);

	// Teleport
	registerClass(L, "Teleport", "Item", LuaScriptInterface::luaTeleportCreate);
	registerMetaMethod(L, "Teleport", "__eq", LuaScriptInterface::luaUserdataCompare);

	registerMethod(L, "Teleport", "getDestination", LuaScriptInterface::luaTeleportGetDestination);
	registerMethod(L, "Teleport", "setDestination", LuaScriptInterface::luaTeleportSetDestination);

	// Podium
	registerClass(L, "Podium", "Item", LuaScriptInterface::luaPodiumCreate);
	registerMetaMethod(L, "Podium", "__eq", LuaScriptInterface::luaUserdataCompare);

	registerMethod(L, "Podium", "getOutfit", LuaScriptInterface::luaPodiumGetOutfit);
	registerMethod(L, "Podium", "setOutfit", LuaScriptInterface::luaPodiumSetOutfit);
	registerMethod(L, "Podium", "hasFlag", LuaScriptInterface::luaPodiumHasFlag);
	registerMethod(L, "Podium", "setFlag", LuaScriptInterface::luaPodiumSetFlag);
	registerMethod(L, "Podium", "getDirection", LuaScriptInterface::luaPodiumGetDirection);
	registerMethod(L, "Podium", "setDirection", LuaScriptInterface::luaPodiumSetDirection);

	// Creature
	registerClass(L, "Creature", "", LuaScriptInterface::luaCreatureCreate);
	registerMetaMethod(L, "Creature", "__eq", LuaScriptInterface::luaUserdataCompare);

	registerMethod(L, "Creature", "getEvents", LuaScriptInterface::luaCreatureGetEvents);
	registerMethod(L, "Creature", "registerEvent", LuaScriptInterface::luaCreatureRegisterEvent);
	registerMethod(L, "Creature", "unregisterEvent", LuaScriptInterface::luaCreatureUnregisterEvent);

	registerMethod(L, "Creature", "isRemoved", LuaScriptInterface::luaCreatureIsRemoved);
	registerMethod(L, "Creature", "isCreature", LuaScriptInterface::luaCreatureIsCreature);
	registerMethod(L, "Creature", "isInGhostMode", LuaScriptInterface::luaCreatureIsInGhostMode);
	registerMethod(L, "Creature", "isHealthHidden", LuaScriptInterface::luaCreatureIsHealthHidden);
	registerMethod(L, "Creature", "isMovementBlocked", LuaScriptInterface::luaCreatureIsMovementBlocked);
	registerMethod(L, "Creature", "isImmune", LuaScriptInterface::luaCreatureIsImmune);

	registerMethod(L, "Creature", "canSee", LuaScriptInterface::luaCreatureCanSee);
	registerMethod(L, "Creature", "canSeeCreature", LuaScriptInterface::luaCreatureCanSeeCreature);
	registerMethod(L, "Creature", "canSeeGhostMode", LuaScriptInterface::luaCreatureCanSeeGhostMode);
	registerMethod(L, "Creature", "canSeeInvisibility", LuaScriptInterface::luaCreatureCanSeeInvisibility);

	registerMethod(L, "Creature", "getParent", LuaScriptInterface::luaCreatureGetParent);

	registerMethod(L, "Creature", "getId", LuaScriptInterface::luaCreatureGetId);
	registerMethod(L, "Creature", "getName", LuaScriptInterface::luaCreatureGetName);

	registerMethod(L, "Creature", "getTarget", LuaScriptInterface::luaCreatureGetTarget);
	registerMethod(L, "Creature", "setTarget", LuaScriptInterface::luaCreatureSetTarget);

	registerMethod(L, "Creature", "getFollowCreature", LuaScriptInterface::luaCreatureGetFollowCreature);
	registerMethod(L, "Creature", "setFollowCreature", LuaScriptInterface::luaCreatureSetFollowCreature);

	registerMethod(L, "Creature", "getMaster", LuaScriptInterface::luaCreatureGetMaster);
	registerMethod(L, "Creature", "setMaster", LuaScriptInterface::luaCreatureSetMaster);

	registerMethod(L, "Creature", "getLight", LuaScriptInterface::luaCreatureGetLight);
	registerMethod(L, "Creature", "setLight", LuaScriptInterface::luaCreatureSetLight);

	registerMethod(L, "Creature", "getSpeed", LuaScriptInterface::luaCreatureGetSpeed);
	registerMethod(L, "Creature", "getBaseSpeed", LuaScriptInterface::luaCreatureGetBaseSpeed);
	registerMethod(L, "Creature", "changeSpeed", LuaScriptInterface::luaCreatureChangeSpeed);

	registerMethod(L, "Creature", "setDropLoot", LuaScriptInterface::luaCreatureSetDropLoot);
	registerMethod(L, "Creature", "setSkillLoss", LuaScriptInterface::luaCreatureSetSkillLoss);

	registerMethod(L, "Creature", "getPosition", LuaScriptInterface::luaCreatureGetPosition);
	registerMethod(L, "Creature", "getTile", LuaScriptInterface::luaCreatureGetTile);
	registerMethod(L, "Creature", "getDirection", LuaScriptInterface::luaCreatureGetDirection);
	registerMethod(L, "Creature", "setDirection", LuaScriptInterface::luaCreatureSetDirection);

	registerMethod(L, "Creature", "getHealth", LuaScriptInterface::luaCreatureGetHealth);
	registerMethod(L, "Creature", "setHealth", LuaScriptInterface::luaCreatureSetHealth);
	registerMethod(L, "Creature", "addHealth", LuaScriptInterface::luaCreatureAddHealth);
	registerMethod(L, "Creature", "getMaxHealth", LuaScriptInterface::luaCreatureGetMaxHealth);
	registerMethod(L, "Creature", "setMaxHealth", LuaScriptInterface::luaCreatureSetMaxHealth);
	registerMethod(L, "Creature", "setHiddenHealth", LuaScriptInterface::luaCreatureSetHiddenHealth);
	registerMethod(L, "Creature", "setMovementBlocked", LuaScriptInterface::luaCreatureSetMovementBlocked);

	registerMethod(L, "Creature", "getSkull", LuaScriptInterface::luaCreatureGetSkull);
	registerMethod(L, "Creature", "setSkull", LuaScriptInterface::luaCreatureSetSkull);

	registerMethod(L, "Creature", "getOutfit", LuaScriptInterface::luaCreatureGetOutfit);
	registerMethod(L, "Creature", "setOutfit", LuaScriptInterface::luaCreatureSetOutfit);

	registerMethod(L, "Creature", "getCondition", LuaScriptInterface::luaCreatureGetCondition);
	registerMethod(L, "Creature", "addCondition", LuaScriptInterface::luaCreatureAddCondition);
	registerMethod(L, "Creature", "removeCondition", LuaScriptInterface::luaCreatureRemoveCondition);
	registerMethod(L, "Creature", "hasCondition", LuaScriptInterface::luaCreatureHasCondition);

	registerMethod(L, "Creature", "remove", LuaScriptInterface::luaCreatureRemove);
	registerMethod(L, "Creature", "teleportTo", LuaScriptInterface::luaCreatureTeleportTo);
	registerMethod(L, "Creature", "say", LuaScriptInterface::luaCreatureSay);

	registerMethod(L, "Creature", "getDamageMap", LuaScriptInterface::luaCreatureGetDamageMap);

	registerMethod(L, "Creature", "getSummons", LuaScriptInterface::luaCreatureGetSummons);

	registerMethod(L, "Creature", "getDescription", LuaScriptInterface::luaCreatureGetDescription);

	registerMethod(L, "Creature", "getPathTo", LuaScriptInterface::luaCreatureGetPathTo);
	registerMethod(L, "Creature", "move", LuaScriptInterface::luaCreatureMove);

	registerMethod(L, "Creature", "getZone", LuaScriptInterface::luaCreatureGetZone);

	registerMethod(L, "Creature", "hasIcon", LuaScriptInterface::luaCreatureHasIcon);
	registerMethod(L, "Creature", "setIcon", LuaScriptInterface::luaCreatureSetIcon);
	registerMethod(L, "Creature", "getIcon", LuaScriptInterface::luaCreatureGetIcon);
	registerMethod(L, "Creature", "removeIcon", LuaScriptInterface::luaCreatureRemoveIcon);

	registerMethod(L, "Creature", "getStorageValue", LuaScriptInterface::luaCreatureGetStorageValue);
	registerMethod(L, "Creature", "setStorageValue", LuaScriptInterface::luaCreatureSetStorageValue);

	// Player
	registerClass(L, "Player", "Creature", LuaScriptInterface::luaPlayerCreate);
	registerMetaMethod(L, "Player", "__eq", LuaScriptInterface::luaUserdataCompare);

	registerMethod(L, "Player", "isPlayer", LuaScriptInterface::luaPlayerIsPlayer);

	registerMethod(L, "Player", "getGuid", LuaScriptInterface::luaPlayerGetGuid);
	registerMethod(L, "Player", "getIp", LuaScriptInterface::luaPlayerGetIp);
	registerMethod(L, "Player", "getAccountId", LuaScriptInterface::luaPlayerGetAccountId);
	registerMethod(L, "Player", "getLastLoginSaved", LuaScriptInterface::luaPlayerGetLastLoginSaved);
	registerMethod(L, "Player", "getLastLogout", LuaScriptInterface::luaPlayerGetLastLogout);

	registerMethod(L, "Player", "getAccountType", LuaScriptInterface::luaPlayerGetAccountType);
	registerMethod(L, "Player", "setAccountType", LuaScriptInterface::luaPlayerSetAccountType);

	registerMethod(L, "Player", "getCapacity", LuaScriptInterface::luaPlayerGetCapacity);
	registerMethod(L, "Player", "setCapacity", LuaScriptInterface::luaPlayerSetCapacity);

	registerMethod(L, "Player", "getFreeCapacity", LuaScriptInterface::luaPlayerGetFreeCapacity);

	registerMethod(L, "Player", "getDepotChest", LuaScriptInterface::luaPlayerGetDepotChest);
	registerMethod(L, "Player", "getInbox", LuaScriptInterface::luaPlayerGetInbox);

	registerMethod(L, "Player", "getSkullTime", LuaScriptInterface::luaPlayerGetSkullTime);
	registerMethod(L, "Player", "setSkullTime", LuaScriptInterface::luaPlayerSetSkullTime);
	registerMethod(L, "Player", "getDeathPenalty", LuaScriptInterface::luaPlayerGetDeathPenalty);

	registerMethod(L, "Player", "getExperience", LuaScriptInterface::luaPlayerGetExperience);
	registerMethod(L, "Player", "addExperience", LuaScriptInterface::luaPlayerAddExperience);
	registerMethod(L, "Player", "removeExperience", LuaScriptInterface::luaPlayerRemoveExperience);
	registerMethod(L, "Player", "getLevel", LuaScriptInterface::luaPlayerGetLevel);
	registerMethod(L, "Player", "getLevelPercent", LuaScriptInterface::luaPlayerGetLevelPercent);

	registerMethod(L, "Player", "getMagicLevel", LuaScriptInterface::luaPlayerGetMagicLevel);
	registerMethod(L, "Player", "getMagicLevelPercent", LuaScriptInterface::luaPlayerGetMagicLevelPercent);
	registerMethod(L, "Player", "getBaseMagicLevel", LuaScriptInterface::luaPlayerGetBaseMagicLevel);
	registerMethod(L, "Player", "getMana", LuaScriptInterface::luaPlayerGetMana);
	registerMethod(L, "Player", "addMana", LuaScriptInterface::luaPlayerAddMana);
	registerMethod(L, "Player", "getMaxMana", LuaScriptInterface::luaPlayerGetMaxMana);
	registerMethod(L, "Player", "setMaxMana", LuaScriptInterface::luaPlayerSetMaxMana);
	registerMethod(L, "Player", "setManaShieldBar", LuaScriptInterface::luaPlayerSetManaShieldBar);
	registerMethod(L, "Player", "getManaSpent", LuaScriptInterface::luaPlayerGetManaSpent);
	registerMethod(L, "Player", "addManaSpent", LuaScriptInterface::luaPlayerAddManaSpent);
	registerMethod(L, "Player", "removeManaSpent", LuaScriptInterface::luaPlayerRemoveManaSpent);

	registerMethod(L, "Player", "getBaseMaxHealth", LuaScriptInterface::luaPlayerGetBaseMaxHealth);
	registerMethod(L, "Player", "getBaseMaxMana", LuaScriptInterface::luaPlayerGetBaseMaxMana);

	registerMethod(L, "Player", "getSkillLevel", LuaScriptInterface::luaPlayerGetSkillLevel);
	registerMethod(L, "Player", "getEffectiveSkillLevel", LuaScriptInterface::luaPlayerGetEffectiveSkillLevel);
	registerMethod(L, "Player", "getSkillPercent", LuaScriptInterface::luaPlayerGetSkillPercent);
	registerMethod(L, "Player", "getSkillTries", LuaScriptInterface::luaPlayerGetSkillTries);
	registerMethod(L, "Player", "addSkillTries", LuaScriptInterface::luaPlayerAddSkillTries);
	registerMethod(L, "Player", "removeSkillTries", LuaScriptInterface::luaPlayerRemoveSkillTries);
	registerMethod(L, "Player", "getSpecialSkill", LuaScriptInterface::luaPlayerGetSpecialSkill);
	registerMethod(L, "Player", "addSpecialSkill", LuaScriptInterface::luaPlayerAddSpecialSkill);

	registerMethod(L, "Player", "addOfflineTrainingTime", LuaScriptInterface::luaPlayerAddOfflineTrainingTime);
	registerMethod(L, "Player", "getOfflineTrainingTime", LuaScriptInterface::luaPlayerGetOfflineTrainingTime);
	registerMethod(L, "Player", "removeOfflineTrainingTime", LuaScriptInterface::luaPlayerRemoveOfflineTrainingTime);

	registerMethod(L, "Player", "addOfflineTrainingTries", LuaScriptInterface::luaPlayerAddOfflineTrainingTries);

	registerMethod(L, "Player", "getOfflineTrainingSkill", LuaScriptInterface::luaPlayerGetOfflineTrainingSkill);
	registerMethod(L, "Player", "setOfflineTrainingSkill", LuaScriptInterface::luaPlayerSetOfflineTrainingSkill);

	registerMethod(L, "Player", "getItemCount", LuaScriptInterface::luaPlayerGetItemCount);
	registerMethod(L, "Player", "getItemById", LuaScriptInterface::luaPlayerGetItemById);

	registerMethod(L, "Player", "getVocation", LuaScriptInterface::luaPlayerGetVocation);
	registerMethod(L, "Player", "setVocation", LuaScriptInterface::luaPlayerSetVocation);

	registerMethod(L, "Player", "getSex", LuaScriptInterface::luaPlayerGetSex);
	registerMethod(L, "Player", "setSex", LuaScriptInterface::luaPlayerSetSex);

	registerMethod(L, "Player", "getTown", LuaScriptInterface::luaPlayerGetTown);
	registerMethod(L, "Player", "setTown", LuaScriptInterface::luaPlayerSetTown);

	registerMethod(L, "Player", "getGuild", LuaScriptInterface::luaPlayerGetGuild);
	registerMethod(L, "Player", "setGuild", LuaScriptInterface::luaPlayerSetGuild);

	registerMethod(L, "Player", "getGuildLevel", LuaScriptInterface::luaPlayerGetGuildLevel);
	registerMethod(L, "Player", "setGuildLevel", LuaScriptInterface::luaPlayerSetGuildLevel);

	registerMethod(L, "Player", "getGuildNick", LuaScriptInterface::luaPlayerGetGuildNick);
	registerMethod(L, "Player", "setGuildNick", LuaScriptInterface::luaPlayerSetGuildNick);

	registerMethod(L, "Player", "getGroup", LuaScriptInterface::luaPlayerGetGroup);
	registerMethod(L, "Player", "setGroup", LuaScriptInterface::luaPlayerSetGroup);

	registerMethod(L, "Player", "getStamina", LuaScriptInterface::luaPlayerGetStamina);
	registerMethod(L, "Player", "setStamina", LuaScriptInterface::luaPlayerSetStamina);

	registerMethod(L, "Player", "getSoul", LuaScriptInterface::luaPlayerGetSoul);
	registerMethod(L, "Player", "addSoul", LuaScriptInterface::luaPlayerAddSoul);
	registerMethod(L, "Player", "getMaxSoul", LuaScriptInterface::luaPlayerGetMaxSoul);

	registerMethod(L, "Player", "getBankBalance", LuaScriptInterface::luaPlayerGetBankBalance);
	registerMethod(L, "Player", "setBankBalance", LuaScriptInterface::luaPlayerSetBankBalance);

	registerMethod(L, "Player", "addItem", LuaScriptInterface::luaPlayerAddItem);
	registerMethod(L, "Player", "addItemEx", LuaScriptInterface::luaPlayerAddItemEx);
	registerMethod(L, "Player", "removeItem", LuaScriptInterface::luaPlayerRemoveItem);
	registerMethod(L, "Player", "sendSupplyUsed", LuaScriptInterface::luaPlayerSendSupplyUsed);

	registerMethod(L, "Player", "getMoney", LuaScriptInterface::luaPlayerGetMoney);
	registerMethod(L, "Player", "addMoney", LuaScriptInterface::luaPlayerAddMoney);
	registerMethod(L, "Player", "removeMoney", LuaScriptInterface::luaPlayerRemoveMoney);

	registerMethod(L, "Player", "showTextDialog", LuaScriptInterface::luaPlayerShowTextDialog);

	registerMethod(L, "Player", "sendTextMessage", LuaScriptInterface::luaPlayerSendTextMessage);
	registerMethod(L, "Player", "sendChannelMessage", LuaScriptInterface::luaPlayerSendChannelMessage);
	registerMethod(L, "Player", "sendPrivateMessage", LuaScriptInterface::luaPlayerSendPrivateMessage);
	registerMethod(L, "Player", "channelSay", LuaScriptInterface::luaPlayerChannelSay);
	registerMethod(L, "Player", "openChannel", LuaScriptInterface::luaPlayerOpenChannel);

	registerMethod(L, "Player", "getSlotItem", LuaScriptInterface::luaPlayerGetSlotItem);

	registerMethod(L, "Player", "getParty", LuaScriptInterface::luaPlayerGetParty);

	registerMethod(L, "Player", "addOutfit", LuaScriptInterface::luaPlayerAddOutfit);
	registerMethod(L, "Player", "addOutfitAddon", LuaScriptInterface::luaPlayerAddOutfitAddon);
	registerMethod(L, "Player", "removeOutfit", LuaScriptInterface::luaPlayerRemoveOutfit);
	registerMethod(L, "Player", "removeOutfitAddon", LuaScriptInterface::luaPlayerRemoveOutfitAddon);
	registerMethod(L, "Player", "hasOutfit", LuaScriptInterface::luaPlayerHasOutfit);
	registerMethod(L, "Player", "canWearOutfit", LuaScriptInterface::luaPlayerCanWearOutfit);
	registerMethod(L, "Player", "sendOutfitWindow", LuaScriptInterface::luaPlayerSendOutfitWindow);

	registerMethod(L, "Player", "sendEditPodium", LuaScriptInterface::luaPlayerSendEditPodium);

	registerMethod(L, "Player", "addMount", LuaScriptInterface::luaPlayerAddMount);
	registerMethod(L, "Player", "removeMount", LuaScriptInterface::luaPlayerRemoveMount);
	registerMethod(L, "Player", "hasMount", LuaScriptInterface::luaPlayerHasMount);
	registerMethod(L, "Player", "toggleMount", LuaScriptInterface::luaPlayerToggleMount);

	registerMethod(L, "Player", "getPremiumEndsAt", LuaScriptInterface::luaPlayerGetPremiumEndsAt);
	registerMethod(L, "Player", "setPremiumEndsAt", LuaScriptInterface::luaPlayerSetPremiumEndsAt);

	registerMethod(L, "Player", "hasBlessing", LuaScriptInterface::luaPlayerHasBlessing);
	registerMethod(L, "Player", "addBlessing", LuaScriptInterface::luaPlayerAddBlessing);
	registerMethod(L, "Player", "removeBlessing", LuaScriptInterface::luaPlayerRemoveBlessing);

	registerMethod(L, "Player", "canLearnSpell", LuaScriptInterface::luaPlayerCanLearnSpell);
	registerMethod(L, "Player", "learnSpell", LuaScriptInterface::luaPlayerLearnSpell);
	registerMethod(L, "Player", "forgetSpell", LuaScriptInterface::luaPlayerForgetSpell);
	registerMethod(L, "Player", "hasLearnedSpell", LuaScriptInterface::luaPlayerHasLearnedSpell);

	registerMethod(L, "Player", "sendTutorial", LuaScriptInterface::luaPlayerSendTutorial);
	registerMethod(L, "Player", "addMapMark", LuaScriptInterface::luaPlayerAddMapMark);

	registerMethod(L, "Player", "save", LuaScriptInterface::luaPlayerSave);
	registerMethod(L, "Player", "popupFYI", LuaScriptInterface::luaPlayerPopupFYI);

	registerMethod(L, "Player", "isPzLocked", LuaScriptInterface::luaPlayerIsPzLocked);

	registerMethod(L, "Player", "getClient", LuaScriptInterface::luaPlayerGetClient);

	registerMethod(L, "Player", "getHouse", LuaScriptInterface::luaPlayerGetHouse);
	registerMethod(L, "Player", "sendHouseWindow", LuaScriptInterface::luaPlayerSendHouseWindow);
	registerMethod(L, "Player", "setEditHouse", LuaScriptInterface::luaPlayerSetEditHouse);

	registerMethod(L, "Player", "setGhostMode", LuaScriptInterface::luaPlayerSetGhostMode);

	registerMethod(L, "Player", "getContainerId", LuaScriptInterface::luaPlayerGetContainerId);
	registerMethod(L, "Player", "getContainerById", LuaScriptInterface::luaPlayerGetContainerById);
	registerMethod(L, "Player", "getContainerIndex", LuaScriptInterface::luaPlayerGetContainerIndex);

	registerMethod(L, "Player", "getInstantSpells", LuaScriptInterface::luaPlayerGetInstantSpells);
	registerMethod(L, "Player", "canCast", LuaScriptInterface::luaPlayerCanCast);

	registerMethod(L, "Player", "hasChaseMode", LuaScriptInterface::luaPlayerHasChaseMode);
	registerMethod(L, "Player", "hasSecureMode", LuaScriptInterface::luaPlayerHasSecureMode);
	registerMethod(L, "Player", "getFightMode", LuaScriptInterface::luaPlayerGetFightMode);

	registerMethod(L, "Player", "getStoreInbox", LuaScriptInterface::luaPlayerGetStoreInbox);

	registerMethod(L, "Player", "isNearDepotBox", LuaScriptInterface::luaPlayerIsNearDepotBox);

	registerMethod(L, "Player", "getIdleTime", LuaScriptInterface::luaPlayerGetIdleTime);
	registerMethod(L, "Player", "resetIdleTime", LuaScriptInterface::luaPlayerResetIdleTime);

	registerMethod(L, "Player", "sendCreatureSquare", LuaScriptInterface::luaPlayerSendCreatureSquare);

	registerMethod(L, "Player", "getClientExpDisplay", LuaScriptInterface::luaPlayerGetClientExpDisplay);
	registerMethod(L, "Player", "setClientExpDisplay", LuaScriptInterface::luaPlayerSetClientExpDisplay);

	registerMethod(L, "Player", "getClientStaminaBonusDisplay",
	               LuaScriptInterface::luaPlayerGetClientStaminaBonusDisplay);
	registerMethod(L, "Player", "setClientStaminaBonusDisplay",
	               LuaScriptInterface::luaPlayerSetClientStaminaBonusDisplay);

	registerMethod(L, "Player", "getClientLowLevelBonusDisplay",
	               LuaScriptInterface::luaPlayerGetClientLowLevelBonusDisplay);
	registerMethod(L, "Player", "setClientLowLevelBonusDisplay",
	               LuaScriptInterface::luaPlayerSetClientLowLevelBonusDisplay);

	registerMethod(L, "Player", "sendResourceBalance", LuaScriptInterface::luaPlayerSendResourceBalance);

	// Monster
	registerClass(L, "Monster", "Creature", LuaScriptInterface::luaMonsterCreate);
	registerMetaMethod(L, "Monster", "__eq", LuaScriptInterface::luaUserdataCompare);

	registerMethod(L, "Monster", "isMonster", LuaScriptInterface::luaMonsterIsMonster);

	registerMethod(L, "Monster", "getId", LuaScriptInterface::luaMonsterGetId);
	registerMethod(L, "Monster", "getType", LuaScriptInterface::luaMonsterGetType);

	registerMethod(L, "Monster", "rename", LuaScriptInterface::luaMonsterRename);

	registerMethod(L, "Monster", "getSpawnPosition", LuaScriptInterface::luaMonsterGetSpawnPosition);
	registerMethod(L, "Monster", "isInSpawnRange", LuaScriptInterface::luaMonsterIsInSpawnRange);

	registerMethod(L, "Monster", "isIdle", LuaScriptInterface::luaMonsterIsIdle);
	registerMethod(L, "Monster", "setIdle", LuaScriptInterface::luaMonsterSetIdle);

	registerMethod(L, "Monster", "isTarget", LuaScriptInterface::luaMonsterIsTarget);
	registerMethod(L, "Monster", "isOpponent", LuaScriptInterface::luaMonsterIsOpponent);
	registerMethod(L, "Monster", "isFriend", LuaScriptInterface::luaMonsterIsFriend);

	registerMethod(L, "Monster", "addFriend", LuaScriptInterface::luaMonsterAddFriend);
	registerMethod(L, "Monster", "removeFriend", LuaScriptInterface::luaMonsterRemoveFriend);
	registerMethod(L, "Monster", "getFriendList", LuaScriptInterface::luaMonsterGetFriendList);
	registerMethod(L, "Monster", "getFriendCount", LuaScriptInterface::luaMonsterGetFriendCount);

	registerMethod(L, "Monster", "addTarget", LuaScriptInterface::luaMonsterAddTarget);
	registerMethod(L, "Monster", "removeTarget", LuaScriptInterface::luaMonsterRemoveTarget);
	registerMethod(L, "Monster", "getTargetList", LuaScriptInterface::luaMonsterGetTargetList);
	registerMethod(L, "Monster", "getTargetCount", LuaScriptInterface::luaMonsterGetTargetCount);

	registerMethod(L, "Monster", "selectTarget", LuaScriptInterface::luaMonsterSelectTarget);
	registerMethod(L, "Monster", "searchTarget", LuaScriptInterface::luaMonsterSearchTarget);

	registerMethod(L, "Monster", "isWalkingToSpawn", LuaScriptInterface::luaMonsterIsWalkingToSpawn);
	registerMethod(L, "Monster", "walkToSpawn", LuaScriptInterface::luaMonsterWalkToSpawn);

	registerMethod(L, "Monster", "hasSpecialIcon", LuaScriptInterface::luaMonsterHasIcon);
	registerMethod(L, "Monster", "setSpecialIcon", LuaScriptInterface::luaMonsterSetIcon);
	registerMethod(L, "Monster", "getSpecialIcon", LuaScriptInterface::luaMonsterGetIcon);
	registerMethod(L, "Monster", "removeSpecialIcon", LuaScriptInterface::luaMonsterRemoveIcon);

	// Npc
	registerClass(L, "Npc", "Creature", LuaScriptInterface::luaNpcCreate);
	registerMetaMethod(L, "Npc", "__eq", LuaScriptInterface::luaUserdataCompare);

	registerMethod(L, "Npc", "isNpc", LuaScriptInterface::luaNpcIsNpc);

	registerMethod(L, "Npc", "setMasterPos", LuaScriptInterface::luaNpcSetMasterPos);

	registerMethod(L, "Npc", "getSpeechBubble", LuaScriptInterface::luaNpcGetSpeechBubble);
	registerMethod(L, "Npc", "setSpeechBubble", LuaScriptInterface::luaNpcSetSpeechBubble);

	registerMethod(L, "Npc", "getSpectators", LuaScriptInterface::luaNpcGetSpectators);

	// NpcType
	registerClass(L, "NpcType", "", LuaScriptInterface::luaNpcTypeCreate);
	registerMethod(L, "NpcType", "name", LuaScriptInterface::luaNpcTypeName);

	registerMethod(L, "NpcType", "eventType", LuaScriptInterface::luaNpcTypeEventType);
	registerMethod(L, "NpcType", "onSay", LuaScriptInterface::luaNpcTypeOnCallback);
	registerMethod(L, "NpcType", "onDisappear", LuaScriptInterface::luaNpcTypeOnCallback);
	registerMethod(L, "NpcType", "onAppear", LuaScriptInterface::luaNpcTypeOnCallback);
	registerMethod(L, "NpcType", "onMove", LuaScriptInterface::luaNpcTypeOnCallback);
	registerMethod(L, "NpcType", "onPlayerCloseChannel", LuaScriptInterface::luaNpcTypeOnCallback);
	registerMethod(L, "NpcType", "onPlayerEndTrade", LuaScriptInterface::luaNpcTypeOnCallback);
	registerMethod(L, "NpcType", "onThink", LuaScriptInterface::luaNpcTypeOnCallback);
	registerMethod(L, "NpcType", "onSight", LuaScriptInterface::luaNpcTypeOnCallback);
	registerMethod(L, "NpcType", "onSpeechBubble", LuaScriptInterface::luaNpcTypeOnCallback);

	registerMethod(L, "NpcType", "speechBubble", LuaScriptInterface::luaNpcTypeSpeechBubble);
	registerMethod(L, "NpcType", "walkInterval", LuaScriptInterface::luaNpcTypeWalkTicks);
	registerMethod(L, "NpcType", "walkSpeed", LuaScriptInterface::luaNpcTypeBaseSpeed);
	registerMethod(L, "NpcType", "spawnRadius", LuaScriptInterface::luaNpcTypeMasterRadius);
	registerMethod(L, "NpcType", "floorChange", LuaScriptInterface::luaNpcTypeFloorChange);
	registerMethod(L, "NpcType", "attackable", LuaScriptInterface::luaNpcTypeAttackable);
	registerMethod(L, "NpcType", "ignoreHeight", LuaScriptInterface::luaNpcTypeIgnoreHeight);
	registerMethod(L, "NpcType", "isIdle", LuaScriptInterface::luaNpcTypeIsIdle);
	registerMethod(L, "NpcType", "pushable", LuaScriptInterface::luaNpcTypePushable);
	registerMethod(L, "NpcType", "outfit", LuaScriptInterface::luaNpcTypeDefaultOutfit);
	registerMethod(L, "NpcType", "parameters", LuaScriptInterface::luaNpcTypeParameter);
	registerMethod(L, "NpcType", "health", LuaScriptInterface::luaNpcTypeHealth);
	registerMethod(L, "NpcType", "maxHealth", LuaScriptInterface::luaNpcTypeMaxHealth);
	registerMethod(L, "NpcType", "sight", LuaScriptInterface::luaNpcTypeSight);

	// Guild
	registerClass(L, "Guild", "", LuaScriptInterface::luaGuildCreate);
	registerMetaMethod(L, "Guild", "__eq", LuaScriptInterface::luaUserdataCompare);

	registerMethod(L, "Guild", "getId", LuaScriptInterface::luaGuildGetId);
	registerMethod(L, "Guild", "getName", LuaScriptInterface::luaGuildGetName);
	registerMethod(L, "Guild", "getMembersOnline", LuaScriptInterface::luaGuildGetMembersOnline);

	registerMethod(L, "Guild", "addRank", LuaScriptInterface::luaGuildAddRank);
	registerMethod(L, "Guild", "getRankById", LuaScriptInterface::luaGuildGetRankById);
	registerMethod(L, "Guild", "getRankByLevel", LuaScriptInterface::luaGuildGetRankByLevel);

	registerMethod(L, "Guild", "getMotd", LuaScriptInterface::luaGuildGetMotd);
	registerMethod(L, "Guild", "setMotd", LuaScriptInterface::luaGuildSetMotd);

	// Group
	registerClass(L, "Group", "", LuaScriptInterface::luaGroupCreate);
	registerMetaMethod(L, "Group", "__eq", LuaScriptInterface::luaUserdataCompare);

	registerMethod(L, "Group", "getId", LuaScriptInterface::luaGroupGetId);
	registerMethod(L, "Group", "getName", LuaScriptInterface::luaGroupGetName);
	registerMethod(L, "Group", "getFlags", LuaScriptInterface::luaGroupGetFlags);
	registerMethod(L, "Group", "getAccess", LuaScriptInterface::luaGroupGetAccess);
	registerMethod(L, "Group", "getMaxDepotItems", LuaScriptInterface::luaGroupGetMaxDepotItems);
	registerMethod(L, "Group", "getMaxVipEntries", LuaScriptInterface::luaGroupGetMaxVipEntries);
	registerMethod(L, "Group", "hasFlag", LuaScriptInterface::luaGroupHasFlag);

	// Vocation
	registerClass(L, "Vocation", "", LuaScriptInterface::luaVocationCreate);
	registerMetaMethod(L, "Vocation", "__eq", LuaScriptInterface::luaUserdataCompare);

	registerMethod(L, "Vocation", "getId", LuaScriptInterface::luaVocationGetId);
	registerMethod(L, "Vocation", "getClientId", LuaScriptInterface::luaVocationGetClientId);
	registerMethod(L, "Vocation", "getName", LuaScriptInterface::luaVocationGetName);
	registerMethod(L, "Vocation", "getDescription", LuaScriptInterface::luaVocationGetDescription);

	registerMethod(L, "Vocation", "getRequiredSkillTries", LuaScriptInterface::luaVocationGetRequiredSkillTries);
	registerMethod(L, "Vocation", "getRequiredManaSpent", LuaScriptInterface::luaVocationGetRequiredManaSpent);

	registerMethod(L, "Vocation", "getCapacityGain", LuaScriptInterface::luaVocationGetCapacityGain);

	registerMethod(L, "Vocation", "getHealthGain", LuaScriptInterface::luaVocationGetHealthGain);
	registerMethod(L, "Vocation", "getHealthGainTicks", LuaScriptInterface::luaVocationGetHealthGainTicks);
	registerMethod(L, "Vocation", "getHealthGainAmount", LuaScriptInterface::luaVocationGetHealthGainAmount);

	registerMethod(L, "Vocation", "getManaGain", LuaScriptInterface::luaVocationGetManaGain);
	registerMethod(L, "Vocation", "getManaGainTicks", LuaScriptInterface::luaVocationGetManaGainTicks);
	registerMethod(L, "Vocation", "getManaGainAmount", LuaScriptInterface::luaVocationGetManaGainAmount);

	registerMethod(L, "Vocation", "getMaxSoul", LuaScriptInterface::luaVocationGetMaxSoul);
	registerMethod(L, "Vocation", "getSoulGainTicks", LuaScriptInterface::luaVocationGetSoulGainTicks);

	registerMethod(L, "Vocation", "getAttackSpeed", LuaScriptInterface::luaVocationGetAttackSpeed);
	registerMethod(L, "Vocation", "getBaseSpeed", LuaScriptInterface::luaVocationGetBaseSpeed);

	registerMethod(L, "Vocation", "getDemotion", LuaScriptInterface::luaVocationGetDemotion);
	registerMethod(L, "Vocation", "getPromotion", LuaScriptInterface::luaVocationGetPromotion);

	registerMethod(L, "Vocation", "allowsPvp", LuaScriptInterface::luaVocationAllowsPvp);

	// Town
	registerClass(L, "Town", "", LuaScriptInterface::luaTownCreate);
	registerMetaMethod(L, "Town", "__eq", LuaScriptInterface::luaUserdataCompare);

	registerMethod(L, "Town", "getId", LuaScriptInterface::luaTownGetId);
	registerMethod(L, "Town", "getName", LuaScriptInterface::luaTownGetName);
	registerMethod(L, "Town", "getTemplePosition", LuaScriptInterface::luaTownGetTemplePosition);

	// House
	registerClass(L, "House", "", LuaScriptInterface::luaHouseCreate);
	registerMetaMethod(L, "House", "__eq", LuaScriptInterface::luaUserdataCompare);

	registerMethod(L, "House", "getId", LuaScriptInterface::luaHouseGetId);
	registerMethod(L, "House", "getName", LuaScriptInterface::luaHouseGetName);
	registerMethod(L, "House", "getTown", LuaScriptInterface::luaHouseGetTown);
	registerMethod(L, "House", "getExitPosition", LuaScriptInterface::luaHouseGetExitPosition);

	registerMethod(L, "House", "getRent", LuaScriptInterface::luaHouseGetRent);
	registerMethod(L, "House", "setRent", LuaScriptInterface::luaHouseSetRent);

	registerMethod(L, "House", "getPaidUntil", LuaScriptInterface::luaHouseGetPaidUntil);
	registerMethod(L, "House", "setPaidUntil", LuaScriptInterface::luaHouseSetPaidUntil);

	registerMethod(L, "House", "getPayRentWarnings", LuaScriptInterface::luaHouseGetPayRentWarnings);
	registerMethod(L, "House", "setPayRentWarnings", LuaScriptInterface::luaHouseSetPayRentWarnings);

	registerMethod(L, "House", "getOwnerName", LuaScriptInterface::luaHouseGetOwnerName);
	registerMethod(L, "House", "getOwnerGuid", LuaScriptInterface::luaHouseGetOwnerGuid);
	registerMethod(L, "House", "setOwnerGuid", LuaScriptInterface::luaHouseSetOwnerGuid);
	registerMethod(L, "House", "startTrade", LuaScriptInterface::luaHouseStartTrade);

	registerMethod(L, "House", "getBeds", LuaScriptInterface::luaHouseGetBeds);
	registerMethod(L, "House", "getBedCount", LuaScriptInterface::luaHouseGetBedCount);

	registerMethod(L, "House", "getDoors", LuaScriptInterface::luaHouseGetDoors);
	registerMethod(L, "House", "getDoorCount", LuaScriptInterface::luaHouseGetDoorCount);
	registerMethod(L, "House", "getDoorIdByPosition", LuaScriptInterface::luaHouseGetDoorIdByPosition);

	registerMethod(L, "House", "getTiles", LuaScriptInterface::luaHouseGetTiles);
	registerMethod(L, "House", "getItems", LuaScriptInterface::luaHouseGetItems);
	registerMethod(L, "House", "getTileCount", LuaScriptInterface::luaHouseGetTileCount);

	registerMethod(L, "House", "canEditAccessList", LuaScriptInterface::luaHouseCanEditAccessList);
	registerMethod(L, "House", "getAccessList", LuaScriptInterface::luaHouseGetAccessList);
	registerMethod(L, "House", "setAccessList", LuaScriptInterface::luaHouseSetAccessList);

	registerMethod(L, "House", "kickPlayer", LuaScriptInterface::luaHouseKickPlayer);

	registerMethod(L, "House", "save", LuaScriptInterface::luaHouseSave);

	// ItemType
	registerClass(L, "ItemType", "", LuaScriptInterface::luaItemTypeCreate);
	registerMetaMethod(L, "ItemType", "__eq", LuaScriptInterface::luaUserdataCompare);

	registerMethod(L, "ItemType", "isCorpse", LuaScriptInterface::luaItemTypeIsCorpse);
	registerMethod(L, "ItemType", "isDoor", LuaScriptInterface::luaItemTypeIsDoor);
	registerMethod(L, "ItemType", "isContainer", LuaScriptInterface::luaItemTypeIsContainer);
	registerMethod(L, "ItemType", "isFluidContainer", LuaScriptInterface::luaItemTypeIsFluidContainer);
	registerMethod(L, "ItemType", "isMovable", LuaScriptInterface::luaItemTypeIsMovable);
	registerMethod(L, "ItemType", "isRune", LuaScriptInterface::luaItemTypeIsRune);
	registerMethod(L, "ItemType", "isStackable", LuaScriptInterface::luaItemTypeIsStackable);
	registerMethod(L, "ItemType", "isReadable", LuaScriptInterface::luaItemTypeIsReadable);
	registerMethod(L, "ItemType", "isWritable", LuaScriptInterface::luaItemTypeIsWritable);
	registerMethod(L, "ItemType", "isBlocking", LuaScriptInterface::luaItemTypeIsBlocking);
	registerMethod(L, "ItemType", "isGroundTile", LuaScriptInterface::luaItemTypeIsGroundTile);
	registerMethod(L, "ItemType", "isMagicField", LuaScriptInterface::luaItemTypeIsMagicField);
	registerMethod(L, "ItemType", "isUseable", LuaScriptInterface::luaItemTypeIsUseable);
	registerMethod(L, "ItemType", "isPickupable", LuaScriptInterface::luaItemTypeIsPickupable);
	registerMethod(L, "ItemType", "isRotatable", LuaScriptInterface::luaItemTypeIsRotatable);

	registerMethod(L, "ItemType", "getType", LuaScriptInterface::luaItemTypeGetType);
	registerMethod(L, "ItemType", "getGroup", LuaScriptInterface::luaItemTypeGetGroup);
	registerMethod(L, "ItemType", "getId", LuaScriptInterface::luaItemTypeGetId);
	registerMethod(L, "ItemType", "getClientId", LuaScriptInterface::luaItemTypeGetClientId);
	registerMethod(L, "ItemType", "getName", LuaScriptInterface::luaItemTypeGetName);
	registerMethod(L, "ItemType", "getPluralName", LuaScriptInterface::luaItemTypeGetPluralName);
	registerMethod(L, "ItemType", "getRotateTo", LuaScriptInterface::luaItemTypeGetRotateTo);
	registerMethod(L, "ItemType", "getArticle", LuaScriptInterface::luaItemTypeGetArticle);
	registerMethod(L, "ItemType", "getDescription", LuaScriptInterface::luaItemTypeGetDescription);
	registerMethod(L, "ItemType", "getSlotPosition", LuaScriptInterface::luaItemTypeGetSlotPosition);

	registerMethod(L, "ItemType", "getCharges", LuaScriptInterface::luaItemTypeGetCharges);
	registerMethod(L, "ItemType", "getFluidSource", LuaScriptInterface::luaItemTypeGetFluidSource);
	registerMethod(L, "ItemType", "getCapacity", LuaScriptInterface::luaItemTypeGetCapacity);
	registerMethod(L, "ItemType", "getWeight", LuaScriptInterface::luaItemTypeGetWeight);
	registerMethod(L, "ItemType", "getWorth", LuaScriptInterface::luaItemTypeGetWorth);

	registerMethod(L, "ItemType", "getHitChance", LuaScriptInterface::luaItemTypeGetHitChance);
	registerMethod(L, "ItemType", "getShootRange", LuaScriptInterface::luaItemTypeGetShootRange);

	registerMethod(L, "ItemType", "getAttack", LuaScriptInterface::luaItemTypeGetAttack);
	registerMethod(L, "ItemType", "getAttackSpeed", LuaScriptInterface::luaItemTypeGetAttackSpeed);
	registerMethod(L, "ItemType", "getDefense", LuaScriptInterface::luaItemTypeGetDefense);
	registerMethod(L, "ItemType", "getExtraDefense", LuaScriptInterface::luaItemTypeGetExtraDefense);
	registerMethod(L, "ItemType", "getArmor", LuaScriptInterface::luaItemTypeGetArmor);
	registerMethod(L, "ItemType", "getWeaponType", LuaScriptInterface::luaItemTypeGetWeaponType);

	registerMethod(L, "ItemType", "getElementType", LuaScriptInterface::luaItemTypeGetElementType);
	registerMethod(L, "ItemType", "getElementDamage", LuaScriptInterface::luaItemTypeGetElementDamage);

	registerMethod(L, "ItemType", "getTransformEquipId", LuaScriptInterface::luaItemTypeGetTransformEquipId);
	registerMethod(L, "ItemType", "getTransformDeEquipId", LuaScriptInterface::luaItemTypeGetTransformDeEquipId);
	registerMethod(L, "ItemType", "getDestroyId", LuaScriptInterface::luaItemTypeGetDestroyId);
	registerMethod(L, "ItemType", "getDecayId", LuaScriptInterface::luaItemTypeGetDecayId);
	registerMethod(L, "ItemType", "getRequiredLevel", LuaScriptInterface::luaItemTypeGetRequiredLevel);
	registerMethod(L, "ItemType", "getAmmoType", LuaScriptInterface::luaItemTypeGetAmmoType);
	registerMethod(L, "ItemType", "getCorpseType", LuaScriptInterface::luaItemTypeGetCorpseType);
	registerMethod(L, "ItemType", "getClassification", LuaScriptInterface::luaItemTypeGetClassification);

	registerMethod(L, "ItemType", "getAbilities", LuaScriptInterface::luaItemTypeGetAbilities);

	registerMethod(L, "ItemType", "hasShowAttributes", LuaScriptInterface::luaItemTypeHasShowAttributes);
	registerMethod(L, "ItemType", "hasShowCount", LuaScriptInterface::luaItemTypeHasShowCount);
	registerMethod(L, "ItemType", "hasShowCharges", LuaScriptInterface::luaItemTypeHasShowCharges);
	registerMethod(L, "ItemType", "hasShowDuration", LuaScriptInterface::luaItemTypeHasShowDuration);
	registerMethod(L, "ItemType", "hasAllowDistRead", LuaScriptInterface::luaItemTypeHasAllowDistRead);
	registerMethod(L, "ItemType", "getWieldInfo", LuaScriptInterface::luaItemTypeGetWieldInfo);
	registerMethod(L, "ItemType", "getDurationMin", LuaScriptInterface::luaItemTypeGetDurationMin);
	registerMethod(L, "ItemType", "getDurationMax", LuaScriptInterface::luaItemTypeGetDurationMax);
	registerMethod(L, "ItemType", "getLevelDoor", LuaScriptInterface::luaItemTypeGetLevelDoor);
	registerMethod(L, "ItemType", "getRuneSpellName", LuaScriptInterface::luaItemTypeGetRuneSpellName);
	registerMethod(L, "ItemType", "getVocationString", LuaScriptInterface::luaItemTypeGetVocationString);
	registerMethod(L, "ItemType", "getMinReqLevel", LuaScriptInterface::luaItemTypeGetMinReqLevel);
	registerMethod(L, "ItemType", "getMinReqMagicLevel", LuaScriptInterface::luaItemTypeGetMinReqMagicLevel);
	registerMethod(L, "ItemType", "getMarketBuyStatistics", LuaScriptInterface::luaItemTypeGetMarketBuyStatistics);
	registerMethod(L, "ItemType", "getMarketSellStatistics", LuaScriptInterface::luaItemTypeGetMarketSellStatistics);

	registerMethod(L, "ItemType", "hasSubType", LuaScriptInterface::luaItemTypeHasSubType);

	registerMethod(L, "ItemType", "isStoreItem", LuaScriptInterface::luaItemTypeIsStoreItem);

	// Combat
	registerClass(L, "Combat", "", LuaScriptInterface::luaCombatCreate);
	registerMetaMethod(L, "Combat", "__eq", LuaScriptInterface::luaUserdataCompare);
	registerMetaMethod(L, "Combat", "__gc", LuaScriptInterface::luaCombatDelete);
	registerMethod(L, "Combat", "delete", LuaScriptInterface::luaCombatDelete);

	registerMethod(L, "Combat", "setParameter", LuaScriptInterface::luaCombatSetParameter);
	registerMethod(L, "Combat", "getParameter", LuaScriptInterface::luaCombatGetParameter);

	registerMethod(L, "Combat", "setFormula", LuaScriptInterface::luaCombatSetFormula);

	registerMethod(L, "Combat", "setArea", LuaScriptInterface::luaCombatSetArea);
	registerMethod(L, "Combat", "addCondition", LuaScriptInterface::luaCombatAddCondition);
	registerMethod(L, "Combat", "clearConditions", LuaScriptInterface::luaCombatClearConditions);
	registerMethod(L, "Combat", "setCallback", LuaScriptInterface::luaCombatSetCallback);
	registerMethod(L, "Combat", "setOrigin", LuaScriptInterface::luaCombatSetOrigin);

	registerMethod(L, "Combat", "execute", LuaScriptInterface::luaCombatExecute);

	// Condition
	registerClass(L, "Condition", "", LuaScriptInterface::luaConditionCreate);
	registerMetaMethod(L, "Condition", "__eq", LuaScriptInterface::luaUserdataCompare);
	registerMetaMethod(L, "Condition", "__gc", LuaScriptInterface::luaConditionDelete);

	registerMethod(L, "Condition", "getId", LuaScriptInterface::luaConditionGetId);
	registerMethod(L, "Condition", "getSubId", LuaScriptInterface::luaConditionGetSubId);
	registerMethod(L, "Condition", "getType", LuaScriptInterface::luaConditionGetType);
	registerMethod(L, "Condition", "getIcons", LuaScriptInterface::luaConditionGetIcons);
	registerMethod(L, "Condition", "getEndTime", LuaScriptInterface::luaConditionGetEndTime);

	registerMethod(L, "Condition", "clone", LuaScriptInterface::luaConditionClone);

	registerMethod(L, "Condition", "getTicks", LuaScriptInterface::luaConditionGetTicks);
	registerMethod(L, "Condition", "setTicks", LuaScriptInterface::luaConditionSetTicks);

	registerMethod(L, "Condition", "setParameter", LuaScriptInterface::luaConditionSetParameter);
	registerMethod(L, "Condition", "getParameter", LuaScriptInterface::luaConditionGetParameter);

	registerMethod(L, "Condition", "setFormula", LuaScriptInterface::luaConditionSetFormula);
	registerMethod(L, "Condition", "setOutfit", LuaScriptInterface::luaConditionSetOutfit);

	registerMethod(L, "Condition", "addDamage", LuaScriptInterface::luaConditionAddDamage);

	// Outfit
	registerClass(L, "Outfit", "", LuaScriptInterface::luaOutfitCreate);
	registerMetaMethod(L, "Outfit", "__eq", LuaScriptInterface::luaOutfitCompare);

	// MonsterType
	registerClass(L, "MonsterType", "", LuaScriptInterface::luaMonsterTypeCreate);
	registerMetaMethod(L, "MonsterType", "__eq", LuaScriptInterface::luaUserdataCompare);

	registerMethod(L, "MonsterType", "isAttackable", LuaScriptInterface::luaMonsterTypeIsAttackable);
	registerMethod(L, "MonsterType", "isChallengeable", LuaScriptInterface::luaMonsterTypeIsChallengeable);
	registerMethod(L, "MonsterType", "isConvinceable", LuaScriptInterface::luaMonsterTypeIsConvinceable);
	registerMethod(L, "MonsterType", "isSummonable", LuaScriptInterface::luaMonsterTypeIsSummonable);
	registerMethod(L, "MonsterType", "isIgnoringSpawnBlock", LuaScriptInterface::luaMonsterTypeIsIgnoringSpawnBlock);
	registerMethod(L, "MonsterType", "isIllusionable", LuaScriptInterface::luaMonsterTypeIsIllusionable);
	registerMethod(L, "MonsterType", "isHostile", LuaScriptInterface::luaMonsterTypeIsHostile);
	registerMethod(L, "MonsterType", "isPushable", LuaScriptInterface::luaMonsterTypeIsPushable);
	registerMethod(L, "MonsterType", "isHealthHidden", LuaScriptInterface::luaMonsterTypeIsHealthHidden);
	registerMethod(L, "MonsterType", "isBoss", LuaScriptInterface::luaMonsterTypeIsBoss);

	registerMethod(L, "MonsterType", "canPushItems", LuaScriptInterface::luaMonsterTypeCanPushItems);
	registerMethod(L, "MonsterType", "canPushCreatures", LuaScriptInterface::luaMonsterTypeCanPushCreatures);

	registerMethod(L, "MonsterType", "canWalkOnEnergy", LuaScriptInterface::luaMonsterTypeCanWalkOnEnergy);
	registerMethod(L, "MonsterType", "canWalkOnFire", LuaScriptInterface::luaMonsterTypeCanWalkOnFire);
	registerMethod(L, "MonsterType", "canWalkOnPoison", LuaScriptInterface::luaMonsterTypeCanWalkOnPoison);

	registerMethod(L, "MonsterType", "name", LuaScriptInterface::luaMonsterTypeName);
	registerMethod(L, "MonsterType", "nameDescription", LuaScriptInterface::luaMonsterTypeNameDescription);

	registerMethod(L, "MonsterType", "health", LuaScriptInterface::luaMonsterTypeHealth);
	registerMethod(L, "MonsterType", "maxHealth", LuaScriptInterface::luaMonsterTypeMaxHealth);
	registerMethod(L, "MonsterType", "runHealth", LuaScriptInterface::luaMonsterTypeRunHealth);
	registerMethod(L, "MonsterType", "experience", LuaScriptInterface::luaMonsterTypeExperience);
	registerMethod(L, "MonsterType", "skull", LuaScriptInterface::luaMonsterTypeSkull);

	registerMethod(L, "MonsterType", "combatImmunities", LuaScriptInterface::luaMonsterTypeCombatImmunities);
	registerMethod(L, "MonsterType", "conditionImmunities", LuaScriptInterface::luaMonsterTypeConditionImmunities);

	registerMethod(L, "MonsterType", "getAttackList", LuaScriptInterface::luaMonsterTypeGetAttackList);
	registerMethod(L, "MonsterType", "addAttack", LuaScriptInterface::luaMonsterTypeAddAttack);

	registerMethod(L, "MonsterType", "getDefenseList", LuaScriptInterface::luaMonsterTypeGetDefenseList);
	registerMethod(L, "MonsterType", "addDefense", LuaScriptInterface::luaMonsterTypeAddDefense);

	registerMethod(L, "MonsterType", "getElementList", LuaScriptInterface::luaMonsterTypeGetElementList);
	registerMethod(L, "MonsterType", "addElement", LuaScriptInterface::luaMonsterTypeAddElement);

	registerMethod(L, "MonsterType", "getVoices", LuaScriptInterface::luaMonsterTypeGetVoices);
	registerMethod(L, "MonsterType", "addVoice", LuaScriptInterface::luaMonsterTypeAddVoice);

	registerMethod(L, "MonsterType", "getLoot", LuaScriptInterface::luaMonsterTypeGetLoot);
	registerMethod(L, "MonsterType", "addLoot", LuaScriptInterface::luaMonsterTypeAddLoot);

	registerMethod(L, "MonsterType", "getCreatureEvents", LuaScriptInterface::luaMonsterTypeGetCreatureEvents);
	registerMethod(L, "MonsterType", "registerEvent", LuaScriptInterface::luaMonsterTypeRegisterEvent);

	registerMethod(L, "MonsterType", "eventType", LuaScriptInterface::luaMonsterTypeEventType);
	registerMethod(L, "MonsterType", "onThink", LuaScriptInterface::luaMonsterTypeEventOnCallback);
	registerMethod(L, "MonsterType", "onAppear", LuaScriptInterface::luaMonsterTypeEventOnCallback);
	registerMethod(L, "MonsterType", "onDisappear", LuaScriptInterface::luaMonsterTypeEventOnCallback);
	registerMethod(L, "MonsterType", "onMove", LuaScriptInterface::luaMonsterTypeEventOnCallback);
	registerMethod(L, "MonsterType", "onSay", LuaScriptInterface::luaMonsterTypeEventOnCallback);

	registerMethod(L, "MonsterType", "getSummonList", LuaScriptInterface::luaMonsterTypeGetSummonList);
	registerMethod(L, "MonsterType", "addSummon", LuaScriptInterface::luaMonsterTypeAddSummon);

	registerMethod(L, "MonsterType", "maxSummons", LuaScriptInterface::luaMonsterTypeMaxSummons);

	registerMethod(L, "MonsterType", "armor", LuaScriptInterface::luaMonsterTypeArmor);
	registerMethod(L, "MonsterType", "defense", LuaScriptInterface::luaMonsterTypeDefense);
	registerMethod(L, "MonsterType", "outfit", LuaScriptInterface::luaMonsterTypeOutfit);
	registerMethod(L, "MonsterType", "race", LuaScriptInterface::luaMonsterTypeRace);
	registerMethod(L, "MonsterType", "corpseId", LuaScriptInterface::luaMonsterTypeCorpseId);
	registerMethod(L, "MonsterType", "manaCost", LuaScriptInterface::luaMonsterTypeManaCost);
	registerMethod(L, "MonsterType", "baseSpeed", LuaScriptInterface::luaMonsterTypeBaseSpeed);
	registerMethod(L, "MonsterType", "light", LuaScriptInterface::luaMonsterTypeLight);

	registerMethod(L, "MonsterType", "staticAttackChance", LuaScriptInterface::luaMonsterTypeStaticAttackChance);
	registerMethod(L, "MonsterType", "targetDistance", LuaScriptInterface::luaMonsterTypeTargetDistance);
	registerMethod(L, "MonsterType", "yellChance", LuaScriptInterface::luaMonsterTypeYellChance);
	registerMethod(L, "MonsterType", "yellSpeedTicks", LuaScriptInterface::luaMonsterTypeYellSpeedTicks);
	registerMethod(L, "MonsterType", "changeTargetChance", LuaScriptInterface::luaMonsterTypeChangeTargetChance);
	registerMethod(L, "MonsterType", "changeTargetSpeed", LuaScriptInterface::luaMonsterTypeChangeTargetSpeed);

	registerMethod(L, "MonsterType", "bestiaryInfo", LuaScriptInterface::luaMonsterTypeBestiaryInfo);

	// Loot
	registerClass(L, "Loot", "", LuaScriptInterface::luaCreateLoot);
	registerMetaMethod(L, "Loot", "__gc", LuaScriptInterface::luaDeleteLoot);
	registerMethod(L, "Loot", "delete", LuaScriptInterface::luaDeleteLoot);

	registerMethod(L, "Loot", "setId", LuaScriptInterface::luaLootSetId);
	registerMethod(L, "Loot", "setMaxCount", LuaScriptInterface::luaLootSetMaxCount);
	registerMethod(L, "Loot", "setSubType", LuaScriptInterface::luaLootSetSubType);
	registerMethod(L, "Loot", "setChance", LuaScriptInterface::luaLootSetChance);
	registerMethod(L, "Loot", "setActionId", LuaScriptInterface::luaLootSetActionId);
	registerMethod(L, "Loot", "setDescription", LuaScriptInterface::luaLootSetDescription);
	registerMethod(L, "Loot", "addChildLoot", LuaScriptInterface::luaLootAddChildLoot);

	// MonsterSpell
	registerClass(L, "MonsterSpell", "", LuaScriptInterface::luaCreateMonsterSpell);
	registerMetaMethod(L, "MonsterSpell", "__gc", LuaScriptInterface::luaDeleteMonsterSpell);
	registerMethod(L, "MonsterSpell", "delete", LuaScriptInterface::luaDeleteMonsterSpell);

	registerMethod(L, "MonsterSpell", "setType", LuaScriptInterface::luaMonsterSpellSetType);
	registerMethod(L, "MonsterSpell", "setScriptName", LuaScriptInterface::luaMonsterSpellSetScriptName);
	registerMethod(L, "MonsterSpell", "setChance", LuaScriptInterface::luaMonsterSpellSetChance);
	registerMethod(L, "MonsterSpell", "setInterval", LuaScriptInterface::luaMonsterSpellSetInterval);
	registerMethod(L, "MonsterSpell", "setRange", LuaScriptInterface::luaMonsterSpellSetRange);
	registerMethod(L, "MonsterSpell", "setCombatValue", LuaScriptInterface::luaMonsterSpellSetCombatValue);
	registerMethod(L, "MonsterSpell", "setCombatType", LuaScriptInterface::luaMonsterSpellSetCombatType);
	registerMethod(L, "MonsterSpell", "setAttackValue", LuaScriptInterface::luaMonsterSpellSetAttackValue);
	registerMethod(L, "MonsterSpell", "setNeedTarget", LuaScriptInterface::luaMonsterSpellSetNeedTarget);
	registerMethod(L, "MonsterSpell", "setNeedDirection", LuaScriptInterface::luaMonsterSpellSetNeedDirection);
	registerMethod(L, "MonsterSpell", "setCombatLength", LuaScriptInterface::luaMonsterSpellSetCombatLength);
	registerMethod(L, "MonsterSpell", "setCombatSpread", LuaScriptInterface::luaMonsterSpellSetCombatSpread);
	registerMethod(L, "MonsterSpell", "setCombatRadius", LuaScriptInterface::luaMonsterSpellSetCombatRadius);
	registerMethod(L, "MonsterSpell", "setCombatRing", LuaScriptInterface::luaMonsterSpellSetCombatRing);
	registerMethod(L, "MonsterSpell", "setConditionType", LuaScriptInterface::luaMonsterSpellSetConditionType);
	registerMethod(L, "MonsterSpell", "setConditionDamage", LuaScriptInterface::luaMonsterSpellSetConditionDamage);
	registerMethod(L, "MonsterSpell", "setConditionSpeedChange",
	               LuaScriptInterface::luaMonsterSpellSetConditionSpeedChange);
	registerMethod(L, "MonsterSpell", "setConditionDuration", LuaScriptInterface::luaMonsterSpellSetConditionDuration);
	registerMethod(L, "MonsterSpell", "setConditionDrunkenness",
	               LuaScriptInterface::luaMonsterSpellSetConditionDrunkenness);
	registerMethod(L, "MonsterSpell", "setConditionTickInterval",
	               LuaScriptInterface::luaMonsterSpellSetConditionTickInterval);
	registerMethod(L, "MonsterSpell", "setCombatShootEffect", LuaScriptInterface::luaMonsterSpellSetCombatShootEffect);
	registerMethod(L, "MonsterSpell", "setCombatEffect", LuaScriptInterface::luaMonsterSpellSetCombatEffect);
	registerMethod(L, "MonsterSpell", "setOutfit", LuaScriptInterface::luaMonsterSpellSetOutfit);

	// Party
	registerClass(L, "Party", "", LuaScriptInterface::luaPartyCreate);
	registerMetaMethod(L, "Party", "__eq", LuaScriptInterface::luaUserdataCompare);

	registerMethod(L, "Party", "disband", LuaScriptInterface::luaPartyDisband);

	registerMethod(L, "Party", "getLeader", LuaScriptInterface::luaPartyGetLeader);
	registerMethod(L, "Party", "setLeader", LuaScriptInterface::luaPartySetLeader);

	registerMethod(L, "Party", "getMembers", LuaScriptInterface::luaPartyGetMembers);
	registerMethod(L, "Party", "getMemberCount", LuaScriptInterface::luaPartyGetMemberCount);

	registerMethod(L, "Party", "getInvitees", LuaScriptInterface::luaPartyGetInvitees);
	registerMethod(L, "Party", "getInviteeCount", LuaScriptInterface::luaPartyGetInviteeCount);

	registerMethod(L, "Party", "addInvite", LuaScriptInterface::luaPartyAddInvite);
	registerMethod(L, "Party", "removeInvite", LuaScriptInterface::luaPartyRemoveInvite);

	registerMethod(L, "Party", "addMember", LuaScriptInterface::luaPartyAddMember);
	registerMethod(L, "Party", "removeMember", LuaScriptInterface::luaPartyRemoveMember);

	registerMethod(L, "Party", "isSharedExperienceActive", LuaScriptInterface::luaPartyIsSharedExperienceActive);
	registerMethod(L, "Party", "isSharedExperienceEnabled", LuaScriptInterface::luaPartyIsSharedExperienceEnabled);
	registerMethod(L, "Party", "isMemberSharingExp", LuaScriptInterface::luaPartyIsMemberSharingExp);
	registerMethod(L, "Party", "shareExperience", LuaScriptInterface::luaPartyShareExperience);
	registerMethod(L, "Party", "setSharedExperience", LuaScriptInterface::luaPartySetSharedExperience);

	// Spells
	registerClass(L, "Spell", "", LuaScriptInterface::luaSpellCreate);
	registerMetaMethod(L, "Spell", "__eq", LuaScriptInterface::luaUserdataCompare);

	registerMethod(L, "Spell", "onCastSpell", LuaScriptInterface::luaSpellOnCastSpell);
	registerMethod(L, "Spell", "register", LuaScriptInterface::luaSpellRegister);
	registerMethod(L, "Spell", "name", LuaScriptInterface::luaSpellName);
	registerMethod(L, "Spell", "id", LuaScriptInterface::luaSpellId);
	registerMethod(L, "Spell", "group", LuaScriptInterface::luaSpellGroup);
	registerMethod(L, "Spell", "cooldown", LuaScriptInterface::luaSpellCooldown);
	registerMethod(L, "Spell", "groupCooldown", LuaScriptInterface::luaSpellGroupCooldown);
	registerMethod(L, "Spell", "level", LuaScriptInterface::luaSpellLevel);
	registerMethod(L, "Spell", "magicLevel", LuaScriptInterface::luaSpellMagicLevel);
	registerMethod(L, "Spell", "mana", LuaScriptInterface::luaSpellMana);
	registerMethod(L, "Spell", "manaPercent", LuaScriptInterface::luaSpellManaPercent);
	registerMethod(L, "Spell", "soul", LuaScriptInterface::luaSpellSoul);
	registerMethod(L, "Spell", "range", LuaScriptInterface::luaSpellRange);
	registerMethod(L, "Spell", "isPremium", LuaScriptInterface::luaSpellPremium);
	registerMethod(L, "Spell", "isEnabled", LuaScriptInterface::luaSpellEnabled);
	registerMethod(L, "Spell", "needTarget", LuaScriptInterface::luaSpellNeedTarget);
	registerMethod(L, "Spell", "needWeapon", LuaScriptInterface::luaSpellNeedWeapon);
	registerMethod(L, "Spell", "needLearn", LuaScriptInterface::luaSpellNeedLearn);
	registerMethod(L, "Spell", "isSelfTarget", LuaScriptInterface::luaSpellSelfTarget);
	registerMethod(L, "Spell", "isBlocking", LuaScriptInterface::luaSpellBlocking);
	registerMethod(L, "Spell", "isAggressive", LuaScriptInterface::luaSpellAggressive);
	registerMethod(L, "Spell", "isPzLock", LuaScriptInterface::luaSpellPzLock);
	registerMethod(L, "Spell", "vocation", LuaScriptInterface::luaSpellVocation);

	// only for InstantSpell
	registerMethod(L, "Spell", "words", LuaScriptInterface::luaSpellWords);
	registerMethod(L, "Spell", "needDirection", LuaScriptInterface::luaSpellNeedDirection);
	registerMethod(L, "Spell", "hasParams", LuaScriptInterface::luaSpellHasParams);
	registerMethod(L, "Spell", "hasPlayerNameParam", LuaScriptInterface::luaSpellHasPlayerNameParam);
	registerMethod(L, "Spell", "needCasterTargetOrDirection", LuaScriptInterface::luaSpellNeedCasterTargetOrDirection);
	registerMethod(L, "Spell", "isBlockingWalls", LuaScriptInterface::luaSpellIsBlockingWalls);

	// only for RuneSpells
	registerMethod(L, "Spell", "runeLevel", LuaScriptInterface::luaSpellRuneLevel);
	registerMethod(L, "Spell", "runeMagicLevel", LuaScriptInterface::luaSpellRuneMagicLevel);
	registerMethod(L, "Spell", "runeId", LuaScriptInterface::luaSpellRuneId);
	registerMethod(L, "Spell", "charges", LuaScriptInterface::luaSpellCharges);
	registerMethod(L, "Spell", "allowFarUse", LuaScriptInterface::luaSpellAllowFarUse);
	registerMethod(L, "Spell", "blockWalls", LuaScriptInterface::luaSpellBlockWalls);
	registerMethod(L, "Spell", "checkFloor", LuaScriptInterface::luaSpellCheckFloor);

	// Action
	registerClass(L, "Action", "", LuaScriptInterface::luaCreateAction);
	registerMethod(L, "Action", "onUse", LuaScriptInterface::luaActionOnUse);
	registerMethod(L, "Action", "register", LuaScriptInterface::luaActionRegister);
	registerMethod(L, "Action", "id", LuaScriptInterface::luaActionItemId);
	registerMethod(L, "Action", "aid", LuaScriptInterface::luaActionActionId);
	registerMethod(L, "Action", "uid", LuaScriptInterface::luaActionUniqueId);
	registerMethod(L, "Action", "allowFarUse", LuaScriptInterface::luaActionAllowFarUse);
	registerMethod(L, "Action", "blockWalls", LuaScriptInterface::luaActionBlockWalls);
	registerMethod(L, "Action", "checkFloor", LuaScriptInterface::luaActionCheckFloor);

	// TalkAction
	registerClass(L, "TalkAction", "", LuaScriptInterface::luaCreateTalkaction);
	registerMethod(L, "TalkAction", "onSay", LuaScriptInterface::luaTalkactionOnSay);
	registerMethod(L, "TalkAction", "register", LuaScriptInterface::luaTalkactionRegister);
	registerMethod(L, "TalkAction", "separator", LuaScriptInterface::luaTalkactionSeparator);
	registerMethod(L, "TalkAction", "access", LuaScriptInterface::luaTalkactionAccess);
	registerMethod(L, "TalkAction", "accountType", LuaScriptInterface::luaTalkactionAccountType);

	// CreatureEvent
	registerClass(L, "CreatureEvent", "", LuaScriptInterface::luaCreateCreatureEvent);
	registerMethod(L, "CreatureEvent", "type", LuaScriptInterface::luaCreatureEventType);
	registerMethod(L, "CreatureEvent", "register", LuaScriptInterface::luaCreatureEventRegister);
	registerMethod(L, "CreatureEvent", "onLogin", LuaScriptInterface::luaCreatureEventOnCallback);
	registerMethod(L, "CreatureEvent", "onLogout", LuaScriptInterface::luaCreatureEventOnCallback);
	registerMethod(L, "CreatureEvent", "onThink", LuaScriptInterface::luaCreatureEventOnCallback);
	registerMethod(L, "CreatureEvent", "onPrepareDeath", LuaScriptInterface::luaCreatureEventOnCallback);
	registerMethod(L, "CreatureEvent", "onDeath", LuaScriptInterface::luaCreatureEventOnCallback);
	registerMethod(L, "CreatureEvent", "onKill", LuaScriptInterface::luaCreatureEventOnCallback);
	registerMethod(L, "CreatureEvent", "onAdvance", LuaScriptInterface::luaCreatureEventOnCallback);
	registerMethod(L, "CreatureEvent", "onModalWindow", LuaScriptInterface::luaCreatureEventOnCallback);
	registerMethod(L, "CreatureEvent", "onTextEdit", LuaScriptInterface::luaCreatureEventOnCallback);
	registerMethod(L, "CreatureEvent", "onHealthChange", LuaScriptInterface::luaCreatureEventOnCallback);
	registerMethod(L, "CreatureEvent", "onManaChange", LuaScriptInterface::luaCreatureEventOnCallback);
	registerMethod(L, "CreatureEvent", "onExtendedOpcode", LuaScriptInterface::luaCreatureEventOnCallback);

	// MoveEvent
	registerClass(L, "MoveEvent", "", LuaScriptInterface::luaCreateMoveEvent);
	registerMethod(L, "MoveEvent", "type", LuaScriptInterface::luaMoveEventType);
	registerMethod(L, "MoveEvent", "register", LuaScriptInterface::luaMoveEventRegister);
	registerMethod(L, "MoveEvent", "level", LuaScriptInterface::luaMoveEventLevel);
	registerMethod(L, "MoveEvent", "magicLevel", LuaScriptInterface::luaMoveEventMagLevel);
	registerMethod(L, "MoveEvent", "slot", LuaScriptInterface::luaMoveEventSlot);
	registerMethod(L, "MoveEvent", "id", LuaScriptInterface::luaMoveEventItemId);
	registerMethod(L, "MoveEvent", "aid", LuaScriptInterface::luaMoveEventActionId);
	registerMethod(L, "MoveEvent", "uid", LuaScriptInterface::luaMoveEventUniqueId);
	registerMethod(L, "MoveEvent", "position", LuaScriptInterface::luaMoveEventPosition);
	registerMethod(L, "MoveEvent", "premium", LuaScriptInterface::luaMoveEventPremium);
	registerMethod(L, "MoveEvent", "vocation", LuaScriptInterface::luaMoveEventVocation);
	registerMethod(L, "MoveEvent", "tileItem", LuaScriptInterface::luaMoveEventTileItem);
	registerMethod(L, "MoveEvent", "onEquip", LuaScriptInterface::luaMoveEventOnCallback);
	registerMethod(L, "MoveEvent", "onDeEquip", LuaScriptInterface::luaMoveEventOnCallback);
	registerMethod(L, "MoveEvent", "onStepIn", LuaScriptInterface::luaMoveEventOnCallback);
	registerMethod(L, "MoveEvent", "onStepOut", LuaScriptInterface::luaMoveEventOnCallback);
	registerMethod(L, "MoveEvent", "onAddItem", LuaScriptInterface::luaMoveEventOnCallback);
	registerMethod(L, "MoveEvent", "onRemoveItem", LuaScriptInterface::luaMoveEventOnCallback);

	// GlobalEvent
	registerClass(L, "GlobalEvent", "", LuaScriptInterface::luaCreateGlobalEvent);
	registerMethod(L, "GlobalEvent", "type", LuaScriptInterface::luaGlobalEventType);
	registerMethod(L, "GlobalEvent", "register", LuaScriptInterface::luaGlobalEventRegister);
	registerMethod(L, "GlobalEvent", "time", LuaScriptInterface::luaGlobalEventTime);
	registerMethod(L, "GlobalEvent", "interval", LuaScriptInterface::luaGlobalEventInterval);
	registerMethod(L, "GlobalEvent", "onThink", LuaScriptInterface::luaGlobalEventOnCallback);
	registerMethod(L, "GlobalEvent", "onTime", LuaScriptInterface::luaGlobalEventOnCallback);
	registerMethod(L, "GlobalEvent", "onStartup", LuaScriptInterface::luaGlobalEventOnCallback);
	registerMethod(L, "GlobalEvent", "onShutdown", LuaScriptInterface::luaGlobalEventOnCallback);
	registerMethod(L, "GlobalEvent", "onRecord", LuaScriptInterface::luaGlobalEventOnCallback);
	registerMethod(L, "GlobalEvent", "onSave", LuaScriptInterface::luaGlobalEventOnCallback);

	// Weapon
	registerClass(L, "Weapon", "", LuaScriptInterface::luaCreateWeapon);
	registerMethod(L, "Weapon", "action", LuaScriptInterface::luaWeaponAction);
	registerMethod(L, "Weapon", "register", LuaScriptInterface::luaWeaponRegister);
	registerMethod(L, "Weapon", "id", LuaScriptInterface::luaWeaponId);
	registerMethod(L, "Weapon", "level", LuaScriptInterface::luaWeaponLevel);
	registerMethod(L, "Weapon", "magicLevel", LuaScriptInterface::luaWeaponMagicLevel);
	registerMethod(L, "Weapon", "mana", LuaScriptInterface::luaWeaponMana);
	registerMethod(L, "Weapon", "manaPercent", LuaScriptInterface::luaWeaponManaPercent);
	registerMethod(L, "Weapon", "health", LuaScriptInterface::luaWeaponHealth);
	registerMethod(L, "Weapon", "healthPercent", LuaScriptInterface::luaWeaponHealthPercent);
	registerMethod(L, "Weapon", "soul", LuaScriptInterface::luaWeaponSoul);
	registerMethod(L, "Weapon", "breakChance", LuaScriptInterface::luaWeaponBreakChance);
	registerMethod(L, "Weapon", "premium", LuaScriptInterface::luaWeaponPremium);
	registerMethod(L, "Weapon", "wieldUnproperly", LuaScriptInterface::luaWeaponUnproperly);
	registerMethod(L, "Weapon", "vocation", LuaScriptInterface::luaWeaponVocation);
	registerMethod(L, "Weapon", "onUseWeapon", LuaScriptInterface::luaWeaponOnUseWeapon);
	registerMethod(L, "Weapon", "element", LuaScriptInterface::luaWeaponElement);
	registerMethod(L, "Weapon", "attack", LuaScriptInterface::luaWeaponAttack);
	registerMethod(L, "Weapon", "defense", LuaScriptInterface::luaWeaponDefense);
	registerMethod(L, "Weapon", "range", LuaScriptInterface::luaWeaponRange);
	registerMethod(L, "Weapon", "charges", LuaScriptInterface::luaWeaponCharges);
	registerMethod(L, "Weapon", "duration", LuaScriptInterface::luaWeaponDuration);
	registerMethod(L, "Weapon", "decayTo", LuaScriptInterface::luaWeaponDecayTo);
	registerMethod(L, "Weapon", "transformEquipTo", LuaScriptInterface::luaWeaponTransformEquipTo);
	registerMethod(L, "Weapon", "transformDeEquipTo", LuaScriptInterface::luaWeaponTransformDeEquipTo);
	registerMethod(L, "Weapon", "slotType", LuaScriptInterface::luaWeaponSlotType);
	registerMethod(L, "Weapon", "hitChance", LuaScriptInterface::luaWeaponHitChance);
	registerMethod(L, "Weapon", "extraElement", LuaScriptInterface::luaWeaponExtraElement);

	// exclusively for distance weapons
	registerMethod(L, "Weapon", "ammoType", LuaScriptInterface::luaWeaponAmmoType);
	registerMethod(L, "Weapon", "maxHitChance", LuaScriptInterface::luaWeaponMaxHitChance);

	// exclusively for wands
	registerMethod(L, "Weapon", "damage", LuaScriptInterface::luaWeaponWandDamage);

	// exclusively for wands & distance weapons
	registerMethod(L, "Weapon", "shootType", LuaScriptInterface::luaWeaponShootType);

	// XML
	registerClass(L, "XMLDocument", "", LuaScriptInterface::luaCreateXmlDocument);
	registerMetaMethod(L, "XMLDocument", "__gc", LuaScriptInterface::luaDeleteXmlDocument);
	registerMethod(L, "XMLDocument", "delete", LuaScriptInterface::luaDeleteXmlDocument);

	registerMethod(L, "XMLDocument", "child", LuaScriptInterface::luaXmlDocumentChild);

	registerClass(L, "XMLNode", "");
	registerMetaMethod(L, "XMLNode", "__gc", LuaScriptInterface::luaDeleteXmlNode);
	registerMethod(L, "XMLNode", "delete", LuaScriptInterface::luaDeleteXmlNode);

	registerMethod(L, "XMLNode", "attribute", LuaScriptInterface::luaXmlNodeAttribute);
	registerMethod(L, "XMLNode", "name", LuaScriptInterface::luaXmlNodeName);
	registerMethod(L, "XMLNode", "firstChild", LuaScriptInterface::luaXmlNodeFirstChild);
	registerMethod(L, "XMLNode", "nextSibling", LuaScriptInterface::luaXmlNodeNextSibling);
}

#undef registerEnum
#undef registerEnumIn

ScriptEnvironment* tfs::lua::getScriptEnv()
{
	assert(scriptEnvIndex >= 0 && scriptEnvIndex < static_cast<int32_t>(scriptEnv.size()));
	return &scriptEnv[scriptEnvIndex];
}

bool tfs::lua::reserveScriptEnv() { return ++scriptEnvIndex < static_cast<int32_t>(scriptEnv.size()); }

void tfs::lua::resetScriptEnv()
{
	assert(scriptEnvIndex >= 0);
	scriptEnv[scriptEnvIndex--].resetEnv();
}

// Get
bool tfs::lua::getBoolean(lua_State* L, int32_t arg) { return lua_toboolean(L, arg) != 0; }
bool tfs::lua::getBoolean(lua_State* L, int32_t arg, bool defaultValue)
{
	if (lua_isboolean(L, arg) == 0) {
		return defaultValue;
	}
	return lua_toboolean(L, arg) != 0;
}

// Push

void tfs::lua::registerMethod(lua_State* L, std::string_view globalName, std::string_view methodName,
                              lua_CFunction func)
{
	// globalName.methodName = func
	lua_getglobal(L, globalName.data());
	lua_pushcfunction(L, func);
	lua_setfield(L, -2, methodName.data());

	// pop globalName
	lua_pop(L, 1);
}

int LuaScriptInterface::luaDoPlayerAddItem()
{
	// doPlayerAddItem(cid, itemid, <optional: default: 1> count/subtype, <optional: default: 1> canDropOnMap)
	// doPlayerAddItem(cid, itemid, <optional: default: 1> count, <optional: default: 1> canDropOnMap, <optional:
	// default: 1>subtype)
	Player* player = tfs::lua::getPlayer(L, 1);
	if (!player) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_PLAYER_NOT_FOUND));
		context.push_boolean(false);
		return 1;
	}

	uint16_t itemId = context.get_number<uint16_t>(2);
	int32_t count = tfs::lua::getNumber<int32_t>(L, 3, 1);
	bool canDropOnMap = tfs::lua::getBoolean(L, 4, true);
	uint16_t subType = context.get_number<uint16_t>(5, 1);

	const ItemType& it = Item::items[itemId];
	int32_t itemCount;

	auto parameters = lua_gettop(L);
	if (parameters > 4) {
		// subtype already supplied, count then is the amount
		itemCount = std::max<int32_t>(1, count);
	} else if (it.hasSubType()) {
		if (it.stackable) {
			itemCount = static_cast<int32_t>(std::ceil(static_cast<float>(count) / ITEM_STACK_SIZE));
		} else {
			itemCount = 1;
		}
		subType = count;
	} else {
		itemCount = std::max<int32_t>(1, count);
	}

	while (itemCount > 0) {
		uint16_t stackCount = subType;
		if (it.stackable && stackCount > ITEM_STACK_SIZE) {
			stackCount = ITEM_STACK_SIZE;
		}

		Item* newItem = Item::CreateItem(itemId, stackCount);
		if (!newItem) {
			reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_ITEM_NOT_FOUND));
			context.push_boolean(false);
			return 1;
		}

		if (it.stackable) {
			subType -= stackCount;
		}

		ReturnValue ret = g_game.internalPlayerAddItem(player, newItem, canDropOnMap);
		if (ret != RETURNVALUE_NOERROR) {
			delete newItem;
			context.push_boolean(false);
			return 1;
		}

		if (--itemCount == 0) {
			if (newItem->getParent()) {
				uint32_t uid = tfs::lua::getScriptEnv()->addThing(newItem);
				context.push_number(uid);
				return 1;
			} else {
				// stackable item stacked with existing object, newItem will be released
				context.push_boolean(false);
				return 1;
			}
		}
	}

	context.push_boolean(false);
	return 1;
}

int LuaScriptInterface::luaDebugPrint()
{
	// debugPrint(text)
	reportErrorFunc(L, tfs::lua::getString(L, -1));
	return 0;
}

int LuaScriptInterface::luaGetWorldUpTime()
{
	// getWorldUpTime()
	uint64_t uptime = (OTSYS_TIME() - ProtocolStatus::start) / 1000;
	context.push_number(uptime);
	return 1;
}

int LuaScriptInterface::luaGetSubTypeName()
{
	// getSubTypeName(subType)
	int32_t subType = tfs::lua::getNumber<int32_t>(L, 1);
	if (subType > 0) {
		tfs::lua::pushString(L, Item::items[subType].name);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreateCombatArea()
{
	// createCombatArea({area}, <optional> {extArea})
	ScriptEnvironment* env = tfs::lua::getScriptEnv();
	if (env->getScriptId() != EVENT_ID_LOADING) {
		reportErrorFunc(L, "This function can only be used while loading the script.");
		context.push_boolean(false);
		return 1;
	}

	uint32_t areaId = g_luaEnvironment.createAreaObject(env->getScriptInterface());
	AreaCombat* area = g_luaEnvironment.getAreaObject(areaId);

	int parameters = lua_gettop(L);
	if (parameters >= 2) {
		uint32_t rowsExtArea;
		std::vector<uint32_t> vecExtArea;
		if (!lua_istable(L, 2) || !getArea(L, vecExtArea, rowsExtArea)) {
			reportErrorFunc(L, "Invalid extended area table.");
			context.push_boolean(false);
			return 1;
		}
		area->setupExtArea(vecExtArea, rowsExtArea);
	}

	uint32_t rowsArea = 0;
	std::vector<uint32_t> vecArea;
	if (!lua_istable(L, 1) || !getArea(L, vecArea, rowsArea)) {
		reportErrorFunc(L, "Invalid area table.");
		context.push_boolean(false);
		return 1;
	}

	area->setupArea(vecArea, rowsArea);
	context.push_number(areaId);
	return 1;
}

int LuaScriptInterface::luaDoAreaCombat()
{
	// doAreaCombat(cid, type, pos, area, min, max, effect[, origin = ORIGIN_SPELL[, blockArmor = false[, blockShield =
	// false[, ignoreResistances = false]]]])
	Creature* creature = tfs::lua::getCreature(L, 1);
	if (!creature && (!isNumber(L, 1) || context.get_number<uint32_t>(1) != 0)) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
		context.push_boolean(false);
		return 1;
	}

	uint32_t areaId = context.get_number<uint32_t>(4);
	const AreaCombat* area = g_luaEnvironment.getAreaObject(areaId);
	if (area || areaId == 0) {
		CombatType_t combatType = tfs::lua::getNumber<CombatType_t>(L, 2);

		CombatParams params;
		params.combatType = combatType;
		params.impactEffect = context.get_number<uint8_t>(7);

		params.blockedByArmor = tfs::lua::getBoolean(L, 9, false);
		params.blockedByShield = tfs::lua::getBoolean(L, 10, false);
		params.ignoreResistances = tfs::lua::getBoolean(L, 11, false);

		CombatDamage damage;
		damage.origin = tfs::lua::getNumber<CombatOrigin>(L, 8, ORIGIN_SPELL);
		damage.primary.type = combatType;
		damage.primary.value = normal_random(tfs::lua::getNumber<int32_t>(L, 5), tfs::lua::getNumber<int32_t>(L, 6));

		Combat::doAreaCombat(creature, tfs::lua::getPosition(L, 3), area, damage, params);
		context.push_boolean(true);
	} else {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_AREA_NOT_FOUND));
		context.push_boolean(false);
	}
	return 1;
}

int LuaScriptInterface::luaDoTargetCombat()
{
	// doTargetCombat(cid, target, type, min, max, effect[, origin = ORIGIN_SPELL[, blockArmor = false[, blockShield =
	// false[, ignoreResistances = false]]]])
	Creature* creature = tfs::lua::getCreature(L, 1);
	if (!creature && (!isNumber(L, 1) || context.get_number<uint32_t>(1) != 0)) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
		context.push_boolean(false);
		return 1;
	}

	Creature* target = tfs::lua::getCreature(L, 2);
	if (!target) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
		context.push_boolean(false);
		return 1;
	}

	CombatType_t combatType = tfs::lua::getNumber<CombatType_t>(L, 3);

	CombatParams params{
	    .combatType = combatType,
	    .impactEffect = context.get_number<uint8_t>(6),
	    .blockedByArmor = tfs::lua::getBoolean(L, 8, false),
	    .blockedByShield = tfs::lua::getBoolean(L, 9, false),
	    .ignoreResistances = tfs::lua::getBoolean(L, 10, false),
	};

	CombatDamage damage{
	    .primary =
	        {
	            .type = combatType,
	            .value = normal_random(tfs::lua::getNumber<int32_t>(L, 4), tfs::lua::getNumber<int32_t>(L, 5)),
	        },
	    .origin = tfs::lua::getNumber<CombatOrigin>(L, 7, ORIGIN_SPELL),
	};

	Combat::doTargetCombat(creature, target, damage, params);
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaDoChallengeCreature()
{
	// doChallengeCreature(cid, target[, force = false])
	Creature* creature = tfs::lua::getCreature(L, 1);
	if (!creature) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
		context.push_boolean(false);
		return 1;
	}

	Creature* target = tfs::lua::getCreature(L, 2);
	if (!target) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
		context.push_boolean(false);
		return 1;
	}

	target->challengeCreature(creature, tfs::lua::getBoolean(L, 3, false));
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaIsValidUID()
{
	// isValidUID(uid)
	context.push_boolean(tfs::lua::getScriptEnv()->getThingByUID(context.get_number<uint32_t>(-1)) != nullptr);
	return 1;
}

int LuaScriptInterface::luaIsDepot()
{
	// isDepot(uid)
	Container* container = tfs::lua::getScriptEnv()->getContainerByUID(context.get_number<uint32_t>(-1));
	context.push_boolean(container && container->getDepotLocker());
	return 1;
}

int LuaScriptInterface::luaIsMoveable()
{
	// isMoveable(uid)
	// isMovable(uid)
	Thing* thing = tfs::lua::getScriptEnv()->getThingByUID(context.get_number<uint32_t>(-1));
	context.push_boolean(thing && thing->isPushable());
	return 1;
}

int LuaScriptInterface::luaGetDepotId()
{
	// getDepotId(uid)
	uint32_t uid = context.get_number<uint32_t>(-1);

	Container* container = tfs::lua::getScriptEnv()->getContainerByUID(uid);
	if (!container) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_CONTAINER_NOT_FOUND));
		context.push_boolean(false);
		return 1;
	}

	DepotLocker* depotLocker = container->getDepotLocker();
	if (!depotLocker) {
		reportErrorFunc(L, "Depot not found");
		context.push_boolean(false);
		return 1;
	}

	context.push_number(depotLocker->getDepotId());
	return 1;
}

int LuaScriptInterface::luaAddEvent()
{
	// addEvent(callback, delay, ...)
	int parameters = lua_gettop(L);
	if (parameters < 2) {
		reportErrorFunc(L, fmt::format("Not enough parameters: {:d}.", parameters));
		context.push_boolean(false);
		return 1;
	}

	if (!lua_isfunction(L, 1)) {
		reportErrorFunc(L, "callback parameter should be a function.");
		context.push_boolean(false);
		return 1;
	}

	if (!isNumber(L, 2)) {
		reportErrorFunc(L, "delay parameter should be a number.");
		context.push_boolean(false);
		return 1;
	}

	if (ConfigManager::getBoolean(ConfigManager::WARN_UNSAFE_SCRIPTS) ||
	    ConfigManager::getBoolean(ConfigManager::CONVERT_UNSAFE_SCRIPTS)) {
		std::vector<std::pair<int32_t, LuaDataType>> indexes;
		for (int i = 3; i <= parameters; ++i) {
			if (lua_getmetatable(L, i) == 0) {
				continue;
			}
			context.raw_geti(-1, 't');

			LuaDataType type = tfs::lua::getNumber<LuaDataType>(L, -1);
			if (type != LuaData_Unknown && type != LuaData_Tile) {
				indexes.push_back({i, type});
			}
			lua_pop(L, 2);
		}

		if (!indexes.empty()) {
			if (ConfigManager::getBoolean(ConfigManager::WARN_UNSAFE_SCRIPTS)) {
				bool plural = indexes.size() > 1;

				std::string warningString = "Argument";
				if (plural) {
					warningString += 's';
				}

				for (const auto& entry : indexes) {
					if (entry == indexes.front()) {
						warningString += ' ';
					} else if (entry == indexes.back()) {
						warningString += " and ";
					} else {
						warningString += ", ";
					}
					warningString += '#';
					warningString += std::to_string(entry.first);
				}

				if (plural) {
					warningString += " are unsafe";
				} else {
					warningString += " is unsafe";
				}

				reportErrorFunc(L, warningString);
			}

			if (ConfigManager::getBoolean(ConfigManager::CONVERT_UNSAFE_SCRIPTS)) {
				for (const auto& entry : indexes) {
					switch (entry.second) {
						case LuaData_Item:
						case LuaData_Container:
						case LuaData_Teleport:
						case LuaData_Podium: {
							lua_getglobal(L, "Item");
							lua_getfield(L, -1, "getUniqueId");
							break;
						}
						case LuaData_Player:
						case LuaData_Monster:
						case LuaData_Npc: {
							lua_getglobal(L, "Creature");
							lua_getfield(L, -1, "getId");
							break;
						}
						default:
							break;
					}
					lua_replace(L, -2);
					lua_pushvalue(L, entry.first);
					lua_call(L, 1, 1);
					lua_replace(L, entry.first);
				}
			}
		}
	}

	LuaTimerEventDesc eventDesc;
	eventDesc.parameters.reserve(parameters -
	                             2); // safe to use -2 since we garanteed that there is at least two parameters
	for (int i = 0; i < parameters - 2; ++i) {
		eventDesc.parameters.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
	}

	uint32_t delay = std::max<uint32_t>(100, context.get_number<uint32_t>(2));
	lua_pop(L, 1);

	eventDesc.function = luaL_ref(L, LUA_REGISTRYINDEX);
	eventDesc.scriptId = tfs::lua::getScriptEnv()->getScriptId();

	auto& lastTimerEventId = g_luaEnvironment.lastEventTimerId;
	eventDesc.eventId = g_scheduler.addEvent(
	    createSchedulerTask(delay, [=]() { g_luaEnvironment.executeTimerEvent(lastTimerEventId); }));

	g_luaEnvironment.timerEvents.emplace(lastTimerEventId, std::move(eventDesc));
	context.push_number(lastTimerEventId++);
	return 1;
}

int LuaScriptInterface::luaStopEvent()
{
	// stopEvent(eventid)
	uint32_t eventId = context.get_number<uint32_t>(1);

	auto& timerEvents = g_luaEnvironment.timerEvents;
	auto it = timerEvents.find(eventId);
	if (it == timerEvents.end()) {
		context.push_boolean(false);
		return 1;
	}

	LuaTimerEventDesc timerEventDesc = std::move(it->second);
	timerEvents.erase(it);

	g_scheduler.stopEvent(timerEventDesc.eventId);
	luaL_unref(L, LUA_REGISTRYINDEX, timerEventDesc.function);

	for (auto parameter : timerEventDesc.parameters) {
		luaL_unref(L, LUA_REGISTRYINDEX, parameter);
	}

	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaSaveServer()
{
	g_globalEvents->save();
	g_game.saveGameState();
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaCleanMap()
{
	context.push_number(g_game.map.clean());
	return 1;
}

int LuaScriptInterface::luaIsInWar()
{
	// isInWar(cid, target)
	Player* player = tfs::lua::getPlayer(L, 1);
	if (!player) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_PLAYER_NOT_FOUND));
		context.push_boolean(false);
		return 1;
	}

	Player* targetPlayer = tfs::lua::getPlayer(L, 2);
	if (!targetPlayer) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_PLAYER_NOT_FOUND));
		context.push_boolean(false);
		return 1;
	}

	context.push_boolean(player->isInWar(targetPlayer));
	return 1;
}

int LuaScriptInterface::luaGetWaypointPositionByName()
{
	// getWaypointPositionByName(name)
	auto& waypoints = g_game.map.waypoints;

	auto it = waypoints.find(tfs::lua::getString(L, -1));
	if (it != waypoints.end()) {
		tfs::lua::pushPosition(L, it->second);
	} else {
		context.push_boolean(false);
	}
	return 1;
}

int LuaScriptInterface::luaSendChannelMessage()
{
	// sendChannelMessage(channelId, type, message)
	uint32_t channelId = context.get_number<uint32_t>(1);
	ChatChannel* channel = g_chat->getChannelById(channelId);
	if (!channel) {
		context.push_boolean(false);
		return 1;
	}

	SpeakClasses type = tfs::lua::getNumber<SpeakClasses>(L, 2);
	std::string message = tfs::lua::getString(L, 3);
	channel->sendToAll(message, type);
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaSendGuildChannelMessage()
{
	// sendGuildChannelMessage(guildId, type, message)
	uint32_t guildId = context.get_number<uint32_t>(1);
	ChatChannel* channel = g_chat->getGuildChannelById(guildId);
	if (!channel) {
		context.push_boolean(false);
		return 1;
	}

	SpeakClasses type = tfs::lua::getNumber<SpeakClasses>(L, 2);
	std::string message = tfs::lua::getString(L, 3);
	channel->sendToAll(message, type);
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaIsScriptsInterface()
{
	// isScriptsInterface()
	if (tfs::lua::getScriptEnv()->getScriptInterface() == &g_scripts->getScriptInterface()) {
		context.push_boolean(true);
	} else {
		reportErrorFunc(L, "Event: can only be called inside (data/scripts/)");
		context.push_boolean(false);
	}
	return 1;
}

#ifndef LUAJIT_VERSION
const luaL_Reg LuaScriptInterface::luaBitReg[] = {
    //{"tobit", LuaScriptInterface::luaBitToBit},
    {"bnot", LuaScriptInterface::luaBitNot},
    {"band", LuaScriptInterface::luaBitAnd},
    {"bor", LuaScriptInterface::luaBitOr},
    {"bxor", LuaScriptInterface::luaBitXor},
    {"lshift", LuaScriptInterface::luaBitLeftShift},
    {"rshift", LuaScriptInterface::luaBitRightShift},
    //{"arshift", LuaScriptInterface::luaBitArithmeticalRightShift},
    //{"rol", LuaScriptInterface::luaBitRotateLeft},
    //{"ror", LuaScriptInterface::luaBitRotateRight},
    //{"bswap", LuaScriptInterface::luaBitSwapEndian},
    //{"tohex", LuaScriptInterface::luaBitToHex},
    {nullptr, nullptr}};

int LuaScriptInterface::luaBitNot()
{
	context.push_number(~context.get_number<uint32_t>(-1));
	return 1;
}

#define MULTIOP(name, op) \
	int LuaScriptInterface::luaBit##name() \
	{ \
		int n = lua_gettop(L); \
		uint32_t w = context.get_number<uint32_t>(-1); \
		for (int i = 1; i < n; ++i) w op context.get_number<uint32_t>(i); \
		context.push_number(w); \
		return 1; \
	}

MULTIOP(And, &=)
MULTIOP(Or, |=)
MULTIOP(Xor, ^=)

#define SHIFTOP(name, op) \
	int LuaScriptInterface::luaBit##name() \
	{ \
		uint32_t n1 = context.get_number<uint32_t>(1), n2 = context.get_number<uint32_t>(2); \
		context.push_number((n1 op n2)); \
		return 1; \
	}

SHIFTOP(LeftShift, <<)
SHIFTOP(RightShift, >>)
#endif

const luaL_Reg LuaScriptInterface::luaConfigManagerTable[] = {
    {"getString", LuaScriptInterface::luaConfigManagerGetString},
    {"getNumber", LuaScriptInterface::luaConfigManagerGetNumber},
    {"getBoolean", LuaScriptInterface::luaConfigManagerGetBoolean},
    {nullptr, nullptr}};

int LuaScriptInterface::luaConfigManagerGetString()
{
	tfs::lua::pushString(L, ConfigManager::getString(tfs::lua::getNumber<ConfigManager::string_config_t>(L, -1)));
	return 1;
}

int LuaScriptInterface::luaConfigManagerGetNumber()
{
	context.push_number(ConfigManager::getNumber(tfs::lua::getNumber<ConfigManager::integer_config_t>(L, -1)));
	return 1;
}

int LuaScriptInterface::luaConfigManagerGetBoolean()
{
	context.push_boolean(ConfigManager::getBoolean(tfs::lua::getNumber<ConfigManager::boolean_config_t>(L, -1)));
	return 1;
}

const luaL_Reg LuaScriptInterface::luaDatabaseTable[] = {
    {"query", LuaScriptInterface::luaDatabaseExecute},
    {"asyncQuery", LuaScriptInterface::luaDatabaseAsyncExecute},
    {"storeQuery", LuaScriptInterface::luaDatabaseStoreQuery},
    {"asyncStoreQuery", LuaScriptInterface::luaDatabaseAsyncStoreQuery},
    {"escapeString", LuaScriptInterface::luaDatabaseEscapeString},
    {"escapeBlob", LuaScriptInterface::luaDatabaseEscapeBlob},
    {"lastInsertId", LuaScriptInterface::luaDatabaseLastInsertId},
    {"tableExists", LuaScriptInterface::luaDatabaseTableExists},
    {nullptr, nullptr}};

int LuaScriptInterface::luaDatabaseExecute()
{
	// db.query(query)
	context.push_boolean(Database::getInstance().executeQuery(tfs::lua::getString(L, -1)));
	return 1;
}

int LuaScriptInterface::luaDatabaseAsyncExecute()
{
	// db.asyncQuery(query, callback)
	std::function<void(const DBResult_ptr&, bool)> callback;
	if (lua_gettop(L) > 1) {
		int32_t ref = luaL_ref(L, LUA_REGISTRYINDEX);
		auto scriptId = tfs::lua::getScriptEnv()->getScriptId();
		callback = [ref, scriptId](const DBResult_ptr&, bool success) {
			lua_State* L = g_luaEnvironment.getLuaState();
			if (!L) {
				return;
			}

			if (!tfs::lua::reserveScriptEnv()) {
				luaL_unref(L, LUA_REGISTRYINDEX, ref);
				return;
			}

			context.raw_geti(LUA_REGISTRYINDEX, ref);
			context.push_boolean(success);
			auto env = tfs::lua::getScriptEnv();
			env->setScriptId(scriptId, &g_luaEnvironment);
			g_luaEnvironment.callFunction(1);

			luaL_unref(L, LUA_REGISTRYINDEX, ref);
		};
	}
	g_databaseTasks.addTask(tfs::lua::getString(L, -1), callback);
	return 0;
}

int LuaScriptInterface::luaDatabaseStoreQuery()
{
	// db.storeQuery(query)
	if (DBResult_ptr res = Database::getInstance().storeQuery(tfs::lua::getString(L, -1))) {
		context.push_number(addResult(res));
	} else {
		context.push_boolean(false);
	}
	return 1;
}

int LuaScriptInterface::luaDatabaseAsyncStoreQuery()
{
	// db.asyncStoreQuery(query, callback)
	std::function<void(const DBResult_ptr&, bool)> callback;
	if (lua_gettop(L) > 1) {
		int32_t ref = luaL_ref(L, LUA_REGISTRYINDEX);
		auto scriptId = tfs::lua::getScriptEnv()->getScriptId();
		callback = [ref, scriptId](const DBResult_ptr& result, bool) {
			lua_State* L = g_luaEnvironment.getLuaState();
			if (!L) {
				return;
			}

			if (!tfs::lua::reserveScriptEnv()) {
				luaL_unref(L, LUA_REGISTRYINDEX, ref);
				return;
			}

			context.raw_geti(LUA_REGISTRYINDEX, ref);
			if (result) {
				context.push_number(addResult(result));
			} else {
				context.push_boolean(false);
			}
			auto env = tfs::lua::getScriptEnv();
			env->setScriptId(scriptId, &g_luaEnvironment);
			g_luaEnvironment.callFunction(1);

			luaL_unref(L, LUA_REGISTRYINDEX, ref);
		};
	}
	g_databaseTasks.addTask(tfs::lua::getString(L, -1), callback, true);
	return 0;
}

int LuaScriptInterface::luaDatabaseEscapeString()
{
	// db.escapeString(s)
	tfs::lua::pushString(L, Database::getInstance().escapeString(tfs::lua::getString(L, -1)));
	return 1;
}

int LuaScriptInterface::luaDatabaseEscapeBlob()
{
	// db.escapeBlob(s, length)
	uint32_t length = context.get_number<uint32_t>(2);
	tfs::lua::pushString(L, Database::getInstance().escapeBlob(tfs::lua::getString(L, 1).data(), length));
	return 1;
}

int LuaScriptInterface::luaDatabaseLastInsertId()
{
	// db.lastInsertId()
	context.push_number(Database::getInstance().getLastInsertId());
	return 1;
}

int LuaScriptInterface::luaDatabaseTableExists()
{
	// db.tableExists(tableName)
	context.push_boolean(DatabaseManager::tableExists(tfs::lua::getString(L, -1)));
	return 1;
}

const luaL_Reg LuaScriptInterface::luaResultTable[] = {
    {"getNumber", LuaScriptInterface::luaResultGetNumber}, {"getString", LuaScriptInterface::luaResultGetString},
    {"getStream", LuaScriptInterface::luaResultGetStream}, {"next", LuaScriptInterface::luaResultNext},
    {"free", LuaScriptInterface::luaResultFree},           {nullptr, nullptr}};

int LuaScriptInterface::luaResultGetNumber()
{
	DBResult_ptr res = getResultByID(context.get_number<uint32_t>(1));
	if (!res) {
		context.push_boolean(false);
		return 1;
	}

	const std::string& s = tfs::lua::getString(L, 2);
	context.push_number(res->getNumber<int64_t>(s));
	return 1;
}

int LuaScriptInterface::luaResultGetString()
{
	DBResult_ptr res = getResultByID(context.get_number<uint32_t>(1));
	if (!res) {
		context.push_boolean(false);
		return 1;
	}

	const std::string& s = tfs::lua::getString(L, 2);
	tfs::lua::pushString(L, res->getString(s));
	return 1;
}

int LuaScriptInterface::luaResultGetStream()
{
	DBResult_ptr res = getResultByID(context.get_number<uint32_t>(1));
	if (!res) {
		context.push_boolean(false);
		return 1;
	}

	auto stream = res->getString(tfs::lua::getString(L, 2));
	lua_pushlstring(L, stream.data(), stream.size());
	context.push_number(stream.size());
	return 2;
}

int LuaScriptInterface::luaResultNext()
{
	DBResult_ptr res = getResultByID(context.get_number<uint32_t>(-1));
	if (!res) {
		context.push_boolean(false);
		return 1;
	}

	context.push_boolean(res->next());
	return 1;
}

int LuaScriptInterface::luaResultFree()
{
	context.push_boolean(removeResult(context.get_number<uint32_t>(-1)));
	return 1;
}

// Userdata
int LuaScriptInterface::luaUserdataCompare()
{
	// userdataA == userdataB
	context.push_boolean(tfs::lua::getUserdata<void>(L, 1) == tfs::lua::getUserdata<void>(L, 2));
	return 1;
}

// _G
int LuaScriptInterface::luaIsType()
{
	// isType(derived, base)
	lua_getmetatable(L, -2);
	lua_getmetatable(L, -2);

	context.raw_geti(-2, 'p');
	uint_fast8_t parentsB = tfs::lua::getNumber<uint_fast8_t>(L, 1);

	context.raw_geti(-3, 'h');
	size_t hashB = tfs::lua::getNumber<size_t>(L, 1);

	context.raw_geti(-3, 'p');
	uint_fast8_t parentsA = tfs::lua::getNumber<uint_fast8_t>(L, 1);
	for (uint_fast8_t i = parentsA; i < parentsB; ++i) {
		lua_getfield(L, -3, "__index");
		lua_replace(L, -4);
	}

	context.raw_geti(-4, 'h');
	size_t hashA = tfs::lua::getNumber<size_t>(L, 1);

	context.push_boolean(hashA == hashB);
	return 1;
}

int LuaScriptInterface::luaRawGetMetatable()
{
	// rawgetmetatable(metatableName)
	luaL_getmetatable(L, tfs::lua::getString(L, 1).data());
	return 1;
}

// os
int LuaScriptInterface::luaSystemTime()
{
	// os.mtime()
	context.push_number(OTSYS_TIME());
	return 1;
}

// table
int LuaScriptInterface::luaTableCreate()
{
	// table.create(arrayLength, keyLength)
	lua_createtable(L, tfs::lua::getNumber<int32_t>(L, 1), tfs::lua::getNumber<int32_t>(L, 2));
	return 1;
}

int LuaScriptInterface::luaTablePack()
{
	// table.pack(...)
	int n = lua_gettop(L);         /* number of elements to pack */
	lua_createtable(L, n, 1);      /* create result table */
	lua_insert(L, 1);              /* put it at index 1 */
	for (int i = n; i >= 1; i--) { /* assign elements */
		lua_rawseti(L, 1, i);
	}
	if (luaL_callmeta(L, -1, "__index") != 0) {
		lua_replace(L, -2);
	}
	lua_pushinteger(L, n);
	lua_setfield(L, 1, "n"); /* t.n = number of elements */
	return 1;                /* return table */
}

// DB Insert
int LuaScriptInterface::luaDBInsertCreate()
{
	// DBInsert(query)
	if (lua_isstring(L, 2)) {
		tfs::lua::pushUserdata(L, new DBInsert(tfs::lua::getString(L, 2)));
		tfs::lua::setMetatable(L, -1, "DBInsert");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaDBInsertAddRow()
{
	// insert:addRow(row)
	DBInsert* insert = tfs::lua::getUserdata<DBInsert>(L, 1);
	if (insert) {
		context.push_boolean(insert->addRow(tfs::lua::getString(L, 2)));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaDBInsertExecute()
{
	// insert:execute()
	DBInsert* insert = tfs::lua::getUserdata<DBInsert>(L, 1);
	if (insert) {
		context.push_boolean(insert->execute());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaDBInsertDelete()
{
	DBInsert** insertPtr = tfs::lua::getRawUserdata<DBInsert>(L, 1);
	if (insertPtr && *insertPtr) {
		delete *insertPtr;
		*insertPtr = nullptr;
	}
	return 0;
}

// DB Transaction
int LuaScriptInterface::luaDBTransactionCreate()
{
	// DBTransaction()
	tfs::lua::pushUserdata(L, new DBTransaction);
	tfs::lua::setMetatable(L, -1, "DBTransaction");
	return 1;
}

int LuaScriptInterface::luaDBTransactionBegin()
{
	// transaction:begin()
	DBTransaction* transaction = tfs::lua::getUserdata<DBTransaction>(L, 1);
	if (transaction) {
		context.push_boolean(transaction->begin());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaDBTransactionCommit()
{
	// transaction:commit()
	DBTransaction* transaction = tfs::lua::getUserdata<DBTransaction>(L, 1);
	if (transaction) {
		context.push_boolean(transaction->commit());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaDBTransactionDelete()
{
	DBTransaction** transactionPtr = tfs::lua::getRawUserdata<DBTransaction>(L, 1);
	if (transactionPtr && *transactionPtr) {
		delete *transactionPtr;
		*transactionPtr = nullptr;
	}
	return 0;
}

// Game
int LuaScriptInterface::luaGameGetSpectators()
{
	// Game.getSpectators(position[, multifloor = false[, onlyPlayer = false[, minRangeX = 0[, maxRangeX = 0[, minRangeY
	// = 0[, maxRangeY = 0]]]]]])
	const Position& position = tfs::lua::getPosition(L, 1);
	bool multifloor = tfs::lua::getBoolean(L, 2, false);
	bool onlyPlayers = tfs::lua::getBoolean(L, 3, false);
	int32_t minRangeX = tfs::lua::getNumber<int32_t>(L, 4, 0);
	int32_t maxRangeX = tfs::lua::getNumber<int32_t>(L, 5, 0);
	int32_t minRangeY = tfs::lua::getNumber<int32_t>(L, 6, 0);
	int32_t maxRangeY = tfs::lua::getNumber<int32_t>(L, 7, 0);

	Spectators spectators;
	g_game.map.getSpectators(spectators, position, multifloor, onlyPlayers, minRangeX, maxRangeX, minRangeY, maxRangeY);

	lua_createtable(L, spectators.size(), 0);

	int index = 0;
	for (Creature* creature : spectators) {
		tfs::lua::pushUserdata(L, creature);
		tfs::lua::setCreatureMetatable(L, -1, creature);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaGameGetPlayers()
{
	// Game.getPlayers()
	lua_createtable(L, g_game.getPlayersOnline(), 0);

	int index = 0;
	for (const auto& playerEntry : g_game.getPlayers()) {
		tfs::lua::pushUserdata(L, playerEntry.second);
		tfs::lua::setMetatable(L, -1, "Player");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaGameGetNpcs()
{
	// Game.getNpcs()
	lua_createtable(L, g_game.getNpcsOnline(), 0);

	int index = 0;
	for (const auto& npcEntry : g_game.getNpcs()) {
		tfs::lua::pushUserdata(L, npcEntry.second);
		tfs::lua::setMetatable(L, -1, "Npc");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaGameGetMonsters()
{
	// Game.getMonsters()
	lua_createtable(L, g_game.getMonstersOnline(), 0);

	int index = 0;
	for (const auto& monsterEntry : g_game.getMonsters()) {
		tfs::lua::pushUserdata(L, monsterEntry.second);
		tfs::lua::setMetatable(L, -1, "Monster");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaGameLoadMap()
{
	// Game.loadMap(path)
	const std::string& path = tfs::lua::getString(L, 1);
	g_dispatcher.addTask([path]() {
		try {
			g_game.loadMap(path);
		} catch (const std::exception& e) {
			// FIXME: Should only catch some exceptions
			std::cout << "[Error - LuaScriptInterface::luaGameLoadMap] Failed to load map: " << e.what() << '\n';
		}
	});
	return 0;
}

int LuaScriptInterface::luaGameGetExperienceStage()
{
	// Game.getExperienceStage(level)
	uint32_t level = context.get_number<uint32_t>(1);
	context.push_number(ConfigManager::getExperienceStage(level));
	return 1;
}

int LuaScriptInterface::luaGameGetExperienceForLevel()
{
	// Game.getExperienceForLevel(level)
	const uint32_t level = context.get_number<uint32_t>(1);
	if (level == 0) {
		context.push_number(0);
	} else {
		context.push_number(Player::getExpForLevel(level));
	}
	return 1;
}

int LuaScriptInterface::luaGameGetMonsterCount()
{
	// Game.getMonsterCount()
	context.push_number(g_game.getMonstersOnline());
	return 1;
}

int LuaScriptInterface::luaGameGetPlayerCount()
{
	// Game.getPlayerCount()
	context.push_number(g_game.getPlayersOnline());
	return 1;
}

int LuaScriptInterface::luaGameGetNpcCount()
{
	// Game.getNpcCount()
	context.push_number(g_game.getNpcsOnline());
	return 1;
}

int LuaScriptInterface::luaGameGetMonsterTypes()
{
	// Game.getMonsterTypes()
	auto& type = g_monsters.monsters;
	lua_createtable(L, type.size(), 0);

	for (const auto& [name, mType] : type) {
		tfs::lua::pushUserdata(L, &mType);
		tfs::lua::setMetatable(L, -1, "MonsterType");
		lua_setfield(L, -2, name.data());
	}
	return 1;
}

int LuaScriptInterface::luaGameGetBestiary()
{
	// Game.getBestiary()
	lua_createtable(L, 0, g_monsters.bestiary.size());
	int classIndex = 0;
	for (const auto& [className, monsters] : g_monsters.bestiary) {
		lua_createtable(L, 0, 2);
		tfs::lua::pushString(L, className);
		lua_setfield(L, -2, "name");

		lua_createtable(L, 0, monsters.size());
		int index = 0;
		for (const auto& monsterName : monsters) {
			tfs::lua::pushUserdata(L, g_monsters.getMonsterType(monsterName));
			tfs::lua::setMetatable(L, -1, "MonsterType");
			lua_rawseti(L, -2, ++index);
		}

		lua_setfield(L, -2, "monsterTypes");
		lua_rawseti(L, -2, ++classIndex);
	}
	return 1;
}

int LuaScriptInterface::luaGameGetCurrencyItems()
{
	// Game.getCurrencyItems()
	const auto& currencyItems = Item::items.currencyItems;
	size_t size = currencyItems.size();
	lua_createtable(L, size, 0);

	for (const auto& it : currencyItems) {
		const ItemType& itemType = Item::items[it.second];
		tfs::lua::pushUserdata(L, &itemType);
		tfs::lua::setMetatable(L, -1, "ItemType");
		lua_rawseti(L, -2, size--);
	}
	return 1;
}

int LuaScriptInterface::luaGameGetItemTypeByClientId()
{
	// Game.getItemTypeByClientId(clientId)
	uint16_t spriteId = context.get_number<uint16_t>(1);
	const ItemType& itemType = Item::items.getItemIdByClientId(spriteId);
	if (itemType.id != 0) {
		tfs::lua::pushUserdata(L, &itemType);
		tfs::lua::setMetatable(L, -1, "ItemType");
	} else {
		context.push_nil();
	}

	return 1;
}

int LuaScriptInterface::luaGameGetMountIdByLookType()
{
	// Game.getMountIdByLookType(lookType)
	Mount* mount = nullptr;
	if (isNumber(L, 1)) {
		mount = g_game.mounts.getMountByClientID(context.get_number<uint16_t>(1));
	}

	if (mount) {
		context.push_number(mount->id);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaGameGetTowns()
{
	// Game.getTowns()
	const auto& towns = g_game.map.towns.getTowns();
	lua_createtable(L, towns.size(), 0);

	int index = 0;
	for (auto townEntry : towns) {
		tfs::lua::pushUserdata(L, townEntry.second);
		tfs::lua::setMetatable(L, -1, "Town");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaGameGetHouses()
{
	// Game.getHouses()
	const auto& houses = g_game.map.houses.getHouses();
	lua_createtable(L, houses.size(), 0);

	int index = 0;
	for (auto houseEntry : houses) {
		tfs::lua::pushUserdata(L, houseEntry.second);
		tfs::lua::setMetatable(L, -1, "House");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaGameGetOutfits()
{
	// Game.getOutfits(playerSex)
	if (!isNumber(L, 1)) {
		context.push_nil();
		return 1;
	}

	PlayerSex_t playerSex = tfs::lua::getNumber<PlayerSex_t>(L, 1);
	if (playerSex > PLAYERSEX_LAST) {
		context.push_nil();
		return 1;
	}

	const auto& outfits = Outfits::getInstance().getOutfits(playerSex);
	lua_createtable(L, outfits.size(), 0);

	int index = 0;
	for (const auto& outfit : outfits) {
		tfs::lua::pushOutfit(L, &outfit);
		lua_rawseti(L, -2, ++index);
	}

	return 1;
}

int LuaScriptInterface::luaGameGetMounts()
{
	// Game.getMounts()
	const auto& mounts = g_game.mounts.getMounts();
	lua_createtable(L, mounts.size(), 0);

	int index = 0;
	for (const auto& mount : mounts) {
		lua_createtable(L, 0, 5);

		setField(L, "name", mount.name);
		setField(L, "speed", mount.speed);
		setField(L, "clientId", mount.clientId);
		setField(L, "id", mount.id);
		setField(L, "premium", mount.premium);

		lua_rawseti(L, -2, ++index);
	}

	return 1;
}

int LuaScriptInterface::luaGameGetVocations()
{
	// Game.getVocations()
	const auto& vocations = g_vocations.getVocations();
	lua_createtable(L, vocations.size(), 0);

	int index = 0;
	for (const auto& [id, vocation] : vocations) {
		tfs::lua::pushUserdata(L, &vocation);
		tfs::lua::setMetatable(L, -1, "Vocation");
		lua_rawseti(L, -2, ++index);
	}

	return 1;
}

int LuaScriptInterface::luaGameGetGameState()
{
	// Game.getGameState()
	context.push_number(g_game.getGameState());
	return 1;
}

int LuaScriptInterface::luaGameSetGameState()
{
	// Game.setGameState(state)
	GameState_t state = tfs::lua::getNumber<GameState_t>(L, 1);
	g_game.setGameState(state);
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaGameGetWorldType()
{
	// Game.getWorldType()
	context.push_number(g_game.getWorldType());
	return 1;
}

int LuaScriptInterface::luaGameSetWorldType()
{
	// Game.setWorldType(type)
	WorldType_t type = tfs::lua::getNumber<WorldType_t>(L, 1);
	g_game.setWorldType(type);
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaGameGetReturnMessage()
{
	// Game.getReturnMessage(value)
	ReturnValue value = tfs::lua::getNumber<ReturnValue>(L, 1);
	tfs::lua::pushString(L, getReturnMessage(value));
	return 1;
}

int LuaScriptInterface::luaGameGetItemAttributeByName()
{
	// Game.getItemAttributeByName(name)
	context.push_number(stringToItemAttribute(tfs::lua::getString(L, 1)));
	return 1;
}

int LuaScriptInterface::luaGameCreateItem()
{
	// Game.createItem(itemId[, count[, position]])
	uint16_t count = context.get_number<uint16_t>(2, 1);
	uint16_t id;
	if (isNumber(L, 1)) {
		id = context.get_number<uint16_t>(1);
	} else {
		id = Item::items.getItemIdByName(tfs::lua::getString(L, 1));
		if (id == 0) {
			context.push_nil();
			return 1;
		}
	}

	const ItemType& it = Item::items[id];
	if (it.stackable) {
		count = std::min<uint16_t>(count, ITEM_STACK_SIZE);
	}

	Item* item = Item::CreateItem(id, count);
	if (!item) {
		context.push_nil();
		return 1;
	}

	if (lua_gettop(L) >= 3) {
		const Position& position = tfs::lua::getPosition(L, 3);
		Tile* tile = g_game.map.getTile(position);
		if (!tile) {
			delete item;
			context.push_nil();
			return 1;
		}

		g_game.internalAddItem(tile, item, INDEX_WHEREEVER, FLAG_NOLIMIT);
	} else {
		addTempItem(item);
		item->setParent(VirtualCylinder::virtualCylinder);
	}

	tfs::lua::pushUserdata(L, item);
	tfs::lua::setItemMetatable(L, -1, item);
	return 1;
}

int LuaScriptInterface::luaGameCreateContainer()
{
	// Game.createContainer(itemId, size[, position])
	uint16_t size = context.get_number<uint16_t>(2);
	uint16_t id;
	if (isNumber(L, 1)) {
		id = context.get_number<uint16_t>(1);
	} else {
		id = Item::items.getItemIdByName(tfs::lua::getString(L, 1));
		if (id == 0) {
			context.push_nil();
			return 1;
		}
	}

	Container* container = Item::CreateItemAsContainer(id, size);
	if (!container) {
		context.push_nil();
		return 1;
	}

	if (lua_gettop(L) >= 3) {
		const Position& position = tfs::lua::getPosition(L, 3);
		Tile* tile = g_game.map.getTile(position);
		if (!tile) {
			delete container;
			context.push_nil();
			return 1;
		}

		g_game.internalAddItem(tile, container, INDEX_WHEREEVER, FLAG_NOLIMIT);
	} else {
		addTempItem(container);
		container->setParent(VirtualCylinder::virtualCylinder);
	}

	tfs::lua::pushUserdata(L, container);
	tfs::lua::setMetatable(L, -1, "Container");
	return 1;
}

int LuaScriptInterface::luaGameCreateMonster()
{
	// Game.createMonster(monsterName, position[, extended = false[, force = false[, magicEffect = CONST_ME_TELEPORT]]])
	Monster* monster = Monster::createMonster(tfs::lua::getString(L, 1));
	if (!monster) {
		context.push_nil();
		return 1;
	}

	const Position& position = tfs::lua::getPosition(L, 2);
	bool extended = tfs::lua::getBoolean(L, 3, false);
	bool force = tfs::lua::getBoolean(L, 4, false);
	MagicEffectClasses magicEffect = tfs::lua::getNumber<MagicEffectClasses>(L, 5, CONST_ME_TELEPORT);
	if (g_events->eventMonsterOnSpawn(monster, position, false, true) || force) {
		if (g_game.placeCreature(monster, position, extended, force, magicEffect)) {
			tfs::lua::pushUserdata(L, monster);
			tfs::lua::setMetatable(L, -1, "Monster");
		} else {
			delete monster;
			context.push_nil();
		}
	} else {
		delete monster;
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaGameCreateNpc()
{
	// Game.createNpc(npcName, position[, extended = false[, force = false[, magicEffect = CONST_ME_TELEPORT]]])
	Npc* npc = Npc::createNpc(tfs::lua::getString(L, 1));
	if (!npc) {
		context.push_nil();
		return 1;
	}

	const Position& position = tfs::lua::getPosition(L, 2);
	npc->setMasterPos(position);
	bool extended = tfs::lua::getBoolean(L, 3, false);
	bool force = tfs::lua::getBoolean(L, 4, false);
	MagicEffectClasses magicEffect = tfs::lua::getNumber<MagicEffectClasses>(L, 5, CONST_ME_TELEPORT);
	if (g_game.placeCreature(npc, position, extended, force, magicEffect)) {
		tfs::lua::pushUserdata(L, npc);
		tfs::lua::setMetatable(L, -1, "Npc");
	} else {
		delete npc;
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaGameCreateTile()
{
	// Game.createTile(x, y, z[, isDynamic = false])
	// Game.createTile(position[, isDynamic = false])
	Position position;
	bool isDynamic;
	if (lua_istable(L, 1)) {
		position = tfs::lua::getPosition(L, 1);
		isDynamic = tfs::lua::getBoolean(L, 2, false);
	} else {
		position.x = context.get_number<uint16_t>(1);
		position.y = context.get_number<uint16_t>(2);
		position.z = context.get_number<uint16_t>(3);
		isDynamic = tfs::lua::getBoolean(L, 4, false);
	}

	Tile* tile = g_game.map.getTile(position);
	if (!tile) {
		if (isDynamic) {
			tile = new DynamicTile(position.x, position.y, position.z);
		} else {
			tile = new StaticTile(position.x, position.y, position.z);
		}

		g_game.map.setTile(position, tile);
	}

	tfs::lua::pushUserdata(L, tile);
	tfs::lua::setMetatable(L, -1, "Tile");
	return 1;
}

int LuaScriptInterface::luaGameCreateMonsterType()
{
	// Game.createMonsterType(name)
	if (tfs::lua::getScriptEnv()->getScriptInterface() != &g_scripts->getScriptInterface()) {
		reportErrorFunc(L, "MonsterTypes can only be registered in the Scripts interface.");
		context.push_nil();
		return 1;
	}

	const std::string& name = tfs::lua::getString(L, 1);
	if (name.length() == 0) {
		context.push_nil();
		return 1;
	}

	MonsterType* monsterType = g_monsters.getMonsterType(name, false);
	if (!monsterType) {
		monsterType = &g_monsters.monsters[boost::algorithm::to_lower_copy(name)];
		monsterType->name = name;
		monsterType->nameDescription = "a " + name;
	} else {
		monsterType->info.lootItems.clear();
		monsterType->info.attackSpells.clear();
		monsterType->info.defenseSpells.clear();
		monsterType->info.scripts.clear();
		monsterType->info.thinkEvent = -1;
		monsterType->info.creatureAppearEvent = -1;
		monsterType->info.creatureDisappearEvent = -1;
		monsterType->info.creatureMoveEvent = -1;
		monsterType->info.creatureSayEvent = -1;
	}

	tfs::lua::pushUserdata(L, monsterType);
	tfs::lua::setMetatable(L, -1, "MonsterType");
	return 1;
}

int LuaScriptInterface::luaGameCreateNpcType()
{
	// Game.createNpcType(name)
	if (tfs::lua::getScriptEnv()->getScriptInterface() != Npcs::getScriptInterface()) {
		reportErrorFunc(L, "NpcTypes can only be registered in the Npcs interface.");
		context.push_nil();
		return 1;
	}

	const std::string& name = tfs::lua::getString(L, 1);
	if (name.length() == 0) {
		context.push_nil();
		return 1;
	}

	NpcType* npcType = Npcs::getNpcType(name);
	if (!npcType) {
		npcType = new NpcType();
		npcType->name = name;
		npcType->fromLua = true;
		Npcs::addNpcType(name, npcType);
	}

	tfs::lua::pushUserdata<NpcType>(L, npcType);
	tfs::lua::setMetatable(L, -1, "NpcType");
	return 1;
}

int LuaScriptInterface::luaGameStartEvent()
{
	// Game.startEvent(event)
	const std::string& eventName = tfs::lua::getString(L, 1);

	const auto& eventMap = g_globalEvents->getEventMap(GLOBALEVENT_TIMER);
	if (auto it = eventMap.find(eventName); it != eventMap.end()) {
		context.push_boolean(it->second.executeEvent());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaGameGetClientVersion()
{
	// Game.getClientVersion()
	lua_createtable(L, 0, 3);
	setField(L, "min", CLIENT_VERSION_MIN);
	setField(L, "max", CLIENT_VERSION_MAX);
	setField(L, "string", CLIENT_VERSION_STR);
	return 1;
}

int LuaScriptInterface::luaGameReload()
{
	// Game.reload(reloadType)
	ReloadTypes_t reloadType = tfs::lua::getNumber<ReloadTypes_t>(L, 1);
	if (reloadType == RELOAD_TYPE_GLOBAL) {
		context.push_boolean(g_luaEnvironment.loadFile("data/global.lua") == 0);
		context.push_boolean(g_scripts->loadScripts("scripts/lib", true, true));
		lua_gc(g_luaEnvironment.getLuaState(), LUA_GCCOLLECT, 0);
		return 2;
	}

	context.push_boolean(g_game.reload(reloadType));
	lua_gc(g_luaEnvironment.getLuaState(), LUA_GCCOLLECT, 0);
	return 1;
}

// Variant
int LuaScriptInterface::luaVariantCreate()
{
	// Variant(number or string or position or thing)
	LuaVariant variant;
	if (lua_isuserdata(L, 2)) {
		if (Thing* thing = tfs::lua::getThing(L, 2)) {
			variant.setTargetPosition(thing->getPosition());
		}
	} else if (lua_istable(L, 2)) {
		variant.setPosition(tfs::lua::getPosition(L, 2));
	} else if (isNumber(L, 2)) {
		variant.setNumber(context.get_number<uint32_t>(2));
	} else if (lua_isstring(L, 2)) {
		variant.setString(tfs::lua::getString(L, 2));
	}
	tfs::lua::pushVariant(L, variant);
	return 1;
}

int LuaScriptInterface::luaVariantGetNumber()
{
	// Variant:getNumber()
	const LuaVariant& variant = getVariant(L, 1);
	if (variant.isNumber()) {
		context.push_number(variant.getNumber());
	} else {
		context.push_number(0);
	}
	return 1;
}

int LuaScriptInterface::luaVariantGetString()
{
	// Variant:getString()
	const LuaVariant& variant = getVariant(L, 1);
	if (variant.isString()) {
		tfs::lua::pushString(L, variant.getString());
	} else {
		tfs::lua::pushString(L, std::string());
	}
	return 1;
}

int LuaScriptInterface::luaVariantGetPosition()
{
	// Variant:getPosition()
	const LuaVariant& variant = getVariant(L, 1);
	if (variant.isPosition()) {
		tfs::lua::pushPosition(L, variant.getPosition());
	} else if (variant.isTargetPosition()) {
		tfs::lua::pushPosition(L, variant.getTargetPosition());
	} else {
		tfs::lua::pushPosition(L, Position());
	}
	return 1;
}

// Position
int LuaScriptInterface::luaPositionCreate()
{
	// Position([x = 0[, y = 0[, z = 0[, stackpos = 0]]]])
	// Position([position])
	if (lua_gettop(L) <= 1) {
		tfs::lua::pushPosition(L, Position());
		return 1;
	}

	int32_t stackpos;
	if (lua_istable(L, 2)) {
		const Position& position = tfs::lua::getPosition(L, 2, stackpos);
		tfs::lua::pushPosition(L, position, stackpos);
	} else {
		uint16_t x = context.get_number<uint16_t>(2, 0);
		uint16_t y = context.get_number<uint16_t>(3, 0);
		uint8_t z = context.get_number<uint8_t>(4, 0);
		stackpos = tfs::lua::getNumber<int32_t>(L, 5, 0);

		tfs::lua::pushPosition(L, Position(x, y, z), stackpos);
	}
	return 1;
}

int LuaScriptInterface::luaPositionIsSightClear()
{
	// position:isSightClear(positionEx[, sameFloor = true])
	bool sameFloor = tfs::lua::getBoolean(L, 3, true);
	const Position& positionEx = tfs::lua::getPosition(L, 2);
	const Position& position = tfs::lua::getPosition(L, 1);
	context.push_boolean(g_game.isSightClear(position, positionEx, sameFloor));
	return 1;
}

int LuaScriptInterface::luaPositionSendMagicEffect()
{
	// position:sendMagicEffect(magicEffect[, player = nullptr])
	Spectators spectators;
	if (lua_gettop(L) >= 3) {
		Player* player = tfs::lua::getPlayer(L, 3);
		if (player) {
			spectators.insert(player);
		}
	}

	MagicEffectClasses magicEffect = tfs::lua::getNumber<MagicEffectClasses>(L, 2);
	if (magicEffect == CONST_ME_NONE) {
		context.push_boolean(false);
		return 1;
	}

	const Position& position = tfs::lua::getPosition(L, 1);
	if (!spectators.empty()) {
		Game::addMagicEffect(spectators, position, magicEffect);
	} else {
		g_game.addMagicEffect(position, magicEffect);
	}

	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaPositionSendDistanceEffect()
{
	// position:sendDistanceEffect(positionEx, distanceEffect[, player = nullptr])
	Spectators spectators;
	if (lua_gettop(L) >= 4) {
		Player* player = tfs::lua::getPlayer(L, 4);
		if (player) {
			spectators.insert(player);
		}
	}

	ShootType_t distanceEffect = tfs::lua::getNumber<ShootType_t>(L, 3);
	const Position& positionEx = tfs::lua::getPosition(L, 2);
	const Position& position = tfs::lua::getPosition(L, 1);
	if (!spectators.empty()) {
		Game::addDistanceEffect(spectators, position, positionEx, distanceEffect);
	} else {
		g_game.addDistanceEffect(position, positionEx, distanceEffect);
	}

	context.push_boolean(true);
	return 1;
}

// Tile
int LuaScriptInterface::luaTileCreate()
{
	// Tile(x, y, z)
	// Tile(position)
	Tile* tile;
	if (lua_istable(L, 2)) {
		tile = g_game.map.getTile(tfs::lua::getPosition(L, 2));
	} else {
		uint8_t z = context.get_number<uint8_t>(4);
		uint16_t y = context.get_number<uint16_t>(3);
		uint16_t x = context.get_number<uint16_t>(2);
		tile = g_game.map.getTile(x, y, z);
	}

	if (tile) {
		tfs::lua::pushUserdata(L, tile);
		tfs::lua::setMetatable(L, -1, "Tile");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaTileRemove()
{
	// tile:remove()
	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (!tile) {
		context.push_nil();
		return 1;
	}

	if (g_game.isTileInCleanList(tile)) {
		g_game.removeTileToClean(tile);
	}

	g_game.map.removeTile(tile->getPosition());
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaTileGetPosition()
{
	// tile:getPosition()
	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (tile) {
		tfs::lua::pushPosition(L, tile->getPosition());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaTileGetGround()
{
	// tile:getGround()
	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (tile && tile->getGround()) {
		tfs::lua::pushUserdata(L, tile->getGround());
		tfs::lua::setItemMetatable(L, -1, tile->getGround());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaTileGetThing()
{
	// tile:getThing(index)
	int32_t index = tfs::lua::getNumber<int32_t>(L, 2);
	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (!tile) {
		context.push_nil();
		return 1;
	}

	Thing* thing = tile->getThing(index);
	if (!thing) {
		context.push_nil();
		return 1;
	}

	if (Creature* creature = thing->getCreature()) {
		tfs::lua::pushUserdata(L, creature);
		tfs::lua::setCreatureMetatable(L, -1, creature);
	} else if (Item* item = thing->getItem()) {
		tfs::lua::pushUserdata(L, item);
		tfs::lua::setItemMetatable(L, -1, item);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaTileGetThingCount()
{
	// tile:getThingCount()
	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (tile) {
		context.push_number(tile->getThingCount());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaTileGetTopVisibleThing()
{
	// tile:getTopVisibleThing(creature)
	Creature* creature = tfs::lua::getCreature(L, 2);
	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (!tile) {
		context.push_nil();
		return 1;
	}

	Thing* thing = tile->getTopVisibleThing(creature);
	if (!thing) {
		context.push_nil();
		return 1;
	}

	if (Creature* visibleCreature = thing->getCreature()) {
		tfs::lua::pushUserdata(L, visibleCreature);
		tfs::lua::setCreatureMetatable(L, -1, visibleCreature);
	} else if (Item* visibleItem = thing->getItem()) {
		tfs::lua::pushUserdata(L, visibleItem);
		tfs::lua::setItemMetatable(L, -1, visibleItem);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaTileGetTopTopItem()
{
	// tile:getTopTopItem()
	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (!tile) {
		context.push_nil();
		return 1;
	}

	Item* item = tile->getTopTopItem();
	if (item) {
		tfs::lua::pushUserdata(L, item);
		tfs::lua::setItemMetatable(L, -1, item);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaTileGetTopDownItem()
{
	// tile:getTopDownItem()
	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (!tile) {
		context.push_nil();
		return 1;
	}

	Item* item = tile->getTopDownItem();
	if (item) {
		tfs::lua::pushUserdata(L, item);
		tfs::lua::setItemMetatable(L, -1, item);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaTileGetFieldItem()
{
	// tile:getFieldItem()
	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (!tile) {
		context.push_nil();
		return 1;
	}

	Item* item = tile->getFieldItem();
	if (item) {
		tfs::lua::pushUserdata(L, item);
		tfs::lua::setItemMetatable(L, -1, item);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaTileGetItemById()
{
	// tile:getItemById(itemId[, subType = -1])
	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (!tile) {
		context.push_nil();
		return 1;
	}

	uint16_t itemId;
	if (isNumber(L, 2)) {
		itemId = context.get_number<uint16_t>(2);
	} else {
		itemId = Item::items.getItemIdByName(tfs::lua::getString(L, 2));
		if (itemId == 0) {
			context.push_nil();
			return 1;
		}
	}
	int32_t subType = tfs::lua::getNumber<int32_t>(L, 3, -1);

	Item* item = g_game.findItemOfType(tile, itemId, false, subType);
	if (item) {
		tfs::lua::pushUserdata(L, item);
		tfs::lua::setItemMetatable(L, -1, item);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaTileGetItemByType()
{
	// tile:getItemByType(itemType)
	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (!tile) {
		context.push_nil();
		return 1;
	}

	bool found;

	ItemTypes_t itemType = tfs::lua::getNumber<ItemTypes_t>(L, 2);
	switch (itemType) {
		case ITEM_TYPE_TELEPORT:
			found = tile->hasFlag(TILESTATE_TELEPORT);
			break;
		case ITEM_TYPE_MAGICFIELD:
			found = tile->hasFlag(TILESTATE_MAGICFIELD);
			break;
		case ITEM_TYPE_MAILBOX:
			found = tile->hasFlag(TILESTATE_MAILBOX);
			break;
		case ITEM_TYPE_TRASHHOLDER:
			found = tile->hasFlag(TILESTATE_TRASHHOLDER);
			break;
		case ITEM_TYPE_BED:
			found = tile->hasFlag(TILESTATE_BED);
			break;
		case ITEM_TYPE_DEPOT:
			found = tile->hasFlag(TILESTATE_DEPOT);
			break;
		default:
			found = true;
			break;
	}

	if (!found) {
		context.push_nil();
		return 1;
	}

	if (Item* item = tile->getGround()) {
		const ItemType& it = Item::items[item->getID()];
		if (it.type == itemType) {
			tfs::lua::pushUserdata(L, item);
			tfs::lua::setItemMetatable(L, -1, item);
			return 1;
		}
	}

	if (const TileItemVector* items = tile->getItemList()) {
		for (Item* item : *items) {
			const ItemType& it = Item::items[item->getID()];
			if (it.type == itemType) {
				tfs::lua::pushUserdata(L, item);
				tfs::lua::setItemMetatable(L, -1, item);
				return 1;
			}
		}
	}

	context.push_nil();
	return 1;
}

int LuaScriptInterface::luaTileGetItemByTopOrder()
{
	// tile:getItemByTopOrder(topOrder)
	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (!tile) {
		context.push_nil();
		return 1;
	}

	int32_t topOrder = tfs::lua::getNumber<int32_t>(L, 2);

	Item* item = tile->getItemByTopOrder(topOrder);
	if (!item) {
		context.push_nil();
		return 1;
	}

	tfs::lua::pushUserdata(L, item);
	tfs::lua::setItemMetatable(L, -1, item);
	return 1;
}

int LuaScriptInterface::luaTileGetItemCountById()
{
	// tile:getItemCountById(itemId[, subType = -1])
	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (!tile) {
		context.push_nil();
		return 1;
	}

	int32_t subType = tfs::lua::getNumber<int32_t>(L, 3, -1);

	uint16_t itemId;
	if (isNumber(L, 2)) {
		itemId = context.get_number<uint16_t>(2);
	} else {
		itemId = Item::items.getItemIdByName(tfs::lua::getString(L, 2));
		if (itemId == 0) {
			context.push_nil();
			return 1;
		}
	}

	context.push_number(tile->getItemTypeCount(itemId, subType));
	return 1;
}

int LuaScriptInterface::luaTileGetBottomCreature()
{
	// tile:getBottomCreature()
	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (!tile) {
		context.push_nil();
		return 1;
	}

	const Creature* creature = tile->getBottomCreature();
	if (!creature) {
		context.push_nil();
		return 1;
	}

	tfs::lua::pushUserdata(L, creature);
	tfs::lua::setCreatureMetatable(L, -1, creature);
	return 1;
}

int LuaScriptInterface::luaTileGetTopCreature()
{
	// tile:getTopCreature()
	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (!tile) {
		context.push_nil();
		return 1;
	}

	Creature* creature = tile->getTopCreature();
	if (!creature) {
		context.push_nil();
		return 1;
	}

	tfs::lua::pushUserdata(L, creature);
	tfs::lua::setCreatureMetatable(L, -1, creature);
	return 1;
}

int LuaScriptInterface::luaTileGetBottomVisibleCreature()
{
	// tile:getBottomVisibleCreature(creature)
	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (!tile) {
		context.push_nil();
		return 1;
	}

	Creature* creature = tfs::lua::getCreature(L, 2);
	if (!creature) {
		context.push_nil();
		return 1;
	}

	const Creature* visibleCreature = tile->getBottomVisibleCreature(creature);
	if (visibleCreature) {
		tfs::lua::pushUserdata(L, visibleCreature);
		tfs::lua::setCreatureMetatable(L, -1, visibleCreature);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaTileGetTopVisibleCreature()
{
	// tile:getTopVisibleCreature(creature)
	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (!tile) {
		context.push_nil();
		return 1;
	}

	Creature* creature = tfs::lua::getCreature(L, 2);
	if (!creature) {
		context.push_nil();
		return 1;
	}

	Creature* visibleCreature = tile->getTopVisibleCreature(creature);
	if (visibleCreature) {
		tfs::lua::pushUserdata(L, visibleCreature);
		tfs::lua::setCreatureMetatable(L, -1, visibleCreature);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaTileGetItems()
{
	// tile:getItems()
	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (!tile) {
		context.push_nil();
		return 1;
	}

	TileItemVector* itemVector = tile->getItemList();
	if (!itemVector) {
		context.push_nil();
		return 1;
	}

	lua_createtable(L, itemVector->size(), 0);

	int index = 0;
	for (Item* item : *itemVector) {
		tfs::lua::pushUserdata(L, item);
		tfs::lua::setItemMetatable(L, -1, item);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaTileGetItemCount()
{
	// tile:getItemCount()
	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (!tile) {
		context.push_nil();
		return 1;
	}

	context.push_number(tile->getItemCount());
	return 1;
}

int LuaScriptInterface::luaTileGetDownItemCount()
{
	// tile:getDownItemCount()
	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (tile) {
		context.push_number(tile->getDownItemCount());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaTileGetTopItemCount()
{
	// tile:getTopItemCount()
	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (!tile) {
		context.push_nil();
		return 1;
	}

	context.push_number(tile->getTopItemCount());
	return 1;
}

int LuaScriptInterface::luaTileGetCreatures()
{
	// tile:getCreatures()
	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (!tile) {
		context.push_nil();
		return 1;
	}

	CreatureVector* creatureVector = tile->getCreatures();
	if (!creatureVector) {
		context.push_nil();
		return 1;
	}

	lua_createtable(L, creatureVector->size(), 0);

	int index = 0;
	for (Creature* creature : *creatureVector) {
		tfs::lua::pushUserdata(L, creature);
		tfs::lua::setCreatureMetatable(L, -1, creature);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaTileGetCreatureCount()
{
	// tile:getCreatureCount()
	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (!tile) {
		context.push_nil();
		return 1;
	}

	context.push_number(tile->getCreatureCount());
	return 1;
}

int LuaScriptInterface::luaTileHasProperty()
{
	// tile:hasProperty(property[, item])
	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (!tile) {
		context.push_nil();
		return 1;
	}

	Item* item;
	if (lua_gettop(L) >= 3) {
		item = tfs::lua::getUserdata<Item>(L, 3);
	} else {
		item = nullptr;
	}

	ITEMPROPERTY property = tfs::lua::getNumber<ITEMPROPERTY>(L, 2);
	if (item) {
		context.push_boolean(tile->hasProperty(item, property));
	} else {
		context.push_boolean(tile->hasProperty(property));
	}
	return 1;
}

int LuaScriptInterface::luaTileGetThingIndex()
{
	// tile:getThingIndex(thing)
	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (!tile) {
		context.push_nil();
		return 1;
	}

	Thing* thing = tfs::lua::getThing(L, 2);
	if (thing) {
		context.push_number(tile->getThingIndex(thing));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaTileHasFlag()
{
	// tile:hasFlag(flag)
	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (tile) {
		tileflags_t flag = tfs::lua::getNumber<tileflags_t>(L, 2);
		context.push_boolean(tile->hasFlag(flag));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaTileQueryAdd()
{
	// tile:queryAdd(thing[, flags])
	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (!tile) {
		context.push_nil();
		return 1;
	}

	Thing* thing = tfs::lua::getThing(L, 2);
	if (thing) {
		uint32_t flags = context.get_number<uint32_t>(3, 0);
		context.push_number(tile->queryAdd(0, *thing, 1, flags));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaTileAddItem()
{
	// tile:addItem(itemId[, count/subType = 1[, flags = 0]])
	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (!tile) {
		context.push_nil();
		return 1;
	}

	uint16_t itemId;
	if (isNumber(L, 2)) {
		itemId = context.get_number<uint16_t>(2);
	} else {
		itemId = Item::items.getItemIdByName(tfs::lua::getString(L, 2));
		if (itemId == 0) {
			context.push_nil();
			return 1;
		}
	}

	uint32_t subType = context.get_number<uint32_t>(3, 1);

	Item* item = Item::CreateItem(itemId, std::min<uint32_t>(subType, ITEM_STACK_SIZE));
	if (!item) {
		context.push_nil();
		return 1;
	}

	uint32_t flags = context.get_number<uint32_t>(4, 0);

	ReturnValue ret = g_game.internalAddItem(tile, item, INDEX_WHEREEVER, flags);
	if (ret == RETURNVALUE_NOERROR) {
		tfs::lua::pushUserdata(L, item);
		tfs::lua::setItemMetatable(L, -1, item);
	} else {
		delete item;
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaTileAddItemEx()
{
	// tile:addItemEx(item[, flags = 0])
	Item* item = tfs::lua::getUserdata<Item>(L, 2);
	if (!item) {
		context.push_nil();
		return 1;
	}

	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (!tile) {
		context.push_nil();
		return 1;
	}

	if (item->getParent() != VirtualCylinder::virtualCylinder) {
		reportErrorFunc(L, "Item already has a parent");
		context.push_nil();
		return 1;
	}

	uint32_t flags = context.get_number<uint32_t>(3, 0);
	ReturnValue ret = g_game.internalAddItem(tile, item, INDEX_WHEREEVER, flags);
	if (ret == RETURNVALUE_NOERROR) {
		tfs::lua::removeTempItem(item);
	}
	context.push_number(ret);
	return 1;
}

int LuaScriptInterface::luaTileGetHouse()
{
	// tile:getHouse()
	Tile* tile = tfs::lua::getUserdata<Tile>(L, 1);
	if (!tile) {
		context.push_nil();
		return 1;
	}

	if (HouseTile* houseTile = dynamic_cast<HouseTile*>(tile)) {
		tfs::lua::pushUserdata(L, houseTile->getHouse());
		tfs::lua::setMetatable(L, -1, "House");
	} else {
		context.push_nil();
	}
	return 1;
}

// NetworkMessage
int LuaScriptInterface::luaNetworkMessageCreate()
{
	// NetworkMessage()
	tfs::lua::pushUserdata(L, new NetworkMessage);
	tfs::lua::setMetatable(L, -1, "NetworkMessage");
	return 1;
}

int LuaScriptInterface::luaNetworkMessageDelete()
{
	NetworkMessage** messagePtr = tfs::lua::getRawUserdata<NetworkMessage>(L, 1);
	if (messagePtr && *messagePtr) {
		delete *messagePtr;
		*messagePtr = nullptr;
	}
	return 0;
}

int LuaScriptInterface::luaNetworkMessageGetByte()
{
	// networkMessage:getByte()
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		context.push_number(message->getByte());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageGetU16()
{
	// networkMessage:getU16()
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		context.push_number(message->get<uint16_t>());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageGetU32()
{
	// networkMessage:getU32()
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		context.push_number(message->get<uint32_t>());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageGetU64()
{
	// networkMessage:getU64()
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		context.push_number(message->get<uint64_t>());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageGetString()
{
	// networkMessage:getString()
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		tfs::lua::pushString(L, message->getString());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageGetPosition()
{
	// networkMessage:getPosition()
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		tfs::lua::pushPosition(L, message->getPosition());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageAddByte()
{
	// networkMessage:addByte(number)
	uint8_t number = context.get_number<uint8_t>(2);
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->addByte(number);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageAddU16()
{
	// networkMessage:addU16(number)
	uint16_t number = context.get_number<uint16_t>(2);
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->add<uint16_t>(number);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageAddU32()
{
	// networkMessage:addU32(number)
	uint32_t number = context.get_number<uint32_t>(2);
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->add<uint32_t>(number);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageAddU64()
{
	// networkMessage:addU64(number)
	uint64_t number = tfs::lua::getNumber<uint64_t>(L, 2);
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->add<uint64_t>(number);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageAddString()
{
	// networkMessage:addString(string)
	const std::string& string = tfs::lua::getString(L, 2);
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->addString(string);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageAddPosition()
{
	// networkMessage:addPosition(position)
	const Position& position = tfs::lua::getPosition(L, 2);
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->addPosition(position);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageAddDouble()
{
	// networkMessage:addDouble(number)
	double number = tfs::lua::getNumber<double>(L, 2);
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->addDouble(number);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageAddItem()
{
	// networkMessage:addItem(item)
	Item* item = tfs::lua::getUserdata<Item>(L, 2);
	if (!item) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_ITEM_NOT_FOUND));
		context.push_nil();
		return 1;
	}

	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->addItem(item);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageAddItemId()
{
	// networkMessage:addItemId(itemId)
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (!message) {
		context.push_nil();
		return 1;
	}

	uint16_t itemId;
	if (isNumber(L, 2)) {
		itemId = context.get_number<uint16_t>(2);
	} else {
		itemId = Item::items.getItemIdByName(tfs::lua::getString(L, 2));
		if (itemId == 0) {
			context.push_nil();
			return 1;
		}
	}

	message->addItemId(itemId);
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaNetworkMessageReset()
{
	// networkMessage:reset()
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->reset();
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageSeek()
{
	// networkMessage:seek(position)
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message && isNumber(L, 2)) {
		context.push_boolean(message->setBufferPosition(context.get_number<uint16_t>(2)));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageTell()
{
	// networkMessage:tell()
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		context.push_number(message->getBufferPosition() - message->INITIAL_BUFFER_POSITION);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageLength()
{
	// networkMessage:len()
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		context.push_number(message->getLength());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageSkipBytes()
{
	// networkMessage:skipBytes(number)
	int16_t number = tfs::lua::getNumber<int16_t>(L, 2);
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->skipBytes(number);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageSendToPlayer()
{
	// networkMessage:sendToPlayer(player)
	NetworkMessage* message = tfs::lua::getUserdata<NetworkMessage>(L, 1);
	if (!message) {
		context.push_nil();
		return 1;
	}

	Player* player = tfs::lua::getPlayer(L, 2);
	if (player) {
		player->sendNetworkMessage(*message);
		context.push_boolean(true);
	} else {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_PLAYER_NOT_FOUND));
		context.push_nil();
	}
	return 1;
}

// ModalWindow
int LuaScriptInterface::luaModalWindowCreate()
{
	// ModalWindow(id, title, message)
	const std::string& message = tfs::lua::getString(L, 4);
	const std::string& title = tfs::lua::getString(L, 3);
	uint32_t id = context.get_number<uint32_t>(2);

	tfs::lua::pushUserdata(L, new ModalWindow(id, title, message));
	tfs::lua::setMetatable(L, -1, "ModalWindow");
	return 1;
}

int LuaScriptInterface::luaModalWindowDelete()
{
	ModalWindow** windowPtr = tfs::lua::getRawUserdata<ModalWindow>(L, 1);
	if (windowPtr && *windowPtr) {
		delete *windowPtr;
		*windowPtr = nullptr;
	}
	return 0;
}

int LuaScriptInterface::luaModalWindowGetId()
{
	// modalWindow:getId()
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		context.push_number(window->id);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaModalWindowGetTitle()
{
	// modalWindow:getTitle()
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		tfs::lua::pushString(L, window->title);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaModalWindowGetMessage()
{
	// modalWindow:getMessage()
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		tfs::lua::pushString(L, window->message);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaModalWindowSetTitle()
{
	// modalWindow:setTitle(text)
	const std::string& text = tfs::lua::getString(L, 2);
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		window->title = text;
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaModalWindowSetMessage()
{
	// modalWindow:setMessage(text)
	const std::string& text = tfs::lua::getString(L, 2);
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		window->message = text;
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}\

int LuaScriptInterface::luaModalWindowGetButtonCount()
{
	// modalWindow:getButtonCount()
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		context.push_number(window->buttons.size());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaModalWindowGetChoiceCount()
{
	// modalWindow:getChoiceCount()
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		context.push_number(window->choices.size());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaModalWindowAddButton()
{
	// modalWindow:addButton(id, text)
	const std::string& text = tfs::lua::getString(L, 3);
	uint8_t id = context.get_number<uint8_t>(2);
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		window->buttons.emplace_back(text, id);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaModalWindowAddChoice()
{
	// modalWindow:addChoice(id, text)
	const std::string& text = tfs::lua::getString(L, 3);
	uint8_t id = context.get_number<uint8_t>(2);
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		window->choices.emplace_back(text, id);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaModalWindowGetDefaultEnterButton()
{
	// modalWindow:getDefaultEnterButton()
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		context.push_number(window->defaultEnterButton);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaModalWindowSetDefaultEnterButton()
{
	// modalWindow:setDefaultEnterButton(buttonId)
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		window->defaultEnterButton = context.get_number<uint8_t>(2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaModalWindowGetDefaultEscapeButton()
{
	// modalWindow:getDefaultEscapeButton()
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		context.push_number(window->defaultEscapeButton);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaModalWindowSetDefaultEscapeButton()
{
	// modalWindow:setDefaultEscapeButton(buttonId)
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		window->defaultEscapeButton = context.get_number<uint8_t>(2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaModalWindowHasPriority()
{
	// modalWindow:hasPriority()
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		context.push_boolean(window->priority);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaModalWindowSetPriority()
{
	// modalWindow:setPriority(priority)
	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		window->priority = tfs::lua::getBoolean(L, 2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaModalWindowSendToPlayer()
{
	// modalWindow:sendToPlayer(player)
	Player* player = tfs::lua::getPlayer(L, 2);
	if (!player) {
		context.push_nil();
		return 1;
	}

	ModalWindow* window = tfs::lua::getUserdata<ModalWindow>(L, 1);
	if (window) {
		if (!player->hasModalWindowOpen(window->id)) {
			player->sendModalWindow(*window);
		}
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

// Item
int LuaScriptInterface::luaItemCreate()
{
	// Item(uid)
	uint32_t id = context.get_number<uint32_t>(2);

	Item* item = tfs::lua::getScriptEnv()->getItemByUID(id);
	if (item) {
		tfs::lua::pushUserdata(L, item);
		tfs::lua::setItemMetatable(L, -1, item);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemIsItem()
{
	// item:isItem()
	context.push_boolean(tfs::lua::getUserdata<const Item>(L, 1) != nullptr);
	return 1;
}

int LuaScriptInterface::luaItemGetParent()
{
	// item:getParent()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (!item) {
		context.push_nil();
		return 1;
	}

	Cylinder* parent = item->getParent();
	if (!parent) {
		context.push_nil();
		return 1;
	}

	tfs::lua::pushCylinder(L, parent);
	return 1;
}

int LuaScriptInterface::luaItemGetTopParent()
{
	// item:getTopParent()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (!item) {
		context.push_nil();
		return 1;
	}

	Cylinder* topParent = item->getTopParent();
	if (!topParent) {
		context.push_nil();
		return 1;
	}

	tfs::lua::pushCylinder(L, topParent);
	return 1;
}

int LuaScriptInterface::luaItemGetId()
{
	// item:getId()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		context.push_number(item->getID());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemClone()
{
	// item:clone()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (!item) {
		context.push_nil();
		return 1;
	}

	Item* clone = item->clone();
	if (!clone) {
		context.push_nil();
		return 1;
	}

	addTempItem(clone);
	clone->setParent(VirtualCylinder::virtualCylinder);

	tfs::lua::pushUserdata(L, clone);
	tfs::lua::setItemMetatable(L, -1, clone);
	return 1;
}

int LuaScriptInterface::luaItemSplit()
{
	// item:split([count = 1])
	Item** itemPtr = tfs::lua::getRawUserdata<Item>(L, 1);
	if (!itemPtr) {
		context.push_nil();
		return 1;
	}

	Item* item = *itemPtr;
	if (!item || !item->isStackable()) {
		context.push_nil();
		return 1;
	}

	uint16_t count = std::min<uint16_t>(context.get_number<uint16_t>(2, 1), item->getItemCount());
	uint16_t diff = item->getItemCount() - count;

	Item* splitItem = item->clone();
	if (!splitItem) {
		context.push_nil();
		return 1;
	}

	splitItem->setItemCount(count);

	ScriptEnvironment* env = tfs::lua::getScriptEnv();
	uint32_t uid = env->addThing(item);

	Item* newItem = g_game.transformItem(item, item->getID(), diff);
	if (item->isRemoved()) {
		env->removeItemByUID(uid);
	}

	if (newItem && newItem != item) {
		env->insertItem(uid, newItem);
	}

	*itemPtr = newItem;

	splitItem->setParent(VirtualCylinder::virtualCylinder);
	addTempItem(splitItem);

	tfs::lua::pushUserdata(L, splitItem);
	tfs::lua::setItemMetatable(L, -1, splitItem);
	return 1;
}

int LuaScriptInterface::luaItemRemove()
{
	// item:remove([count = -1])
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		int32_t count = tfs::lua::getNumber<int32_t>(L, 2, -1);
		context.push_boolean(g_game.internalRemoveItem(item, count) == RETURNVALUE_NOERROR);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemGetUniqueId()
{
	// item:getUniqueId()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		uint32_t uniqueId = item->getUniqueId();
		if (uniqueId == 0) {
			uniqueId = tfs::lua::getScriptEnv()->addThing(item);
		}
		context.push_number(uniqueId);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemGetActionId()
{
	// item:getActionId()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		context.push_number(item->getActionId());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemSetActionId()
{
	// item:setActionId(actionId)
	uint16_t actionId = context.get_number<uint16_t>(2);
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		item->setActionId(actionId);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemGetCount()
{
	// item:getCount()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		context.push_number(item->getItemCount());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemGetCharges()
{
	// item:getCharges()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		context.push_number(item->getCharges());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemGetFluidType()
{
	// item:getFluidType()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		context.push_number(item->getFluidType());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemGetWeight()
{
	// item:getWeight()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		context.push_number(item->getWeight());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemGetWorth()
{
	// item:getWorth()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		context.push_number(item->getWorth());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemGetSubType()
{
	// item:getSubType()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		context.push_number(item->getSubType());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemGetName()
{
	// item:getName()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		tfs::lua::pushString(L, item->getName());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemGetPluralName()
{
	// item:getPluralName()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		tfs::lua::pushString(L, item->getPluralName());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemGetArticle()
{
	// item:getArticle()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		tfs::lua::pushString(L, item->getArticle());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemGetPosition()
{
	// item:getPosition()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		tfs::lua::pushPosition(L, item->getPosition());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemGetTile()
{
	// item:getTile()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (!item) {
		context.push_nil();
		return 1;
	}

	Tile* tile = item->getTile();
	if (tile) {
		tfs::lua::pushUserdata(L, tile);
		tfs::lua::setMetatable(L, -1, "Tile");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemHasAttribute()
{
	// item:hasAttribute(key)
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (!item) {
		context.push_nil();
		return 1;
	}

	itemAttrTypes attribute;
	if (isNumber(L, 2)) {
		attribute = tfs::lua::getNumber<itemAttrTypes>(L, 2);
	} else if (lua_isstring(L, 2)) {
		attribute = stringToItemAttribute(tfs::lua::getString(L, 2));
	} else {
		attribute = ITEM_ATTRIBUTE_NONE;
	}

	context.push_boolean(item->hasAttribute(attribute));
	return 1;
}

int LuaScriptInterface::luaItemGetAttribute()
{
	// item:getAttribute(key)
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (!item) {
		context.push_nil();
		return 1;
	}

	itemAttrTypes attribute;
	if (isNumber(L, 2)) {
		attribute = tfs::lua::getNumber<itemAttrTypes>(L, 2);
	} else if (lua_isstring(L, 2)) {
		attribute = stringToItemAttribute(tfs::lua::getString(L, 2));
	} else {
		attribute = ITEM_ATTRIBUTE_NONE;
	}

	if (ItemAttributes::isIntAttrType(attribute)) {
		context.push_number(item->getIntAttr(attribute));
	} else if (ItemAttributes::isStrAttrType(attribute)) {
		tfs::lua::pushString(L, item->getStrAttr(attribute));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemSetAttribute()
{
	// item:setAttribute(key, value)
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (!item) {
		context.push_nil();
		return 1;
	}

	itemAttrTypes attribute;
	if (isNumber(L, 2)) {
		attribute = tfs::lua::getNumber<itemAttrTypes>(L, 2);
	} else if (lua_isstring(L, 2)) {
		attribute = stringToItemAttribute(tfs::lua::getString(L, 2));
	} else {
		attribute = ITEM_ATTRIBUTE_NONE;
	}

	if (ItemAttributes::isIntAttrType(attribute)) {
		if (attribute == ITEM_ATTRIBUTE_UNIQUEID) {
			reportErrorFunc(L, "Attempt to set protected key \"uid\"");
			context.push_boolean(false);
			return 1;
		}

		item->setIntAttr(attribute, tfs::lua::getNumber<int32_t>(L, 3));
		context.push_boolean(true);
	} else if (ItemAttributes::isStrAttrType(attribute)) {
		item->setStrAttr(attribute, tfs::lua::getString(L, 3));
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemRemoveAttribute()
{
	// item:removeAttribute(key)
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (!item) {
		context.push_nil();
		return 1;
	}

	itemAttrTypes attribute;
	if (isNumber(L, 2)) {
		attribute = tfs::lua::getNumber<itemAttrTypes>(L, 2);
	} else if (lua_isstring(L, 2)) {
		attribute = stringToItemAttribute(tfs::lua::getString(L, 2));
	} else {
		attribute = ITEM_ATTRIBUTE_NONE;
	}

	bool ret = attribute != ITEM_ATTRIBUTE_UNIQUEID;
	if (ret) {
		item->removeAttribute(attribute);
	} else {
		reportErrorFunc(L, "Attempt to erase protected key \"uid\"");
	}
	context.push_boolean(ret);
	return 1;
}

int LuaScriptInterface::luaItemGetCustomAttribute()
{
	// item:getCustomAttribute(key)
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (!item) {
		context.push_nil();
		return 1;
	}

	const ItemAttributes::CustomAttribute* attr;
	if (isNumber(L, 2)) {
		attr = item->getCustomAttribute(tfs::lua::getNumber<int64_t>(L, 2));
	} else if (lua_isstring(L, 2)) {
		attr = item->getCustomAttribute(tfs::lua::getString(L, 2));
	} else {
		context.push_nil();
		return 1;
	}

	if (attr) {
		attr->pushToLua(L);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemSetCustomAttribute()
{
	// item:setCustomAttribute(key, value)
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (!item) {
		context.push_nil();
		return 1;
	}

	std::string key;
	if (isNumber(L, 2)) {
		key = std::to_string(tfs::lua::getNumber<int64_t>(L, 2));
	} else if (lua_isstring(L, 2)) {
		key = tfs::lua::getString(L, 2);
	} else {
		context.push_nil();
		return 1;
	}

	ItemAttributes::CustomAttribute val;
	if (isNumber(L, 3)) {
		double tmp = tfs::lua::getNumber<double>(L, 3);
		if (std::floor(tmp) < tmp) {
			val.set<double>(tmp);
		} else {
			val.set<int64_t>(tmp);
		}
	} else if (lua_isstring(L, 3)) {
		val.set<std::string>(tfs::lua::getString(L, 3));
	} else if (lua_isboolean(L, 3)) {
		val.set<bool>(tfs::lua::getBoolean(L, 3));
	} else {
		context.push_nil();
		return 1;
	}

	item->setCustomAttribute(key, val);
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaItemRemoveCustomAttribute()
{
	// item:removeCustomAttribute(key)
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (!item) {
		context.push_nil();
		return 1;
	}

	if (isNumber(L, 2)) {
		context.push_boolean(item->removeCustomAttribute(tfs::lua::getNumber<int64_t>(L, 2)));
	} else if (lua_isstring(L, 2)) {
		context.push_boolean(item->removeCustomAttribute(tfs::lua::getString(L, 2)));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemMoveTo()
{
	// item:moveTo(position or cylinder[, flags])
	Item** itemPtr = tfs::lua::getRawUserdata<Item>(L, 1);
	if (!itemPtr) {
		context.push_nil();
		return 1;
	}

	Item* item = *itemPtr;
	if (!item || item->isRemoved()) {
		context.push_nil();
		return 1;
	}

	Cylinder* toCylinder;
	if (lua_isuserdata(L, 2)) {
		const LuaDataType type = getUserdataType(L, 2);
		switch (type) {
			case LuaData_Container:
				toCylinder = tfs::lua::getUserdata<Container>(L, 2);
				break;
			case LuaData_Player:
				toCylinder = tfs::lua::getUserdata<Player>(L, 2);
				break;
			case LuaData_Tile:
				toCylinder = tfs::lua::getUserdata<Tile>(L, 2);
				break;
			default:
				toCylinder = nullptr;
				break;
		}
	} else {
		toCylinder = g_game.map.getTile(tfs::lua::getPosition(L, 2));
	}

	if (!toCylinder) {
		context.push_nil();
		return 1;
	}

	if (item->getParent() == toCylinder) {
		context.push_boolean(true);
		return 1;
	}

	uint32_t flags = tfs::lua::getNumber<uint32_t>(
	    L, 3, FLAG_NOLIMIT | FLAG_IGNOREBLOCKITEM | FLAG_IGNOREBLOCKCREATURE | FLAG_IGNORENOTMOVEABLE);

	if (item->getParent() == VirtualCylinder::virtualCylinder) {
		tfs::lua::pushBoolean(L,
		                      g_game.internalAddItem(toCylinder, item, INDEX_WHEREEVER, flags) == RETURNVALUE_NOERROR);
	} else {
		Item* moveItem = nullptr;
		ReturnValue ret = g_game.internalMoveItem(item->getParent(), toCylinder, INDEX_WHEREEVER, item,
		                                          item->getItemCount(), &moveItem, flags);
		if (moveItem) {
			*itemPtr = moveItem;
		}
		context.push_boolean(ret == RETURNVALUE_NOERROR);
	}
	return 1;
}

int LuaScriptInterface::luaItemTransform()
{
	// item:transform(itemId[, count/subType = -1])
	Item** itemPtr = tfs::lua::getRawUserdata<Item>(L, 1);
	if (!itemPtr) {
		context.push_nil();
		return 1;
	}

	Item*& item = *itemPtr;
	if (!item) {
		context.push_nil();
		return 1;
	}

	uint16_t itemId;
	if (isNumber(L, 2)) {
		itemId = context.get_number<uint16_t>(2);
	} else {
		itemId = Item::items.getItemIdByName(tfs::lua::getString(L, 2));
		if (itemId == 0) {
			context.push_nil();
			return 1;
		}
	}

	int32_t subType = tfs::lua::getNumber<int32_t>(L, 3, -1);
	if (item->getID() == itemId && (subType == -1 || subType == item->getSubType())) {
		context.push_boolean(true);
		return 1;
	}

	const ItemType& it = Item::items[itemId];
	if (it.stackable) {
		subType = std::min<int32_t>(subType, ITEM_STACK_SIZE);
	}

	ScriptEnvironment* env = tfs::lua::getScriptEnv();
	uint32_t uid = env->addThing(item);

	Item* newItem = g_game.transformItem(item, itemId, subType);
	if (item->isRemoved()) {
		env->removeItemByUID(uid);
	}

	if (newItem && newItem != item) {
		env->insertItem(uid, newItem);
	}

	item = newItem;
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaItemDecay()
{
	// item:decay(decayId)
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		if (isNumber(L, 2)) {
			item->setDecayTo(tfs::lua::getNumber<int32_t>(L, 2));
		}

		g_game.startDecay(item);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemGetSpecialDescription()
{
	// item:getSpecialDescription()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		tfs::lua::pushString(L, item->getSpecialDescription());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemHasProperty()
{
	// item:hasProperty(property)
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		ITEMPROPERTY property = tfs::lua::getNumber<ITEMPROPERTY>(L, 2);
		context.push_boolean(item->hasProperty(property));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemIsLoadedFromMap()
{
	// item:isLoadedFromMap()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		context.push_boolean(item->isLoadedFromMap());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemSetStoreItem()
{
	// item:setStoreItem(storeItem)
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (!item) {
		context.push_nil();
		return 1;
	}

	item->setStoreItem(tfs::lua::getBoolean(L, 2, false));
	return 1;
}

int LuaScriptInterface::luaItemIsStoreItem()
{
	// item:isStoreItem()
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		context.push_boolean(item->isStoreItem());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemSetReflect()
{
	// item:setReflect(combatType, reflect)
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (!item) {
		context.push_nil();
		return 1;
	}

	Reflect reflect{
	    tfs::lua::getField<uint16_t>(L, 3, "percent"),
	    tfs::lua::getField<uint16_t>(L, 3, "chance"),
	};
	lua_pop(L, 2);

	item->setReflect(tfs::lua::getNumber<CombatType_t>(L, 2), reflect);
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaItemGetReflect()
{
	// item:getReflect(combatType[, total = true])
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		const Reflect& reflect =
		    item->getReflect(tfs::lua::getNumber<CombatType_t>(L, 2), tfs::lua::getBoolean(L, 3, true));

		lua_createtable(L, 0, 2);
		setField(L, "percent", reflect.percent);
		setField(L, "chance", reflect.chance);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemSetBoostPercent()
{
	// item:setBoostPercent(combatType, percent)
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (!item) {
		context.push_nil();
		return 1;
	}

	item->setBoostPercent(tfs::lua::getNumber<CombatType_t>(L, 2), context.get_number<uint16_t>(3));
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaItemGetBoostPercent()
{
	// item:getBoostPercent(combatType[, total = true])
	Item* item = tfs::lua::getUserdata<Item>(L, 1);
	if (item) {
		lua_pushnumber(
		    L, item->getBoostPercent(tfs::lua::getNumber<CombatType_t>(L, 2), tfs::lua::getBoolean(L, 3, true)));
	} else {
		context.push_nil();
	}
	return 1;
}

// Container
int LuaScriptInterface::luaContainerCreate()
{
	// Container(uid)
	uint32_t id = context.get_number<uint32_t>(2);

	Container* container = tfs::lua::getScriptEnv()->getContainerByUID(id);
	if (container) {
		tfs::lua::pushUserdata(L, container);
		tfs::lua::setMetatable(L, -1, "Container");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaContainerGetSize()
{
	// container:getSize()
	Container* container = tfs::lua::getUserdata<Container>(L, 1);
	if (container) {
		context.push_number(container->size());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaContainerGetCapacity()
{
	// container:getCapacity()
	Container* container = tfs::lua::getUserdata<Container>(L, 1);
	if (container) {
		context.push_number(container->capacity());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaContainerGetEmptySlots()
{
	// container:getEmptySlots([recursive = false])
	Container* container = tfs::lua::getUserdata<Container>(L, 1);
	if (!container) {
		context.push_nil();
		return 1;
	}

	uint32_t slots = container->capacity() - container->size();
	bool recursive = tfs::lua::getBoolean(L, 2, false);
	if (recursive) {
		for (ContainerIterator it = container->iterator(); it.hasNext(); it.advance()) {
			if (Container* tmpContainer = (*it)->getContainer()) {
				slots += tmpContainer->capacity() - tmpContainer->size();
			}
		}
	}
	context.push_number(slots);
	return 1;
}

int LuaScriptInterface::luaContainerGetItemHoldingCount()
{
	// container:getItemHoldingCount()
	Container* container = tfs::lua::getUserdata<Container>(L, 1);
	if (container) {
		context.push_number(container->getItemHoldingCount());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaContainerGetItem()
{
	// container:getItem(index)
	Container* container = tfs::lua::getUserdata<Container>(L, 1);
	if (!container) {
		context.push_nil();
		return 1;
	}

	uint32_t index = context.get_number<uint32_t>(2);
	Item* item = container->getItemByIndex(index);
	if (item) {
		tfs::lua::pushUserdata(L, item);
		tfs::lua::setItemMetatable(L, -1, item);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaContainerHasItem()
{
	// container:hasItem(item)
	Item* item = tfs::lua::getUserdata<Item>(L, 2);
	Container* container = tfs::lua::getUserdata<Container>(L, 1);
	if (container) {
		context.push_boolean(container->isHoldingItem(item));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaContainerAddItem()
{
	// container:addItem(itemId[, count/subType = 1[, index = INDEX_WHEREEVER[, flags = 0]]])
	Container* container = tfs::lua::getUserdata<Container>(L, 1);
	if (!container) {
		context.push_nil();
		return 1;
	}

	uint16_t itemId;
	if (isNumber(L, 2)) {
		itemId = context.get_number<uint16_t>(2);
	} else {
		itemId = Item::items.getItemIdByName(tfs::lua::getString(L, 2));
		if (itemId == 0) {
			context.push_nil();
			return 1;
		}
	}

	const ItemType& it = Item::items[itemId];

	int32_t itemCount = 1;
	int32_t subType = 1;
	uint32_t count = context.get_number<uint32_t>(3, 1);

	if (it.hasSubType()) {
		if (it.stackable) {
			itemCount = std::ceil(count / static_cast<float>(ITEM_STACK_SIZE));
		}

		subType = count;
	} else {
		itemCount = std::max<int32_t>(1, count);
	}

	bool hasTable = itemCount > 1;
	if (hasTable) {
		lua_newtable(L);
	} else if (itemCount == 0) {
		context.push_nil();
		return 1;
	}

	int32_t index = tfs::lua::getNumber<int32_t>(L, 4, INDEX_WHEREEVER);
	uint32_t flags = context.get_number<uint32_t>(5, 0);

	for (int32_t i = 1; i <= itemCount; ++i) {
		int32_t stackCount = std::min<int32_t>(subType, ITEM_STACK_SIZE);
		Item* item = Item::CreateItem(itemId, stackCount);
		if (!item) {
			reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_ITEM_NOT_FOUND));
			if (!hasTable) {
				context.push_nil();
			}
			return 1;
		}

		if (it.stackable) {
			subType -= stackCount;
		}

		ReturnValue ret = g_game.internalAddItem(container, item, index, flags);
		if (ret != RETURNVALUE_NOERROR) {
			delete item;
			if (!hasTable) {
				context.push_nil();
			}
			return 1;
		}

		if (hasTable) {
			context.push_number(i);
			tfs::lua::pushUserdata(L, item);
			tfs::lua::setItemMetatable(L, -1, item);
			lua_settable(L, -3);
		} else {
			tfs::lua::pushUserdata(L, item);
			tfs::lua::setItemMetatable(L, -1, item);
		}
	}
	return 1;
}

int LuaScriptInterface::luaContainerAddItemEx()
{
	// container:addItemEx(item[, index = INDEX_WHEREEVER[, flags = 0]])
	Item* item = tfs::lua::getUserdata<Item>(L, 2);
	if (!item) {
		context.push_nil();
		return 1;
	}

	Container* container = tfs::lua::getUserdata<Container>(L, 1);
	if (!container) {
		context.push_nil();
		return 1;
	}

	if (item->getParent() != VirtualCylinder::virtualCylinder) {
		reportErrorFunc(L, "Item already has a parent");
		context.push_nil();
		return 1;
	}

	int32_t index = tfs::lua::getNumber<int32_t>(L, 3, INDEX_WHEREEVER);
	uint32_t flags = context.get_number<uint32_t>(4, 0);
	ReturnValue ret = g_game.internalAddItem(container, item, index, flags);
	if (ret == RETURNVALUE_NOERROR) {
		tfs::lua::removeTempItem(item);
	}
	context.push_number(ret);
	return 1;
}

int LuaScriptInterface::luaContainerGetCorpseOwner()
{
	// container:getCorpseOwner()
	Container* container = tfs::lua::getUserdata<Container>(L, 1);
	if (container) {
		context.push_number(container->getCorpseOwner());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaContainerGetItemCountById()
{
	// container:getItemCountById(itemId[, subType = -1])
	Container* container = tfs::lua::getUserdata<Container>(L, 1);
	if (!container) {
		context.push_nil();
		return 1;
	}

	uint16_t itemId;
	if (isNumber(L, 2)) {
		itemId = context.get_number<uint16_t>(2);
	} else {
		itemId = Item::items.getItemIdByName(tfs::lua::getString(L, 2));
		if (itemId == 0) {
			context.push_nil();
			return 1;
		}
	}

	int32_t subType = tfs::lua::getNumber<int32_t>(L, 3, -1);
	context.push_number(container->getItemTypeCount(itemId, subType));
	return 1;
}

int LuaScriptInterface::luaContainerGetItems()
{
	// container:getItems([recursive = false])
	Container* container = tfs::lua::getUserdata<Container>(L, 1);
	if (!container) {
		context.push_nil();
		return 1;
	}

	bool recursive = tfs::lua::getBoolean(L, 2, false);
	std::vector<Item*> items = container->getItems(recursive);

	lua_createtable(L, items.size(), 0);

	int index = 0;
	for (Item* item : items) {
		tfs::lua::pushUserdata(L, item);
		tfs::lua::setItemMetatable(L, -1, item);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

// Teleport
int LuaScriptInterface::luaTeleportCreate()
{
	// Teleport(uid)
	uint32_t id = context.get_number<uint32_t>(2);

	Item* item = tfs::lua::getScriptEnv()->getItemByUID(id);
	if (item && item->getTeleport()) {
		tfs::lua::pushUserdata(L, item);
		tfs::lua::setMetatable(L, -1, "Teleport");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaTeleportGetDestination()
{
	// teleport:getDestination()
	Teleport* teleport = tfs::lua::getUserdata<Teleport>(L, 1);
	if (teleport) {
		tfs::lua::pushPosition(L, teleport->getDestPos());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaTeleportSetDestination()
{
	// teleport:setDestination(position)
	Teleport* teleport = tfs::lua::getUserdata<Teleport>(L, 1);
	if (teleport) {
		teleport->setDestPos(tfs::lua::getPosition(L, 2));
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

// Podium
int LuaScriptInterface::luaPodiumCreate()
{
	// Podium(uid)
	uint32_t id = context.get_number<uint32_t>(2);

	Item* item = tfs::lua::getScriptEnv()->getItemByUID(id);
	if (item && item->getPodium()) {
		tfs::lua::pushUserdata(L, item);
		tfs::lua::setMetatable(L, -1, "Podium");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPodiumGetOutfit()
{
	// podium:getOutfit()
	const Podium* podium = tfs::lua::getUserdata<const Podium>(L, 1);
	if (podium) {
		tfs::lua::pushOutfit(L, podium->getOutfit());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPodiumSetOutfit()
{
	// podium:setOutfit(outfit)
	Podium* podium = tfs::lua::getUserdata<Podium>(L, 1);
	if (podium) {
		podium->setOutfit(getOutfit(L, 2));
		g_game.updatePodium(podium);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPodiumHasFlag()
{
	// podium:hasFlag(flag)
	Podium* podium = tfs::lua::getUserdata<Podium>(L, 1);
	if (podium) {
		PodiumFlags flag = tfs::lua::getNumber<PodiumFlags>(L, 2);
		context.push_boolean(podium->hasFlag(flag));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPodiumSetFlag()
{
	// podium:setFlag(flag, value)
	uint8_t value = tfs::lua::getBoolean(L, 3);
	PodiumFlags flag = tfs::lua::getNumber<PodiumFlags>(L, 2);
	Podium* podium = tfs::lua::getUserdata<Podium>(L, 1);

	if (podium) {
		podium->setFlagValue(flag, value);
		g_game.updatePodium(podium);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPodiumGetDirection()
{
	// podium:getDirection()
	const Podium* podium = tfs::lua::getUserdata<const Podium>(L, 1);
	if (podium) {
		context.push_number(podium->getDirection());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPodiumSetDirection()
{
	// podium:setDirection(direction)
	Podium* podium = tfs::lua::getUserdata<Podium>(L, 1);
	if (podium) {
		podium->setDirection(tfs::lua::getNumber<Direction>(L, 2));
		g_game.updatePodium(podium);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

// Creature
int LuaScriptInterface::luaCreatureCreate()
{
	// Creature(id or name or userdata)
	Creature* creature;
	if (isNumber(L, 2)) {
		creature = g_game.getCreatureByID(context.get_number<uint32_t>(2));
	} else if (lua_isstring(L, 2)) {
		creature = g_game.getCreatureByName(tfs::lua::getString(L, 2));
	} else if (lua_isuserdata(L, 2)) {
		LuaDataType type = getUserdataType(L, 2);
		if (type != LuaData_Player && type != LuaData_Monster && type != LuaData_Npc) {
			context.push_nil();
			return 1;
		}
		creature = tfs::lua::getUserdata<Creature>(L, 2);
	} else {
		creature = nullptr;
	}

	if (creature) {
		tfs::lua::pushUserdata(L, creature);
		tfs::lua::setCreatureMetatable(L, -1, creature);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetEvents()
{
	// creature:getEvents(type)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		context.push_nil();
		return 1;
	}

	CreatureEventType_t eventType = tfs::lua::getNumber<CreatureEventType_t>(L, 2);
	const auto& eventList = creature->getCreatureEvents(eventType);
	lua_createtable(L, eventList.size(), 0);

	int index = 0;
	for (CreatureEvent* event : eventList) {
		tfs::lua::pushString(L, event->getName());
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureRegisterEvent()
{
	// creature:registerEvent(name)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (creature) {
		const std::string& name = tfs::lua::getString(L, 2);
		context.push_boolean(creature->registerCreatureEvent(name));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureUnregisterEvent()
{
	// creature:unregisterEvent(name)
	const std::string& name = tfs::lua::getString(L, 2);
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (creature) {
		context.push_boolean(creature->unregisterCreatureEvent(name));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureIsRemoved()
{
	// creature:isRemoved()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		context.push_boolean(creature->isRemoved());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureIsCreature()
{
	// creature:isCreature()
	context.push_boolean(tfs::lua::getUserdata<const Creature>(L, 1) != nullptr);
	return 1;
}

int LuaScriptInterface::luaCreatureIsInGhostMode()
{
	// creature:isInGhostMode()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		context.push_boolean(creature->isInGhostMode());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureIsHealthHidden()
{
	// creature:isHealthHidden()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		context.push_boolean(creature->isHealthHidden());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureIsMovementBlocked()
{
	// creature:isMovementBlocked()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		context.push_boolean(creature->isMovementBlocked());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureCanSee()
{
	// creature:canSee(position)
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		const Position& position = tfs::lua::getPosition(L, 2);
		context.push_boolean(creature->canSee(position));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureCanSeeCreature()
{
	// creature:canSeeCreature(creature)
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		const Creature* otherCreature = tfs::lua::getCreature(L, 2);
		if (!otherCreature) {
			reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
			context.push_boolean(false);
			return 1;
		}

		context.push_boolean(creature->canSeeCreature(otherCreature));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureCanSeeGhostMode()
{
	// creature:canSeeGhostMode(creature)
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		const Creature* otherCreature = tfs::lua::getCreature(L, 2);
		if (!otherCreature) {
			reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
			context.push_boolean(false);
			return 1;
		}

		context.push_boolean(creature->canSeeGhostMode(otherCreature));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureCanSeeInvisibility()
{
	// creature:canSeeInvisibility()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		context.push_boolean(creature->canSeeInvisibility());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetParent()
{
	// creature:getParent()
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		context.push_nil();
		return 1;
	}

	Cylinder* parent = creature->getParent();
	if (!parent) {
		context.push_nil();
		return 1;
	}

	tfs::lua::pushCylinder(L, parent);
	return 1;
}

int LuaScriptInterface::luaCreatureGetId()
{
	// creature:getId()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		context.push_number(creature->getID());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetName()
{
	// creature:getName()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		tfs::lua::pushString(L, creature->getName());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetTarget()
{
	// creature:getTarget()
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		context.push_nil();
		return 1;
	}

	Creature* target = creature->getAttackedCreature();
	if (target) {
		tfs::lua::pushUserdata(L, target);
		tfs::lua::setCreatureMetatable(L, -1, target);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureSetTarget()
{
	// creature:setTarget(target)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (creature) {
		context.push_boolean(creature->setAttackedCreature(tfs::lua::getCreature(L, 2)));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetFollowCreature()
{
	// creature:getFollowCreature()
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		context.push_nil();
		return 1;
	}

	Creature* followCreature = creature->getFollowCreature();
	if (followCreature) {
		tfs::lua::pushUserdata(L, followCreature);
		tfs::lua::setCreatureMetatable(L, -1, followCreature);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureSetFollowCreature()
{
	// creature:setFollowCreature(followedCreature)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (creature) {
		context.push_boolean(creature->setFollowCreature(tfs::lua::getCreature(L, 2)));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetMaster()
{
	// creature:getMaster()
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		context.push_nil();
		return 1;
	}

	Creature* master = creature->getMaster();
	if (!master) {
		context.push_nil();
		return 1;
	}

	tfs::lua::pushUserdata(L, master);
	tfs::lua::setCreatureMetatable(L, -1, master);
	return 1;
}

int LuaScriptInterface::luaCreatureSetMaster()
{
	// creature:setMaster(master)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		context.push_nil();
		return 1;
	}

	context.push_boolean(creature->setMaster(tfs::lua::getCreature(L, 2)));

	// update summon icon
	g_game.updateKnownCreature(creature);
	return 1;
}

int LuaScriptInterface::luaCreatureGetLight()
{
	// creature:getLight()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (!creature) {
		context.push_nil();
		return 1;
	}

	LightInfo lightInfo = creature->getCreatureLight();
	context.push_number(lightInfo.level);
	context.push_number(lightInfo.color);
	return 2;
}

int LuaScriptInterface::luaCreatureSetLight()
{
	// creature:setLight(color, level)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		context.push_nil();
		return 1;
	}

	LightInfo light;
	light.color = context.get_number<uint8_t>(2);
	light.level = context.get_number<uint8_t>(3);
	creature->setCreatureLight(light);
	g_game.changeLight(creature);
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaCreatureGetSpeed()
{
	// creature:getSpeed()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		context.push_number(creature->getSpeed());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetBaseSpeed()
{
	// creature:getBaseSpeed()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		context.push_number(creature->getBaseSpeed());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureChangeSpeed()
{
	// creature:changeSpeed(delta)
	Creature* creature = tfs::lua::getCreature(L, 1);
	if (!creature) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
		context.push_boolean(false);
		return 1;
	}

	int32_t delta = tfs::lua::getNumber<int32_t>(L, 2);
	g_game.changeSpeed(creature, delta);
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaCreatureSetDropLoot()
{
	// creature:setDropLoot(doDrop)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (creature) {
		creature->setDropLoot(tfs::lua::getBoolean(L, 2));
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureSetSkillLoss()
{
	// creature:setSkillLoss(skillLoss)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (creature) {
		creature->setSkillLoss(tfs::lua::getBoolean(L, 2));
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetPosition()
{
	// creature:getPosition()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		tfs::lua::pushPosition(L, creature->getPosition());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetTile()
{
	// creature:getTile()
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		context.push_nil();
		return 1;
	}

	Tile* tile = creature->getTile();
	if (tile) {
		tfs::lua::pushUserdata(L, tile);
		tfs::lua::setMetatable(L, -1, "Tile");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetDirection()
{
	// creature:getDirection()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		context.push_number(creature->getDirection());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureSetDirection()
{
	// creature:setDirection(direction)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (creature) {
		context.push_boolean(g_game.internalCreatureTurn(creature, tfs::lua::getNumber<Direction>(L, 2)));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetHealth()
{
	// creature:getHealth()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		context.push_number(creature->getHealth());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureSetHealth()
{
	// creature:setHealth(health)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		context.push_nil();
		return 1;
	}

	creature->health = std::min<int32_t>(context.get_number<uint32_t>(2), creature->healthMax);
	g_game.addCreatureHealth(creature);

	Player* player = creature->getPlayer();
	if (player) {
		player->sendStats();
	}
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaCreatureAddHealth()
{
	// creature:addHealth(healthChange)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		context.push_nil();
		return 1;
	}

	CombatDamage damage;
	damage.primary.value = tfs::lua::getNumber<int32_t>(L, 2);
	if (damage.primary.value >= 0) {
		damage.primary.type = COMBAT_HEALING;
	} else {
		damage.primary.type = COMBAT_UNDEFINEDDAMAGE;
	}
	context.push_boolean(g_game.combatChangeHealth(nullptr, creature, damage));
	return 1;
}

int LuaScriptInterface::luaCreatureGetMaxHealth()
{
	// creature:getMaxHealth()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		context.push_number(creature->getMaxHealth());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureSetMaxHealth()
{
	// creature:setMaxHealth(maxHealth)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		context.push_nil();
		return 1;
	}

	creature->healthMax = context.get_number<uint32_t>(2);
	creature->health = std::min<int32_t>(creature->health, creature->healthMax);
	g_game.addCreatureHealth(creature);

	Player* player = creature->getPlayer();
	if (player) {
		player->sendStats();
	}
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaCreatureSetHiddenHealth()
{
	// creature:setHiddenHealth(hide)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (creature) {
		creature->setHiddenHealth(tfs::lua::getBoolean(L, 2));
		g_game.addCreatureHealth(creature);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureSetMovementBlocked()
{
	// creature:setMovementBlocked(state)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (creature) {
		creature->setMovementBlocked(tfs::lua::getBoolean(L, 2));
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetSkull()
{
	// creature:getSkull()
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (creature) {
		context.push_number(creature->getSkull());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureSetSkull()
{
	// creature:setSkull(skull)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (creature) {
		creature->setSkull(tfs::lua::getNumber<Skulls_t>(L, 2));
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetOutfit()
{
	// creature:getOutfit()
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		tfs::lua::pushOutfit(L, creature->getCurrentOutfit());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureSetOutfit()
{
	// creature:setOutfit(outfit)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (creature) {
		creature->defaultOutfit = getOutfit(L, 2);
		g_game.internalCreatureChangeOutfit(creature, creature->defaultOutfit);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetCondition()
{
	// creature:getCondition(conditionType[, conditionId = CONDITIONID_COMBAT[, subId = 0]])
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		context.push_nil();
		return 1;
	}

	ConditionType_t conditionType = tfs::lua::getNumber<ConditionType_t>(L, 2);
	ConditionId_t conditionId = tfs::lua::getNumber<ConditionId_t>(L, 3, CONDITIONID_COMBAT);
	uint32_t subId = context.get_number<uint32_t>(4, 0);

	Condition* condition = creature->getCondition(conditionType, conditionId, subId);
	if (condition) {
		tfs::lua::pushUserdata(L, condition);
		setWeakMetatable(L, -1, "Condition");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureAddCondition()
{
	// creature:addCondition(condition[, force = false])
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	Condition* condition = tfs::lua::getUserdata<Condition>(L, 2);
	if (creature && condition) {
		bool force = tfs::lua::getBoolean(L, 3, false);
		context.push_boolean(creature->addCondition(condition->clone(), force));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureRemoveCondition()
{
	// creature:removeCondition(conditionType[, conditionId = CONDITIONID_COMBAT[, subId = 0[, force = false]]])
	// creature:removeCondition(condition[, force = false])
	Creature* const creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		context.push_nil();
		return 1;
	}

	Condition* creatureCondition = nullptr;
	bool force = false;

	if (lua_isuserdata(L, 2)) {
		const Condition* const condition = tfs::lua::getUserdata<Condition>(L, 2);
		const ConditionType_t conditionType = condition->getType();
		const ConditionId_t conditionId = condition->getId();
		const uint32_t subId = condition->getSubId();
		creatureCondition = creature->getCondition(conditionType, conditionId, subId);
		force = tfs::lua::getBoolean(L, 3, false);
	} else {
		const ConditionType_t conditionType = tfs::lua::getNumber<ConditionType_t>(L, 2);
		const ConditionId_t conditionId = tfs::lua::getNumber<ConditionId_t>(L, 3, CONDITIONID_COMBAT);
		const uint32_t subId = context.get_number<uint32_t>(4, 0);
		creatureCondition = creature->getCondition(conditionType, conditionId, subId);
		force = tfs::lua::getBoolean(L, 5, false);
	}

	if (creatureCondition) {
		creature->removeCondition(creatureCondition, force);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureHasCondition()
{
	// creature:hasCondition(conditionType[, subId = 0])
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		context.push_nil();
		return 1;
	}

	ConditionType_t conditionType = tfs::lua::getNumber<ConditionType_t>(L, 2);
	uint32_t subId = context.get_number<uint32_t>(3, 0);
	context.push_boolean(creature->hasCondition(conditionType, subId));
	return 1;
}

int LuaScriptInterface::luaCreatureIsImmune()
{
	// creature:isImmune(condition or conditionType)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		context.push_nil();
		return 1;
	}

	if (isNumber(L, 2)) {
		context.push_boolean(creature->isImmune(tfs::lua::getNumber<ConditionType_t>(L, 2)));
	} else if (Condition* condition = tfs::lua::getUserdata<Condition>(L, 2)) {
		context.push_boolean(creature->isImmune(condition->getType()));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureRemove()
{
	// creature:remove()
	Creature** creaturePtr = tfs::lua::getRawUserdata<Creature>(L, 1);
	if (!creaturePtr) {
		context.push_nil();
		return 1;
	}

	Creature* creature = *creaturePtr;
	if (!creature) {
		context.push_nil();
		return 1;
	}

	Player* player = creature->getPlayer();
	if (player) {
		player->kickPlayer(true);
	} else {
		g_game.removeCreature(creature);
	}

	*creaturePtr = nullptr;
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaCreatureTeleportTo()
{
	// creature:teleportTo(position[, pushMovement = false])
	bool pushMovement = tfs::lua::getBoolean(L, 3, false);

	const Position& position = tfs::lua::getPosition(L, 2);
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		context.push_nil();
		return 1;
	}

	const Position oldPosition = creature->getPosition();
	if (g_game.internalTeleport(creature, position, pushMovement) != RETURNVALUE_NOERROR) {
		context.push_boolean(false);
		return 1;
	}

	if (pushMovement) {
		if (oldPosition.x == position.x) {
			if (oldPosition.y < position.y) {
				g_game.internalCreatureTurn(creature, DIRECTION_SOUTH);
			} else {
				g_game.internalCreatureTurn(creature, DIRECTION_NORTH);
			}
		} else if (oldPosition.x > position.x) {
			g_game.internalCreatureTurn(creature, DIRECTION_WEST);
		} else if (oldPosition.x < position.x) {
			g_game.internalCreatureTurn(creature, DIRECTION_EAST);
		}
	}
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaCreatureSay()
{
	// creature:say(text[, type = TALKTYPE_MONSTER_SAY[, ghost = false[, target = nullptr[, position]]]])
	int parameters = lua_gettop(L);

	Position position;
	if (parameters >= 6) {
		position = tfs::lua::getPosition(L, 6);
		if (!position.x || !position.y) {
			reportErrorFunc(L, "Invalid position specified.");
			context.push_boolean(false);
			return 1;
		}
	}

	Creature* target = nullptr;
	if (parameters >= 5) {
		target = tfs::lua::getCreature(L, 5);
	}

	bool ghost = tfs::lua::getBoolean(L, 4, false);

	SpeakClasses type = tfs::lua::getNumber<SpeakClasses>(L, 3, TALKTYPE_MONSTER_SAY);
	const std::string& text = tfs::lua::getString(L, 2);
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		context.push_nil();
		return 1;
	}

	Spectators spectators;
	if (target) {
		spectators.insert(target);
	}

	// Prevent infinity echo on event onHear
	bool echo = tfs::lua::getScriptEnv()->getScriptId() == g_events->getScriptId(EventInfoId::CREATURE_ONHEAR);

	if (position.x != 0) {
		tfs::lua::pushBoolean(
		    L, g_game.internalCreatureSay(creature, type, text, ghost, std::move(spectators), &position, echo));
	} else {
		tfs::lua::pushBoolean(
		    L, g_game.internalCreatureSay(creature, type, text, ghost, std::move(spectators), nullptr, echo));
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetDamageMap()
{
	// creature:getDamageMap()
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		context.push_nil();
		return 1;
	}

	lua_createtable(L, creature->damageMap.size(), 0);
	for (const auto& damageEntry : creature->damageMap) {
		lua_createtable(L, 0, 2);
		setField(L, "total", damageEntry.second.total);
		setField(L, "ticks", damageEntry.second.ticks);
		lua_rawseti(L, -2, damageEntry.first);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetSummons()
{
	// creature:getSummons()
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		context.push_nil();
		return 1;
	}

	lua_createtable(L, creature->getSummonCount(), 0);

	int index = 0;
	for (Creature* summon : creature->getSummons()) {
		tfs::lua::pushUserdata(L, summon);
		tfs::lua::setCreatureMetatable(L, -1, summon);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetDescription()
{
	// creature:getDescription(distance)
	int32_t distance = tfs::lua::getNumber<int32_t>(L, 2);
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (creature) {
		tfs::lua::pushString(L, creature->getDescription(distance));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetPathTo()
{
	// creature:getPathTo(pos[, minTargetDist = 0[, maxTargetDist = 1[, fullPathSearch = true[, clearSight = true[,
	// maxSearchDist = 0]]]]])
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		context.push_nil();
		return 1;
	}

	const Position& position = tfs::lua::getPosition(L, 2);

	FindPathParams fpp;
	fpp.minTargetDist = tfs::lua::getNumber<int32_t>(L, 3, 0);
	fpp.maxTargetDist = tfs::lua::getNumber<int32_t>(L, 4, 1);
	fpp.fullPathSearch = tfs::lua::getBoolean(L, 5, fpp.fullPathSearch);
	fpp.clearSight = tfs::lua::getBoolean(L, 6, fpp.clearSight);
	fpp.maxSearchDist = tfs::lua::getNumber<int32_t>(L, 7, fpp.maxSearchDist);

	std::vector<Direction> dirList;
	if (creature->getPathTo(position, dirList, fpp)) {
		lua_newtable(L);

		int index = 0;
		for (auto it = dirList.rbegin(); it != dirList.rend(); ++it) {
			context.push_number(*it);
			lua_rawseti(L, -2, ++index);
		}
	} else {
		context.push_boolean(false);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureMove()
{
	// creature:move(direction)
	// creature:move(tile[, flags = 0])
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		context.push_nil();
		return 1;
	}

	if (isNumber(L, 2)) {
		Direction direction = tfs::lua::getNumber<Direction>(L, 2);
		if (direction > DIRECTION_LAST) {
			context.push_nil();
			return 1;
		}
		context.push_number(g_game.internalMoveCreature(creature, direction, FLAG_NOLIMIT));
	} else {
		Tile* tile = tfs::lua::getUserdata<Tile>(L, 2);
		if (!tile) {
			context.push_nil();
			return 1;
		}
		context.push_number(g_game.internalMoveCreature(*creature, *tile, context.get_number<uint32_t>(3)));
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetZone()
{
	// creature:getZone()
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (creature) {
		context.push_number(creature->getZone());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureHasIcon()
{
	// creature:hasIcon(iconId)
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (creature) {
		auto iconId = tfs::lua::getNumber<CreatureIcon_t>(L, 2);
		context.push_boolean(creature->getIcons().contains(iconId));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureSetIcon()
{
	// creature:setIcon(iconId, value)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		context.push_nil();
		return 1;
	}

	auto iconId = tfs::lua::getNumber<CreatureIcon_t>(L, 2);
	if (iconId > CREATURE_ICON_LAST) {
		reportErrorFunc(L, "Invalid Creature Icon Id");
		context.push_boolean(false);
		return 1;
	}

	creature->getIcons()[iconId] = context.get_number<uint16_t>(3);
	creature->updateIcons();
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaCreatureGetIcon()
{
	// creature:getIcon(iconId)
	const Creature* creature = tfs::lua::getUserdata<const Creature>(L, 1);
	if (!creature) {
		context.push_nil();
		return 1;
	}

	auto iconId = tfs::lua::getNumber<CreatureIcon_t>(L, 2);
	const auto& icons = creature->getIcons();
	auto it = icons.find(iconId);
	if (it != icons.end()) {
		lua_pushinteger(L, it->second);
	} else {
		lua_pushinteger(L, 0);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureRemoveIcon()
{
	// creature:removeIcon(iconId)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		context.push_nil();
		return 1;
	}

	auto iconId = tfs::lua::getNumber<CreatureIcon_t>(L, 2);
	auto& icons = creature->getIcons();
	auto it = icons.find(iconId);
	if (it != icons.end()) {
		icons.erase(it);
		creature->updateIcons();
		context.push_boolean(true);
	} else {
		context.push_boolean(false);
	}

	return 1;
}

int LuaScriptInterface::luaCreatureGetStorageValue()
{
	// creature:getStorageValue(key)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		context.push_nil();
		return 1;
	}

	uint32_t key = context.get_number<uint32_t>(2);
	if (auto storage = creature->getStorageValue(key)) {
		context.push_number(storage.value());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureSetStorageValue()
{
	// creature:setStorageValue(key, value)
	Creature* creature = tfs::lua::getUserdata<Creature>(L, 1);
	if (!creature) {
		context.push_nil();
		return 1;
	}

	uint32_t key = context.get_number<uint32_t>(2);
	if (IS_IN_KEYRANGE(key, RESERVED_RANGE)) {
		reportErrorFunc(L, fmt::format("Accessing reserved range: {:d}", key));
		context.push_boolean(false);
		return 1;
	}

	if (lua_isnoneornil(L, 3)) {
		creature->setStorageValue(key, std::nullopt);
	} else {
		int32_t value = tfs::lua::getNumber<int32_t>(L, 3);
		creature->setStorageValue(key, value);
	}

	context.push_boolean(true);
	return 1;
}

// Player
int LuaScriptInterface::luaPlayerCreate()
{
	// Player(id or guid or name or userdata)
	Player* player;
	if (isNumber(L, 2)) {
		uint32_t id = context.get_number<uint32_t>(2);
		if (id >= CREATURE_ID_MIN && id <= Player::playerIDLimit) {
			player = g_game.getPlayerByID(id);
		} else {
			player = g_game.getPlayerByGUID(id);
		}
	} else if (lua_isstring(L, 2)) {
		ReturnValue ret = g_game.getPlayerByNameWildcard(tfs::lua::getString(L, 2), player);
		if (ret != RETURNVALUE_NOERROR) {
			context.push_nil();
			context.push_number(ret);
			return 2;
		}
	} else if (lua_isuserdata(L, 2)) {
		if (getUserdataType(L, 2) != LuaData_Player) {
			context.push_nil();
			return 1;
		}
		player = tfs::lua::getUserdata<Player>(L, 2);
	} else {
		player = nullptr;
	}

	if (player) {
		tfs::lua::pushUserdata(L, player);
		tfs::lua::setMetatable(L, -1, "Player");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerIsPlayer()
{
	// player:isPlayer()
	context.push_boolean(tfs::lua::getUserdata<const Player>(L, 1) != nullptr);
	return 1;
}

int LuaScriptInterface::luaPlayerGetGuid()
{
	// player:getGuid()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->getGUID());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetIp()
{
	// player:getIp()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		tfs::lua::pushString(L, player->getIP().to_string());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetAccountId()
{
	// player:getAccountId()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->getAccount());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetLastLoginSaved()
{
	// player:getLastLoginSaved()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->getLastLoginSaved());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetLastLogout()
{
	// player:getLastLogout()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->getLastLogout());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetAccountType()
{
	// player:getAccountType()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->getAccountType());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetAccountType()
{
	// player:setAccountType(accountType)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		player->accountType = tfs::lua::getNumber<AccountType_t>(L, 2);
		IOLoginData::setAccountType(player->getAccount(), player->accountType);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetCapacity()
{
	// player:getCapacity()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->getCapacity());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetCapacity()
{
	// player:setCapacity(capacity)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		player->capacity = context.get_number<uint32_t>(2);
		player->sendStats();
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetFreeCapacity()
{
	// player:getFreeCapacity()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->getFreeCapacity());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetDepotChest()
{
	// player:getDepotChest(depotId[, autoCreate = false])
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	uint32_t depotId = context.get_number<uint32_t>(2);
	bool autoCreate = tfs::lua::getBoolean(L, 3, false);
	DepotChest* depotChest = player->getDepotChest(depotId, autoCreate);
	if (depotChest) {
		tfs::lua::pushUserdata<Item>(L, depotChest);
		tfs::lua::setItemMetatable(L, -1, depotChest);
	} else {
		context.push_boolean(false);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetInbox()
{
	// player:getInbox()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	Inbox* inbox = player->getInbox();
	if (inbox) {
		tfs::lua::pushUserdata<Item>(L, inbox);
		tfs::lua::setItemMetatable(L, -1, inbox);
	} else {
		context.push_boolean(false);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetSkullTime()
{
	// player:getSkullTime()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->getSkullTicks());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetSkullTime()
{
	// player:setSkullTime(skullTime)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		player->setSkullTicks(tfs::lua::getNumber<int64_t>(L, 2));
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetDeathPenalty()
{
	// player:getDeathPenalty()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->getLostPercent() * 100);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetExperience()
{
	// player:getExperience()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->getExperience());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddExperience()
{
	// player:addExperience(experience[, sendText = false])
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		uint64_t experience = tfs::lua::getNumber<uint64_t>(L, 2);
		bool sendText = tfs::lua::getBoolean(L, 3, false);
		player->addExperience(nullptr, experience, sendText);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerRemoveExperience()
{
	// player:removeExperience(experience[, sendText = false])
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		uint64_t experience = tfs::lua::getNumber<uint64_t>(L, 2);
		bool sendText = tfs::lua::getBoolean(L, 3, false);
		player->removeExperience(experience, sendText);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetLevel()
{
	// player:getLevel()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->getLevel());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetLevelPercent()
{
	// player:getLevelPercent()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->getLevelPercent());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetMagicLevel()
{
	// player:getMagicLevel()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->getMagicLevel());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetMagicLevelPercent()
{
	// player:getMagicLevelPercent()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->getMagicLevelPercent());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetBaseMagicLevel()
{
	// player:getBaseMagicLevel()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->getBaseMagicLevel());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetMana()
{
	// player:getMana()
	const Player* player = tfs::lua::getUserdata<const Player>(L, 1);
	if (player) {
		context.push_number(player->getMana());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddMana()
{
	// player:addMana(manaChange[, animationOnLoss = false])
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	int32_t manaChange = tfs::lua::getNumber<int32_t>(L, 2);
	bool animationOnLoss = tfs::lua::getBoolean(L, 3, false);
	if (!animationOnLoss && manaChange < 0) {
		player->changeMana(manaChange);
	} else {
		CombatDamage damage;
		damage.primary.value = manaChange;
		damage.origin = ORIGIN_NONE;
		g_game.combatChangeMana(nullptr, player, damage);
	}
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaPlayerGetMaxMana()
{
	// player:getMaxMana()
	const Player* player = tfs::lua::getUserdata<const Player>(L, 1);
	if (player) {
		context.push_number(player->getMaxMana());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetMaxMana()
{
	// player:setMaxMana(maxMana)
	Player* player = tfs::lua::getPlayer(L, 1);
	if (player) {
		player->manaMax = tfs::lua::getNumber<int32_t>(L, 2);
		player->mana = std::min<int32_t>(player->mana, player->manaMax);
		player->sendStats();
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetManaShieldBar()
{
	// player:setManaShieldBar(capacity, value)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		player->setMaxManaShieldBar(context.get_number<uint16_t>(2));
		player->setManaShieldBar(context.get_number<uint16_t>(3));
		player->sendStats();
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetManaSpent()
{
	// player:getManaSpent()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->getSpentMana());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddManaSpent()
{
	// player:addManaSpent(amount)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		player->addManaSpent(tfs::lua::getNumber<uint64_t>(L, 2));
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerRemoveManaSpent()
{
	// player:removeManaSpent(amount[, notify = true])
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		player->removeManaSpent(tfs::lua::getNumber<uint64_t>(L, 2), tfs::lua::getBoolean(L, 3, true));
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetBaseMaxHealth()
{
	// player:getBaseMaxHealth()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->healthMax);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetBaseMaxMana()
{
	// player:getBaseMaxMana()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->manaMax);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetSkillLevel()
{
	// player:getSkillLevel(skillType)
	skills_t skillType = tfs::lua::getNumber<skills_t>(L, 2);
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player && skillType <= SKILL_LAST) {
		context.push_number(player->skills[skillType].level);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetEffectiveSkillLevel()
{
	// player:getEffectiveSkillLevel(skillType)
	skills_t skillType = tfs::lua::getNumber<skills_t>(L, 2);
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player && skillType <= SKILL_LAST) {
		context.push_number(player->getSkillLevel(skillType));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetSkillPercent()
{
	// player:getSkillPercent(skillType)
	skills_t skillType = tfs::lua::getNumber<skills_t>(L, 2);
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player && skillType <= SKILL_LAST) {
		context.push_number(player->skills[skillType].percent);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetSkillTries()
{
	// player:getSkillTries(skillType)
	skills_t skillType = tfs::lua::getNumber<skills_t>(L, 2);
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player && skillType <= SKILL_LAST) {
		context.push_number(player->skills[skillType].tries);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddSkillTries()
{
	// player:addSkillTries(skillType, tries)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		skills_t skillType = tfs::lua::getNumber<skills_t>(L, 2);
		uint64_t tries = tfs::lua::getNumber<uint64_t>(L, 3);
		player->addSkillAdvance(skillType, tries);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerRemoveSkillTries()
{
	// player:removeSkillTries(skillType, tries[, notify = true])
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		skills_t skillType = tfs::lua::getNumber<skills_t>(L, 2);
		uint64_t tries = tfs::lua::getNumber<uint64_t>(L, 3);
		player->removeSkillTries(skillType, tries, tfs::lua::getBoolean(L, 4, true));
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetSpecialSkill()
{
	// player:getSpecialSkill(specialSkillType)
	SpecialSkills_t specialSkillType = tfs::lua::getNumber<SpecialSkills_t>(L, 2);
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player && specialSkillType <= SPECIALSKILL_LAST) {
		context.push_number(player->getSpecialSkill(specialSkillType));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddSpecialSkill()
{
	// player:addSpecialSkill(specialSkillType, value)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	SpecialSkills_t specialSkillType = tfs::lua::getNumber<SpecialSkills_t>(L, 2);
	if (specialSkillType > SPECIALSKILL_LAST) {
		context.push_nil();
		return 1;
	}

	player->setVarSpecialSkill(specialSkillType, tfs::lua::getNumber<int32_t>(L, 3));
	player->sendSkills();
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaPlayerAddOfflineTrainingTime()
{
	// player:addOfflineTrainingTime(time)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		int32_t time = tfs::lua::getNumber<int32_t>(L, 2);
		player->addOfflineTrainingTime(time);
		player->sendStats();
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetOfflineTrainingTime()
{
	// player:getOfflineTrainingTime()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->getOfflineTrainingTime());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerRemoveOfflineTrainingTime()
{
	// player:removeOfflineTrainingTime(time)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		int32_t time = tfs::lua::getNumber<int32_t>(L, 2);
		player->removeOfflineTrainingTime(time);
		player->sendStats();
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddOfflineTrainingTries()
{
	// player:addOfflineTrainingTries(skillType, tries)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		skills_t skillType = tfs::lua::getNumber<skills_t>(L, 2);
		uint64_t tries = tfs::lua::getNumber<uint64_t>(L, 3);
		context.push_boolean(player->addOfflineTrainingTries(skillType, tries));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetOfflineTrainingSkill()
{
	// player:getOfflineTrainingSkill()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->getOfflineTrainingSkill());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetOfflineTrainingSkill()
{
	// player:setOfflineTrainingSkill(skillId)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		int32_t skillId = tfs::lua::getNumber<int32_t>(L, 2);
		player->setOfflineTrainingSkill(skillId);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetItemCount()
{
	// player:getItemCount(itemId[[, subType = -1], ignoreEquipped = false])
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	uint16_t itemId;
	if (isNumber(L, 2)) {
		itemId = context.get_number<uint16_t>(2);
	} else {
		itemId = Item::items.getItemIdByName(tfs::lua::getString(L, 2));
		if (itemId == 0) {
			context.push_nil();
			return 1;
		}
	}

	int32_t subType = tfs::lua::getNumber<int32_t>(L, 3, -1);
	bool ignoreEquipped = tfs::lua::getBoolean(L, 4, false);
	context.push_number(player->getItemTypeCount(itemId, subType, ignoreEquipped));
	return 1;
}

int LuaScriptInterface::luaPlayerGetItemById()
{
	// player:getItemById(itemId, deepSearch[, subType = -1])
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	uint16_t itemId;
	if (isNumber(L, 2)) {
		itemId = context.get_number<uint16_t>(2);
	} else {
		itemId = Item::items.getItemIdByName(tfs::lua::getString(L, 2));
		if (itemId == 0) {
			context.push_nil();
			return 1;
		}
	}
	bool deepSearch = tfs::lua::getBoolean(L, 3);
	int32_t subType = tfs::lua::getNumber<int32_t>(L, 4, -1);

	Item* item = g_game.findItemOfType(player, itemId, deepSearch, subType);
	if (item) {
		tfs::lua::pushUserdata(L, item);
		tfs::lua::setItemMetatable(L, -1, item);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetVocation()
{
	// player:getVocation()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		tfs::lua::pushUserdata(L, player->getVocation());
		tfs::lua::setMetatable(L, -1, "Vocation");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetVocation()
{
	// player:setVocation(id or name or userdata)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	Vocation* vocation;
	if (isNumber(L, 2)) {
		vocation = g_vocations.getVocation(context.get_number<uint16_t>(2));
	} else if (lua_isstring(L, 2)) {
		vocation = g_vocations.getVocation(g_vocations.getVocationId(tfs::lua::getString(L, 2)));
	} else if (lua_isuserdata(L, 2)) {
		vocation = tfs::lua::getUserdata<Vocation>(L, 2);
	} else {
		vocation = nullptr;
	}

	if (!vocation) {
		context.push_boolean(false);
		return 1;
	}

	player->setVocation(vocation->getId());
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaPlayerGetSex()
{
	// player:getSex()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->getSex());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetSex()
{
	// player:setSex(newSex)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		PlayerSex_t newSex = tfs::lua::getNumber<PlayerSex_t>(L, 2);
		player->setSex(newSex);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetTown()
{
	// player:getTown()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		tfs::lua::pushUserdata(L, player->getTown());
		tfs::lua::setMetatable(L, -1, "Town");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetTown()
{
	// player:setTown(town)
	Town* town = tfs::lua::getUserdata<Town>(L, 2);
	if (!town) {
		context.push_boolean(false);
		return 1;
	}

	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		player->setTown(town);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetGuild()
{
	// player:getGuild()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	if (const auto& guild = player->getGuild()) {
		pushSharedPtr(L, guild);
		tfs::lua::setMetatable(L, -1, "Guild");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetGuild()
{
	// player:setGuild(guild)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	player->setGuild(getSharedPtr<Guild>(L, 2));
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaPlayerGetGuildLevel()
{
	// player:getGuildLevel()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player && player->getGuild()) {
		context.push_number(player->getGuildRank()->level);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetGuildLevel()
{
	// player:setGuildLevel(level)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	const auto& guild = player->getGuild();
	if (!guild) {
		context.push_nil();
		return 1;
	}

	uint8_t level = context.get_number<uint8_t>(2);
	if (auto rank = guild->getRankByLevel(level)) {
		player->setGuildRank(rank);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetGuildNick()
{
	// player:getGuildNick()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		tfs::lua::pushString(L, player->getGuildNick());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetGuildNick()
{
	// player:setGuildNick(nick)
	const std::string& nick = tfs::lua::getString(L, 2);
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		player->setGuildNick(nick);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetGroup()
{
	// player:getGroup()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		tfs::lua::pushUserdata(L, player->getGroup());
		tfs::lua::setMetatable(L, -1, "Group");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetGroup()
{
	// player:setGroup(group)
	Group* group = tfs::lua::getUserdata<Group>(L, 2);
	if (!group) {
		context.push_boolean(false);
		return 1;
	}

	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		player->setGroup(group);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetStamina()
{
	// player:getStamina()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->getStaminaMinutes());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetStamina()
{
	// player:setStamina(stamina)
	uint16_t stamina = context.get_number<uint16_t>(2);
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		player->staminaMinutes = std::min<uint16_t>(2520, stamina);
		player->sendStats();
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetSoul()
{
	// player:getSoul()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->getSoul());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddSoul()
{
	// player:addSoul(soulChange)
	int32_t soulChange = tfs::lua::getNumber<int32_t>(L, 2);
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		player->changeSoul(soulChange);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetMaxSoul()
{
	// player:getMaxSoul()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player && player->vocation) {
		context.push_number(player->vocation->getSoulMax());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetBankBalance()
{
	// player:getBankBalance()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->getBankBalance());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetBankBalance()
{
	// player:setBankBalance(bankBalance)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	int64_t balance = tfs::lua::getNumber<int64_t>(L, 2);
	if (balance < 0) {
		reportErrorFunc(L, "Invalid bank balance value.");
		context.push_nil();
		return 1;
	}

	player->setBankBalance(balance);
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaPlayerAddItem()
{
	// player:addItem(itemId[, count = 1[, canDropOnMap = true[, subType = 1[, slot = CONST_SLOT_WHEREEVER]]]])
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_boolean(false);
		return 1;
	}

	uint16_t itemId;
	if (isNumber(L, 2)) {
		itemId = context.get_number<uint16_t>(2);
	} else {
		itemId = Item::items.getItemIdByName(tfs::lua::getString(L, 2));
		if (itemId == 0) {
			context.push_nil();
			return 1;
		}
	}

	int32_t count = tfs::lua::getNumber<int32_t>(L, 3, 1);
	int32_t subType = tfs::lua::getNumber<int32_t>(L, 5, 1);

	const ItemType& it = Item::items[itemId];

	int32_t itemCount = 1;
	int parameters = lua_gettop(L);
	if (parameters >= 5) {
		itemCount = std::max<int32_t>(1, count);
	} else if (it.hasSubType()) {
		if (it.stackable) {
			itemCount = std::ceil(count / static_cast<float>(ITEM_STACK_SIZE));
		}

		subType = count;
	} else {
		itemCount = std::max<int32_t>(1, count);
	}

	bool hasTable = itemCount > 1;
	if (hasTable) {
		lua_newtable(L);
	} else if (itemCount == 0) {
		context.push_nil();
		return 1;
	}

	bool canDropOnMap = tfs::lua::getBoolean(L, 4, true);
	slots_t slot = tfs::lua::getNumber<slots_t>(L, 6, CONST_SLOT_WHEREEVER);
	for (int32_t i = 1; i <= itemCount; ++i) {
		int32_t stackCount = subType;
		if (it.stackable) {
			stackCount = std::min<int32_t>(stackCount, ITEM_STACK_SIZE);
			subType -= stackCount;
		}

		Item* item = Item::CreateItem(itemId, stackCount);
		if (!item) {
			if (!hasTable) {
				context.push_nil();
			}
			return 1;
		}

		ReturnValue ret = g_game.internalPlayerAddItem(player, item, canDropOnMap, slot);
		if (ret != RETURNVALUE_NOERROR) {
			delete item;
			if (!hasTable) {
				context.push_nil();
			}
			return 1;
		}

		if (hasTable) {
			context.push_number(i);
			tfs::lua::pushUserdata(L, item);
			tfs::lua::setItemMetatable(L, -1, item);
			lua_settable(L, -3);
		} else {
			tfs::lua::pushUserdata(L, item);
			tfs::lua::setItemMetatable(L, -1, item);
		}
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddItemEx()
{
	// player:addItemEx(item[, canDropOnMap = false[, index = INDEX_WHEREEVER[, flags = 0]]])
	// player:addItemEx(item[, canDropOnMap = true[, slot = CONST_SLOT_WHEREEVER]])
	Item* item = tfs::lua::getUserdata<Item>(L, 2);
	if (!item) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_ITEM_NOT_FOUND));
		context.push_boolean(false);
		return 1;
	}

	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	if (item->getParent() != VirtualCylinder::virtualCylinder) {
		reportErrorFunc(L, "Item already has a parent");
		context.push_boolean(false);
		return 1;
	}

	bool canDropOnMap = tfs::lua::getBoolean(L, 3, false);
	ReturnValue returnValue;
	if (canDropOnMap) {
		slots_t slot = tfs::lua::getNumber<slots_t>(L, 4, CONST_SLOT_WHEREEVER);
		returnValue = g_game.internalPlayerAddItem(player, item, true, slot);
	} else {
		int32_t index = tfs::lua::getNumber<int32_t>(L, 4, INDEX_WHEREEVER);
		uint32_t flags = context.get_number<uint32_t>(5, 0);
		returnValue = g_game.internalAddItem(player, item, index, flags);
	}

	if (returnValue == RETURNVALUE_NOERROR) {
		tfs::lua::removeTempItem(item);
	}
	context.push_number(returnValue);
	return 1;
}

int LuaScriptInterface::luaPlayerRemoveItem()
{
	// player:removeItem(itemId, count[, subType = -1[, ignoreEquipped = false]])
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	uint16_t itemId;
	if (isNumber(L, 2)) {
		itemId = context.get_number<uint16_t>(2);
	} else {
		itemId = Item::items.getItemIdByName(tfs::lua::getString(L, 2));
		if (itemId == 0) {
			context.push_nil();
			return 1;
		}
	}

	uint32_t count = context.get_number<uint32_t>(3);
	int32_t subType = tfs::lua::getNumber<int32_t>(L, 4, -1);
	bool ignoreEquipped = tfs::lua::getBoolean(L, 5, false);
	context.push_boolean(player->removeItemOfType(itemId, count, subType, ignoreEquipped));
	return 1;
}

int LuaScriptInterface::luaPlayerSendSupplyUsed()
{
	// player:sendSupplyUsed(item)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_PLAYER_NOT_FOUND));
		context.push_boolean(false);
		return 1;
	}

	Item* item = tfs::lua::getUserdata<Item>(L, 2);
	if (!item) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_ITEM_NOT_FOUND));
		context.push_boolean(false);
		return 1;
	}

	player->sendSupplyUsed(item->getClientID());
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaPlayerGetMoney()
{
	// player:getMoney()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->getMoney());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddMoney()
{
	// player:addMoney(money)
	uint64_t money = tfs::lua::getNumber<uint64_t>(L, 2);
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		g_game.addMoney(player, money);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerRemoveMoney()
{
	// player:removeMoney(money)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		uint64_t money = tfs::lua::getNumber<uint64_t>(L, 2);
		context.push_boolean(g_game.removeMoney(player, money));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerShowTextDialog()
{
	// player:showTextDialog(id or name or userdata[, text[, canWrite[, length]]])
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	int32_t length = tfs::lua::getNumber<int32_t>(L, 5, -1);
	bool canWrite = tfs::lua::getBoolean(L, 4, false);
	std::string text;

	int parameters = lua_gettop(L);
	if (parameters >= 3) {
		text = tfs::lua::getString(L, 3);
	}

	Item* item;
	if (isNumber(L, 2)) {
		item = Item::CreateItem(context.get_number<uint16_t>(2));
	} else if (lua_isstring(L, 2)) {
		item = Item::CreateItem(Item::items.getItemIdByName(tfs::lua::getString(L, 2)));
	} else if (lua_isuserdata(L, 2)) {
		if (getUserdataType(L, 2) != LuaData_Item) {
			context.push_boolean(false);
			return 1;
		}

		item = tfs::lua::getUserdata<Item>(L, 2);
	} else {
		item = nullptr;
	}

	if (!item) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_ITEM_NOT_FOUND));
		context.push_boolean(false);
		return 1;
	}

	if (length < 0) {
		length = Item::items[item->getID()].maxTextLen;
	}

	if (!text.empty()) {
		item->setText(text);
		length = std::max<int32_t>(text.size(), length);
	}

	item->setParent(player);
	player->windowTextId++;
	player->writeItem = item;
	player->maxWriteLen = length;
	player->sendTextWindow(item, length, canWrite);
	lua_pushinteger(L, player->windowTextId);
	return 1;
}

int LuaScriptInterface::luaPlayerSendTextMessage()
{
	// player:sendTextMessage(type, text[, position, primaryValue = 0, primaryColor = TEXTCOLOR_NONE[,
	// secondaryValue = 0, secondaryColor = TEXTCOLOR_NONE]]) player:sendTextMessage(type, text, channelId)

	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	int parameters = lua_gettop(L);

	TextMessage message(tfs::lua::getNumber<MessageClasses>(L, 2), tfs::lua::getString(L, 3));
	if (parameters == 4) {
		uint16_t channelId = context.get_number<uint16_t>(4);
		ChatChannel* channel = g_chat->getChannel(*player, channelId);
		if (!channel || !channel->hasUser(*player)) {
			context.push_boolean(false);
			return 1;
		}
		message.channelId = channelId;
	} else {
		if (parameters >= 6) {
			message.position = tfs::lua::getPosition(L, 4);
			message.primary.value = tfs::lua::getNumber<int32_t>(L, 5);
			message.primary.color = tfs::lua::getNumber<TextColor_t>(L, 6);
		}

		if (parameters >= 8) {
			message.secondary.value = tfs::lua::getNumber<int32_t>(L, 7);
			message.secondary.color = tfs::lua::getNumber<TextColor_t>(L, 8);
		}
	}

	player->sendTextMessage(message);
	context.push_boolean(true);

	return 1;
}

int LuaScriptInterface::luaPlayerSendChannelMessage()
{
	// player:sendChannelMessage(author, text, type, channelId)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	uint16_t channelId = context.get_number<uint16_t>(5);
	SpeakClasses type = tfs::lua::getNumber<SpeakClasses>(L, 4);
	const std::string& text = tfs::lua::getString(L, 3);
	const std::string& author = tfs::lua::getString(L, 2);
	player->sendChannelMessage(author, text, type, channelId);
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaPlayerSendPrivateMessage()
{
	// player:sendPrivateMessage(speaker, text[, type])
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	const Player* speaker = tfs::lua::getUserdata<const Player>(L, 2);
	const std::string& text = tfs::lua::getString(L, 3);
	SpeakClasses type = tfs::lua::getNumber<SpeakClasses>(L, 4, TALKTYPE_PRIVATE_FROM);
	player->sendPrivateMessage(speaker, type, text);
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaPlayerChannelSay()
{
	// player:channelSay(speaker, type, text, channelId)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	Creature* speaker = tfs::lua::getCreature(L, 2);
	SpeakClasses type = tfs::lua::getNumber<SpeakClasses>(L, 3);
	const std::string& text = tfs::lua::getString(L, 4);
	uint16_t channelId = context.get_number<uint16_t>(5);
	player->sendToChannel(speaker, type, text, channelId);
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaPlayerOpenChannel()
{
	// player:openChannel(channelId)
	uint16_t channelId = context.get_number<uint16_t>(2);
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		g_game.playerOpenChannel(player->getID(), channelId);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetSlotItem()
{
	// player:getSlotItem(slot)
	const Player* player = tfs::lua::getUserdata<const Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	uint32_t slot = context.get_number<uint32_t>(2);
	Thing* thing = player->getThing(slot);
	if (!thing) {
		context.push_nil();
		return 1;
	}

	Item* item = thing->getItem();
	if (item) {
		tfs::lua::pushUserdata(L, item);
		tfs::lua::setItemMetatable(L, -1, item);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetParty()
{
	// player:getParty()
	const Player* player = tfs::lua::getUserdata<const Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	Party* party = player->getParty();
	if (party) {
		tfs::lua::pushUserdata(L, party);
		tfs::lua::setMetatable(L, -1, "Party");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddOutfit()
{
	// player:addOutfit(lookType)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		player->addOutfit(context.get_number<uint16_t>(2), 0);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddOutfitAddon()
{
	// player:addOutfitAddon(lookType, addon)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		uint16_t lookType = context.get_number<uint16_t>(2);
		uint8_t addon = context.get_number<uint8_t>(3);
		player->addOutfit(lookType, addon);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerRemoveOutfit()
{
	// player:removeOutfit(lookType)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		uint16_t lookType = context.get_number<uint16_t>(2);
		context.push_boolean(player->removeOutfit(lookType));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerRemoveOutfitAddon()
{
	// player:removeOutfitAddon(lookType, addon)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		uint16_t lookType = context.get_number<uint16_t>(2);
		uint8_t addon = context.get_number<uint8_t>(3);
		context.push_boolean(player->removeOutfitAddon(lookType, addon));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerHasOutfit()
{
	// player:hasOutfit(lookType[, addon = 0])
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		uint16_t lookType = context.get_number<uint16_t>(2);
		uint8_t addon = context.get_number<uint8_t>(3, 0);
		context.push_boolean(player->hasOutfit(lookType, addon));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerCanWearOutfit()
{
	// player:canWearOutfit(lookType[, addon = 0])
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		uint16_t lookType = context.get_number<uint16_t>(2);
		uint8_t addon = context.get_number<uint8_t>(3, 0);
		context.push_boolean(player->canWear(lookType, addon));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSendOutfitWindow()
{
	// player:sendOutfitWindow()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		player->sendOutfitWindow();
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSendEditPodium()
{
	// player:sendEditPodium(item)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	Item* item = tfs::lua::getUserdata<Item>(L, 2);
	if (player && item) {
		player->sendPodiumWindow(item);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddMount()
{
	// player:addMount(mountId or mountName)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	uint16_t mountId;
	if (isNumber(L, 2)) {
		mountId = context.get_number<uint16_t>(2);
	} else {
		Mount* mount = g_game.mounts.getMountByName(tfs::lua::getString(L, 2));
		if (!mount) {
			context.push_nil();
			return 1;
		}
		mountId = mount->id;
	}
	context.push_boolean(player->tameMount(mountId));
	return 1;
}

int LuaScriptInterface::luaPlayerRemoveMount()
{
	// player:removeMount(mountId or mountName)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	uint16_t mountId;
	if (isNumber(L, 2)) {
		mountId = context.get_number<uint16_t>(2);
	} else {
		Mount* mount = g_game.mounts.getMountByName(tfs::lua::getString(L, 2));
		if (!mount) {
			context.push_nil();
			return 1;
		}
		mountId = mount->id;
	}
	context.push_boolean(player->untameMount(mountId));
	return 1;
}

int LuaScriptInterface::luaPlayerHasMount()
{
	// player:hasMount(mountId or mountName)
	const Player* player = tfs::lua::getUserdata<const Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	Mount* mount = nullptr;
	if (isNumber(L, 2)) {
		mount = g_game.mounts.getMountByID(context.get_number<uint16_t>(2));
	} else {
		mount = g_game.mounts.getMountByName(tfs::lua::getString(L, 2));
	}

	if (mount) {
		context.push_boolean(player->hasMount(mount));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerToggleMount()
{
	// player:toggleMount(mount)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	bool mount = tfs::lua::getBoolean(L, 2);
	context.push_boolean(player->toggleMount(mount));
	return 1;
}

int LuaScriptInterface::luaPlayerGetPremiumEndsAt()
{
	// player:getPremiumEndsAt()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->premiumEndsAt);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetPremiumEndsAt()
{
	// player:setPremiumEndsAt(timestamp)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	time_t timestamp = tfs::lua::getNumber<time_t>(L, 2);

	player->setPremiumTime(timestamp);
	IOLoginData::updatePremiumTime(player->getAccount(), timestamp);
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaPlayerHasBlessing()
{
	// player:hasBlessing(blessing)
	uint8_t blessing = context.get_number<uint8_t>(2) - 1;
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_boolean(player->hasBlessing(blessing));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddBlessing()
{
	// player:addBlessing(blessing)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	uint8_t blessing = context.get_number<uint8_t>(2) - 1;
	if (player->hasBlessing(blessing)) {
		context.push_boolean(false);
		return 1;
	}

	player->addBlessing(blessing);
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaPlayerRemoveBlessing()
{
	// player:removeBlessing(blessing)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	uint8_t blessing = context.get_number<uint8_t>(2) - 1;
	if (!player->hasBlessing(blessing)) {
		context.push_boolean(false);
		return 1;
	}

	player->removeBlessing(blessing);
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaPlayerCanLearnSpell()
{
	// player:canLearnSpell(spellName)
	const Player* player = tfs::lua::getUserdata<const Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	const std::string& spellName = tfs::lua::getString(L, 2);
	InstantSpell* spell = g_spells->getInstantSpellByName(spellName);
	if (!spell) {
		reportErrorFunc(L, "Spell \"" + spellName + "\" not found");
		context.push_boolean(false);
		return 1;
	}

	if (player->hasFlag(PlayerFlag_IgnoreSpellCheck)) {
		context.push_boolean(true);
		return 1;
	}

	if (!spell->hasVocationSpellMap(player->getVocationId())) {
		context.push_boolean(false);
	} else if (player->getLevel() < spell->getLevel()) {
		context.push_boolean(false);
	} else if (player->getMagicLevel() < spell->getMagicLevel()) {
		context.push_boolean(false);
	} else {
		context.push_boolean(true);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerLearnSpell()
{
	// player:learnSpell(spellName)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		const std::string& spellName = tfs::lua::getString(L, 2);
		player->learnInstantSpell(spellName);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerForgetSpell()
{
	// player:forgetSpell(spellName)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		const std::string& spellName = tfs::lua::getString(L, 2);
		player->forgetInstantSpell(spellName);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerHasLearnedSpell()
{
	// player:hasLearnedSpell(spellName)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		const std::string& spellName = tfs::lua::getString(L, 2);
		context.push_boolean(player->hasLearnedInstantSpell(spellName));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSendTutorial()
{
	// player:sendTutorial(tutorialId)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		uint8_t tutorialId = context.get_number<uint8_t>(2);
		player->sendTutorial(tutorialId);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddMapMark()
{
	// player:addMapMark(position, type, description)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		const Position& position = tfs::lua::getPosition(L, 2);
		uint8_t type = context.get_number<uint8_t>(3);
		const std::string& description = tfs::lua::getString(L, 4);
		player->sendAddMarker(position, type, description);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSave()
{
	// player:save()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		player->loginPosition = player->getPosition();
		context.push_boolean(IOLoginData::savePlayer(player));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerPopupFYI()
{
	// player:popupFYI(message)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		const std::string& message = tfs::lua::getString(L, 2);
		player->sendFYIBox(message);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerIsPzLocked()
{
	// player:isPzLocked()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_boolean(player->isPzLocked());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetClient()
{
	// player:getClient()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		lua_createtable(L, 0, 2);
		setField(L, "version", player->getProtocolVersion());
		setField(L, "os", player->getOperatingSystem());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetHouse()
{
	// player:getHouse()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	House* house = g_game.map.houses.getHouseByPlayerId(player->getGUID());
	if (house) {
		tfs::lua::pushUserdata(L, house);
		tfs::lua::setMetatable(L, -1, "House");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSendHouseWindow()
{
	// player:sendHouseWindow(house, listId)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	House* house = tfs::lua::getUserdata<House>(L, 2);
	if (!house) {
		context.push_nil();
		return 1;
	}

	uint32_t listId = context.get_number<uint32_t>(3);
	player->sendHouseWindow(house, listId);
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaPlayerSetEditHouse()
{
	// player:setEditHouse(house, listId)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	House* house = tfs::lua::getUserdata<House>(L, 2);
	if (!house) {
		context.push_nil();
		return 1;
	}

	uint32_t listId = context.get_number<uint32_t>(3);
	player->setEditHouse(house, listId);
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaPlayerSetGhostMode()
{
	// player:setGhostMode(enabled[, magicEffect = CONST_ME_TELEPORT])
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	bool enabled = tfs::lua::getBoolean(L, 2);
	if (player->isInGhostMode() == enabled) {
		context.push_boolean(true);
		return 1;
	}

	MagicEffectClasses magicEffect = tfs::lua::getNumber<MagicEffectClasses>(L, 3, CONST_ME_TELEPORT);

	player->switchGhostMode();

	Tile* tile = player->getTile();
	const Position& position = player->getPosition();
	const bool isInvisible = player->isInvisible();

	Spectators spectators;
	g_game.map.getSpectators(spectators, position, true, true);
	for (Creature* spectator : spectators) {
		assert(dynamic_cast<Player*>(spectator) != nullptr);

		Player* spectatorPlayer = static_cast<Player*>(spectator);
		if (spectatorPlayer != player && !spectatorPlayer->isAccessPlayer()) {
			if (enabled) {
				spectatorPlayer->sendRemoveTileCreature(player, position,
				                                        tile->getClientIndexOfCreature(spectatorPlayer, player));
			} else {
				spectatorPlayer->sendCreatureAppear(player, position, magicEffect);
			}
		} else {
			if (isInvisible) {
				continue;
			}

			spectatorPlayer->sendCreatureChangeVisible(player, !enabled);
		}
	}

	if (player->isInGhostMode()) {
		for (const auto& it : g_game.getPlayers()) {
			if (!it.second->isAccessPlayer()) {
				it.second->notifyStatusChange(player, VIPSTATUS_OFFLINE);
			}
		}
		IOLoginData::updateOnlineStatus(player->getGUID(), false);
	} else {
		for (const auto& it : g_game.getPlayers()) {
			if (!it.second->isAccessPlayer()) {
				it.second->notifyStatusChange(player, VIPSTATUS_ONLINE);
			}
		}
		IOLoginData::updateOnlineStatus(player->getGUID(), true);
	}
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaPlayerGetContainerId()
{
	// player:getContainerId(container)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	Container* container = tfs::lua::getUserdata<Container>(L, 2);
	if (container) {
		context.push_number(player->getContainerID(container));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetContainerById()
{
	// player:getContainerById(id)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	Container* container = player->getContainerByID(context.get_number<uint8_t>(2));
	if (container) {
		tfs::lua::pushUserdata(L, container);
		tfs::lua::setMetatable(L, -1, "Container");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetContainerIndex()
{
	// player:getContainerIndex(id)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->getContainerIndex(context.get_number<uint8_t>(2)));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetInstantSpells()
{
	// player:getInstantSpells()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	std::vector<const InstantSpell*> spells;
	for (auto& spell : g_spells->getInstantSpells()) {
		if (spell.second.canCast(player)) {
			spells.push_back(&spell.second);
		}
	}

	lua_createtable(L, spells.size(), 0);

	int index = 0;
	for (auto spell : spells) {
		lua_createtable(L, 0, 7);

		setField(L, "name", spell->getName());
		setField(L, "words", spell->getWords());
		setField(L, "level", spell->getLevel());
		setField(L, "mlevel", spell->getMagicLevel());
		setField(L, "mana", spell->getMana());
		setField(L, "manapercent", spell->getManaPercent());
		setField(L, "params", spell->getHasParam());

		tfs::lua::setMetatable(L, -1, "Spell");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerCanCast()
{
	// player:canCast(spell)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	InstantSpell* spell = tfs::lua::getUserdata<InstantSpell>(L, 2);
	if (player && spell) {
		context.push_boolean(spell->canCast(player));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerHasChaseMode()
{
	// player:hasChaseMode()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_boolean(player->chaseMode);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerHasSecureMode()
{
	// player:hasSecureMode()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_boolean(player->secureMode);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetFightMode()
{
	// player:getFightMode()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->fightMode);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetStoreInbox()
{
	// player:getStoreInbox()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	Container* storeInbox = player->getStoreInbox();
	if (!storeInbox) {
		context.push_nil();
		return 1;
	}

	tfs::lua::pushUserdata(L, storeInbox);
	tfs::lua::setMetatable(L, -1, "Container");
	return 1;
}

int LuaScriptInterface::luaPlayerIsNearDepotBox()
{
	// player:isNearDepotBox()
	const Player* const player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	context.push_boolean(player->isNearDepotBox());
	return 1;
}

int LuaScriptInterface::luaPlayerGetIdleTime()
{
	// player:getIdleTime()
	const Player* const player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	context.push_number(player->getIdleTime());
	return 1;
}

int LuaScriptInterface::luaPlayerResetIdleTime()
{
	// player:resetIdleTime()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	player->resetIdleTime();
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaPlayerSendCreatureSquare()
{
	// player:sendCreatureSquare(creature, color)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (!player) {
		context.push_nil();
		return 1;
	}

	auto creature = tfs::lua::getCreature(L, 2);
	if (!creature) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
		context.push_boolean(false);
		return 1;
	}

	player->sendCreatureSquare(creature, tfs::lua::getNumber<SquareColor_t>(L, 3));
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaPlayerGetClientExpDisplay()
{
	// player:getClientExpDisplay()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->getClientExpDisplay());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetClientExpDisplay()
{
	// player:setClientExpDisplay(value)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		player->setClientExpDisplay(context.get_number<uint16_t>(2));
		player->sendStats();
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetClientStaminaBonusDisplay()
{
	// player:getClientStaminaBonusDisplay()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->getClientStaminaBonusDisplay());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetClientStaminaBonusDisplay()
{
	// player:setClientStaminaBonusDisplay(value)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		player->setClientStaminaBonusDisplay(context.get_number<uint16_t>(2));
		player->sendStats();
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetClientLowLevelBonusDisplay()
{
	// player:getClientLowLevelBonusDisplay()
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		context.push_number(player->getClientLowLevelBonusDisplay());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetClientLowLevelBonusDisplay()
{
	// player:setClientLowLevelBonusDisplay(value)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		player->setClientLowLevelBonusDisplay(context.get_number<uint16_t>(2));
		player->sendStats();
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSendResourceBalance()
{
	// player:sendResourceBalance(resource, amount)
	Player* player = tfs::lua::getUserdata<Player>(L, 1);
	if (player) {
		const ResourceTypes_t resourceType = tfs::lua::getNumber<ResourceTypes_t>(L, 2);
		uint64_t amount = tfs::lua::getNumber<uint64_t>(L, 3);
		player->sendResourceBalance(resourceType, amount);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

// Monster
int LuaScriptInterface::luaMonsterCreate()
{
	// Monster(id or userdata)
	Monster* monster;
	if (isNumber(L, 2)) {
		monster = g_game.getMonsterByID(context.get_number<uint32_t>(2));
	} else if (lua_isuserdata(L, 2)) {
		if (getUserdataType(L, 2) != LuaData_Monster) {
			context.push_nil();
			return 1;
		}
		monster = tfs::lua::getUserdata<Monster>(L, 2);
	} else {
		monster = nullptr;
	}

	if (monster) {
		tfs::lua::pushUserdata(L, monster);
		tfs::lua::setMetatable(L, -1, "Monster");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterIsMonster()
{
	// monster:isMonster()
	context.push_boolean(tfs::lua::getUserdata<const Monster>(L, 1) != nullptr);
	return 1;
}

int LuaScriptInterface::luaMonsterGetId()
{
	// monster:getId()
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (monster) {
		// Set monster id if it's not set yet (only for onSpawn event)
		if (tfs::lua::getScriptEnv()->getScriptId() == g_events->getScriptId(EventInfoId::MONSTER_ONSPAWN)) {
			monster->setID();
		}

		lua_pushinteger(L, monster->getID());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterGetType()
{
	// monster:getType()
	const Monster* monster = tfs::lua::getUserdata<const Monster>(L, 1);
	if (monster) {
		tfs::lua::pushUserdata(L, monster->mType);
		tfs::lua::setMetatable(L, -1, "MonsterType");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterRename()
{
	// monster:rename(name[, nameDescription])
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (!monster) {
		context.push_nil();
		return 1;
	}

	monster->setName(tfs::lua::getString(L, 2));
	if (lua_gettop(L) >= 3) {
		monster->setNameDescription(tfs::lua::getString(L, 3));
	}

	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaMonsterGetSpawnPosition()
{
	// monster:getSpawnPosition()
	const Monster* monster = tfs::lua::getUserdata<const Monster>(L, 1);
	if (monster) {
		tfs::lua::pushPosition(L, monster->getMasterPos());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterIsInSpawnRange()
{
	// monster:isInSpawnRange([position])
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (monster) {
		tfs::lua::pushBoolean(
		    L, monster->isInSpawnRange(lua_gettop(L) >= 2 ? tfs::lua::getPosition(L, 2) : monster->getPosition()));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterIsIdle()
{
	// monster:isIdle()
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (monster) {
		context.push_boolean(monster->getIdleStatus());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSetIdle()
{
	// monster:setIdle(idle)
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (!monster) {
		context.push_nil();
		return 1;
	}

	monster->setIdle(tfs::lua::getBoolean(L, 2));
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaMonsterIsTarget()
{
	// monster:isTarget(creature)
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (monster) {
		const Creature* creature = tfs::lua::getCreature(L, 2);
		if (!creature) {
			reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
			context.push_boolean(false);
			return 1;
		}

		context.push_boolean(monster->isTarget(creature));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterIsOpponent()
{
	// monster:isOpponent(creature)
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (monster) {
		const Creature* creature = tfs::lua::getCreature(L, 2);
		if (!creature) {
			reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
			context.push_boolean(false);
			return 1;
		}

		context.push_boolean(monster->isOpponent(creature));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterIsFriend()
{
	// monster:isFriend(creature)
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (monster) {
		const Creature* creature = tfs::lua::getCreature(L, 2);
		if (!creature) {
			reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
			context.push_boolean(false);
			return 1;
		}

		context.push_boolean(monster->isFriend(creature));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterAddFriend()
{
	// monster:addFriend(creature)
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (monster) {
		Creature* creature = tfs::lua::getCreature(L, 2);
		if (!creature) {
			reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
			context.push_boolean(false);
			return 1;
		}

		monster->addFriend(creature);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterRemoveFriend()
{
	// monster:removeFriend(creature)
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (monster) {
		Creature* creature = tfs::lua::getCreature(L, 2);
		if (!creature) {
			reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
			context.push_boolean(false);
			return 1;
		}

		monster->removeFriend(creature);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterGetFriendList()
{
	// monster:getFriendList()
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (!monster) {
		context.push_nil();
		return 1;
	}

	const auto& friendList = monster->getFriendList();
	lua_createtable(L, friendList.size(), 0);

	int index = 0;
	for (Creature* creature : friendList) {
		tfs::lua::pushUserdata(L, creature);
		tfs::lua::setCreatureMetatable(L, -1, creature);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterGetFriendCount()
{
	// monster:getFriendCount()
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (monster) {
		context.push_number(monster->getFriendList().size());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterAddTarget()
{
	// monster:addTarget(creature[, pushFront = false])
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (!monster) {
		context.push_nil();
		return 1;
	}

	Creature* creature = tfs::lua::getCreature(L, 2);
	if (!creature) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
		context.push_boolean(false);
		return 1;
	}

	bool pushFront = tfs::lua::getBoolean(L, 3, false);
	monster->addTarget(creature, pushFront);
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaMonsterRemoveTarget()
{
	// monster:removeTarget(creature)
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (!monster) {
		context.push_nil();
		return 1;
	}

	Creature* creature = tfs::lua::getCreature(L, 2);
	if (!creature) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
		context.push_boolean(false);
		return 1;
	}

	monster->removeTarget(creature);
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaMonsterGetTargetList()
{
	// monster:getTargetList()
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (!monster) {
		context.push_nil();
		return 1;
	}

	const auto& targetList = monster->getTargetList();
	lua_createtable(L, targetList.size(), 0);

	int index = 0;
	for (Creature* creature : targetList) {
		tfs::lua::pushUserdata(L, creature);
		tfs::lua::setCreatureMetatable(L, -1, creature);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterGetTargetCount()
{
	// monster:getTargetCount()
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (monster) {
		context.push_number(monster->getTargetList().size());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSelectTarget()
{
	// monster:selectTarget(creature)
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (monster) {
		Creature* creature = tfs::lua::getCreature(L, 2);
		if (!creature) {
			reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
			context.push_boolean(false);
			return 1;
		}

		context.push_boolean(monster->selectTarget(creature));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSearchTarget()
{
	// monster:searchTarget([searchType = TARGETSEARCH_DEFAULT])
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (monster) {
		TargetSearchType_t searchType = tfs::lua::getNumber<TargetSearchType_t>(L, 2, TARGETSEARCH_DEFAULT);
		context.push_boolean(monster->searchTarget(searchType));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterIsWalkingToSpawn()
{
	// monster:isWalkingToSpawn()
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (monster) {
		context.push_boolean(monster->isWalkingToSpawn());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterWalkToSpawn()
{
	// monster:walkToSpawn()
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (monster) {
		context.push_boolean(monster->walkToSpawn());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterHasIcon()
{
	// monster:hasSpecialIcon(iconId)
	const Monster* monster = tfs::lua::getUserdata<const Monster>(L, 1);
	if (monster) {
		auto iconId = tfs::lua::getNumber<MonsterIcon_t>(L, 2);
		context.push_boolean(monster->getSpecialIcons().contains(iconId));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSetIcon()
{
	// monster:setSpecialIcon(iconId, value)
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (!monster) {
		context.push_nil();
		return 1;
	}

	auto iconId = tfs::lua::getNumber<MonsterIcon_t>(L, 2);
	if (iconId > MONSTER_ICON_LAST) {
		reportErrorFunc(L, "Invalid Monster Icon Id");
		context.push_boolean(false);
		return 1;
	}

	monster->getSpecialIcons()[iconId] = context.get_number<uint16_t>(3);
	monster->updateIcons();
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaMonsterGetIcon()
{
	// monster:getSpecialIcon(iconId)
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (!monster) {
		context.push_nil();
		return 1;
	}

	auto iconId = tfs::lua::getNumber<MonsterIcon_t>(L, 2);
	const auto& icons = monster->getSpecialIcons();
	auto it = icons.find(iconId);
	if (it != icons.end()) {
		lua_pushinteger(L, it->second);
	} else {
		lua_pushinteger(L, 0);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterRemoveIcon()
{
	// monster:removeSpecialIcon(iconId)
	Monster* monster = tfs::lua::getUserdata<Monster>(L, 1);
	if (!monster) {
		context.push_nil();
		return 1;
	}

	auto iconId = tfs::lua::getNumber<MonsterIcon_t>(L, 2);
	auto& icons = monster->getSpecialIcons();
	auto it = icons.find(iconId);
	if (it != icons.end()) {
		icons.erase(it);
		monster->updateIcons();
		context.push_boolean(true);
	} else {
		context.push_boolean(false);
	}
	return 1;
}

// Npc
int LuaScriptInterface::luaNpcCreate()
{
	// Npc([id or name or userdata])
	Npc* npc;
	if (lua_gettop(L) >= 2) {
		if (isNumber(L, 2)) {
			npc = g_game.getNpcByID(context.get_number<uint32_t>(2));
		} else if (lua_isstring(L, 2)) {
			npc = g_game.getNpcByName(tfs::lua::getString(L, 2));
		} else if (lua_isuserdata(L, 2)) {
			if (getUserdataType(L, 2) != LuaData_Npc) {
				context.push_nil();
				return 1;
			}
			npc = tfs::lua::getUserdata<Npc>(L, 2);
		} else {
			npc = nullptr;
		}
	} else {
		npc = tfs::lua::getScriptEnv()->getNpc();
	}

	if (npc) {
		tfs::lua::pushUserdata(L, npc);
		tfs::lua::setMetatable(L, -1, "Npc");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNpcIsNpc()
{
	// npc:isNpc()
	context.push_boolean(tfs::lua::getUserdata<const Npc>(L, 1) != nullptr);
	return 1;
}

int LuaScriptInterface::luaNpcSetMasterPos()
{
	// npc:setMasterPos(pos[, radius])
	Npc* npc = tfs::lua::getUserdata<Npc>(L, 1);
	if (!npc) {
		context.push_nil();
		return 1;
	}

	const Position& pos = tfs::lua::getPosition(L, 2);
	int32_t radius = tfs::lua::getNumber<int32_t>(L, 3, 1);
	npc->setMasterPos(pos, radius);
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaNpcGetSpeechBubble()
{
	// npc:getSpeechBubble()
	Npc* npc = tfs::lua::getUserdata<Npc>(L, 1);
	if (npc) {
		context.push_number(npc->getSpeechBubble());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNpcSetSpeechBubble()
{
	// npc:setSpeechBubble(speechBubble)
	Npc* npc = tfs::lua::getUserdata<Npc>(L, 1);
	if (!npc) {
		context.push_nil();
		return 1;
	}

	if (!isNumber(L, 2)) {
		context.push_nil();
		return 1;
	}

	uint8_t speechBubble = context.get_number<uint8_t>(2);
	if (speechBubble > SPEECHBUBBLE_LAST) {
		context.push_nil();
	} else {
		npc->setSpeechBubble(speechBubble);

		// update creature speech bubble
		g_game.updateKnownCreature(npc);
		context.push_boolean(true);
	}
	return 1;
}

int LuaScriptInterface::luaNpcGetSpectators()
{
	// npc:getSpectators()
	Npc* npc = tfs::lua::getUserdata<Npc>(L, 1);
	if (!npc) {
		context.push_nil();
		return 1;
	}

	const auto& spectators = npc->getSpectators();
	lua_createtable(L, spectators.size(), 0);

	int index = 0;
	for (const auto& spectatorPlayer : npc->getSpectators()) {
		tfs::lua::pushUserdata(L, spectatorPlayer);
		tfs::lua::setMetatable(L, -1, "Player");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

// NpcType
int LuaScriptInterface::luaNpcTypeCreate()
{
	// NpcType(name)
	auto name = tfs::lua::getString(L, 2);
	auto npcType = Npcs::getNpcType(name);
	if (npcType) {
		tfs::lua::pushUserdata<NpcType>(L, npcType);
		tfs::lua::setMetatable(L, -1, "NpcType");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNpcTypeEventType()
{
	// get: npcType:eventType() set: npcType:eventType(string)
	NpcType* npcType = tfs::lua::getUserdata<NpcType>(L, 1);
	if (npcType) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushString(L, npcType->eventType);
		} else {
			std::string type = tfs::lua::getString(L, 2);
			const static auto tmp = std::array{"say",      "disappear", "appear", "move",        "closechannel",
			                                   "endtrade", "think",     "sight",  "speechbubble"};

			const auto it = std::find(tmp.begin(), tmp.end(), type);
			if (it != tmp.end()) {
				npcType->eventType = type;
				context.push_boolean(true);
				return 1;
			}

			std::cout << "[Warning - Npc::eventType] Unknown eventType name: " << type << " for npc: " << npcType->name
			          << std::endl;
			context.push_nil();
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNpcTypeOnCallback()
{
	// npcType:onSay(callback)
	// npcType:onDisappear(callback)
	// npcType:onAppear(callback)
	// npcType:onMove(callback)
	// npcType:onPlayerCloseChannel(callback)
	// npcType:onPlayerEndTrade(callback)
	// npcType:onThink(callback)
	// npcType:onSight(callback)
	NpcType* npcType = tfs::lua::getUserdata<NpcType>(L, 1);
	if (npcType) {
		if (npcType->loadCallback(Npcs::getScriptInterface())) {
			context.push_boolean(true);
			return 1;
		}
		context.push_boolean(false);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNpcTypeName()
{
	// get: npcType:name() set: npcType:name(string)
	NpcType* npcType = tfs::lua::getUserdata<NpcType>(L, 1);
	if (npcType) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushString(L, npcType->name);
		} else {
			std::string name = tfs::lua::getString(L, 2);
			npcType->name = name;
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNpcTypeSpeechBubble()
{
	// get: npcType:speechBubble() set: npcType:speechBubble(SPEECH_BUBBLE_)
	NpcType* npcType = tfs::lua::getUserdata<NpcType>(L, 1);
	if (npcType) {
		if (lua_gettop(L) == 1) {
			context.push_number(npcType->speechBubble);
		} else {
			uint8_t bubble = context.get_number<uint8_t>(2);
			npcType->speechBubble = bubble;
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNpcTypeWalkTicks()
{
	// get: npcType:walkTicks() set: npcType:walkTicks(ticks)
	NpcType* npcType = tfs::lua::getUserdata<NpcType>(L, 1);
	if (npcType) {
		if (lua_gettop(L) == 1) {
			context.push_number(npcType->walkTicks);
		} else {
			uint32_t ticks = context.get_number<uint32_t>(2);
			npcType->walkTicks = ticks;
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNpcTypeBaseSpeed()
{
	// get: npcType:baseSpeed() set: npcType:baseSpeed(speed)
	NpcType* npcType = tfs::lua::getUserdata<NpcType>(L, 1);
	if (npcType) {
		if (lua_gettop(L) == 1) {
			context.push_number(npcType->baseSpeed);
		} else {
			uint32_t speed = context.get_number<uint32_t>(2);
			npcType->baseSpeed = speed;
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNpcTypeMasterRadius()
{
	// get: npcType:masterRadius() set: npcType:masterRadius(radius)
	NpcType* npcType = tfs::lua::getUserdata<NpcType>(L, 1);
	if (npcType) {
		if (lua_gettop(L) == 1) {
			context.push_number(npcType->masterRadius);
		} else {
			int32_t radius = tfs::lua::getNumber<int32_t>(L, 2);
			npcType->masterRadius = radius;
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNpcTypeFloorChange()
{
	// get: npcType:floorChange() set: npcType:floorChange(bool)
	NpcType* npcType = tfs::lua::getUserdata<NpcType>(L, 1);
	if (npcType) {
		if (lua_gettop(L) == 1) {
			context.push_boolean(npcType->floorChange);
		} else {
			bool b = tfs::lua::getBoolean(L, 2);
			npcType->floorChange = b;
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNpcTypeAttackable()
{
	// get: npcType:attackable() set: npcType:attackable(bool)
	NpcType* npcType = tfs::lua::getUserdata<NpcType>(L, 1);
	if (npcType) {
		if (lua_gettop(L) == 1) {
			context.push_boolean(npcType->attackable);
		} else {
			bool b = tfs::lua::getBoolean(L, 2);
			npcType->attackable = b;
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNpcTypeIgnoreHeight()
{
	// get: npcType:ignoreHeight() set: npcType:ignoreHeight(bool)
	NpcType* npcType = tfs::lua::getUserdata<NpcType>(L, 1);
	if (npcType) {
		if (lua_gettop(L) == 1) {
			context.push_boolean(npcType->ignoreHeight);
		} else {
			bool b = tfs::lua::getBoolean(L, 2);
			npcType->ignoreHeight = b;
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNpcTypeIsIdle()
{
	// get: npcType:isIdle() set: npcType:isIdle(bool)
	NpcType* npcType = tfs::lua::getUserdata<NpcType>(L, 1);
	if (npcType) {
		if (lua_gettop(L) == 1) {
			context.push_boolean(npcType->isIdle);
		} else {
			bool b = tfs::lua::getBoolean(L, 2);
			npcType->isIdle = b;
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNpcTypePushable()
{
	// get: npcType:pushable() set: npcType:pushable(bool)
	NpcType* npcType = tfs::lua::getUserdata<NpcType>(L, 1);
	if (npcType) {
		if (lua_gettop(L) == 1) {
			context.push_boolean(npcType->pushable);
		} else {
			bool b = tfs::lua::getBoolean(L, 2);
			npcType->pushable = b;
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNpcTypeDefaultOutfit()
{
	// get: npcType:defaultOutfit() set: npcType:defaultOutfit(outfit)
	NpcType* npcType = tfs::lua::getUserdata<NpcType>(L, 1);
	if (npcType) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushOutfit(L, npcType->defaultOutfit);
		} else {
			auto outfit = getOutfit(L, 2);
			npcType->defaultOutfit = outfit;
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNpcTypeParameter()
{
	// get: npcType:parameters() set: npcType:parameters(key, value)
	NpcType* npcType = tfs::lua::getUserdata<NpcType>(L, 1);
	if (npcType) {
		if (lua_gettop(L) == 1) {
			lua_createtable(L, npcType->parameters.size(), 0);
			for (auto i : npcType->parameters) {
				setField(L, i.first, i.second);
			}
		} else {
			std::string key = tfs::lua::getString(L, 2);
			std::string value = tfs::lua::getString(L, 3);
			npcType->parameters[key] = value;
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNpcTypeHealth()
{
	// get: npcType:health() set: npcType:health(health)
	NpcType* npcType = tfs::lua::getUserdata<NpcType>(L, 1);
	if (npcType) {
		if (lua_gettop(L) == 1) {
			context.push_number(npcType->health);
		} else {
			int32_t health = tfs::lua::getNumber<int32_t>(L, 2);
			npcType->health = health;
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNpcTypeMaxHealth()
{
	// get: npcType:maxHealth() set: npcType:maxHealth(health)
	NpcType* npcType = tfs::lua::getUserdata<NpcType>(L, 1);
	if (npcType) {
		if (lua_gettop(L) == 1) {
			context.push_number(npcType->healthMax);
		} else {
			int32_t health = tfs::lua::getNumber<int32_t>(L, 2);
			npcType->healthMax = health;
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaNpcTypeSight()
{
	// get: npcType:sight() set: npcType:sight(x, y)
	NpcType* npcType = tfs::lua::getUserdata<NpcType>(L, 1);
	if (npcType) {
		if (lua_gettop(L) == 1) {
			context.push_number(npcType->sightX);
			context.push_number(npcType->sightY);
			return 2;
		} else {
			npcType->sightX = context.get_number<uint16_t>(2);
			npcType->sightY = context.get_number<uint16_t>(3);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

// Guild
int LuaScriptInterface::luaGuildCreate()
{
	// Guild(id)
	uint32_t id = context.get_number<uint32_t>(2);

	if (const auto& guild = g_game.getGuild(id)) {
		pushSharedPtr(L, guild);
		tfs::lua::setMetatable(L, -1, "Guild");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaGuildGetId()
{
	// guild:getId()
	if (const auto& guild = getSharedPtr<Guild>(L, 1)) {
		context.push_number(guild->getId());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaGuildGetName()
{
	// guild:getName()
	if (const auto& guild = getSharedPtr<Guild>(L, 1)) {
		tfs::lua::pushString(L, guild->getName());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaGuildGetMembersOnline()
{
	// guild:getMembersOnline()
	const auto& guild = getSharedPtr<const Guild>(L, 1);
	if (!guild) {
		context.push_nil();
		return 1;
	}

	const auto& members = guild->getMembersOnline();
	lua_createtable(L, members.size(), 0);

	int index = 0;
	for (Player* player : members) {
		tfs::lua::pushUserdata(L, player);
		tfs::lua::setMetatable(L, -1, "Player");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaGuildAddRank()
{
	// guild:addRank(id, name, level)
	if (const auto& guild = getSharedPtr<Guild>(L, 1)) {
		uint32_t id = context.get_number<uint32_t>(2);
		const std::string& name = tfs::lua::getString(L, 3);
		uint8_t level = context.get_number<uint8_t>(4);
		guild->addRank(id, name, level);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaGuildGetRankById()
{
	// guild:getRankById(id)
	const auto& guild = getSharedPtr<Guild>(L, 1);
	if (!guild) {
		context.push_nil();
		return 1;
	}

	uint32_t id = context.get_number<uint32_t>(2);
	if (auto rank = guild->getRankById(id)) {
		lua_createtable(L, 0, 3);
		setField(L, "id", rank->id);
		setField(L, "name", rank->name);
		setField(L, "level", rank->level);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaGuildGetRankByLevel()
{
	// guild:getRankByLevel(level)
	const auto& guild = getSharedPtr<const Guild>(L, 1);
	if (!guild) {
		context.push_nil();
		return 1;
	}

	uint8_t level = context.get_number<uint8_t>(2);
	if (auto rank = guild->getRankByLevel(level)) {
		lua_createtable(L, 0, 3);
		setField(L, "id", rank->id);
		setField(L, "name", rank->name);
		setField(L, "level", rank->level);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaGuildGetMotd()
{
	// guild:getMotd()
	if (const auto& guild = getSharedPtr<Guild>(L, 1)) {
		tfs::lua::pushString(L, guild->getMotd());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaGuildSetMotd()
{
	// guild:setMotd(motd)
	if (const auto& guild = getSharedPtr<Guild>(L, 1)) {
		const std::string& motd = tfs::lua::getString(L, 2);
		guild->setMotd(motd);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

// Group
int LuaScriptInterface::luaGroupCreate()
{
	// Group(id)
	uint32_t id = context.get_number<uint32_t>(2);

	Group* group = g_game.groups.getGroup(id);
	if (group) {
		tfs::lua::pushUserdata(L, group);
		tfs::lua::setMetatable(L, -1, "Group");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaGroupGetId()
{
	// group:getId()
	Group* group = tfs::lua::getUserdata<Group>(L, 1);
	if (group) {
		context.push_number(group->id);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaGroupGetName()
{
	// group:getName()
	Group* group = tfs::lua::getUserdata<Group>(L, 1);
	if (group) {
		tfs::lua::pushString(L, group->name);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaGroupGetFlags()
{
	// group:getFlags()
	Group* group = tfs::lua::getUserdata<Group>(L, 1);
	if (group) {
		context.push_number(group->flags);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaGroupGetAccess()
{
	// group:getAccess()
	Group* group = tfs::lua::getUserdata<Group>(L, 1);
	if (group) {
		context.push_boolean(group->access);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaGroupGetMaxDepotItems()
{
	// group:getMaxDepotItems()
	Group* group = tfs::lua::getUserdata<Group>(L, 1);
	if (group) {
		context.push_number(group->maxDepotItems);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaGroupGetMaxVipEntries()
{
	// group:getMaxVipEntries()
	Group* group = tfs::lua::getUserdata<Group>(L, 1);
	if (group) {
		context.push_number(group->maxVipEntries);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaGroupHasFlag()
{
	// group:hasFlag(flag)
	Group* group = tfs::lua::getUserdata<Group>(L, 1);
	if (group) {
		PlayerFlags flag = tfs::lua::getNumber<PlayerFlags>(L, 2);
		context.push_boolean((group->flags & flag) != 0);
	} else {
		context.push_nil();
	}
	return 1;
}

// Vocation
int LuaScriptInterface::luaVocationCreate()
{
	// Vocation(id or name)
	uint32_t id;
	if (isNumber(L, 2)) {
		id = context.get_number<uint32_t>(2);
	} else {
		id = g_vocations.getVocationId(tfs::lua::getString(L, 2));
	}

	Vocation* vocation = g_vocations.getVocation(id);
	if (vocation) {
		tfs::lua::pushUserdata(L, vocation);
		tfs::lua::setMetatable(L, -1, "Vocation");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetId()
{
	// vocation:getId()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		context.push_number(vocation->getId());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetClientId()
{
	// vocation:getClientId()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		context.push_number(vocation->getClientId());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetName()
{
	// vocation:getName()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		tfs::lua::pushString(L, vocation->getVocName());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetDescription()
{
	// vocation:getDescription()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		tfs::lua::pushString(L, vocation->getVocDescription());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetRequiredSkillTries()
{
	// vocation:getRequiredSkillTries(skillType, skillLevel)
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		skills_t skillType = tfs::lua::getNumber<skills_t>(L, 2);
		uint16_t skillLevel = context.get_number<uint16_t>(3);
		context.push_number(vocation->getReqSkillTries(skillType, skillLevel));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetRequiredManaSpent()
{
	// vocation:getRequiredManaSpent(magicLevel)
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		uint32_t magicLevel = context.get_number<uint32_t>(2);
		context.push_number(vocation->getReqMana(magicLevel));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetCapacityGain()
{
	// vocation:getCapacityGain()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		context.push_number(vocation->getCapGain());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetHealthGain()
{
	// vocation:getHealthGain()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		context.push_number(vocation->getHPGain());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetHealthGainTicks()
{
	// vocation:getHealthGainTicks()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		context.push_number(vocation->getHealthGainTicks());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetHealthGainAmount()
{
	// vocation:getHealthGainAmount()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		context.push_number(vocation->getHealthGainAmount());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetManaGain()
{
	// vocation:getManaGain()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		context.push_number(vocation->getManaGain());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetManaGainTicks()
{
	// vocation:getManaGainTicks()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		context.push_number(vocation->getManaGainTicks());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetManaGainAmount()
{
	// vocation:getManaGainAmount()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		context.push_number(vocation->getManaGainAmount());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetMaxSoul()
{
	// vocation:getMaxSoul()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		context.push_number(vocation->getSoulMax());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetSoulGainTicks()
{
	// vocation:getSoulGainTicks()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		context.push_number(vocation->getSoulGainTicks());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetAttackSpeed()
{
	// vocation:getAttackSpeed()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		context.push_number(vocation->getAttackSpeed());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetBaseSpeed()
{
	// vocation:getBaseSpeed()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		context.push_number(vocation->getBaseSpeed());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetDemotion()
{
	// vocation:getDemotion()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (!vocation) {
		context.push_nil();
		return 1;
	}

	uint16_t fromId = vocation->getFromVocation();
	if (fromId == VOCATION_NONE) {
		context.push_nil();
		return 1;
	}

	Vocation* demotedVocation = g_vocations.getVocation(fromId);
	if (demotedVocation && demotedVocation != vocation) {
		tfs::lua::pushUserdata(L, demotedVocation);
		tfs::lua::setMetatable(L, -1, "Vocation");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetPromotion()
{
	// vocation:getPromotion()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (!vocation) {
		context.push_nil();
		return 1;
	}

	uint16_t promotedId = g_vocations.getPromotedVocation(vocation->getId());
	if (promotedId == VOCATION_NONE) {
		context.push_nil();
		return 1;
	}

	Vocation* promotedVocation = g_vocations.getVocation(promotedId);
	if (promotedVocation && promotedVocation != vocation) {
		tfs::lua::pushUserdata(L, promotedVocation);
		tfs::lua::setMetatable(L, -1, "Vocation");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaVocationAllowsPvp()
{
	// vocation:allowsPvp()
	Vocation* vocation = tfs::lua::getUserdata<Vocation>(L, 1);
	if (vocation) {
		context.push_boolean(vocation->allowsPvp());
	} else {
		context.push_nil();
	}
	return 1;
}

// Town
int LuaScriptInterface::luaTownCreate()
{
	// Town(id or name)
	Town* town;
	if (isNumber(L, 2)) {
		town = g_game.map.towns.getTown(context.get_number<uint32_t>(2));
	} else if (lua_isstring(L, 2)) {
		town = g_game.map.towns.getTown(tfs::lua::getString(L, 2));
	} else {
		town = nullptr;
	}

	if (town) {
		tfs::lua::pushUserdata(L, town);
		tfs::lua::setMetatable(L, -1, "Town");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaTownGetId()
{
	// town:getId()
	Town* town = tfs::lua::getUserdata<Town>(L, 1);
	if (town) {
		context.push_number(town->getID());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaTownGetName()
{
	// town:getName()
	Town* town = tfs::lua::getUserdata<Town>(L, 1);
	if (town) {
		tfs::lua::pushString(L, town->getName());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaTownGetTemplePosition()
{
	// town:getTemplePosition()
	Town* town = tfs::lua::getUserdata<Town>(L, 1);
	if (town) {
		tfs::lua::pushPosition(L, town->getTemplePosition());
	} else {
		context.push_nil();
	}
	return 1;
}

// House
int LuaScriptInterface::luaHouseCreate()
{
	// House(id)
	House* house = g_game.map.houses.getHouse(context.get_number<uint32_t>(2));
	if (house) {
		tfs::lua::pushUserdata(L, house);
		tfs::lua::setMetatable(L, -1, "House");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetId()
{
	// house:getId()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		context.push_number(house->getId());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetName()
{
	// house:getName()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		tfs::lua::pushString(L, house->getName());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetTown()
{
	// house:getTown()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (!house) {
		context.push_nil();
		return 1;
	}

	Town* town = g_game.map.towns.getTown(house->getTownId());
	if (town) {
		tfs::lua::pushUserdata(L, town);
		tfs::lua::setMetatable(L, -1, "Town");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetExitPosition()
{
	// house:getExitPosition()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		tfs::lua::pushPosition(L, house->getEntryPosition());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetRent()
{
	// house:getRent()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		context.push_number(house->getRent());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaHouseSetRent()
{
	// house:setRent(rent)
	uint32_t rent = context.get_number<uint32_t>(2);
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		house->setRent(rent);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetPaidUntil()
{
	// house:getPaidUntil()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		context.push_number(house->getPaidUntil());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaHouseSetPaidUntil()
{
	// house:setPaidUntil(timestamp)
	time_t timestamp = tfs::lua::getNumber<time_t>(L, 2);
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		house->setPaidUntil(timestamp);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetPayRentWarnings()
{
	// house:getPayRentWarnings()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		context.push_number(house->getPayRentWarnings());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaHouseSetPayRentWarnings()
{
	// house:setPayRentWarnings(warnings)
	uint32_t warnings = context.get_number<uint32_t>(2);
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		house->setPayRentWarnings(warnings);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetOwnerName()
{
	// house:getOwnerName()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		tfs::lua::pushString(L, house->getOwnerName());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetOwnerGuid()
{
	// house:getOwnerGuid()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		context.push_number(house->getOwner());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaHouseSetOwnerGuid()
{
	// house:setOwnerGuid(guid[, updateDatabase = true])
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		uint32_t guid = context.get_number<uint32_t>(2);
		bool updateDatabase = tfs::lua::getBoolean(L, 3, true);
		house->setOwner(guid, updateDatabase);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaHouseStartTrade()
{
	// house:startTrade(player, tradePartner)
	House* house = tfs::lua::getUserdata<House>(L, 1);
	Player* player = tfs::lua::getUserdata<Player>(L, 2);
	Player* tradePartner = tfs::lua::getUserdata<Player>(L, 3);

	if (!player || !tradePartner || !house) {
		context.push_nil();
		return 1;
	}

	if (!tradePartner->getPosition().isInRange(player->getPosition(), 2, 2, 0)) {
		context.push_number(RETURNVALUE_TRADEPLAYERFARAWAY);
		return 1;
	}

	if (house->getOwner() != player->getGUID()) {
		context.push_number(RETURNVALUE_YOUDONTOWNTHISHOUSE);
		return 1;
	}

	if (g_game.map.houses.getHouseByPlayerId(tradePartner->getGUID())) {
		context.push_number(RETURNVALUE_TRADEPLAYERALREADYOWNSAHOUSE);
		return 1;
	}

	if (IOLoginData::hasBiddedOnHouse(tradePartner->getGUID())) {
		context.push_number(RETURNVALUE_TRADEPLAYERHIGHESTBIDDER);
		return 1;
	}

	Item* transferItem = house->getTransferItem();
	if (!transferItem) {
		context.push_number(RETURNVALUE_YOUCANNOTTRADETHISHOUSE);
		return 1;
	}

	transferItem->getParent()->setParent(player);
	if (!g_game.internalStartTrade(player, tradePartner, transferItem)) {
		house->resetTransferItem();
	}

	context.push_number(RETURNVALUE_NOERROR);
	return 1;
}

int LuaScriptInterface::luaHouseGetBeds()
{
	// house:getBeds()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (!house) {
		context.push_nil();
		return 1;
	}

	const auto& beds = house->getBeds();
	lua_createtable(L, beds.size(), 0);

	int index = 0;
	for (BedItem* bedItem : beds) {
		tfs::lua::pushUserdata<Item>(L, bedItem);
		tfs::lua::setItemMetatable(L, -1, bedItem);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetBedCount()
{
	// house:getBedCount()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		context.push_number(house->getBedCount());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetDoors()
{
	// house:getDoors()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (!house) {
		context.push_nil();
		return 1;
	}

	const auto& doors = house->getDoors();
	lua_createtable(L, doors.size(), 0);

	int index = 0;
	for (Door* door : doors) {
		tfs::lua::pushUserdata<Item>(L, door);
		tfs::lua::setItemMetatable(L, -1, door);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetDoorCount()
{
	// house:getDoorCount()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		context.push_number(house->getDoors().size());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetDoorIdByPosition()
{
	// house:getDoorIdByPosition(position)
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (!house) {
		context.push_nil();
		return 1;
	}

	Door* door = house->getDoorByPosition(tfs::lua::getPosition(L, 2));
	if (door) {
		context.push_number(door->getDoorId());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetTiles()
{
	// house:getTiles()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (!house) {
		context.push_nil();
		return 1;
	}

	const auto& tiles = house->getTiles();
	lua_createtable(L, tiles.size(), 0);

	int index = 0;
	for (Tile* tile : tiles) {
		tfs::lua::pushUserdata(L, tile);
		tfs::lua::setMetatable(L, -1, "Tile");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetItems()
{
	// house:getItems()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (!house) {
		context.push_nil();
		return 1;
	}

	const auto& tiles = house->getTiles();
	lua_newtable(L);

	int index = 0;
	for (Tile* tile : tiles) {
		TileItemVector* itemVector = tile->getItemList();
		if (itemVector) {
			for (Item* item : *itemVector) {
				tfs::lua::pushUserdata(L, item);
				tfs::lua::setItemMetatable(L, -1, item);
				lua_rawseti(L, -2, ++index);
			}
		}
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetTileCount()
{
	// house:getTileCount()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (house) {
		context.push_number(house->getTiles().size());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaHouseCanEditAccessList()
{
	// house:canEditAccessList(listId, player)
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (!house) {
		context.push_nil();
		return 1;
	}

	uint32_t listId = context.get_number<uint32_t>(2);
	Player* player = tfs::lua::getPlayer(L, 3);

	context.push_boolean(house->canEditAccessList(listId, player));
	return 1;
}

int LuaScriptInterface::luaHouseGetAccessList()
{
	// house:getAccessList(listId)
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (!house) {
		context.push_nil();
		return 1;
	}

	std::string list;
	uint32_t listId = context.get_number<uint32_t>(2);
	if (house->getAccessList(listId, list)) {
		tfs::lua::pushString(L, list);
	} else {
		context.push_boolean(false);
	}
	return 1;
}

int LuaScriptInterface::luaHouseSetAccessList()
{
	// house:setAccessList(listId, list)
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (!house) {
		context.push_nil();
		return 1;
	}

	uint32_t listId = context.get_number<uint32_t>(2);
	const std::string& list = tfs::lua::getString(L, 3);
	house->setAccessList(listId, list);
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaHouseKickPlayer()
{
	// house:kickPlayer(player, targetPlayer)
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (!house) {
		context.push_nil();
		return 1;
	}

	context.push_boolean(house->kickPlayer(tfs::lua::getPlayer(L, 2), tfs::lua::getPlayer(L, 3)));
	return 1;
}

int LuaScriptInterface::luaHouseSave()
{
	// house:save()
	House* house = tfs::lua::getUserdata<House>(L, 1);
	if (!house) {
		context.push_nil();
		return 1;
	}

	context.push_boolean(IOMapSerialize::saveHouse(house));
	return 1;
}

// ItemType
int LuaScriptInterface::luaItemTypeCreate()
{
	// ItemType(id or name)
	uint32_t id;
	if (isNumber(L, 2)) {
		id = context.get_number<uint32_t>(2);
	} else if (lua_isstring(L, 2)) {
		id = Item::items.getItemIdByName(tfs::lua::getString(L, 2));
	} else {
		context.push_nil();
		return 1;
	}

	const ItemType& itemType = Item::items[id];
	tfs::lua::pushUserdata(L, &itemType);
	tfs::lua::setMetatable(L, -1, "ItemType");
	return 1;
}

int LuaScriptInterface::luaItemTypeIsCorpse()
{
	// itemType:isCorpse()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_boolean(itemType->corpseType != RACE_NONE);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsDoor()
{
	// itemType:isDoor()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_boolean(itemType->isDoor());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsContainer()
{
	// itemType:isContainer()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_boolean(itemType->isContainer());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsFluidContainer()
{
	// itemType:isFluidContainer()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_boolean(itemType->isFluidContainer());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsMovable()
{
	// itemType:isMovable()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_boolean(itemType->moveable);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsRune()
{
	// itemType:isRune()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_boolean(itemType->isRune());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsStackable()
{
	// itemType:isStackable()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_boolean(itemType->stackable);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsReadable()
{
	// itemType:isReadable()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_boolean(itemType->canReadText);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsWritable()
{
	// itemType:isWritable()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_boolean(itemType->canWriteText);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsBlocking()
{
	// itemType:isBlocking()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_boolean(itemType->blockProjectile || itemType->blockSolid);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsGroundTile()
{
	// itemType:isGroundTile()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_boolean(itemType->isGroundTile());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsMagicField()
{
	// itemType:isMagicField()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_boolean(itemType->isMagicField());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsUseable()
{
	// itemType:isUseable()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_boolean(itemType->isUseable());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsPickupable()
{
	// itemType:isPickupable()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_boolean(itemType->isPickupable());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsRotatable()
{
	// itemType:isRotatable()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_boolean(itemType->rotatable);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetType()
{
	// itemType:getType()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_number(itemType->type);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetGroup()
{
	// itemType:getGroup()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_number(itemType->group);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetId()
{
	// itemType:getId()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_number(itemType->id);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetClientId()
{
	// itemType:getClientId()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_number(itemType->clientId);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetName()
{
	// itemType:getName()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushString(L, itemType->name);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetPluralName()
{
	// itemType:getPluralName()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushString(L, itemType->getPluralName());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetRotateTo()
{
	// itemType:getRotateTo()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_number(itemType->rotateTo);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetArticle()
{
	// itemType:getArticle()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushString(L, itemType->article);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetDescription()
{
	// itemType:getDescription()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushString(L, itemType->description);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetSlotPosition()
{
	// itemType:getSlotPosition()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_number(itemType->slotPosition);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetCharges()
{
	// itemType:getCharges()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_number(itemType->charges);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetFluidSource()
{
	// itemType:getFluidSource()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_number(itemType->fluidSource);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetCapacity()
{
	// itemType:getCapacity()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_number(itemType->maxItems);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetWeight()
{
	// itemType:getWeight([count = 1])
	uint16_t count = context.get_number<uint16_t>(2, 1);

	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (!itemType) {
		context.push_nil();
		return 1;
	}

	uint64_t weight = static_cast<uint64_t>(itemType->weight) * std::max<int32_t>(1, count);
	context.push_number(weight);
	return 1;
}

int LuaScriptInterface::luaItemTypeGetWorth()
{
	// itemType:getWorth()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (!itemType) {
		context.push_nil();
		return 1;
	}

	context.push_number(itemType->worth);
	return 1;
}

int LuaScriptInterface::luaItemTypeGetHitChance()
{
	// itemType:getHitChance()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_number(itemType->hitChance);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetShootRange()
{
	// itemType:getShootRange()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_number(itemType->shootRange);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetAttack()
{
	// itemType:getAttack()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_number(itemType->attack);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetAttackSpeed()
{
	// itemType:getAttackSpeed()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_number(itemType->attackSpeed);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetDefense()
{
	// itemType:getDefense()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_number(itemType->defense);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetExtraDefense()
{
	// itemType:getExtraDefense()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_number(itemType->extraDefense);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetArmor()
{
	// itemType:getArmor()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_number(itemType->armor);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetWeaponType()
{
	// itemType:getWeaponType()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_number(itemType->weaponType);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetAmmoType()
{
	// itemType:getAmmoType()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_number(itemType->ammoType);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetCorpseType()
{
	// itemType:getCorpseType()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_number(itemType->corpseType);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetClassification()
{
	// itemType:getClassification()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_number(itemType->classification);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetAbilities()
{
	// itemType:getAbilities()
	ItemType* itemType = tfs::lua::getUserdata<ItemType>(L, 1);
	if (itemType) {
		Abilities& abilities = itemType->getAbilities();
		lua_createtable(L, 10, 12);
		setField(L, "healthGain", abilities.healthGain);
		setField(L, "healthTicks", abilities.healthTicks);
		setField(L, "manaGain", abilities.manaGain);
		setField(L, "manaTicks", abilities.manaTicks);
		setField(L, "conditionImmunities", abilities.conditionImmunities);
		setField(L, "conditionSuppressions", abilities.conditionSuppressions);
		setField(L, "speed", abilities.speed);
		setField(L, "elementDamage", abilities.elementDamage);
		setField(L, "elementType", abilities.elementType);

		lua_pushboolean(L, abilities.manaShield);
		lua_setfield(L, -2, "manaShield");
		lua_pushboolean(L, abilities.invisible);
		lua_setfield(L, -2, "invisible");
		lua_pushboolean(L, abilities.regeneration);
		lua_setfield(L, -2, "regeneration");

		// Stats
		lua_createtable(L, 0, STAT_LAST + 1);
		for (int32_t i = STAT_FIRST; i <= STAT_LAST; i++) {
			context.push_number(abilities.stats[i]);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "stats");

		// Stats percent
		lua_createtable(L, 0, STAT_LAST + 1);
		for (int32_t i = STAT_FIRST; i <= STAT_LAST; i++) {
			context.push_number(abilities.statsPercent[i]);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "statsPercent");

		// Skills
		lua_createtable(L, 0, SKILL_LAST + 1);
		for (int32_t i = SKILL_FIRST; i <= SKILL_LAST; i++) {
			context.push_number(abilities.skills[i]);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "skills");

		// Special skills
		lua_createtable(L, 0, SPECIALSKILL_LAST + 1);
		for (int32_t i = SPECIALSKILL_FIRST; i <= SPECIALSKILL_LAST; i++) {
			context.push_number(abilities.specialSkills[i]);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "specialSkills");

		// Field absorb percent
		lua_createtable(L, 0, COMBAT_COUNT);
		for (int32_t i = 0; i < COMBAT_COUNT; i++) {
			context.push_number(abilities.fieldAbsorbPercent[i]);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "fieldAbsorbPercent");

		// Absorb percent
		lua_createtable(L, 0, COMBAT_COUNT);
		for (int32_t i = 0; i < COMBAT_COUNT; i++) {
			context.push_number(abilities.absorbPercent[i]);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "absorbPercent");

		// special magic level
		lua_createtable(L, 0, COMBAT_COUNT);
		for (int32_t i = 0; i < COMBAT_COUNT; i++) {
			context.push_number(abilities.specialMagicLevelSkill[i]);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "specialMagicLevel");

		// Damage boost percent
		lua_createtable(L, 0, COMBAT_COUNT);
		for (int32_t i = 0; i < COMBAT_COUNT; i++) {
			context.push_number(abilities.boostPercent[i]);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "boostPercent");

		// Reflect chance
		lua_createtable(L, 0, COMBAT_COUNT);
		for (int32_t i = 0; i < COMBAT_COUNT; i++) {
			context.push_number(abilities.reflect[i].chance);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "reflectChance");

		// Reflect percent
		lua_createtable(L, 0, COMBAT_COUNT);
		for (int32_t i = 0; i < COMBAT_COUNT; i++) {
			context.push_number(abilities.reflect[i].percent);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "reflectPercent");
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeHasShowAttributes()
{
	// itemType:hasShowAttributes()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_boolean(itemType->showAttributes);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeHasShowCount()
{
	// itemType:hasShowCount()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_boolean(itemType->showCount);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeHasShowCharges()
{
	// itemType:hasShowCharges()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_boolean(itemType->showCharges);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeHasShowDuration()
{
	// itemType:hasShowDuration()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_boolean(itemType->showDuration);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeHasAllowDistRead()
{
	// itemType:hasAllowDistRead()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_boolean(itemType->allowDistRead);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetWieldInfo()
{
	// itemType:getWieldInfo()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushinteger(L, itemType->wieldInfo);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetDurationMin()
{
	// itemType:getDurationMin()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushinteger(L, itemType->decayTimeMin);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetDurationMax()
{
	// itemType:getDurationMax()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushinteger(L, itemType->decayTimeMax);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetLevelDoor()
{
	// itemType:getLevelDoor()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushinteger(L, itemType->levelDoor);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetRuneSpellName()
{
	// itemType:getRuneSpellName()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType && itemType->isRune()) {
		tfs::lua::pushString(L, itemType->runeSpellName);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetVocationString()
{
	// itemType:getVocationString()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		tfs::lua::pushString(L, itemType->vocationString);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetMinReqLevel()
{
	// itemType:getMinReqLevel()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushinteger(L, itemType->minReqLevel);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetMinReqMagicLevel()
{
	// itemType:getMinReqMagicLevel()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushinteger(L, itemType->minReqMagicLevel);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetMarketBuyStatistics()
{
	// itemType:getMarketBuyStatistics()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		MarketStatistics* statistics = IOMarket::getInstance().getPurchaseStatistics(itemType->id);
		if (statistics) {
			lua_createtable(L, 4, 0);
			setField(L, "numTransactions", statistics->numTransactions);
			setField(L, "totalPrice", statistics->totalPrice);
			setField(L, "highestPrice", statistics->highestPrice);
			setField(L, "lowestPrice", statistics->lowestPrice);
		} else {
			context.push_nil();
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetMarketSellStatistics()
{
	// itemType:getMarketSellStatistics()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		MarketStatistics* statistics = IOMarket::getInstance().getSaleStatistics(itemType->id);
		if (statistics) {
			lua_createtable(L, 4, 0);
			setField(L, "numTransactions", statistics->numTransactions);
			setField(L, "totalPrice", statistics->totalPrice);
			setField(L, "highestPrice", statistics->highestPrice);
			setField(L, "lowestPrice", statistics->lowestPrice);
		} else {
			context.push_nil();
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetElementType()
{
	// itemType:getElementType()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (!itemType) {
		context.push_nil();
		return 1;
	}

	auto& abilities = itemType->abilities;
	if (abilities) {
		context.push_number(abilities->elementType);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetElementDamage()
{
	// itemType:getElementDamage()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (!itemType) {
		context.push_nil();
		return 1;
	}

	auto& abilities = itemType->abilities;
	if (abilities) {
		context.push_number(abilities->elementDamage);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetTransformEquipId()
{
	// itemType:getTransformEquipId()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_number(itemType->transformEquipTo);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetTransformDeEquipId()
{
	// itemType:getTransformDeEquipId()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_number(itemType->transformDeEquipTo);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetDestroyId()
{
	// itemType:getDestroyId()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_number(itemType->destroyTo);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetDecayId()
{
	// itemType:getDecayId()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_number(itemType->decayTo);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetRequiredLevel()
{
	// itemType:getRequiredLevel()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_number(itemType->minReqLevel);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeHasSubType()
{
	// itemType:hasSubType()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_boolean(itemType->hasSubType());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsStoreItem()
{
	// itemType:isStoreItem()
	const ItemType* itemType = tfs::lua::getUserdata<const ItemType>(L, 1);
	if (itemType) {
		context.push_boolean(itemType->storeItem);
	} else {
		context.push_nil();
	}
	return 1;
}

// Combat

int LuaScriptInterface::luaCombatCreate()
{
	// Combat()
	pushSharedPtr(L, g_luaEnvironment.createCombatObject(tfs::lua::getScriptEnv()->getScriptInterface()));
	tfs::lua::setMetatable(L, -1, "Combat");
	return 1;
}

int LuaScriptInterface::luaCombatDelete()
{
	Combat_ptr& combat = getSharedPtr<Combat>(L, 1);
	if (combat) {
		combat.reset();
	}
	return 0;
}

int LuaScriptInterface::luaCombatSetParameter()
{
	// combat:setParameter(key, value)
	const Combat_ptr& combat = getSharedPtr<Combat>(L, 1);
	if (!combat) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_COMBAT_NOT_FOUND));
		context.push_nil();
		return 1;
	}

	CombatParam_t key = tfs::lua::getNumber<CombatParam_t>(L, 2);
	uint32_t value;
	if (lua_isboolean(L, 3)) {
		value = tfs::lua::getBoolean(L, 3) ? 1 : 0;
	} else {
		value = context.get_number<uint32_t>(3);
	}
	combat->setParam(key, value);
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaCombatGetParameter()
{
	// combat:getParameter(key)
	const Combat_ptr& combat = getSharedPtr<Combat>(L, 1);
	if (!combat) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_COMBAT_NOT_FOUND));
		context.push_nil();
		return 1;
	}

	int32_t value = combat->getParam(tfs::lua::getNumber<CombatParam_t>(L, 2));
	if (value == std::numeric_limits<int32_t>().max()) {
		context.push_nil();
		return 1;
	}

	context.push_number(value);
	return 1;
}

int LuaScriptInterface::luaCombatSetFormula()
{
	// combat:setFormula(type, mina, minb, maxa, maxb)
	const Combat_ptr& combat = getSharedPtr<Combat>(L, 1);
	if (!combat) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_COMBAT_NOT_FOUND));
		context.push_nil();
		return 1;
	}

	formulaType_t type = tfs::lua::getNumber<formulaType_t>(L, 2);
	double mina = tfs::lua::getNumber<double>(L, 3);
	double minb = tfs::lua::getNumber<double>(L, 4);
	double maxa = tfs::lua::getNumber<double>(L, 5);
	double maxb = tfs::lua::getNumber<double>(L, 6);
	combat->setPlayerCombatValues(type, mina, minb, maxa, maxb);
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaCombatSetArea()
{
	// combat:setArea(area)
	if (tfs::lua::getScriptEnv()->getScriptId() != EVENT_ID_LOADING) {
		reportErrorFunc(L, "This function can only be used while loading the script.");
		context.push_nil();
		return 1;
	}

	const AreaCombat* area = g_luaEnvironment.getAreaObject(context.get_number<uint32_t>(2));
	if (!area) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_AREA_NOT_FOUND));
		context.push_nil();
		return 1;
	}

	const Combat_ptr& combat = getSharedPtr<Combat>(L, 1);
	if (!combat) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_COMBAT_NOT_FOUND));
		context.push_nil();
		return 1;
	}

	combat->setArea(new AreaCombat(*area));
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaCombatAddCondition()
{
	// combat:addCondition(condition)
	const Combat_ptr& combat = getSharedPtr<Combat>(L, 1);
	if (!combat) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_COMBAT_NOT_FOUND));
		context.push_nil();
		return 1;
	}

	Condition* condition = tfs::lua::getUserdata<Condition>(L, 2);
	if (condition) {
		combat->addCondition(condition->clone());
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCombatClearConditions()
{
	// combat:clearConditions()
	const Combat_ptr& combat = getSharedPtr<Combat>(L, 1);
	if (!combat) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_COMBAT_NOT_FOUND));
		context.push_nil();
		return 1;
	}

	combat->clearConditions();
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaCombatSetCallback()
{
	// combat:setCallback(key, function)
	const Combat_ptr& combat = getSharedPtr<Combat>(L, 1);
	if (!combat) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_COMBAT_NOT_FOUND));
		context.push_nil();
		return 1;
	}

	CallBackParam_t key = tfs::lua::getNumber<CallBackParam_t>(L, 2);
	if (!combat->setCallback(key)) {
		context.push_nil();
		return 1;
	}

	CallBack* callback = combat->getCallback(key);
	if (!callback) {
		context.push_nil();
		return 1;
	}

	const std::string& function = tfs::lua::getString(L, 3);
	context.push_boolean(callback->loadCallBack(tfs::lua::getScriptEnv()->getScriptInterface(), function));
	return 1;
}

int LuaScriptInterface::luaCombatSetOrigin()
{
	// combat:setOrigin(origin)
	const Combat_ptr& combat = getSharedPtr<Combat>(L, 1);
	if (!combat) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_COMBAT_NOT_FOUND));
		context.push_nil();
		return 1;
	}

	combat->setOrigin(tfs::lua::getNumber<CombatOrigin>(L, 2));
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaCombatExecute()
{
	// combat:execute(creature, variant)
	const Combat_ptr& combat = getSharedPtr<Combat>(L, 1);
	if (!combat) {
		reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_COMBAT_NOT_FOUND));
		context.push_nil();
		return 1;
	}

	if (lua_isuserdata(L, 2)) {
		LuaDataType type = getUserdataType(L, 2);
		if (type != LuaData_Player && type != LuaData_Monster && type != LuaData_Npc) {
			context.push_boolean(false);
			return 1;
		}
	}

	Creature* creature = tfs::lua::getCreature(L, 2);

	const LuaVariant& variant = getVariant(L, 3);
	switch (variant.type()) {
		case VARIANT_NUMBER: {
			Creature* target = g_game.getCreatureByID(variant.getNumber());
			if (!target) {
				context.push_boolean(false);
				return 1;
			}

			if (combat->hasArea()) {
				combat->doCombat(creature, target->getPosition());
			} else {
				combat->doCombat(creature, target);
			}
			break;
		}

		case VARIANT_POSITION: {
			combat->doCombat(creature, variant.getPosition());
			break;
		}

		case VARIANT_TARGETPOSITION: {
			if (combat->hasArea()) {
				combat->doCombat(creature, variant.getTargetPosition());
			} else {
				combat->postCombatEffects(creature, variant.getTargetPosition());
				g_game.addMagicEffect(variant.getTargetPosition(), CONST_ME_POFF);
			}
			break;
		}

		case VARIANT_STRING: {
			Player* target = g_game.getPlayerByName(variant.getString());
			if (!target) {
				context.push_boolean(false);
				return 1;
			}

			combat->doCombat(creature, target);
			break;
		}

		case VARIANT_NONE: {
			reportErrorFunc(L, tfs::lua::getErrorDesc(LUA_ERROR_VARIANT_NOT_FOUND));
			context.push_boolean(false);
			return 1;
		}

		default: {
			break;
		}
	}

	context.push_boolean(true);
	return 1;
}

// Condition
int LuaScriptInterface::luaConditionCreate()
{
	// Condition(conditionType[, conditionId = CONDITIONID_COMBAT])
	ConditionType_t conditionType = tfs::lua::getNumber<ConditionType_t>(L, 2);
	ConditionId_t conditionId = tfs::lua::getNumber<ConditionId_t>(L, 3, CONDITIONID_COMBAT);

	Condition* condition = Condition::createCondition(conditionId, conditionType, 0, 0);
	if (condition) {
		tfs::lua::pushUserdata(L, condition);
		tfs::lua::setMetatable(L, -1, "Condition");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaConditionDelete()
{
	// condition:delete()
	Condition** conditionPtr = tfs::lua::getRawUserdata<Condition>(L, 1);
	if (conditionPtr && *conditionPtr) {
		delete *conditionPtr;
		*conditionPtr = nullptr;
	}
	return 0;
}

int LuaScriptInterface::luaConditionGetId()
{
	// condition:getId()
	Condition* condition = tfs::lua::getUserdata<Condition>(L, 1);
	if (condition) {
		context.push_number(condition->getId());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaConditionGetSubId()
{
	// condition:getSubId()
	Condition* condition = tfs::lua::getUserdata<Condition>(L, 1);
	if (condition) {
		context.push_number(condition->getSubId());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaConditionGetType()
{
	// condition:getType()
	Condition* condition = tfs::lua::getUserdata<Condition>(L, 1);
	if (condition) {
		context.push_number(condition->getType());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaConditionGetIcons()
{
	// condition:getIcons()
	Condition* condition = tfs::lua::getUserdata<Condition>(L, 1);
	if (condition) {
		context.push_number(condition->getIcons());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaConditionGetEndTime()
{
	// condition:getEndTime()
	Condition* condition = tfs::lua::getUserdata<Condition>(L, 1);
	if (condition) {
		context.push_number(condition->getEndTime());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaConditionClone()
{
	// condition:clone()
	Condition* condition = tfs::lua::getUserdata<Condition>(L, 1);
	if (condition) {
		tfs::lua::pushUserdata(L, condition->clone());
		tfs::lua::setMetatable(L, -1, "Condition");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaConditionGetTicks()
{
	// condition:getTicks()
	Condition* condition = tfs::lua::getUserdata<Condition>(L, 1);
	if (condition) {
		context.push_number(condition->getTicks());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaConditionSetTicks()
{
	// condition:setTicks(ticks)
	int32_t ticks = tfs::lua::getNumber<int32_t>(L, 2);
	Condition* condition = tfs::lua::getUserdata<Condition>(L, 1);
	if (condition) {
		condition->setTicks(ticks);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaConditionSetParameter()
{
	// condition:setParameter(key, value)
	Condition* condition = tfs::lua::getUserdata<Condition>(L, 1);
	if (!condition) {
		context.push_nil();
		return 1;
	}

	ConditionParam_t key = tfs::lua::getNumber<ConditionParam_t>(L, 2);
	int32_t value;
	if (lua_isboolean(L, 3)) {
		value = tfs::lua::getBoolean(L, 3) ? 1 : 0;
	} else {
		value = tfs::lua::getNumber<int32_t>(L, 3);
	}
	condition->setParam(key, value);
	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaConditionGetParameter()
{
	// condition:getParameter(key)
	Condition* condition = tfs::lua::getUserdata<Condition>(L, 1);
	if (!condition) {
		context.push_nil();
		return 1;
	}

	int32_t value = condition->getParam(tfs::lua::getNumber<ConditionParam_t>(L, 2));
	if (value == std::numeric_limits<int32_t>().max()) {
		context.push_nil();
		return 1;
	}

	context.push_number(value);
	return 1;
}

int LuaScriptInterface::luaConditionSetFormula()
{
	// condition:setFormula(mina, minb, maxa, maxb)
	double maxb = tfs::lua::getNumber<double>(L, 5);
	double maxa = tfs::lua::getNumber<double>(L, 4);
	double minb = tfs::lua::getNumber<double>(L, 3);
	double mina = tfs::lua::getNumber<double>(L, 2);
	ConditionSpeed* condition = dynamic_cast<ConditionSpeed*>(tfs::lua::getUserdata<Condition>(L, 1));
	if (condition) {
		condition->setFormulaVars(mina, minb, maxa, maxb);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaConditionSetOutfit()
{
	// condition:setOutfit(outfit)
	// condition:setOutfit(lookTypeEx, lookType, lookHead, lookBody, lookLegs, lookFeet[, lookAddons[, lookMount]])
	Outfit_t outfit;
	if (lua_istable(L, 2)) {
		outfit = getOutfit(L, 2);
	} else {
		outfit.lookMount = context.get_number<uint16_t>(9, outfit.lookMount);
		outfit.lookAddons = context.get_number<uint8_t>(8, outfit.lookAddons);
		outfit.lookFeet = context.get_number<uint8_t>(7);
		outfit.lookLegs = context.get_number<uint8_t>(6);
		outfit.lookBody = context.get_number<uint8_t>(5);
		outfit.lookHead = context.get_number<uint8_t>(4);
		outfit.lookType = context.get_number<uint16_t>(3);
		outfit.lookTypeEx = context.get_number<uint16_t>(2);
	}

	ConditionOutfit* condition = dynamic_cast<ConditionOutfit*>(tfs::lua::getUserdata<Condition>(L, 1));
	if (condition) {
		condition->setOutfit(outfit);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaConditionAddDamage()
{
	// condition:addDamage(rounds, time, value)
	int32_t value = tfs::lua::getNumber<int32_t>(L, 4);
	int32_t time = tfs::lua::getNumber<int32_t>(L, 3);
	int32_t rounds = tfs::lua::getNumber<int32_t>(L, 2);
	ConditionDamage* condition = dynamic_cast<ConditionDamage*>(tfs::lua::getUserdata<Condition>(L, 1));
	if (condition) {
		context.push_boolean(condition->addDamage(rounds, time, value));
	} else {
		context.push_nil();
	}
	return 1;
}

// Outfit
int LuaScriptInterface::luaOutfitCreate()
{
	// Outfit(looktype)
	const Outfit* outfit = Outfits::getInstance().getOutfitByLookType(context.get_number<uint16_t>(2));
	if (outfit) {
		tfs::lua::pushOutfit(L, outfit);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaOutfitCompare()
{
	// outfit == outfitEx
	Outfit outfitEx = getOutfitClass(L, 2);
	Outfit outfit = getOutfitClass(L, 1);
	context.push_boolean(outfit == outfitEx);
	return 1;
}

// MonsterType
int LuaScriptInterface::luaMonsterTypeCreate()
{
	// MonsterType(name or raceId)
	MonsterType* monsterType;
	if (isNumber(L, 2)) {
		monsterType = g_monsters.getMonsterType(context.get_number<uint32_t>(2));
	} else {
		monsterType = g_monsters.getMonsterType(tfs::lua::getString(L, 2));
	}

	if (monsterType) {
		tfs::lua::pushUserdata(L, monsterType);
		tfs::lua::setMetatable(L, -1, "MonsterType");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeIsAttackable()
{
	// get: monsterType:isAttackable() set: monsterType:isAttackable(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_boolean(monsterType->info.isAttackable);
		} else {
			monsterType->info.isAttackable = tfs::lua::getBoolean(L, 2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeIsChallengeable()
{
	// get: monsterType:isChallengeable() set: monsterType:isChallengeable(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_boolean(monsterType->info.isChallengeable);
		} else {
			monsterType->info.isChallengeable = tfs::lua::getBoolean(L, 2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeIsConvinceable()
{
	// get: monsterType:isConvinceable() set: monsterType:isConvinceable(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_boolean(monsterType->info.isConvinceable);
		} else {
			monsterType->info.isConvinceable = tfs::lua::getBoolean(L, 2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeIsSummonable()
{
	// get: monsterType:isSummonable() set: monsterType:isSummonable(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_boolean(monsterType->info.isSummonable);
		} else {
			monsterType->info.isSummonable = tfs::lua::getBoolean(L, 2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeIsIgnoringSpawnBlock()
{
	// get: monsterType:isIgnoringSpawnBlock() set: monsterType:isIgnoringSpawnBlock(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_boolean(monsterType->info.isIgnoringSpawnBlock);
		} else {
			monsterType->info.isIgnoringSpawnBlock = tfs::lua::getBoolean(L, 2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeIsIllusionable()
{
	// get: monsterType:isIllusionable() set: monsterType:isIllusionable(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_boolean(monsterType->info.isIllusionable);
		} else {
			monsterType->info.isIllusionable = tfs::lua::getBoolean(L, 2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeIsHostile()
{
	// get: monsterType:isHostile() set: monsterType:isHostile(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_boolean(monsterType->info.isHostile);
		} else {
			monsterType->info.isHostile = tfs::lua::getBoolean(L, 2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeIsPushable()
{
	// get: monsterType:isPushable() set: monsterType:isPushable(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_boolean(monsterType->info.pushable);
		} else {
			monsterType->info.pushable = tfs::lua::getBoolean(L, 2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeIsHealthHidden()
{
	// get: monsterType:isHealthHidden() set: monsterType:isHealthHidden(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_boolean(monsterType->info.hiddenHealth);
		} else {
			monsterType->info.hiddenHealth = tfs::lua::getBoolean(L, 2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeIsBoss()
{
	// get: monsterType:isBoss() set: monsterType:isBoss(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_boolean(monsterType->info.isBoss);
		} else {
			monsterType->info.isBoss = tfs::lua::getBoolean(L, 2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeCanPushItems()
{
	// get: monsterType:canPushItems() set: monsterType:canPushItems(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_boolean(monsterType->info.canPushItems);
		} else {
			monsterType->info.canPushItems = tfs::lua::getBoolean(L, 2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeCanPushCreatures()
{
	// get: monsterType:canPushCreatures() set: monsterType:canPushCreatures(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_boolean(monsterType->info.canPushCreatures);
		} else {
			monsterType->info.canPushCreatures = tfs::lua::getBoolean(L, 2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeCanWalkOnEnergy()
{
	// get: monsterType:canWalkOnEnergy() set: monsterType:canWalkOnEnergy(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_boolean(monsterType->info.canWalkOnEnergy);
		} else {
			monsterType->info.canWalkOnEnergy = tfs::lua::getBoolean(L, 2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeCanWalkOnFire()
{
	// get: monsterType:canWalkOnFire() set: monsterType:canWalkOnFire(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_boolean(monsterType->info.canWalkOnFire);
		} else {
			monsterType->info.canWalkOnFire = tfs::lua::getBoolean(L, 2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeCanWalkOnPoison()
{
	// get: monsterType:canWalkOnPoison() set: monsterType:canWalkOnPoison(bool)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_boolean(monsterType->info.canWalkOnPoison);
		} else {
			monsterType->info.canWalkOnPoison = tfs::lua::getBoolean(L, 2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int32_t LuaScriptInterface::luaMonsterTypeName()
{
	// get: monsterType:name() set: monsterType:name(name)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushString(L, monsterType->name);
		} else {
			monsterType->name = tfs::lua::getString(L, 2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeNameDescription()
{
	// get: monsterType:nameDescription() set: monsterType:nameDescription(desc)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushString(L, monsterType->nameDescription);
		} else {
			monsterType->nameDescription = tfs::lua::getString(L, 2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeHealth()
{
	// get: monsterType:health() set: monsterType:health(health)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_number(monsterType->info.health);
		} else {
			monsterType->info.health = tfs::lua::getNumber<int32_t>(L, 2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeMaxHealth()
{
	// get: monsterType:maxHealth() set: monsterType:maxHealth(health)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_number(monsterType->info.healthMax);
		} else {
			monsterType->info.healthMax = tfs::lua::getNumber<int32_t>(L, 2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeRunHealth()
{
	// get: monsterType:runHealth() set: monsterType:runHealth(health)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_number(monsterType->info.runAwayHealth);
		} else {
			monsterType->info.runAwayHealth = tfs::lua::getNumber<int32_t>(L, 2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeExperience()
{
	// get: monsterType:experience() set: monsterType:experience(exp)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_number(monsterType->info.experience);
		} else {
			monsterType->info.experience = tfs::lua::getNumber<uint64_t>(L, 2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeSkull()
{
	// get: monsterType:skull() set: monsterType:skull(str/constant)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_number(monsterType->info.skull);
		} else {
			if (isNumber(L, 2)) {
				monsterType->info.skull = tfs::lua::getNumber<Skulls_t>(L, 2);
			} else {
				monsterType->info.skull = getSkullType(tfs::lua::getString(L, 2));
			}
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeCombatImmunities()
{
	// get: monsterType:combatImmunities() set: monsterType:combatImmunities(immunity)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_number(monsterType->info.damageImmunities);
		} else {
			std::string immunity = tfs::lua::getString(L, 2);
			if (immunity == "physical") {
				monsterType->info.damageImmunities |= COMBAT_PHYSICALDAMAGE;
				context.push_boolean(true);
			} else if (immunity == "energy") {
				monsterType->info.damageImmunities |= COMBAT_ENERGYDAMAGE;
				context.push_boolean(true);
			} else if (immunity == "fire") {
				monsterType->info.damageImmunities |= COMBAT_FIREDAMAGE;
				context.push_boolean(true);
			} else if (immunity == "poison" || immunity == "earth") {
				monsterType->info.damageImmunities |= COMBAT_EARTHDAMAGE;
				context.push_boolean(true);
			} else if (immunity == "drown") {
				monsterType->info.damageImmunities |= COMBAT_DROWNDAMAGE;
				context.push_boolean(true);
			} else if (immunity == "ice") {
				monsterType->info.damageImmunities |= COMBAT_ICEDAMAGE;
				context.push_boolean(true);
			} else if (immunity == "holy") {
				monsterType->info.damageImmunities |= COMBAT_HOLYDAMAGE;
				context.push_boolean(true);
			} else if (immunity == "death") {
				monsterType->info.damageImmunities |= COMBAT_DEATHDAMAGE;
				context.push_boolean(true);
			} else if (immunity == "lifedrain") {
				monsterType->info.damageImmunities |= COMBAT_LIFEDRAIN;
				context.push_boolean(true);
			} else if (immunity == "manadrain") {
				monsterType->info.damageImmunities |= COMBAT_MANADRAIN;
				context.push_boolean(true);
			} else {
				std::cout << "[Warning - Monsters::loadMonster] Unknown immunity name " << immunity
				          << " for monster: " << monsterType->name << '\n';
				context.push_nil();
			}
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeConditionImmunities()
{
	// get: monsterType:conditionImmunities() set: monsterType:conditionImmunities(immunity)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_number(monsterType->info.conditionImmunities);
		} else {
			std::string immunity = tfs::lua::getString(L, 2);
			if (immunity == "physical") {
				monsterType->info.conditionImmunities |= CONDITION_BLEEDING;
				context.push_boolean(true);
			} else if (immunity == "energy") {
				monsterType->info.conditionImmunities |= CONDITION_ENERGY;
				context.push_boolean(true);
			} else if (immunity == "fire") {
				monsterType->info.conditionImmunities |= CONDITION_FIRE;
				context.push_boolean(true);
			} else if (immunity == "poison" || immunity == "earth") {
				monsterType->info.conditionImmunities |= CONDITION_POISON;
				context.push_boolean(true);
			} else if (immunity == "drown") {
				monsterType->info.conditionImmunities |= CONDITION_DROWN;
				context.push_boolean(true);
			} else if (immunity == "ice") {
				monsterType->info.conditionImmunities |= CONDITION_FREEZING;
				context.push_boolean(true);
			} else if (immunity == "holy") {
				monsterType->info.conditionImmunities |= CONDITION_DAZZLED;
				context.push_boolean(true);
			} else if (immunity == "death") {
				monsterType->info.conditionImmunities |= CONDITION_CURSED;
				context.push_boolean(true);
			} else if (immunity == "paralyze") {
				monsterType->info.conditionImmunities |= CONDITION_PARALYZE;
				context.push_boolean(true);
			} else if (immunity == "outfit") {
				monsterType->info.conditionImmunities |= CONDITION_OUTFIT;
				context.push_boolean(true);
			} else if (immunity == "drunk") {
				monsterType->info.conditionImmunities |= CONDITION_DRUNK;
				context.push_boolean(true);
			} else if (immunity == "invisible" || immunity == "invisibility") {
				monsterType->info.conditionImmunities |= CONDITION_INVISIBLE;
				context.push_boolean(true);
			} else if (immunity == "bleed") {
				monsterType->info.conditionImmunities |= CONDITION_BLEEDING;
				context.push_boolean(true);
			} else {
				std::cout << "[Warning - Monsters::loadMonster] Unknown immunity name " << immunity
				          << " for monster: " << monsterType->name << '\n';
				context.push_nil();
			}
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeGetAttackList()
{
	// monsterType:getAttackList()
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (!monsterType) {
		context.push_nil();
		return 1;
	}

	lua_createtable(L, monsterType->info.attackSpells.size(), 0);

	int index = 0;
	for (const auto& spellBlock : monsterType->info.attackSpells) {
		lua_createtable(L, 0, 8);

		setField(L, "chance", spellBlock.chance);
		setField(L, "isCombatSpell", spellBlock.combatSpell ? 1 : 0);
		setField(L, "isMelee", spellBlock.isMelee ? 1 : 0);
		setField(L, "minCombatValue", spellBlock.minCombatValue);
		setField(L, "maxCombatValue", spellBlock.maxCombatValue);
		setField(L, "range", spellBlock.range);
		setField(L, "speed", spellBlock.speed);
		tfs::lua::pushUserdata(L, static_cast<CombatSpell*>(spellBlock.spell));
		lua_setfield(L, -2, "spell");

		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeAddAttack()
{
	// monsterType:addAttack(monsterspell)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 2);
		if (spell) {
			spellBlock_t sb;
			if (g_monsters.deserializeSpell(spell, sb, monsterType->name)) {
				monsterType->info.attackSpells.push_back(std::move(sb));
			} else {
				std::cout << monsterType->name << '\n';
				std::cout << "[Warning - Monsters::loadMonster] Cant load spell. " << spell->name << '\n';
			}
		} else {
			context.push_nil();
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeGetDefenseList()
{
	// monsterType:getDefenseList()
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (!monsterType) {
		context.push_nil();
		return 1;
	}

	lua_createtable(L, monsterType->info.defenseSpells.size(), 0);

	int index = 0;
	for (const auto& spellBlock : monsterType->info.defenseSpells) {
		lua_createtable(L, 0, 8);

		setField(L, "chance", spellBlock.chance);
		setField(L, "isCombatSpell", spellBlock.combatSpell ? 1 : 0);
		setField(L, "isMelee", spellBlock.isMelee ? 1 : 0);
		setField(L, "minCombatValue", spellBlock.minCombatValue);
		setField(L, "maxCombatValue", spellBlock.maxCombatValue);
		setField(L, "range", spellBlock.range);
		setField(L, "speed", spellBlock.speed);
		tfs::lua::pushUserdata(L, static_cast<CombatSpell*>(spellBlock.spell));
		lua_setfield(L, -2, "spell");

		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeAddDefense()
{
	// monsterType:addDefense(monsterspell)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 2);
		if (spell) {
			spellBlock_t sb;
			if (g_monsters.deserializeSpell(spell, sb, monsterType->name)) {
				monsterType->info.defenseSpells.push_back(std::move(sb));
			} else {
				std::cout << monsterType->name << '\n';
				std::cout << "[Warning - Monsters::loadMonster] Cant load spell. " << spell->name << '\n';
			}
		} else {
			context.push_nil();
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeGetElementList()
{
	// monsterType:getElementList()
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (!monsterType) {
		context.push_nil();
		return 1;
	}

	lua_createtable(L, monsterType->info.elementMap.size(), 0);
	for (const auto& elementEntry : monsterType->info.elementMap) {
		context.push_number(elementEntry.second);
		lua_rawseti(L, -2, elementEntry.first);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeAddElement()
{
	// monsterType:addElement(type, percent)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		CombatType_t element = tfs::lua::getNumber<CombatType_t>(L, 2);
		monsterType->info.elementMap[element] = tfs::lua::getNumber<int32_t>(L, 3);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeGetVoices()
{
	// monsterType:getVoices()
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (!monsterType) {
		context.push_nil();
		return 1;
	}

	int index = 0;
	lua_createtable(L, monsterType->info.voiceVector.size(), 0);
	for (const auto& voiceBlock : monsterType->info.voiceVector) {
		lua_createtable(L, 0, 2);
		setField(L, "text", voiceBlock.text);
		setField(L, "yellText", voiceBlock.yellText);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeAddVoice()
{
	// monsterType:addVoice(sentence, interval, chance, yell)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		voiceBlock_t voice;
		voice.text = tfs::lua::getString(L, 2);
		monsterType->info.yellSpeedTicks = context.get_number<uint32_t>(3);
		monsterType->info.yellChance = context.get_number<uint32_t>(4);
		voice.yellText = tfs::lua::getBoolean(L, 5);
		monsterType->info.voiceVector.push_back(voice);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeGetLoot()
{
	// monsterType:getLoot()
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (!monsterType) {
		context.push_nil();
		return 1;
	}

	pushLoot(L, monsterType->info.lootItems);
	return 1;
}

int LuaScriptInterface::luaMonsterTypeAddLoot()
{
	// monsterType:addLoot(loot)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		Loot* loot = tfs::lua::getUserdata<Loot>(L, 2);
		if (loot) {
			monsterType->loadLoot(monsterType, loot->lootBlock);
			context.push_boolean(true);
		} else {
			context.push_nil();
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeGetCreatureEvents()
{
	// monsterType:getCreatureEvents()
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (!monsterType) {
		context.push_nil();
		return 1;
	}

	int index = 0;
	lua_createtable(L, monsterType->info.scripts.size(), 0);
	for (const std::string& creatureEvent : monsterType->info.scripts) {
		tfs::lua::pushString(L, creatureEvent);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeRegisterEvent()
{
	// monsterType:registerEvent(name)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		monsterType->info.scripts.push_back(tfs::lua::getString(L, 2));
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeEventOnCallback()
{
	// monsterType:onThink(callback)
	// monsterType:onAppear(callback)
	// monsterType:onDisappear(callback)
	// monsterType:onMove(callback)
	// monsterType:onSay(callback)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (monsterType->loadCallback(&g_scripts->getScriptInterface())) {
			context.push_boolean(true);
			return 1;
		}
		context.push_boolean(false);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeEventType()
{
	// monstertype:eventType(event)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		monsterType->info.eventType = tfs::lua::getNumber<MonstersEvent_t>(L, 2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeGetSummonList()
{
	// monsterType:getSummonList()
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (!monsterType) {
		context.push_nil();
		return 1;
	}

	int index = 0;
	lua_createtable(L, monsterType->info.summons.size(), 0);
	for (const auto& summonBlock : monsterType->info.summons) {
		lua_createtable(L, 0, 6);
		setField(L, "name", summonBlock.name);
		setField(L, "speed", summonBlock.speed);
		setField(L, "chance", summonBlock.chance);
		setField(L, "max", summonBlock.max);
		setField(L, "effect", summonBlock.effect);
		setField(L, "masterEffect", summonBlock.masterEffect);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeAddSummon()
{
	// monsterType:addSummon(name, interval, chance[, max = -1[, effect = CONST_ME_TELEPORT[, masterEffect =
	// CONST_ME_NONE]]])
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		summonBlock_t summon;
		summon.name = tfs::lua::getString(L, 2);
		summon.speed = tfs::lua::getNumber<int32_t>(L, 3);
		summon.chance = tfs::lua::getNumber<int32_t>(L, 4);
		summon.max = tfs::lua::getNumber<int32_t>(L, 5, -1);
		summon.effect = tfs::lua::getNumber<MagicEffectClasses>(L, 6, CONST_ME_TELEPORT);
		summon.masterEffect = tfs::lua::getNumber<MagicEffectClasses>(L, 7, CONST_ME_NONE);
		monsterType->info.summons.push_back(summon);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeMaxSummons()
{
	// get: monsterType:maxSummons() set: monsterType:maxSummons(ammount)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_number(monsterType->info.maxSummons);
		} else {
			monsterType->info.maxSummons = context.get_number<uint32_t>(2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeArmor()
{
	// get: monsterType:armor() set: monsterType:armor(armor)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_number(monsterType->info.armor);
		} else {
			monsterType->info.armor = tfs::lua::getNumber<int32_t>(L, 2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeDefense()
{
	// get: monsterType:defense() set: monsterType:defense(defense)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_number(monsterType->info.defense);
		} else {
			monsterType->info.defense = tfs::lua::getNumber<int32_t>(L, 2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeOutfit()
{
	// get: monsterType:outfit() set: monsterType:outfit(outfit)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushOutfit(L, monsterType->info.outfit);
		} else {
			monsterType->info.outfit = getOutfit(L, 2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeRace()
{
	// get: monsterType:race() set: monsterType:race(race)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	std::string race = tfs::lua::getString(L, 2);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_number(monsterType->info.race);
		} else {
			if (race == "venom") {
				monsterType->info.race = RACE_VENOM;
			} else if (race == "blood") {
				monsterType->info.race = RACE_BLOOD;
			} else if (race == "undead") {
				monsterType->info.race = RACE_UNDEAD;
			} else if (race == "fire") {
				monsterType->info.race = RACE_FIRE;
			} else if (race == "energy") {
				monsterType->info.race = RACE_ENERGY;
			} else {
				std::cout << "[Warning - Monsters::loadMonster] Unknown race type " << race << ".\n";
				context.push_nil();
				return 1;
			}
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeCorpseId()
{
	// get: monsterType:corpseId() set: monsterType:corpseId(id)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_number(monsterType->info.lookcorpse);
		} else {
			monsterType->info.lookcorpse = context.get_number<uint16_t>(2);
			lua_pushboolean(L, true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeManaCost()
{
	// get: monsterType:manaCost() set: monsterType:manaCost(mana)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_number(monsterType->info.manaCost);
		} else {
			monsterType->info.manaCost = context.get_number<uint32_t>(2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeBaseSpeed()
{
	// get: monsterType:baseSpeed() set: monsterType:baseSpeed(speed)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_number(monsterType->info.baseSpeed);
		} else {
			monsterType->info.baseSpeed = context.get_number<uint32_t>(2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeLight()
{
	// get: monsterType:light() set: monsterType:light(color, level)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (!monsterType) {
		context.push_nil();
		return 1;
	}
	if (lua_gettop(L) == 1) {
		context.push_number(monsterType->info.light.level);
		context.push_number(monsterType->info.light.color);
		return 2;
	} else {
		monsterType->info.light.color = context.get_number<uint8_t>(2);
		monsterType->info.light.level = context.get_number<uint8_t>(3);
		context.push_boolean(true);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeStaticAttackChance()
{
	// get: monsterType:staticAttackChance() set: monsterType:staticAttackChance(chance)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_number(monsterType->info.staticAttackChance);
		} else {
			monsterType->info.staticAttackChance = context.get_number<uint32_t>(2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeTargetDistance()
{
	// get: monsterType:targetDistance() set: monsterType:targetDistance(distance)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_number(monsterType->info.targetDistance);
		} else {
			monsterType->info.targetDistance = tfs::lua::getNumber<int32_t>(L, 2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeYellChance()
{
	// get: monsterType:yellChance() set: monsterType:yellChance(chance)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_number(monsterType->info.yellChance);
		} else {
			monsterType->info.yellChance = context.get_number<uint32_t>(2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeYellSpeedTicks()
{
	// get: monsterType:yellSpeedTicks() set: monsterType:yellSpeedTicks(rate)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_number(monsterType->info.yellSpeedTicks);
		} else {
			monsterType->info.yellSpeedTicks = context.get_number<uint32_t>(2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeChangeTargetChance()
{
	// get: monsterType:changeTargetChance() set: monsterType:changeTargetChance(chance)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_number(monsterType->info.changeTargetChance);
		} else {
			monsterType->info.changeTargetChance = tfs::lua::getNumber<int32_t>(L, 2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeChangeTargetSpeed()
{
	// get: monsterType:changeTargetSpeed() set: monsterType:changeTargetSpeed(speed)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			context.push_number(monsterType->info.changeTargetSpeed);
		} else {
			monsterType->info.changeTargetSpeed = context.get_number<uint32_t>(2);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeBestiaryInfo()
{
	// get: monsterType:bestiaryInfo() set: monsterType:bestiaryInfo(info)
	MonsterType* monsterType = tfs::lua::getUserdata<MonsterType>(L, 1);
	if (!monsterType) {
		context.push_nil();
		return 1;
	}

	if (lua_gettop(L) == 1) {
		const auto& info = monsterType->bestiaryInfo;
		lua_createtable(L, 0, 9);

		setField(L, "class", info.className);
		setField(L, "raceId", info.raceId);
		setField(L, "prowess", info.prowess);
		setField(L, "expertise", info.expertise);
		setField(L, "mastery", info.mastery);
		setField(L, "charmPoints", info.charmPoints);
		setField(L, "difficulty", info.difficulty);
		setField(L, "occurrence", info.occurrence);
		setField(L, "locations", info.locations);
		return 1;
	}

	if (lua_istable(L, 2)) {
		BestiaryInfo info{
		    .className = tfs::lua::getFieldString(L, 2, "class"),
		    .raceId = tfs::lua::getField<uint32_t>(L, 2, "raceId"),
		    .prowess = tfs::lua::getField<uint32_t>(L, 2, "prowess"),
		    .expertise = tfs::lua::getField<uint32_t>(L, 2, "expertise"),
		    .mastery = tfs::lua::getField<uint32_t>(L, 2, "mastery"),
		    .charmPoints = tfs::lua::getField<uint32_t>(L, 2, "charmPoints"),
		    .difficulty = tfs::lua::getField<uint32_t>(L, 2, "difficulty"),
		    .occurrence = tfs::lua::getField<uint32_t>(L, 2, "occurrence"),
		    .locations = tfs::lua::getFieldString(L, 2, "locations"),
		};
		lua_pop(L, 9);

		if (g_monsters.isValidBestiaryInfo(info)) {
			monsterType->bestiaryInfo = std::move(info);
			context.push_boolean(g_monsters.addBestiaryMonsterType(monsterType));
		} else {
			context.push_boolean(false);
		}
		return 1;
	}

	std::cout << "[Warning - LuaScriptInterface::luaMonsterTypeBestiaryInfo] bestiaryInfo must be a table.\n";
	context.push_nil();
	return 1;
}

// Loot
int LuaScriptInterface::luaCreateLoot()
{
	// Loot() will create a new loot item
	tfs::lua::pushUserdata(L, new Loot);
	tfs::lua::setMetatable(L, -1, "Loot");
	return 1;
}

int LuaScriptInterface::luaDeleteLoot()
{
	// loot:delete() loot:__gc()
	Loot** lootPtr = tfs::lua::getRawUserdata<Loot>(L, 1);
	if (lootPtr && *lootPtr) {
		delete *lootPtr;
		*lootPtr = nullptr;
	}
	return 0;
}

int LuaScriptInterface::luaLootSetId()
{
	// loot:setId(id or name)
	Loot* loot = tfs::lua::getUserdata<Loot>(L, 1);
	if (loot) {
		if (isNumber(L, 2)) {
			loot->lootBlock.id = context.get_number<uint16_t>(2);
		} else {
			auto name = tfs::lua::getString(L, 2);
			auto ids = Item::items.nameToItems.equal_range(boost::algorithm::to_lower_copy(name));

			if (ids.first == Item::items.nameToItems.cend()) {
				std::cout << "[Warning - Loot:setId] Unknown loot item \"" << name << "\".\n";
				context.push_boolean(false);
				return 1;
			}

			if (std::next(ids.first) != ids.second) {
				std::cout << "[Warning - Loot:setId] Non-unique loot item \"" << name << "\".\n";
				context.push_boolean(false);
				return 1;
			}

			loot->lootBlock.id = ids.first->second;
		}
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaLootSetSubType()
{
	// loot:setSubType(type)
	Loot* loot = tfs::lua::getUserdata<Loot>(L, 1);
	if (loot) {
		loot->lootBlock.subType = context.get_number<uint16_t>(2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaLootSetChance()
{
	// loot:setChance(chance)
	Loot* loot = tfs::lua::getUserdata<Loot>(L, 1);
	if (loot) {
		loot->lootBlock.chance = context.get_number<uint32_t>(2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaLootSetMaxCount()
{
	// loot:setMaxCount(max)
	Loot* loot = tfs::lua::getUserdata<Loot>(L, 1);
	if (loot) {
		loot->lootBlock.countmax = context.get_number<uint32_t>(2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaLootSetActionId()
{
	// loot:setActionId(actionid)
	Loot* loot = tfs::lua::getUserdata<Loot>(L, 1);
	if (loot) {
		loot->lootBlock.actionId = context.get_number<uint32_t>(2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaLootSetDescription()
{
	// loot:setDescription(desc)
	Loot* loot = tfs::lua::getUserdata<Loot>(L, 1);
	if (loot) {
		loot->lootBlock.text = tfs::lua::getString(L, 2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaLootAddChildLoot()
{
	// loot:addChildLoot(loot)
	Loot* loot = tfs::lua::getUserdata<Loot>(L, 1);
	if (loot) {
		Loot* childLoot = tfs::lua::getUserdata<Loot>(L, 2);
		if (childLoot) {
			loot->lootBlock.childLoot.push_back(childLoot->lootBlock);
			context.push_boolean(true);
		} else {
			context.push_boolean(false);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

// MonsterSpell
int LuaScriptInterface::luaCreateMonsterSpell()
{
	// MonsterSpell() will create a new Monster Spell
	tfs::lua::pushUserdata(L, new MonsterSpell);
	tfs::lua::setMetatable(L, -1, "MonsterSpell");
	return 1;
}

int LuaScriptInterface::luaDeleteMonsterSpell()
{
	// monsterSpell:delete() monsterSpell:__gc()
	MonsterSpell** monsterSpellPtr = tfs::lua::getRawUserdata<MonsterSpell>(L, 1);
	if (monsterSpellPtr && *monsterSpellPtr) {
		delete *monsterSpellPtr;
		*monsterSpellPtr = nullptr;
	}
	return 0;
}

int LuaScriptInterface::luaMonsterSpellSetType()
{
	// monsterSpell:setType(type)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->name = tfs::lua::getString(L, 2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetScriptName()
{
	// monsterSpell:setScriptName(name)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->scriptName = tfs::lua::getString(L, 2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetChance()
{
	// monsterSpell:setChance(chance)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->chance = context.get_number<uint8_t>(2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetInterval()
{
	// monsterSpell:setInterval(interval)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->interval = context.get_number<uint16_t>(2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetRange()
{
	// monsterSpell:setRange(range)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->range = context.get_number<uint8_t>(2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetCombatValue()
{
	// monsterSpell:setCombatValue(min, max)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->minCombatValue = tfs::lua::getNumber<int32_t>(L, 2);
		spell->maxCombatValue = tfs::lua::getNumber<int32_t>(L, 3);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetCombatType()
{
	// monsterSpell:setCombatType(combatType_t)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->combatType = tfs::lua::getNumber<CombatType_t>(L, 2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetAttackValue()
{
	// monsterSpell:setAttackValue(attack, skill)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->attack = tfs::lua::getNumber<int32_t>(L, 2);
		spell->skill = tfs::lua::getNumber<int32_t>(L, 3);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetNeedTarget()
{
	// monsterSpell:setNeedTarget(bool)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->needTarget = tfs::lua::getBoolean(L, 2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetNeedDirection()
{
	// monsterSpell:setNeedDirection(bool)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->needDirection = tfs::lua::getBoolean(L, 2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetCombatLength()
{
	// monsterSpell:setCombatLength(length)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->length = tfs::lua::getNumber<int32_t>(L, 2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetCombatSpread()
{
	// monsterSpell:setCombatSpread(spread)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->spread = tfs::lua::getNumber<int32_t>(L, 2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetCombatRadius()
{
	// monsterSpell:setCombatRadius(radius)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->radius = tfs::lua::getNumber<int32_t>(L, 2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetCombatRing()
{
	// monsterSpell:setCombatRing(ring)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->ring = tfs::lua::getNumber<int32_t>(L, 2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetConditionType()
{
	// monsterSpell:setConditionType(type)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->conditionType = tfs::lua::getNumber<ConditionType_t>(L, 2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetConditionDamage()
{
	// monsterSpell:setConditionDamage(min, max, start)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->conditionMinDamage = tfs::lua::getNumber<int32_t>(L, 2);
		spell->conditionMaxDamage = tfs::lua::getNumber<int32_t>(L, 3);
		spell->conditionStartDamage = tfs::lua::getNumber<int32_t>(L, 4);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetConditionSpeedChange()
{
	// monsterSpell:setConditionSpeedChange(minSpeed[, maxSpeed])
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->minSpeedChange = tfs::lua::getNumber<int32_t>(L, 2);
		spell->maxSpeedChange = tfs::lua::getNumber<int32_t>(L, 3, 0);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetConditionDuration()
{
	// monsterSpell:setConditionDuration(duration)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->duration = tfs::lua::getNumber<int32_t>(L, 2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetConditionDrunkenness()
{
	// monsterSpell:setConditionDrunkenness(drunkenness)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->drunkenness = context.get_number<uint8_t>(2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetConditionTickInterval()
{
	// monsterSpell:setConditionTickInterval(interval)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->tickInterval = tfs::lua::getNumber<int32_t>(L, 2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetCombatShootEffect()
{
	// monsterSpell:setCombatShootEffect(effect)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->shoot = tfs::lua::getNumber<ShootType_t>(L, 2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetCombatEffect()
{
	// monsterSpell:setCombatEffect(effect)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->effect = tfs::lua::getNumber<MagicEffectClasses>(L, 2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetOutfit()
{
	// monsterSpell:setOutfit(outfit)
	MonsterSpell* spell = tfs::lua::getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		if (lua_istable(L, 2)) {
			spell->outfit = getOutfit(L, 2);
		} else if (isNumber(L, 2)) {
			spell->outfit.lookTypeEx = context.get_number<uint16_t>(2);
		} else if (lua_isstring(L, 2)) {
			MonsterType* mType = g_monsters.getMonsterType(tfs::lua::getString(L, 2));
			if (mType) {
				spell->outfit = mType->info.outfit;
			}
		}
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

// Party
int32_t LuaScriptInterface::luaPartyCreate()
{
	// Party(userdata)
	Player* player = tfs::lua::getUserdata<Player>(L, 2);
	if (!player) {
		context.push_nil();
		return 1;
	}

	Party* party = player->getParty();
	if (!party) {
		party = new Party(player);
		g_game.updatePlayerShield(player);
		player->sendCreatureSkull(player);
		tfs::lua::pushUserdata(L, party);
		tfs::lua::setMetatable(L, -1, "Party");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPartyDisband()
{
	// party:disband()
	Party** partyPtr = tfs::lua::getRawUserdata<Party>(L, 1);
	if (partyPtr && *partyPtr) {
		Party*& party = *partyPtr;
		party->disband();
		party = nullptr;
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPartyGetLeader()
{
	// party:getLeader()
	Party* party = tfs::lua::getUserdata<Party>(L, 1);
	if (!party) {
		context.push_nil();
		return 1;
	}

	Player* leader = party->getLeader();
	if (leader) {
		tfs::lua::pushUserdata(L, leader);
		tfs::lua::setMetatable(L, -1, "Player");
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPartySetLeader()
{
	// party:setLeader(player)
	Player* player = tfs::lua::getPlayer(L, 2);
	Party* party = tfs::lua::getUserdata<Party>(L, 1);
	if (party && player) {
		context.push_boolean(party->passPartyLeadership(player, true));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPartyGetMembers()
{
	// party:getMembers()
	Party* party = tfs::lua::getUserdata<Party>(L, 1);
	if (!party) {
		context.push_nil();
		return 1;
	}

	int index = 0;
	lua_createtable(L, party->getMemberCount(), 0);
	for (Player* player : party->getMembers()) {
		tfs::lua::pushUserdata(L, player);
		tfs::lua::setMetatable(L, -1, "Player");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaPartyGetMemberCount()
{
	// party:getMemberCount()
	Party* party = tfs::lua::getUserdata<Party>(L, 1);
	if (party) {
		context.push_number(party->getMemberCount());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPartyGetInvitees()
{
	// party:getInvitees()
	Party* party = tfs::lua::getUserdata<Party>(L, 1);
	if (party) {
		lua_createtable(L, party->getInvitationCount(), 0);

		int index = 0;
		for (Player* player : party->getInvitees()) {
			tfs::lua::pushUserdata(L, player);
			tfs::lua::setMetatable(L, -1, "Player");
			lua_rawseti(L, -2, ++index);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPartyGetInviteeCount()
{
	// party:getInviteeCount()
	Party* party = tfs::lua::getUserdata<Party>(L, 1);
	if (party) {
		context.push_number(party->getInvitationCount());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPartyAddInvite()
{
	// party:addInvite(player)
	Player* player = tfs::lua::getPlayer(L, 2);
	Party* party = tfs::lua::getUserdata<Party>(L, 1);
	if (party && player) {
		context.push_boolean(party->invitePlayer(*player));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPartyRemoveInvite()
{
	// party:removeInvite(player)
	Player* player = tfs::lua::getPlayer(L, 2);
	Party* party = tfs::lua::getUserdata<Party>(L, 1);
	if (party && player) {
		context.push_boolean(party->removeInvite(*player));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPartyAddMember()
{
	// party:addMember(player)
	Player* player = tfs::lua::getPlayer(L, 2);
	Party* party = tfs::lua::getUserdata<Party>(L, 1);
	if (party && player) {
		context.push_boolean(party->joinParty(*player));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPartyRemoveMember()
{
	// party:removeMember(player)
	Player* player = tfs::lua::getPlayer(L, 2);
	Party* party = tfs::lua::getUserdata<Party>(L, 1);
	if (party && player) {
		context.push_boolean(party->leaveParty(player));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPartyIsSharedExperienceActive()
{
	// party:isSharedExperienceActive()
	Party* party = tfs::lua::getUserdata<Party>(L, 1);
	if (party) {
		context.push_boolean(party->isSharedExperienceActive());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPartyIsSharedExperienceEnabled()
{
	// party:isSharedExperienceEnabled()
	Party* party = tfs::lua::getUserdata<Party>(L, 1);
	if (party) {
		context.push_boolean(party->isSharedExperienceEnabled());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPartyIsMemberSharingExp()
{
	// party:isMemberSharingExp(player)
	const Player* player = tfs::lua::getUserdata<const Player>(L, 2);
	Party* party = tfs::lua::getUserdata<Party>(L, 1);
	if (party && player) {
		context.push_boolean(party->getMemberSharedExperienceStatus(player) == SHAREDEXP_OK);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPartyShareExperience()
{
	// party:shareExperience(experience)
	uint64_t experience = tfs::lua::getNumber<uint64_t>(L, 2);
	Party* party = tfs::lua::getUserdata<Party>(L, 1);
	if (party) {
		party->shareExperience(experience);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaPartySetSharedExperience()
{
	// party:setSharedExperience(active)
	bool active = tfs::lua::getBoolean(L, 2);
	Party* party = tfs::lua::getUserdata<Party>(L, 1);
	if (party) {
		context.push_boolean(party->setSharedExperience(party->getLeader(), active));
	} else {
		context.push_nil();
	}
	return 1;
}

// Spells
int LuaScriptInterface::luaSpellCreate()
{
	// Spell(words, name or id) to get an existing spell
	// Spell(type) ex: Spell(SPELL_INSTANT) or Spell(SPELL_RUNE) to create a new spell
	if (lua_gettop(L) == 1) {
		std::cout << "[Error - Spell::luaSpellCreate] There is no parameter set!\n";
		context.push_nil();
		return 1;
	}

	SpellType_t spellType = SPELL_UNDEFINED;

	if (isNumber(L, 2)) {
		int32_t id = tfs::lua::getNumber<int32_t>(L, 2);
		RuneSpell* rune = g_spells->getRuneSpell(id);

		if (rune) {
			tfs::lua::pushUserdata<Spell>(L, rune);
			tfs::lua::setMetatable(L, -1, "Spell");
			return 1;
		}

		spellType = static_cast<SpellType_t>(id);
	} else if (lua_isstring(L, 2)) {
		std::string arg = tfs::lua::getString(L, 2);
		InstantSpell* instant = g_spells->getInstantSpellByName(arg);
		if (instant) {
			tfs::lua::pushUserdata<Spell>(L, instant);
			tfs::lua::setMetatable(L, -1, "Spell");
			return 1;
		}
		instant = g_spells->getInstantSpell(arg);
		if (instant) {
			tfs::lua::pushUserdata<Spell>(L, instant);
			tfs::lua::setMetatable(L, -1, "Spell");
			return 1;
		}
		RuneSpell* rune = g_spells->getRuneSpellByName(arg);
		if (rune) {
			tfs::lua::pushUserdata<Spell>(L, rune);
			tfs::lua::setMetatable(L, -1, "Spell");
			return 1;
		}

		std::string tmp = boost::algorithm::to_lower_copy(arg);
		if (tmp == "instant") {
			spellType = SPELL_INSTANT;
		} else if (tmp == "rune") {
			spellType = SPELL_RUNE;
		}
	}

	if (spellType == SPELL_INSTANT) {
		InstantSpell* spell = new InstantSpell(tfs::lua::getScriptEnv()->getScriptInterface());
		spell->fromLua = true;
		tfs::lua::pushUserdata<Spell>(L, spell);
		tfs::lua::setMetatable(L, -1, "Spell");
		spell->spellType = SPELL_INSTANT;
		return 1;
	} else if (spellType == SPELL_RUNE) {
		RuneSpell* spell = new RuneSpell(tfs::lua::getScriptEnv()->getScriptInterface());
		spell->fromLua = true;
		tfs::lua::pushUserdata<Spell>(L, spell);
		tfs::lua::setMetatable(L, -1, "Spell");
		spell->spellType = SPELL_RUNE;
		return 1;
	}

	context.push_nil();
	return 1;
}

int LuaScriptInterface::luaSpellOnCastSpell()
{
	// spell:onCastSpell(callback)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (spell->spellType == SPELL_INSTANT) {
			InstantSpell* instant = dynamic_cast<InstantSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
			if (!instant->loadCallback()) {
				context.push_boolean(false);
				return 1;
			}
			instant->scripted = true;
			context.push_boolean(true);
		} else if (spell->spellType == SPELL_RUNE) {
			RuneSpell* rune = dynamic_cast<RuneSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
			if (!rune->loadCallback()) {
				context.push_boolean(false);
				return 1;
			}
			rune->scripted = true;
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaSpellRegister()
{
	// spell:register()
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (spell->spellType == SPELL_INSTANT) {
			InstantSpell* instant = dynamic_cast<InstantSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
			if (!instant->isScripted()) {
				context.push_boolean(false);
				return 1;
			}
			context.push_boolean(g_spells->registerInstantLuaEvent(instant));
		} else if (spell->spellType == SPELL_RUNE) {
			RuneSpell* rune = dynamic_cast<RuneSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
			if (rune->getMagicLevel() != 0 || rune->getLevel() != 0) {
				// Change information in the ItemType to get accurate description
				ItemType& iType = Item::items.getItemType(rune->getRuneItemId());
				iType.name = rune->getName();
				iType.runeMagLevel = rune->getMagicLevel();
				iType.runeLevel = rune->getLevel();
				iType.charges = rune->getCharges();
			}
			if (!rune->isScripted()) {
				context.push_boolean(false);
				return 1;
			}
			context.push_boolean(g_spells->registerRuneLuaEvent(rune));
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaSpellName()
{
	// spell:name(name)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			tfs::lua::pushString(L, spell->getName());
		} else {
			spell->setName(tfs::lua::getString(L, 2));
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaSpellId()
{
	// spell:id(id)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			context.push_number(spell->getId());
		} else {
			spell->setId(context.get_number<uint8_t>(2));
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaSpellGroup()
{
	// spell:group(primaryGroup[, secondaryGroup])
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			context.push_number(spell->getGroup());
			context.push_number(spell->getSecondaryGroup());
			return 2;
		} else if (lua_gettop(L) == 2) {
			SpellGroup_t group = tfs::lua::getNumber<SpellGroup_t>(L, 2);
			if (group) {
				spell->setGroup(group);
				context.push_boolean(true);
			} else if (lua_isstring(L, 2)) {
				group = stringToSpellGroup(tfs::lua::getString(L, 2));
				if (group != SPELLGROUP_NONE) {
					spell->setGroup(group);
				} else {
					std::cout << "[Warning - Spell::group] Unknown group: " << tfs::lua::getString(L, 2) << '\n';
					context.push_boolean(false);
					return 1;
				}
				context.push_boolean(true);
			} else {
				std::cout << "[Warning - Spell::group] Unknown group: " << tfs::lua::getString(L, 2) << '\n';
				context.push_boolean(false);
				return 1;
			}
		} else {
			SpellGroup_t primaryGroup = tfs::lua::getNumber<SpellGroup_t>(L, 2);
			SpellGroup_t secondaryGroup = tfs::lua::getNumber<SpellGroup_t>(L, 3);
			if (primaryGroup && secondaryGroup) {
				spell->setGroup(primaryGroup);
				spell->setSecondaryGroup(secondaryGroup);
				context.push_boolean(true);
			} else if (lua_isstring(L, 2) && lua_isstring(L, 3)) {
				primaryGroup = stringToSpellGroup(tfs::lua::getString(L, 2));
				if (primaryGroup != SPELLGROUP_NONE) {
					spell->setGroup(primaryGroup);
				} else {
					std::cout << "[Warning - Spell::group] Unknown primaryGroup: " << tfs::lua::getString(L, 2) << '\n';
					context.push_boolean(false);
					return 1;
				}
				secondaryGroup = stringToSpellGroup(tfs::lua::getString(L, 3));
				if (secondaryGroup != SPELLGROUP_NONE) {
					spell->setSecondaryGroup(secondaryGroup);
				} else {
					std::cout << "[Warning - Spell::group] Unknown secondaryGroup: " << tfs::lua::getString(L, 3)
					          << '\n';
					context.push_boolean(false);
					return 1;
				}
				context.push_boolean(true);
			} else {
				std::cout << "[Warning - Spell::group] Unknown primaryGroup: " << tfs::lua::getString(L, 2)
				          << " or secondaryGroup: " << tfs::lua::getString(L, 3) << '\n';
				context.push_boolean(false);
				return 1;
			}
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaSpellCooldown()
{
	// spell:cooldown(cooldown)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			context.push_number(spell->getCooldown());
		} else {
			spell->setCooldown(context.get_number<uint32_t>(2));
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaSpellGroupCooldown()
{
	// spell:groupCooldown(primaryGroupCd[, secondaryGroupCd])
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			context.push_number(spell->getGroupCooldown());
			context.push_number(spell->getSecondaryCooldown());
			return 2;
		} else if (lua_gettop(L) == 2) {
			spell->setGroupCooldown(context.get_number<uint32_t>(2));
			context.push_boolean(true);
		} else {
			spell->setGroupCooldown(context.get_number<uint32_t>(2));
			spell->setSecondaryCooldown(context.get_number<uint32_t>(3));
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaSpellLevel()
{
	// spell:level(lvl)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			context.push_number(spell->getLevel());
		} else {
			spell->setLevel(context.get_number<uint32_t>(2));
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaSpellMagicLevel()
{
	// spell:magicLevel(lvl)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			context.push_number(spell->getMagicLevel());
		} else {
			spell->setMagicLevel(context.get_number<uint32_t>(2));
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaSpellMana()
{
	// spell:mana(mana)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			context.push_number(spell->getMana());
		} else {
			spell->setMana(context.get_number<uint32_t>(2));
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaSpellManaPercent()
{
	// spell:manaPercent(percent)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			context.push_number(spell->getManaPercent());
		} else {
			spell->setManaPercent(context.get_number<uint32_t>(2));
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaSpellSoul()
{
	// spell:soul(soul)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			context.push_number(spell->getSoulCost());
		} else {
			spell->setSoulCost(context.get_number<uint32_t>(2));
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaSpellRange()
{
	// spell:range(range)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			context.push_number(spell->getRange());
		} else {
			spell->setRange(tfs::lua::getNumber<int32_t>(L, 2));
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaSpellPremium()
{
	// spell:isPremium(bool)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			context.push_boolean(spell->isPremium());
		} else {
			spell->setPremium(tfs::lua::getBoolean(L, 2));
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaSpellEnabled()
{
	// spell:isEnabled(bool)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			context.push_boolean(spell->isEnabled());
		} else {
			spell->setEnabled(tfs::lua::getBoolean(L, 2));
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaSpellNeedTarget()
{
	// spell:needTarget(bool)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			context.push_boolean(spell->getNeedTarget());
		} else {
			spell->setNeedTarget(tfs::lua::getBoolean(L, 2));
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaSpellNeedWeapon()
{
	// spell:needWeapon(bool)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			context.push_boolean(spell->getNeedWeapon());
		} else {
			spell->setNeedWeapon(tfs::lua::getBoolean(L, 2));
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaSpellNeedLearn()
{
	// spell:needLearn(bool)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			context.push_boolean(spell->getNeedLearn());
		} else {
			spell->setNeedLearn(tfs::lua::getBoolean(L, 2));
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaSpellSelfTarget()
{
	// spell:isSelfTarget(bool)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			context.push_boolean(spell->getSelfTarget());
		} else {
			spell->setSelfTarget(tfs::lua::getBoolean(L, 2));
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaSpellBlocking()
{
	// spell:isBlocking(blockingSolid, blockingCreature)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			context.push_boolean(spell->getBlockingSolid());
			context.push_boolean(spell->getBlockingCreature());
			return 2;
		} else {
			spell->setBlockingSolid(tfs::lua::getBoolean(L, 2));
			spell->setBlockingCreature(tfs::lua::getBoolean(L, 3));
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaSpellAggressive()
{
	// spell:isAggressive(bool)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			context.push_boolean(spell->getAggressive());
		} else {
			spell->setAggressive(tfs::lua::getBoolean(L, 2));
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaSpellPzLock()
{
	// spell:isPzLock(bool)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			context.push_boolean(spell->getPzLock());
		} else {
			spell->setPzLock(tfs::lua::getBoolean(L, 2));
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaSpellVocation()
{
	// spell:vocation(vocation)
	Spell* spell = tfs::lua::getUserdata<Spell>(L, 1);
	if (!spell) {
		context.push_nil();
		return 1;
	}

	if (lua_gettop(L) == 1) {
		lua_createtable(L, 0, 0);
		int i = 0;
		for (auto& vocation : spell->getVocationSpellMap()) {
			std::string name = g_vocations.getVocation(vocation.first)->getVocName();
			tfs::lua::pushString(L, name);
			lua_rawseti(L, -2, ++i);
		}
	} else {
		int parameters = lua_gettop(L) - 1; // - 1 because self is a parameter aswell, which we want to skip ofc
		for (int i = 0; i < parameters; ++i) {
			std::string vocStr = tfs::lua::getString(L, 2 + i);
			auto vocList = explodeString(vocStr, ";");
			spell->addVocationSpellMap(vocList[0], vocList.size() > 1 ? booleanString(vocList[1]) : false);
		}
		context.push_boolean(true);
	}
	return 1;
}

// only for InstantSpells
int LuaScriptInterface::luaSpellWords()
{
	// spell:words(words[, separator = ""])
	InstantSpell* spell = dynamic_cast<InstantSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_INSTANT, it means that this actually is no InstantSpell, so we return nil
		if (spell->spellType != SPELL_INSTANT) {
			context.push_nil();
			return 1;
		}

		if (lua_gettop(L) == 1) {
			tfs::lua::pushString(L, spell->getWords());
			tfs::lua::pushString(L, spell->getSeparator());
			return 2;
		} else {
			std::string sep = "";
			if (lua_gettop(L) == 3) {
				sep = tfs::lua::getString(L, 3);
			}
			spell->setWords(tfs::lua::getString(L, 2));
			spell->setSeparator(sep);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

// only for InstantSpells
int LuaScriptInterface::luaSpellNeedDirection()
{
	// spell:needDirection(bool)
	InstantSpell* spell = dynamic_cast<InstantSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_INSTANT, it means that this actually is no InstantSpell, so we return nil
		if (spell->spellType != SPELL_INSTANT) {
			context.push_nil();
			return 1;
		}

		if (lua_gettop(L) == 1) {
			context.push_boolean(spell->getNeedDirection());
		} else {
			spell->setNeedDirection(tfs::lua::getBoolean(L, 2));
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

// only for InstantSpells
int LuaScriptInterface::luaSpellHasParams()
{
	// spell:hasParams(bool)
	InstantSpell* spell = dynamic_cast<InstantSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_INSTANT, it means that this actually is no InstantSpell, so we return nil
		if (spell->spellType != SPELL_INSTANT) {
			context.push_nil();
			return 1;
		}

		if (lua_gettop(L) == 1) {
			context.push_boolean(spell->getHasParam());
		} else {
			spell->setHasParam(tfs::lua::getBoolean(L, 2));
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

// only for InstantSpells
int LuaScriptInterface::luaSpellHasPlayerNameParam()
{
	// spell:hasPlayerNameParam(bool)
	InstantSpell* spell = dynamic_cast<InstantSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_INSTANT, it means that this actually is no InstantSpell, so we return nil
		if (spell->spellType != SPELL_INSTANT) {
			context.push_nil();
			return 1;
		}

		if (lua_gettop(L) == 1) {
			context.push_boolean(spell->getHasPlayerNameParam());
		} else {
			spell->setHasPlayerNameParam(tfs::lua::getBoolean(L, 2));
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

// only for InstantSpells
int LuaScriptInterface::luaSpellNeedCasterTargetOrDirection()
{
	// spell:needCasterTargetOrDirection(bool)
	InstantSpell* spell = dynamic_cast<InstantSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_INSTANT, it means that this actually is no InstantSpell, so we return nil
		if (spell->spellType != SPELL_INSTANT) {
			context.push_nil();
			return 1;
		}

		if (lua_gettop(L) == 1) {
			context.push_boolean(spell->getNeedCasterTargetOrDirection());
		} else {
			spell->setNeedCasterTargetOrDirection(tfs::lua::getBoolean(L, 2));
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

// only for InstantSpells
int LuaScriptInterface::luaSpellIsBlockingWalls()
{
	// spell:blockWalls(bool)
	InstantSpell* spell = dynamic_cast<InstantSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_INSTANT, it means that this actually is no InstantSpell, so we return nil
		if (spell->spellType != SPELL_INSTANT) {
			context.push_nil();
			return 1;
		}

		if (lua_gettop(L) == 1) {
			context.push_boolean(spell->getBlockWalls());
		} else {
			spell->setBlockWalls(tfs::lua::getBoolean(L, 2));
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

// only for RuneSpells
int LuaScriptInterface::luaSpellRuneLevel()
{
	// spell:runeLevel(level)
	RuneSpell* spell = dynamic_cast<RuneSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
	int32_t level = tfs::lua::getNumber<int32_t>(L, 2);
	if (spell) {
		// if spell != SPELL_RUNE, it means that this actually is no RuneSpell, so we return nil
		if (spell->spellType != SPELL_RUNE) {
			context.push_nil();
			return 1;
		}

		if (lua_gettop(L) == 1) {
			context.push_number(spell->getLevel());
		} else {
			spell->setLevel(level);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

// only for RuneSpells
int LuaScriptInterface::luaSpellRuneMagicLevel()
{
	// spell:runeMagicLevel(magLevel)
	RuneSpell* spell = dynamic_cast<RuneSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
	int32_t magLevel = tfs::lua::getNumber<int32_t>(L, 2);
	if (spell) {
		// if spell != SPELL_RUNE, it means that this actually is no RuneSpell, so we return nil
		if (spell->spellType != SPELL_RUNE) {
			context.push_nil();
			return 1;
		}

		if (lua_gettop(L) == 1) {
			context.push_number(spell->getMagicLevel());
		} else {
			spell->setMagicLevel(magLevel);
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

// only for RuneSpells
int LuaScriptInterface::luaSpellRuneId()
{
	// spell:runeId(id)
	RuneSpell* rune = dynamic_cast<RuneSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
	if (rune) {
		// if spell != SPELL_RUNE, it means that this actually is no RuneSpell, so we return nil
		if (rune->spellType != SPELL_RUNE) {
			context.push_nil();
			return 1;
		}

		if (lua_gettop(L) == 1) {
			context.push_number(rune->getRuneItemId());
		} else {
			rune->setRuneItemId(context.get_number<uint16_t>(2));
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

// only for RuneSpells
int LuaScriptInterface::luaSpellCharges()
{
	// spell:charges(charges)
	RuneSpell* spell = dynamic_cast<RuneSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_RUNE, it means that this actually is no RuneSpell, so we return nil
		if (spell->spellType != SPELL_RUNE) {
			context.push_nil();
			return 1;
		}

		if (lua_gettop(L) == 1) {
			context.push_number(spell->getCharges());
		} else {
			spell->setCharges(context.get_number<uint32_t>(2));
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

// only for RuneSpells
int LuaScriptInterface::luaSpellAllowFarUse()
{
	// spell:allowFarUse(bool)
	RuneSpell* spell = dynamic_cast<RuneSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_RUNE, it means that this actually is no RuneSpell, so we return nil
		if (spell->spellType != SPELL_RUNE) {
			context.push_nil();
			return 1;
		}

		if (lua_gettop(L) == 1) {
			context.push_boolean(spell->getAllowFarUse());
		} else {
			spell->setAllowFarUse(tfs::lua::getBoolean(L, 2));
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

// only for RuneSpells
int LuaScriptInterface::luaSpellBlockWalls()
{
	// spell:blockWalls(bool)
	RuneSpell* spell = dynamic_cast<RuneSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_RUNE, it means that this actually is no RuneSpell, so we return nil
		if (spell->spellType != SPELL_RUNE) {
			context.push_nil();
			return 1;
		}

		if (lua_gettop(L) == 1) {
			context.push_boolean(spell->getCheckLineOfSight());
		} else {
			spell->setCheckLineOfSight(tfs::lua::getBoolean(L, 2));
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

// only for RuneSpells
int LuaScriptInterface::luaSpellCheckFloor()
{
	// spell:checkFloor(bool)
	RuneSpell* spell = dynamic_cast<RuneSpell*>(tfs::lua::getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_RUNE, it means that this actually is no RuneSpell, so we return nil
		if (spell->spellType != SPELL_RUNE) {
			context.push_nil();
			return 1;
		}

		if (lua_gettop(L) == 1) {
			context.push_boolean(spell->getCheckFloor());
		} else {
			spell->setCheckFloor(tfs::lua::getBoolean(L, 2));
			context.push_boolean(true);
		}
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreateAction()
{
	// Action()
	if (tfs::lua::getScriptEnv()->getScriptInterface() != &g_scripts->getScriptInterface()) {
		reportErrorFunc(L, "Actions can only be registered in the Scripts interface.");
		context.push_nil();
		return 1;
	}

	Action* action = new Action(tfs::lua::getScriptEnv()->getScriptInterface());
	action->fromLua = true;
	tfs::lua::pushUserdata(L, action);
	tfs::lua::setMetatable(L, -1, "Action");
	return 1;
}

int LuaScriptInterface::luaActionOnUse()
{
	// action:onUse(callback)
	Action* action = tfs::lua::getUserdata<Action>(L, 1);
	if (action) {
		if (!action->loadCallback()) {
			context.push_boolean(false);
			return 1;
		}
		action->scripted = true;
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaActionRegister()
{
	// action:register()
	Action* action = tfs::lua::getUserdata<Action>(L, 1);
	if (action) {
		if (!action->isScripted()) {
			context.push_boolean(false);
			return 1;
		}
		context.push_boolean(g_actions->registerLuaEvent(action));
		g_actions->clearItemIdRange(action);
		g_actions->clearUniqueIdRange(action);
		g_actions->clearActionIdRange(action);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaActionItemId()
{
	// action:id(ids)
	Action* action = tfs::lua::getUserdata<Action>(L, 1);
	if (action) {
		int parameters = lua_gettop(L) - 1; // - 1 because self is a parameter aswell, which we want to skip ofc
		if (parameters > 1) {
			for (int i = 0; i < parameters; ++i) {
				g_actions->addItemId(action, context.get_number<uint16_t>(2 + i));
			}
		} else {
			g_actions->addItemId(action, context.get_number<uint16_t>(2));
		}
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaActionActionId()
{
	// action:aid(aids)
	Action* action = tfs::lua::getUserdata<Action>(L, 1);
	if (action) {
		int parameters = lua_gettop(L) - 1; // - 1 because self is a parameter aswell, which we want to skip ofc
		if (parameters > 1) {
			for (int i = 0; i < parameters; ++i) {
				g_actions->addActionId(action, context.get_number<uint16_t>(2 + i));
			}
		} else {
			g_actions->addActionId(action, context.get_number<uint16_t>(2));
		}
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaActionUniqueId()
{
	// action:uid(uids)
	Action* action = tfs::lua::getUserdata<Action>(L, 1);
	if (action) {
		int parameters = lua_gettop(L) - 1; // - 1 because self is a parameter aswell, which we want to skip ofc
		if (parameters > 1) {
			for (int i = 0; i < parameters; ++i) {
				g_actions->addUniqueId(action, context.get_number<uint16_t>(2 + i));
			}
		} else {
			g_actions->addUniqueId(action, context.get_number<uint16_t>(2));
		}
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaActionAllowFarUse()
{
	// action:allowFarUse(bool)
	Action* action = tfs::lua::getUserdata<Action>(L, 1);
	if (action) {
		action->setAllowFarUse(tfs::lua::getBoolean(L, 2));
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaActionBlockWalls()
{
	// action:blockWalls(bool)
	Action* action = tfs::lua::getUserdata<Action>(L, 1);
	if (action) {
		action->setCheckLineOfSight(tfs::lua::getBoolean(L, 2));
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaActionCheckFloor()
{
	// action:checkFloor(bool)
	Action* action = tfs::lua::getUserdata<Action>(L, 1);
	if (action) {
		action->setCheckFloor(tfs::lua::getBoolean(L, 2));
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreateTalkaction()
{
	// TalkAction(words)
	if (tfs::lua::getScriptEnv()->getScriptInterface() != &g_scripts->getScriptInterface()) {
		reportErrorFunc(L, "TalkActions can only be registered in the Scripts interface.");
		context.push_nil();
		return 1;
	}

	TalkAction* talkAction = new TalkAction(tfs::lua::getScriptEnv()->getScriptInterface());
	for (int i = 2; i <= lua_gettop(L); i++) {
		talkAction->setWords(tfs::lua::getString(L, i));
	}
	talkAction->fromLua = true;
	tfs::lua::pushUserdata(L, talkAction);
	tfs::lua::setMetatable(L, -1, "TalkAction");
	return 1;
}

int LuaScriptInterface::luaTalkactionOnSay()
{
	// talkAction:onSay(callback)
	TalkAction* talk = tfs::lua::getUserdata<TalkAction>(L, 1);
	if (talk) {
		if (!talk->loadCallback()) {
			context.push_boolean(false);
			return 1;
		}
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaTalkactionRegister()
{
	// talkAction:register()
	TalkAction* talk = tfs::lua::getUserdata<TalkAction>(L, 1);
	if (talk) {
		if (!talk->isScripted()) {
			context.push_boolean(false);
			return 1;
		}
		context.push_boolean(g_talkActions->registerLuaEvent(talk));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaTalkactionSeparator()
{
	// talkAction:separator(sep)
	TalkAction* talk = tfs::lua::getUserdata<TalkAction>(L, 1);
	if (talk) {
		talk->setSeparator(tfs::lua::getString(L, 2));
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaTalkactionAccess()
{
	// talkAction:access(needAccess = false)
	TalkAction* talk = tfs::lua::getUserdata<TalkAction>(L, 1);
	if (talk) {
		talk->setNeedAccess(tfs::lua::getBoolean(L, 2));
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaTalkactionAccountType()
{
	// talkAction:accountType(AccountType_t = ACCOUNT_TYPE_NORMAL)
	TalkAction* talk = tfs::lua::getUserdata<TalkAction>(L, 1);
	if (talk) {
		talk->setRequiredAccountType(tfs::lua::getNumber<AccountType_t>(L, 2));
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreateCreatureEvent()
{
	// CreatureEvent(eventName)
	if (tfs::lua::getScriptEnv()->getScriptInterface() != &g_scripts->getScriptInterface()) {
		reportErrorFunc(L, "CreatureEvents can only be registered in the Scripts interface.");
		context.push_nil();
		return 1;
	}

	CreatureEvent* creatureEvent = new CreatureEvent(tfs::lua::getScriptEnv()->getScriptInterface());
	creatureEvent->setName(tfs::lua::getString(L, 2));
	creatureEvent->fromLua = true;
	tfs::lua::pushUserdata(L, creatureEvent);
	tfs::lua::setMetatable(L, -1, "CreatureEvent");
	return 1;
}

int LuaScriptInterface::luaCreatureEventType()
{
	// creatureevent:type(callback)
	CreatureEvent* creature = tfs::lua::getUserdata<CreatureEvent>(L, 1);
	if (creature) {
		std::string typeName = tfs::lua::getString(L, 2);
		std::string tmpStr = boost::algorithm::to_lower_copy(typeName);
		if (tmpStr == "login") {
			creature->setEventType(CREATURE_EVENT_LOGIN);
		} else if (tmpStr == "logout") {
			creature->setEventType(CREATURE_EVENT_LOGOUT);
		} else if (tmpStr == "think") {
			creature->setEventType(CREATURE_EVENT_THINK);
		} else if (tmpStr == "preparedeath") {
			creature->setEventType(CREATURE_EVENT_PREPAREDEATH);
		} else if (tmpStr == "death") {
			creature->setEventType(CREATURE_EVENT_DEATH);
		} else if (tmpStr == "kill") {
			creature->setEventType(CREATURE_EVENT_KILL);
		} else if (tmpStr == "advance") {
			creature->setEventType(CREATURE_EVENT_ADVANCE);
		} else if (tmpStr == "modalwindow") {
			creature->setEventType(CREATURE_EVENT_MODALWINDOW);
		} else if (tmpStr == "textedit") {
			creature->setEventType(CREATURE_EVENT_TEXTEDIT);
		} else if (tmpStr == "healthchange") {
			creature->setEventType(CREATURE_EVENT_HEALTHCHANGE);
		} else if (tmpStr == "manachange") {
			creature->setEventType(CREATURE_EVENT_MANACHANGE);
		} else if (tmpStr == "extendedopcode") {
			creature->setEventType(CREATURE_EVENT_EXTENDED_OPCODE);
		} else {
			std::cout << "[Error - CreatureEvent::configureLuaEvent] Invalid type for creature event: " << typeName
			          << '\n';
			context.push_boolean(false);
		}
		creature->setLoaded(true);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureEventRegister()
{
	// creatureevent:register()
	CreatureEvent* creature = tfs::lua::getUserdata<CreatureEvent>(L, 1);
	if (creature) {
		if (!creature->isScripted()) {
			context.push_boolean(false);
			return 1;
		}
		context.push_boolean(g_creatureEvents->registerLuaEvent(creature));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreatureEventOnCallback()
{
	// creatureevent:onLogin / logout / etc. (callback)
	CreatureEvent* creature = tfs::lua::getUserdata<CreatureEvent>(L, 1);
	if (creature) {
		if (!creature->loadCallback()) {
			context.push_boolean(false);
			return 1;
		}
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreateMoveEvent()
{
	// MoveEvent()
	if (tfs::lua::getScriptEnv()->getScriptInterface() != &g_scripts->getScriptInterface()) {
		reportErrorFunc(L, "MoveEvents can only be registered in the Scripts interface.");
		context.push_nil();
		return 1;
	}

	MoveEvent* moveevent = new MoveEvent(tfs::lua::getScriptEnv()->getScriptInterface());
	moveevent->fromLua = true;
	tfs::lua::pushUserdata(L, moveevent);
	tfs::lua::setMetatable(L, -1, "MoveEvent");
	return 1;
}

int LuaScriptInterface::luaMoveEventType()
{
	// moveevent:type(callback)
	MoveEvent* moveevent = tfs::lua::getUserdata<MoveEvent>(L, 1);
	if (moveevent) {
		std::string typeName = tfs::lua::getString(L, 2);
		std::string tmpStr = boost::algorithm::to_lower_copy(typeName);
		if (tmpStr == "stepin") {
			moveevent->setEventType(MOVE_EVENT_STEP_IN);
			moveevent->stepFunction = moveevent->StepInField;
		} else if (tmpStr == "stepout") {
			moveevent->setEventType(MOVE_EVENT_STEP_OUT);
			moveevent->stepFunction = moveevent->StepOutField;
		} else if (tmpStr == "equip") {
			moveevent->setEventType(MOVE_EVENT_EQUIP);
			moveevent->equipFunction = moveevent->EquipItem;
		} else if (tmpStr == "deequip") {
			moveevent->setEventType(MOVE_EVENT_DEEQUIP);
			moveevent->equipFunction = moveevent->DeEquipItem;
		} else if (tmpStr == "additem") {
			moveevent->setEventType(MOVE_EVENT_ADD_ITEM);
			moveevent->moveFunction = moveevent->AddItemField;
		} else if (tmpStr == "removeitem") {
			moveevent->setEventType(MOVE_EVENT_REMOVE_ITEM);
			moveevent->moveFunction = moveevent->RemoveItemField;
		} else {
			std::cout << "Error: [MoveEvent::configureMoveEvent] No valid event name " << typeName << '\n';
			context.push_boolean(false);
		}
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMoveEventRegister()
{
	// moveevent:register()
	MoveEvent* moveevent = tfs::lua::getUserdata<MoveEvent>(L, 1);
	if (moveevent) {
		if ((moveevent->getEventType() == MOVE_EVENT_EQUIP || moveevent->getEventType() == MOVE_EVENT_DEEQUIP) &&
		    moveevent->getSlot() == SLOTP_WHEREEVER) {
			uint32_t id = g_moveEvents->getItemIdRange(moveevent).at(0);
			ItemType& it = Item::items.getItemType(id);
			moveevent->setSlot(it.slotPosition);
		}
		if (!moveevent->isScripted()) {
			context.push_boolean(g_moveEvents->registerLuaFunction(moveevent));
			g_moveEvents->clearItemIdRange(moveevent);
			return 1;
		}
		context.push_boolean(g_moveEvents->registerLuaEvent(moveevent));
		g_moveEvents->clearItemIdRange(moveevent);
		g_moveEvents->clearActionIdRange(moveevent);
		g_moveEvents->clearUniqueIdRange(moveevent);
		g_moveEvents->clearPosList(moveevent);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMoveEventOnCallback()
{
	// moveevent:onEquip / deEquip / etc. (callback)
	MoveEvent* moveevent = tfs::lua::getUserdata<MoveEvent>(L, 1);
	if (moveevent) {
		if (!moveevent->loadCallback()) {
			context.push_boolean(false);
			return 1;
		}
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMoveEventSlot()
{
	// moveevent:slot(slot)
	MoveEvent* moveevent = tfs::lua::getUserdata<MoveEvent>(L, 1);
	if (!moveevent) {
		context.push_nil();
		return 1;
	}

	if (moveevent->getEventType() == MOVE_EVENT_EQUIP || moveevent->getEventType() == MOVE_EVENT_DEEQUIP) {
		std::string slotName = boost::algorithm::to_lower_copy(tfs::lua::getString(L, 2));
		if (slotName == "head") {
			moveevent->setSlot(SLOTP_HEAD);
		} else if (slotName == "necklace") {
			moveevent->setSlot(SLOTP_NECKLACE);
		} else if (slotName == "backpack") {
			moveevent->setSlot(SLOTP_BACKPACK);
		} else if (slotName == "armor" || slotName == "body") {
			moveevent->setSlot(SLOTP_ARMOR);
		} else if (slotName == "right-hand") {
			moveevent->setSlot(SLOTP_RIGHT);
		} else if (slotName == "left-hand") {
			moveevent->setSlot(SLOTP_LEFT);
		} else if (slotName == "hand" || slotName == "shield") {
			moveevent->setSlot(SLOTP_RIGHT | SLOTP_LEFT);
		} else if (slotName == "legs") {
			moveevent->setSlot(SLOTP_LEGS);
		} else if (slotName == "feet") {
			moveevent->setSlot(SLOTP_FEET);
		} else if (slotName == "ring") {
			moveevent->setSlot(SLOTP_RING);
		} else if (slotName == "ammo") {
			moveevent->setSlot(SLOTP_AMMO);
		} else {
			std::cout << "[Warning - MoveEvent::configureMoveEvent] Unknown slot type: " << slotName << '\n';
			context.push_boolean(false);
			return 1;
		}
	}

	context.push_boolean(true);
	return 1;
}

int LuaScriptInterface::luaMoveEventLevel()
{
	// moveevent:level(lvl)
	MoveEvent* moveevent = tfs::lua::getUserdata<MoveEvent>(L, 1);
	if (moveevent) {
		moveevent->setRequiredLevel(context.get_number<uint32_t>(2));
		moveevent->setWieldInfo(WIELDINFO_LEVEL);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMoveEventMagLevel()
{
	// moveevent:magicLevel(lvl)
	MoveEvent* moveevent = tfs::lua::getUserdata<MoveEvent>(L, 1);
	if (moveevent) {
		moveevent->setRequiredMagLevel(context.get_number<uint32_t>(2));
		moveevent->setWieldInfo(WIELDINFO_MAGLV);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMoveEventPremium()
{
	// moveevent:premium(bool)
	MoveEvent* moveevent = tfs::lua::getUserdata<MoveEvent>(L, 1);
	if (moveevent) {
		moveevent->setNeedPremium(tfs::lua::getBoolean(L, 2));
		moveevent->setWieldInfo(WIELDINFO_PREMIUM);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMoveEventVocation()
{
	// moveevent:vocation(vocName[, showInDescription = false, lastVoc = false])
	MoveEvent* moveevent = tfs::lua::getUserdata<MoveEvent>(L, 1);
	if (moveevent) {
		moveevent->addVocationEquipSet(tfs::lua::getString(L, 2));
		moveevent->setWieldInfo(WIELDINFO_VOCREQ);
		std::string tmp;
		bool showInDescription = false;
		bool lastVoc = false;
		if (tfs::lua::getBoolean(L, 3)) {
			showInDescription = tfs::lua::getBoolean(L, 3);
		}
		if (tfs::lua::getBoolean(L, 4)) {
			lastVoc = tfs::lua::getBoolean(L, 4);
		}
		if (showInDescription) {
			if (moveevent->getVocationString().empty()) {
				tmp = boost::algorithm::to_lower_copy(tfs::lua::getString(L, 2));
				tmp += "s";
				moveevent->setVocationString(tmp);
			} else {
				tmp = moveevent->getVocationString();
				if (lastVoc) {
					tmp += " and ";
				} else {
					tmp += ", ";
				}
				tmp += boost::algorithm::to_lower_copy(tfs::lua::getString(L, 2));
				tmp += "s";
				moveevent->setVocationString(tmp);
			}
		}
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMoveEventTileItem()
{
	// moveevent:tileItem(bool)
	MoveEvent* moveevent = tfs::lua::getUserdata<MoveEvent>(L, 1);
	if (moveevent) {
		moveevent->setTileItem(tfs::lua::getBoolean(L, 2));
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMoveEventItemId()
{
	// moveevent:id(ids)
	MoveEvent* moveevent = tfs::lua::getUserdata<MoveEvent>(L, 1);
	if (moveevent) {
		int parameters = lua_gettop(L) - 1; // - 1 because self is a parameter aswell, which we want to skip ofc
		if (parameters > 1) {
			for (int i = 0; i < parameters; ++i) {
				g_moveEvents->addItemId(moveevent, context.get_number<uint32_t>(2 + i));
			}
		} else {
			g_moveEvents->addItemId(moveevent, context.get_number<uint32_t>(2));
		}
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMoveEventActionId()
{
	// moveevent:aid(ids)
	MoveEvent* moveevent = tfs::lua::getUserdata<MoveEvent>(L, 1);
	if (moveevent) {
		int parameters = lua_gettop(L) - 1; // - 1 because self is a parameter aswell, which we want to skip ofc
		if (parameters > 1) {
			for (int i = 0; i < parameters; ++i) {
				g_moveEvents->addActionId(moveevent, context.get_number<uint32_t>(2 + i));
			}
		} else {
			g_moveEvents->addActionId(moveevent, context.get_number<uint32_t>(2));
		}
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMoveEventUniqueId()
{
	// moveevent:uid(ids)
	MoveEvent* moveevent = tfs::lua::getUserdata<MoveEvent>(L, 1);
	if (moveevent) {
		int parameters = lua_gettop(L) - 1; // - 1 because self is a parameter aswell, which we want to skip ofc
		if (parameters > 1) {
			for (int i = 0; i < parameters; ++i) {
				g_moveEvents->addUniqueId(moveevent, context.get_number<uint32_t>(2 + i));
			}
		} else {
			g_moveEvents->addUniqueId(moveevent, context.get_number<uint32_t>(2));
		}
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaMoveEventPosition()
{
	// moveevent:position(positions)
	MoveEvent* moveevent = tfs::lua::getUserdata<MoveEvent>(L, 1);
	if (moveevent) {
		int parameters = lua_gettop(L) - 1; // - 1 because self is a parameter aswell, which we want to skip ofc
		if (parameters > 1) {
			for (int i = 0; i < parameters; ++i) {
				g_moveEvents->addPosList(moveevent, tfs::lua::getPosition(L, 2 + i));
			}
		} else {
			g_moveEvents->addPosList(moveevent, tfs::lua::getPosition(L, 2));
		}
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaCreateGlobalEvent()
{
	// GlobalEvent(eventName)
	if (tfs::lua::getScriptEnv()->getScriptInterface() != &g_scripts->getScriptInterface()) {
		reportErrorFunc(L, "GlobalEvents can only be registered in the Scripts interface.");
		context.push_nil();
		return 1;
	}

	GlobalEvent* globalEvent = new GlobalEvent(tfs::lua::getScriptEnv()->getScriptInterface());
	globalEvent->setName(tfs::lua::getString(L, 2));
	globalEvent->setEventType(GLOBALEVENT_NONE);
	globalEvent->fromLua = true;
	tfs::lua::pushUserdata(L, globalEvent);
	tfs::lua::setMetatable(L, -1, "GlobalEvent");
	return 1;
}

int LuaScriptInterface::luaGlobalEventType()
{
	// globalevent:type(callback)
	GlobalEvent* global = tfs::lua::getUserdata<GlobalEvent>(L, 1);
	if (global) {
		std::string typeName = tfs::lua::getString(L, 2);
		std::string tmpStr = boost::algorithm::to_lower_copy(typeName);
		if (tmpStr == "startup") {
			global->setEventType(GLOBALEVENT_STARTUP);
		} else if (tmpStr == "shutdown") {
			global->setEventType(GLOBALEVENT_SHUTDOWN);
		} else if (tmpStr == "record") {
			global->setEventType(GLOBALEVENT_RECORD);
		} else if (tmpStr == "timer") {
			global->setEventType(GLOBALEVENT_TIMER);
		} else if (tmpStr == "save") {
			global->setEventType(GLOBALEVENT_SAVE);
		} else {
			std::cout << "[Error - CreatureEvent::configureLuaEvent] Invalid type for global event: " << typeName
			          << '\n';
			context.push_boolean(false);
		}
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaGlobalEventRegister()
{
	// globalevent:register()
	GlobalEvent* globalevent = tfs::lua::getUserdata<GlobalEvent>(L, 1);
	if (globalevent) {
		if (!globalevent->isScripted()) {
			context.push_boolean(false);
			return 1;
		}

		if (globalevent->getEventType() == GLOBALEVENT_NONE && globalevent->getInterval() == 0) {
			std::cout << "[Error - LuaScriptInterface::luaGlobalEventRegister] No interval for globalevent with name "
			          << globalevent->getName() << '\n';
			context.push_boolean(false);
			return 1;
		}

		context.push_boolean(g_globalEvents->registerLuaEvent(globalevent));
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaGlobalEventOnCallback()
{
	// globalevent:onThink / record / etc. (callback)
	GlobalEvent* globalevent = tfs::lua::getUserdata<GlobalEvent>(L, 1);
	if (globalevent) {
		if (!globalevent->loadCallback()) {
			context.push_boolean(false);
			return 1;
		}
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaGlobalEventTime()
{
	// globalevent:time(time)
	GlobalEvent* globalevent = tfs::lua::getUserdata<GlobalEvent>(L, 1);
	if (globalevent) {
		std::string timer = tfs::lua::getString(L, 2);
		std::vector<int32_t> params = vectorAtoi(explodeString(timer, ":"));

		int32_t hour = params.front();
		if (hour < 0 || hour > 23) {
			std::cout << "[Error - GlobalEvent::configureEvent] Invalid hour \"" << timer
			          << "\" for globalevent with name: " << globalevent->getName() << '\n';
			context.push_boolean(false);
			return 1;
		}

		globalevent->setInterval(hour << 16);

		int32_t min = 0;
		int32_t sec = 0;
		if (params.size() > 1) {
			min = params[1];
			if (min < 0 || min > 59) {
				std::cout << "[Error - GlobalEvent::configureEvent] Invalid minute \"" << timer
				          << "\" for globalevent with name: " << globalevent->getName() << '\n';
				context.push_boolean(false);
				return 1;
			}

			if (params.size() > 2) {
				sec = params[2];
				if (sec < 0 || sec > 59) {
					std::cout << "[Error - GlobalEvent::configureEvent] Invalid second \"" << timer
					          << "\" for globalevent with name: " << globalevent->getName() << '\n';
					context.push_boolean(false);
					return 1;
				}
			}
		}

		time_t current_time = time(nullptr);
		tm* timeinfo = localtime(&current_time);
		timeinfo->tm_hour = hour;
		timeinfo->tm_min = min;
		timeinfo->tm_sec = sec;

		time_t difference = static_cast<time_t>(difftime(mktime(timeinfo), current_time));
		if (difference < 0) {
			difference += 86400;
		}

		globalevent->setNextExecution(current_time + difference);
		globalevent->setEventType(GLOBALEVENT_TIMER);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaGlobalEventInterval()
{
	// globalevent:interval(interval)
	GlobalEvent* globalevent = tfs::lua::getUserdata<GlobalEvent>(L, 1);
	if (globalevent) {
		globalevent->setInterval(context.get_number<uint32_t>(2));
		globalevent->setNextExecution(OTSYS_TIME() + context.get_number<uint32_t>(2));
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

// Weapon
int LuaScriptInterface::luaCreateWeapon()
{
	// Weapon(type)
	if (tfs::lua::getScriptEnv()->getScriptInterface() != &g_scripts->getScriptInterface()) {
		reportErrorFunc(L, "Weapons can only be registered in the Scripts interface.");
		context.push_nil();
		return 1;
	}

	WeaponType_t type = tfs::lua::getNumber<WeaponType_t>(L, 2);
	switch (type) {
		case WEAPON_SWORD:
		case WEAPON_AXE:
		case WEAPON_CLUB: {
			WeaponMelee* weapon = new WeaponMelee(tfs::lua::getScriptEnv()->getScriptInterface());
			tfs::lua::pushUserdata(L, weapon);
			tfs::lua::setMetatable(L, -1, "Weapon");
			weapon->weaponType = type;
			weapon->fromLua = true;
			break;
		}
		case WEAPON_DISTANCE:
		case WEAPON_AMMO: {
			WeaponDistance* weapon = new WeaponDistance(tfs::lua::getScriptEnv()->getScriptInterface());
			tfs::lua::pushUserdata(L, weapon);
			tfs::lua::setMetatable(L, -1, "Weapon");
			weapon->weaponType = type;
			weapon->fromLua = true;
			break;
		}
		case WEAPON_WAND: {
			WeaponWand* weapon = new WeaponWand(tfs::lua::getScriptEnv()->getScriptInterface());
			tfs::lua::pushUserdata(L, weapon);
			tfs::lua::setMetatable(L, -1, "Weapon");
			weapon->weaponType = type;
			weapon->fromLua = true;
			break;
		}
		default: {
			context.push_nil();
			break;
		}
	}
	return 1;
}

int LuaScriptInterface::luaWeaponAction()
{
	// weapon:action(callback)
	Weapon* weapon = tfs::lua::getUserdata<Weapon>(L, 1);
	if (weapon) {
		std::string typeName = tfs::lua::getString(L, 2);
		std::string tmpStr = boost::algorithm::to_lower_copy(typeName);
		if (tmpStr == "removecount") {
			weapon->action = WEAPONACTION_REMOVECOUNT;
		} else if (tmpStr == "removecharge") {
			weapon->action = WEAPONACTION_REMOVECHARGE;
		} else if (tmpStr == "move") {
			weapon->action = WEAPONACTION_MOVE;
		} else {
			std::cout << "Error: [Weapon::action] No valid action " << typeName << '\n';
			context.push_boolean(false);
		}
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponRegister()
{
	// weapon:register()
	Weapon** weaponPtr = tfs::lua::getRawUserdata<Weapon>(L, 1);
	if (!weaponPtr) {
		context.push_nil();
		return 1;
	}

	if (auto* weapon = *weaponPtr) {
		if (weapon->weaponType == WEAPON_DISTANCE || weapon->weaponType == WEAPON_AMMO) {
			weapon = tfs::lua::getUserdata<WeaponDistance>(L, 1);
		} else if (weapon->weaponType == WEAPON_WAND) {
			weapon = tfs::lua::getUserdata<WeaponWand>(L, 1);
		} else {
			weapon = tfs::lua::getUserdata<WeaponMelee>(L, 1);
		}

		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);
		it.weaponType = weapon->weaponType;

		if (weapon->getWieldInfo() != 0) {
			it.wieldInfo = weapon->getWieldInfo();
			it.vocationString = weapon->getVocationString();
			it.minReqLevel = weapon->getReqLevel();
			it.minReqMagicLevel = weapon->getReqMagLv();
		}

		weapon->configureWeapon(it);
		context.push_boolean(g_weapons->registerLuaEvent(weapon));
		*weaponPtr = nullptr; // Remove luascript reference
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponOnUseWeapon()
{
	// weapon:onUseWeapon(callback)
	Weapon* weapon = tfs::lua::getUserdata<Weapon>(L, 1);
	if (weapon) {
		if (!weapon->loadCallback()) {
			context.push_boolean(false);
			return 1;
		}
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponUnproperly()
{
	// weapon:wieldUnproperly(bool)
	Weapon* weapon = tfs::lua::getUserdata<Weapon>(L, 1);
	if (weapon) {
		weapon->setWieldUnproperly(tfs::lua::getBoolean(L, 2));
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponLevel()
{
	// weapon:level(lvl)
	Weapon* weapon = tfs::lua::getUserdata<Weapon>(L, 1);
	if (weapon) {
		weapon->setRequiredLevel(context.get_number<uint32_t>(2));
		weapon->setWieldInfo(WIELDINFO_LEVEL);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponMagicLevel()
{
	// weapon:magicLevel(lvl)
	Weapon* weapon = tfs::lua::getUserdata<Weapon>(L, 1);
	if (weapon) {
		weapon->setRequiredMagLevel(context.get_number<uint32_t>(2));
		weapon->setWieldInfo(WIELDINFO_MAGLV);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponMana()
{
	// weapon:mana(mana)
	Weapon* weapon = tfs::lua::getUserdata<Weapon>(L, 1);
	if (weapon) {
		weapon->setMana(context.get_number<uint32_t>(2));
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponManaPercent()
{
	// weapon:manaPercent(percent)
	Weapon* weapon = tfs::lua::getUserdata<Weapon>(L, 1);
	if (weapon) {
		weapon->setManaPercent(context.get_number<uint32_t>(2));
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponHealth()
{
	// weapon:health(health)
	Weapon* weapon = tfs::lua::getUserdata<Weapon>(L, 1);
	if (weapon) {
		weapon->setHealth(tfs::lua::getNumber<int32_t>(L, 2));
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponHealthPercent()
{
	// weapon:healthPercent(percent)
	Weapon* weapon = tfs::lua::getUserdata<Weapon>(L, 1);
	if (weapon) {
		weapon->setHealthPercent(context.get_number<uint32_t>(2));
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponSoul()
{
	// weapon:soul(soul)
	Weapon* weapon = tfs::lua::getUserdata<Weapon>(L, 1);
	if (weapon) {
		weapon->setSoul(context.get_number<uint32_t>(2));
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponBreakChance()
{
	// weapon:breakChance(percent)
	Weapon* weapon = tfs::lua::getUserdata<Weapon>(L, 1);
	if (weapon) {
		weapon->setBreakChance(context.get_number<uint32_t>(2));
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponWandDamage()
{
	// weapon:damage(damage[min, max]) only use this if the weapon is a wand!
	WeaponWand* weapon = tfs::lua::getUserdata<WeaponWand>(L, 1);
	if (weapon) {
		weapon->setMinChange(context.get_number<uint32_t>(2));
		if (lua_gettop(L) > 2) {
			weapon->setMaxChange(context.get_number<uint32_t>(3));
		} else {
			weapon->setMaxChange(context.get_number<uint32_t>(2));
		}
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponElement()
{
	// weapon:element(combatType)
	Weapon* weapon = tfs::lua::getUserdata<Weapon>(L, 1);
	if (weapon) {
		if (!tfs::lua::getNumber<CombatType_t>(L, 2)) {
			std::string element = tfs::lua::getString(L, 2);
			std::string tmpStrValue = boost::algorithm::to_lower_copy(element);
			if (tmpStrValue == "earth") {
				weapon->params.combatType = COMBAT_EARTHDAMAGE;
			} else if (tmpStrValue == "ice") {
				weapon->params.combatType = COMBAT_ICEDAMAGE;
			} else if (tmpStrValue == "energy") {
				weapon->params.combatType = COMBAT_ENERGYDAMAGE;
			} else if (tmpStrValue == "fire") {
				weapon->params.combatType = COMBAT_FIREDAMAGE;
			} else if (tmpStrValue == "death") {
				weapon->params.combatType = COMBAT_DEATHDAMAGE;
			} else if (tmpStrValue == "holy") {
				weapon->params.combatType = COMBAT_HOLYDAMAGE;
			} else {
				std::cout << "[Warning - weapon:element] Type \"" << element << "\" does not exist.\n";
			}
		} else {
			weapon->params.combatType = tfs::lua::getNumber<CombatType_t>(L, 2);
		}
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponPremium()
{
	// weapon:premium(bool)
	Weapon* weapon = tfs::lua::getUserdata<Weapon>(L, 1);
	if (weapon) {
		weapon->setNeedPremium(tfs::lua::getBoolean(L, 2));
		weapon->setWieldInfo(WIELDINFO_PREMIUM);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponVocation()
{
	// weapon:vocation(vocName[, showInDescription = false, lastVoc = false])
	Weapon* weapon = tfs::lua::getUserdata<Weapon>(L, 1);
	if (weapon) {
		weapon->addVocationWeaponSet(tfs::lua::getString(L, 2));
		weapon->setWieldInfo(WIELDINFO_VOCREQ);
		std::string tmp;
		bool showInDescription = tfs::lua::getBoolean(L, 3, false);
		bool lastVoc = tfs::lua::getBoolean(L, 4, false);

		if (showInDescription) {
			if (weapon->getVocationString().empty()) {
				tmp = boost::algorithm::to_lower_copy(tfs::lua::getString(L, 2));
				tmp += "s";
				weapon->setVocationString(tmp);
			} else {
				tmp = weapon->getVocationString();
				if (lastVoc) {
					tmp += " and ";
				} else {
					tmp += ", ";
				}
				tmp += boost::algorithm::to_lower_copy(tfs::lua::getString(L, 2));
				tmp += "s";
				weapon->setVocationString(tmp);
			}
		}
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponId()
{
	// weapon:id(id)
	Weapon* weapon = tfs::lua::getUserdata<Weapon>(L, 1);
	if (weapon) {
		weapon->setID(context.get_number<uint32_t>(2));
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponAttack()
{
	// weapon:attack(atk)
	Weapon* weapon = tfs::lua::getUserdata<Weapon>(L, 1);
	if (weapon) {
		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);
		it.attack = tfs::lua::getNumber<int32_t>(L, 2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponDefense()
{
	// weapon:defense(defense[, extraDefense])
	Weapon* weapon = tfs::lua::getUserdata<Weapon>(L, 1);
	if (weapon) {
		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);
		it.defense = tfs::lua::getNumber<int32_t>(L, 2);
		if (lua_gettop(L) > 2) {
			it.extraDefense = tfs::lua::getNumber<int32_t>(L, 3);
		}
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponRange()
{
	// weapon:range(range)
	Weapon* weapon = tfs::lua::getUserdata<Weapon>(L, 1);
	if (weapon) {
		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);
		it.shootRange = context.get_number<uint8_t>(2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponCharges()
{
	// weapon:charges(charges[, showCharges = true])
	Weapon* weapon = tfs::lua::getUserdata<Weapon>(L, 1);
	if (weapon) {
		bool showCharges = tfs::lua::getBoolean(L, 3, true);
		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);

		it.charges = context.get_number<uint32_t>(2);
		it.showCharges = showCharges;
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponDuration()
{
	// weapon:duration(duration[, showDuration = true])
	Weapon* weapon = tfs::lua::getUserdata<Weapon>(L, 1);
	if (weapon) {
		bool showDuration = tfs::lua::getBoolean(L, 3, true);
		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);

		if (lua_istable(L, 2)) {
			it.decayTimeMin = tfs::lua::getField<uint32_t>(L, 2, "min");
			it.decayTimeMax = tfs::lua::getField<uint32_t>(L, 2, "max");
		} else {
			it.decayTimeMin = context.get_number<uint32_t>(2);
		}

		it.showDuration = showDuration;
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponDecayTo()
{
	// weapon:decayTo([itemid = 0])
	Weapon* weapon = tfs::lua::getUserdata<Weapon>(L, 1);
	if (weapon) {
		uint16_t itemid = context.get_number<uint16_t>(2, 0);
		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);

		it.decayTo = itemid;
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponTransformEquipTo()
{
	// weapon:transformEquipTo(itemid)
	Weapon* weapon = tfs::lua::getUserdata<Weapon>(L, 1);
	if (weapon) {
		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);
		it.transformEquipTo = context.get_number<uint16_t>(2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponTransformDeEquipTo()
{
	// weapon:transformDeEquipTo(itemid)
	Weapon* weapon = tfs::lua::getUserdata<Weapon>(L, 1);
	if (weapon) {
		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);
		it.transformDeEquipTo = context.get_number<uint16_t>(2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponShootType()
{
	// weapon:shootType(type)
	Weapon* weapon = tfs::lua::getUserdata<Weapon>(L, 1);
	if (weapon) {
		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);
		it.shootType = tfs::lua::getNumber<ShootType_t>(L, 2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponSlotType()
{
	// weapon:slotType(slot)
	Weapon* weapon = tfs::lua::getUserdata<Weapon>(L, 1);
	if (weapon) {
		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);
		std::string slot = tfs::lua::getString(L, 2);

		if (slot == "two-handed") {
			it.slotPosition |= SLOTP_TWO_HAND;
		} else {
			it.slotPosition |= SLOTP_HAND;
		}
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponAmmoType()
{
	// weapon:ammoType(type)
	WeaponDistance* weapon = tfs::lua::getUserdata<WeaponDistance>(L, 1);
	if (weapon) {
		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);
		std::string type = tfs::lua::getString(L, 2);

		if (type == "arrow") {
			it.ammoType = AMMO_ARROW;
		} else if (type == "bolt") {
			it.ammoType = AMMO_BOLT;
		} else {
			std::cout << "[Warning - weapon:ammoType] Type \"" << type << "\" does not exist.\n";
			context.push_nil();
			return 1;
		}
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponHitChance()
{
	// weapon:hitChance(chance)
	Weapon* weapon = tfs::lua::getUserdata<Weapon>(L, 1);
	if (weapon) {
		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);
		it.hitChance = tfs::lua::getNumber<int8_t>(L, 2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponMaxHitChance()
{
	// weapon:maxHitChance(max)
	Weapon* weapon = tfs::lua::getUserdata<Weapon>(L, 1);
	if (weapon) {
		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);
		it.maxHitChance = tfs::lua::getNumber<int32_t>(L, 2);
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaWeaponExtraElement()
{
	// weapon:extraElement(atk, combatType)
	Weapon* weapon = tfs::lua::getUserdata<Weapon>(L, 1);
	if (weapon) {
		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);
		it.abilities.get()->elementDamage = context.get_number<uint16_t>(2);

		if (!tfs::lua::getNumber<CombatType_t>(L, 3)) {
			std::string element = tfs::lua::getString(L, 3);
			std::string tmpStrValue = boost::algorithm::to_lower_copy(element);
			if (tmpStrValue == "earth") {
				it.abilities.get()->elementType = COMBAT_EARTHDAMAGE;
			} else if (tmpStrValue == "ice") {
				it.abilities.get()->elementType = COMBAT_ICEDAMAGE;
			} else if (tmpStrValue == "energy") {
				it.abilities.get()->elementType = COMBAT_ENERGYDAMAGE;
			} else if (tmpStrValue == "fire") {
				it.abilities.get()->elementType = COMBAT_FIREDAMAGE;
			} else if (tmpStrValue == "death") {
				it.abilities.get()->elementType = COMBAT_DEATHDAMAGE;
			} else if (tmpStrValue == "holy") {
				it.abilities.get()->elementType = COMBAT_HOLYDAMAGE;
			} else {
				std::cout << "[Warning - weapon:extraElement] Type \"" << element << "\" does not exist.\n";
			}
		} else {
			it.abilities.get()->elementType = tfs::lua::getNumber<CombatType_t>(L, 3);
		}
		context.push_boolean(true);
	} else {
		context.push_nil();
	}
	return 1;
}

// XML
int LuaScriptInterface::luaCreateXmlDocument()
{
	// XMLDocument(filename)
	std::string filename = tfs::lua::getString(L, 2);
	if (filename.empty()) {
		context.push_nil();
		return 1;
	}

	auto doc = std::make_unique<pugi::xml_document>();
	if (auto result = doc->load_file(filename.data())) {
		tfs::lua::pushUserdata(L, doc.release());
		tfs::lua::setMetatable(L, -1, "XMLDocument");
	} else {
		printXMLError("Error - LuaScriptInterface::luaCreateXmlDocument", filename, result);
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaDeleteXmlDocument()
{
	// doc:delete() or doc:__gc()
	pugi::xml_document** document = tfs::lua::getRawUserdata<pugi::xml_document>(L, 1);
	if (document && *document) {
		delete *document;
		*document = nullptr;
	}
	return 1;
}

int LuaScriptInterface::luaXmlDocumentChild()
{
	// doc:child(name)
	pugi::xml_document* document = tfs::lua::getUserdata<pugi::xml_document>(L, 1);
	if (!document) {
		context.push_nil();
		return 1;
	}

	std::string name = tfs::lua::getString(L, 2);
	if (name.empty()) {
		context.push_nil();
		return 1;
	}

	auto node = std::make_unique<pugi::xml_node>(document->child(name.data()));
	tfs::lua::pushUserdata(L, node.release());
	tfs::lua::setMetatable(L, -1, "XMLNode");
	return 1;
}

int LuaScriptInterface::luaDeleteXmlNode()
{
	// node:delete() or node:__gc()
	pugi::xml_node** node = tfs::lua::getRawUserdata<pugi::xml_node>(L, 1);
	if (node && *node) {
		delete *node;
		*node = nullptr;
	}
	return 1;
}

int LuaScriptInterface::luaXmlNodeAttribute()
{
	// node:attribute(name)
	pugi::xml_node* node = tfs::lua::getUserdata<pugi::xml_node>(L, 1);
	if (!node) {
		context.push_nil();
		return 1;
	}

	std::string name = tfs::lua::getString(L, 2);
	if (name.empty()) {
		context.push_nil();
		return 1;
	}

	pugi::xml_attribute attribute = node->attribute(name.data());
	if (attribute) {
		tfs::lua::pushString(L, attribute.value());
	} else {
		context.push_nil();
	}
	return 1;
}

int LuaScriptInterface::luaXmlNodeName()
{
	// node:name()
	pugi::xml_node* node = tfs::lua::getUserdata<pugi::xml_node>(L, 1);
	if (!node) {
		context.push_nil();
		return 1;
	}

	tfs::lua::pushString(L, node->name());
	return 1;
}

int LuaScriptInterface::luaXmlNodeFirstChild()
{
	// node:firstChild()
	pugi::xml_node* node = tfs::lua::getUserdata<pugi::xml_node>(L, 1);
	if (!node) {
		context.push_nil();
		return 1;
	}

	auto firstChild = node->first_child();
	if (!firstChild) {
		context.push_nil();
		return 1;
	}

	auto newNode = std::make_unique<pugi::xml_node>(firstChild);
	tfs::lua::pushUserdata(L, newNode.release());
	tfs::lua::setMetatable(L, -1, "XMLNode");
	return 1;
}

int LuaScriptInterface::luaXmlNodeNextSibling()
{
	// node:nextSibling()
	pugi::xml_node* node = tfs::lua::getUserdata<pugi::xml_node>(L, 1);
	if (!node) {
		context.push_nil();
		return 1;
	}

	auto nextSibling = node->next_sibling();
	if (!nextSibling) {
		context.push_nil();
		return 1;
	}

	auto newNode = std::make_unique<pugi::xml_node>(nextSibling);
	tfs::lua::pushUserdata(L, newNode.release());
	tfs::lua::setMetatable(L, -1, "XMLNode");
	return 1;
}

//
LuaEnvironment::LuaEnvironment() : LuaScriptInterface("Main Interface") {}

LuaEnvironment::~LuaEnvironment()
{
	delete testInterface;
	closeState();
}

bool LuaEnvironment::initState()
{
	if (!context.init()) {
		return false;
	}

	registerFunctions();

	runningEventId = EVENT_ID_USER;
	return true;
}

bool LuaEnvironment::reInitState()
{
	// TODO: get children, reload children
	closeState();
	return initState();
}

bool LuaEnvironment::closeState()
{
	if (!L) {
		return false;
	}

	for (const auto& combatEntry : combatIdMap) {
		clearCombatObjects(combatEntry.first);
	}

	for (const auto& areaEntry : areaIdMap) {
		clearAreaObjects(areaEntry.first);
	}

	for (auto& timerEntry : timerEvents) {
		LuaTimerEventDesc timerEventDesc = std::move(timerEntry.second);
		for (int32_t parameter : timerEventDesc.parameters) {
			luaL_unref(L, LUA_REGISTRYINDEX, parameter);
		}
		luaL_unref(L, LUA_REGISTRYINDEX, timerEventDesc.function);
	}

	combatIdMap.clear();
	areaIdMap.clear();
	timerEvents.clear();
	cacheFiles.clear();

	lua_close(L);
	L = nullptr;
	return true;
}

LuaScriptInterface* LuaEnvironment::getTestInterface()
{
	if (!testInterface) {
		testInterface = new LuaScriptInterface("Test Interface");
		testInterface->initState();
	}
	return testInterface;
}

Combat_ptr LuaEnvironment::getCombatObject(uint32_t id) const
{
	auto it = combatMap.find(id);
	if (it == combatMap.end()) {
		return nullptr;
	}
	return it->second;
}

Combat_ptr LuaEnvironment::createCombatObject(LuaScriptInterface* interface)
{
	Combat_ptr combat = std::make_shared<Combat>();
	combatMap[++lastCombatId] = combat;
	combatIdMap[interface].push_back(lastCombatId);
	return combat;
}

void LuaEnvironment::clearCombatObjects(LuaScriptInterface* interface)
{
	auto it = combatIdMap.find(interface);
	if (it == combatIdMap.end()) {
		return;
	}

	for (uint32_t id : it->second) {
		auto itt = combatMap.find(id);
		if (itt != combatMap.end()) {
			combatMap.erase(itt);
		}
	}
	it->second.clear();
}

AreaCombat* LuaEnvironment::getAreaObject(uint32_t id) const
{
	auto it = areaMap.find(id);
	if (it == areaMap.end()) {
		return nullptr;
	}
	return it->second;
}

uint32_t LuaEnvironment::createAreaObject(LuaScriptInterface* interface)
{
	areaMap[++lastAreaId] = new AreaCombat;
	areaIdMap[interface].push_back(lastAreaId);
	return lastAreaId;
}

void LuaEnvironment::clearAreaObjects(LuaScriptInterface* interface)
{
	auto it = areaIdMap.find(interface);
	if (it == areaIdMap.end()) {
		return;
	}

	for (uint32_t id : it->second) {
		auto itt = areaMap.find(id);
		if (itt != areaMap.end()) {
			delete itt->second;
			areaMap.erase(itt);
		}
	}
	it->second.clear();
}

void LuaEnvironment::executeTimerEvent(uint32_t eventIndex)
{
	auto it = timerEvents.find(eventIndex);
	if (it == timerEvents.end()) {
		return;
	}

	LuaTimerEventDesc timerEventDesc = std::move(it->second);
	timerEvents.erase(it);

	// push function
	context.raw_geti(LUA_REGISTRYINDEX, timerEventDesc.function);

	// push parameters
	for (auto parameter : std::views::reverse(timerEventDesc.parameters)) {
		context.raw_geti(LUA_REGISTRYINDEX, parameter);
	}

	// call the function
	if (tfs::lua::reserveScriptEnv()) {
		ScriptEnvironment* env = tfs::lua::getScriptEnv();
		env->setTimerEvent();
		env->setScriptId(timerEventDesc.scriptId, this);
		callFunction(timerEventDesc.parameters.size());
	} else {
		std::cout << "[Error - LuaScriptInterface::executeTimerEvent] Call stack overflow\n";
	}

	// free resources
	luaL_unref(L, LUA_REGISTRYINDEX, timerEventDesc.function);
	for (auto parameter : timerEventDesc.parameters) {
		luaL_unref(L, LUA_REGISTRYINDEX, parameter);
	}
}

LuaContext::LuaContext() : state(nullptr) {}

LuaContext::~LuaContext() { close(); }

bool LuaContext::init()
{
	state = luaL_newstate();
	if (!state) {
		return false;
	}

	luaL_openlibs(state);
	return true;
}

void LuaContext::close()
{
	if (state) {
		lua_close(state);
		state = nullptr;
	}
}

void LuaContext::push_nil() { lua_pushnil(state); }
void LuaContext::push_number(lua_Number n) { lua_pushnumber(state, n); }
void LuaContext::push_integer(lua_Integer n) { lua_pushinteger(state, n); }
void LuaContext::push_boolean(bool value) { lua_pushboolean(state, value ? 1 : 0); }

void LuaContext::get_table(int index) { lua_gettable(state, index); }
void LuaContext::get_field(int index, const char* key) { lua_getfield(state, index, key); }
void LuaContext::raw_get(int index) { lua_rawget(state, index); }
void LuaContext::raw_geti(int index, int n) { lua_rawgeti(state, index, n); }
void LuaContext::create_table(int narr, int nrec) { lua_createtable(state, narr, nrec); }
void* LuaContext::new_userdata(size_t size) { return lua_newuserdata(state, size); }
bool LuaContext::get_metatable(int objindex) { return lua_getmetatable(state, objindex) != 0; }
void LuaContext::get_fenv(int index) { lua_getfenv(state, index); }
