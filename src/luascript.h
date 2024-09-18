// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#ifndef FS_LUASCRIPT_H
#define FS_LUASCRIPT_H

#include "database.h"
#include "enums.h"
#include "position.h"

#if LUA_VERSION_NUM >= 502
#ifndef LUA_COMPAT_ALL
#ifndef LUA_COMPAT_MODULE
#define luaL_register(L, libname, l) (luaL_newlib(L, l), lua_pushvalue(L, -1), lua_setglobal(L, libname))
#endif
#undef lua_equal
#define lua_equal(L, i1, i2) lua_compare(L, (i1), (i2), LUA_OPEQ)
#endif
#endif

class AreaCombat;
class Combat;
class Container;
class Creature;
class Cylinder;
class Spell;
class Item;
class LuaScriptInterface;
class LuaVariant;
class Npc;
class Player;
class Thing;
struct Outfit;

using Combat_ptr = std::shared_ptr<Combat>;

inline constexpr int32_t EVENT_ID_USER = 1000;

struct LuaTimerEventDesc
{
	int32_t scriptId = -1;
	int32_t function = -1;
	std::vector<int32_t> parameters;
	uint32_t eventId = 0;

	LuaTimerEventDesc() = default;
	LuaTimerEventDesc(LuaTimerEventDesc&& other) = default;
};

class ScriptEnvironment
{
public:
	ScriptEnvironment();
	~ScriptEnvironment();

	// non-copyable
	ScriptEnvironment(const ScriptEnvironment&) = delete;
	ScriptEnvironment& operator=(const ScriptEnvironment&) = delete;

	void resetEnv();

	void setScriptId(int32_t scriptId, LuaScriptInterface* scriptInterface)
	{
		this->scriptId = scriptId;
		interface = scriptInterface;
	}
	bool setCallbackId(int32_t callbackId, LuaScriptInterface* scriptInterface);

	int32_t getScriptId() const { return scriptId; }
	LuaScriptInterface* getScriptInterface() { return interface; }

	void setTimerEvent() { timerEvent = true; }

	auto getEventInfo() const { return std::make_tuple(scriptId, interface, callbackId, timerEvent); }

	uint32_t addThing(Thing* thing);
	void insertItem(uint32_t uid, Item* item);

	void setNpc(Npc* npc) { curNpc = npc; }
	Npc* getNpc() const { return curNpc; }

	Thing* getThingByUID(uint32_t uid);
	Item* getItemByUID(uint32_t uid);
	Container* getContainerByUID(uint32_t uid);
	void removeItemByUID(uint32_t uid);

private:
	LuaScriptInterface* interface;

	// for npc scripts
	Npc* curNpc = nullptr;

	// local item map
	std::unordered_map<uint32_t, Item*> localMap;
	uint32_t lastUID = std::numeric_limits<uint16_t>::max();

	// script file id
	int32_t scriptId;
	int32_t callbackId;
	bool timerEvent;
};

enum ErrorCode_t
{
	LUA_ERROR_PLAYER_NOT_FOUND,
	LUA_ERROR_CREATURE_NOT_FOUND,
	LUA_ERROR_ITEM_NOT_FOUND,
	LUA_ERROR_THING_NOT_FOUND,
	LUA_ERROR_TILE_NOT_FOUND,
	LUA_ERROR_HOUSE_NOT_FOUND,
	LUA_ERROR_COMBAT_NOT_FOUND,
	LUA_ERROR_CONDITION_NOT_FOUND,
	LUA_ERROR_AREA_NOT_FOUND,
	LUA_ERROR_CONTAINER_NOT_FOUND,
	LUA_ERROR_VARIANT_NOT_FOUND,
	LUA_ERROR_VARIANT_UNKNOWN,
	LUA_ERROR_SPELL_NOT_FOUND,
};

class LuaScriptInterface
{
public:
	explicit LuaScriptInterface(std::string interfaceName);
	virtual ~LuaScriptInterface();

	// non-copyable
	LuaScriptInterface(const LuaScriptInterface&) = delete;
	LuaScriptInterface& operator=(const LuaScriptInterface&) = delete;

	virtual bool initState();
	bool reInitState();

	int32_t loadFile(const std::string& file, Npc* npc = nullptr);

	const std::string& getFileById(int32_t scriptId);
	int32_t getEvent(std::string_view eventName);
	int32_t getEvent();
	int32_t getMetaEvent(const std::string& globalName, const std::string& eventName);
	void removeEvent(int32_t scriptId);

	const std::string& getInterfaceName() const { return interfaceName; }
	const std::string& getLastLuaError() const { return lastLuaError; }

	lua_State* getLuaState() const { return L; }

	bool pushFunction(int32_t functionId);

	bool callFunction(int params);
	void callVoidFunction(int params);

#ifndef LUAJIT_VERSION
	static const luaL_Reg luaBitReg[7];
#endif
	static const luaL_Reg luaConfigManagerTable[4];
	static const luaL_Reg luaDatabaseTable[9];
	static const luaL_Reg luaResultTable[6];

	//
	std::string lastLuaError;
	std::string interfaceName;
	std::string loadingFile;

protected:
	virtual bool closeState();

	void registerFunctions();

	lua_State* LuaState = nullptr;
	LuaContext context;

	int32_t eventTableRef = -1;
	int32_t runningEventId = EVENT_ID_USER;

	// script file cache
	std::map<int32_t, std::string> cacheFiles;

private:
	// lua functions
	int luaDoPlayerAddItem();

	// get item info
	int luaGetDepotId();

	// get world info
	int luaGetWorldUpTime();

	// get subtype name
	int luaGetSubTypeName();

	// type validation
	int luaIsDepot();
	int luaIsMoveable();
	int luaIsValidUID();

	//
	int luaCreateCombatArea();

	int luaDoAreaCombat();
	int luaDoTargetCombat();

	int luaDoChallengeCreature();

	int luaDebugPrint();
	int luaAddEvent();
	int luaStopEvent();

	int luaSaveServer();
	int luaCleanMap();

	int luaIsInWar();

	int luaGetWaypointPositionByName();

	int luaSendChannelMessage();
	int luaSendGuildChannelMessage();

	int luaIsScriptsInterface();

#ifndef LUAJIT_VERSION
	int luaBitNot();
	int luaBitAnd();
	int luaBitOr();
	int luaBitXor();
	int luaBitLeftShift();
	int luaBitRightShift();
#endif

	int luaConfigManagerGetString();
	int luaConfigManagerGetNumber();
	int luaConfigManagerGetBoolean();

	int luaDatabaseExecute();
	int luaDatabaseAsyncExecute();
	int luaDatabaseStoreQuery();
	int luaDatabaseAsyncStoreQuery();
	int luaDatabaseEscapeString();
	int luaDatabaseEscapeBlob();
	int luaDatabaseLastInsertId();
	int luaDatabaseTableExists();

	int luaResultGetNumber();
	int luaResultGetString();
	int luaResultGetStream();
	int luaResultNext();
	int luaResultFree();

	// Userdata
	int luaUserdataCompare();

	// _G
	int luaIsType();
	int luaRawGetMetatable();

	// os
	int luaSystemTime();

	// table
	int luaTableCreate();
	int luaTablePack();

	// DB Insert
	int luaDBInsertCreate();
	int luaDBInsertAddRow();
	int luaDBInsertExecute();
	int luaDBInsertDelete();

	// DB Transaction
	int luaDBTransactionCreate();
	int luaDBTransactionDelete();
	int luaDBTransactionBegin();
	int luaDBTransactionCommit();

	// Game
	int luaGameGetSpectators();
	int luaGameGetPlayers();
	int luaGameGetNpcs();
	int luaGameGetMonsters();
	int luaGameLoadMap();

	int luaGameGetExperienceStage();
	int luaGameGetExperienceForLevel();
	int luaGameGetMonsterCount();
	int luaGameGetPlayerCount();
	int luaGameGetNpcCount();
	int luaGameGetMonsterTypes();
	int luaGameGetBestiary();
	int luaGameGetCurrencyItems();
	int luaGameGetItemTypeByClientId();
	int luaGameGetMountIdByLookType();

	int luaGameGetTowns();
	int luaGameGetHouses();
	int luaGameGetOutfits();
	int luaGameGetMounts();
	int luaGameGetVocations();

	int luaGameGetGameState();
	int luaGameSetGameState();

	int luaGameGetWorldType();
	int luaGameSetWorldType();

	int luaGameGetItemAttributeByName();
	int luaGameGetReturnMessage();

	int luaGameCreateItem();
	int luaGameCreateContainer();
	int luaGameCreateMonster();
	int luaGameCreateNpc();
	int luaGameCreateTile();
	int luaGameCreateMonsterType();
	int luaGameCreateNpcType();

	int luaGameStartEvent();

	int luaGameGetClientVersion();

	int luaGameReload();

	// Variant
	int luaVariantCreate();

	int luaVariantGetNumber();
	int luaVariantGetString();
	int luaVariantGetPosition();

	// Position
	int luaPositionCreate();

	int luaPositionIsSightClear();

	int luaPositionSendMagicEffect();
	int luaPositionSendDistanceEffect();

	// Tile
	int luaTileCreate();

	int luaTileRemove();

	int luaTileGetPosition();
	int luaTileGetGround();
	int luaTileGetThing();
	int luaTileGetThingCount();
	int luaTileGetTopVisibleThing();

	int luaTileGetTopTopItem();
	int luaTileGetTopDownItem();
	int luaTileGetFieldItem();

	int luaTileGetItemById();
	int luaTileGetItemByType();
	int luaTileGetItemByTopOrder();
	int luaTileGetItemCountById();

	int luaTileGetBottomCreature();
	int luaTileGetTopCreature();
	int luaTileGetBottomVisibleCreature();
	int luaTileGetTopVisibleCreature();

	int luaTileGetItems();
	int luaTileGetItemCount();
	int luaTileGetDownItemCount();
	int luaTileGetTopItemCount();

	int luaTileGetCreatures();
	int luaTileGetCreatureCount();

	int luaTileHasProperty();
	int luaTileHasFlag();

	int luaTileGetThingIndex();

	int luaTileQueryAdd();
	int luaTileAddItem();
	int luaTileAddItemEx();

	int luaTileGetHouse();

	// NetworkMessage
	int luaNetworkMessageCreate();
	int luaNetworkMessageDelete();

	int luaNetworkMessageGetByte();
	int luaNetworkMessageGetU16();
	int luaNetworkMessageGetU32();
	int luaNetworkMessageGetU64();
	int luaNetworkMessageGetString();
	int luaNetworkMessageGetPosition();

	int luaNetworkMessageAddByte();
	int luaNetworkMessageAddU16();
	int luaNetworkMessageAddU32();
	int luaNetworkMessageAddU64();
	int luaNetworkMessageAddString();
	int luaNetworkMessageAddPosition();
	int luaNetworkMessageAddDouble();
	int luaNetworkMessageAddItem();
	int luaNetworkMessageAddItemId();

	int luaNetworkMessageReset();
	int luaNetworkMessageSeek();
	int luaNetworkMessageTell();
	int luaNetworkMessageLength();
	int luaNetworkMessageSkipBytes();
	int luaNetworkMessageSendToPlayer();

	// ModalWindow
	int luaModalWindowCreate();
	int luaModalWindowDelete();

	int luaModalWindowGetId();
	int luaModalWindowGetTitle();
	int luaModalWindowGetMessage();

	int luaModalWindowSetTitle();
	int luaModalWindowSetMessage();

	int luaModalWindowGetButtonCount();
	int luaModalWindowGetChoiceCount();

	int luaModalWindowAddButton();
	int luaModalWindowAddChoice();

	int luaModalWindowGetDefaultEnterButton();
	int luaModalWindowSetDefaultEnterButton();

	int luaModalWindowGetDefaultEscapeButton();
	int luaModalWindowSetDefaultEscapeButton();

	int luaModalWindowHasPriority();
	int luaModalWindowSetPriority();

	int luaModalWindowSendToPlayer();

	// Item
	int luaItemCreate();

	int luaItemIsItem();

	int luaItemGetParent();
	int luaItemGetTopParent();

	int luaItemGetId();

	int luaItemClone();
	int luaItemSplit();
	int luaItemRemove();

	int luaItemGetUniqueId();
	int luaItemGetActionId();
	int luaItemSetActionId();

	int luaItemGetCount();
	int luaItemGetCharges();
	int luaItemGetFluidType();
	int luaItemGetWeight();
	int luaItemGetWorth();

	int luaItemGetSubType();

	int luaItemGetName();
	int luaItemGetPluralName();
	int luaItemGetArticle();

	int luaItemGetPosition();
	int luaItemGetTile();

	int luaItemHasAttribute();
	int luaItemGetAttribute();
	int luaItemSetAttribute();
	int luaItemRemoveAttribute();
	int luaItemGetCustomAttribute();
	int luaItemSetCustomAttribute();
	int luaItemRemoveCustomAttribute();

	int luaItemMoveTo();
	int luaItemTransform();
	int luaItemDecay();

	int luaItemGetSpecialDescription();

	int luaItemHasProperty();
	int luaItemIsLoadedFromMap();

	int luaItemSetStoreItem();
	int luaItemIsStoreItem();

	int luaItemSetReflect();
	int luaItemGetReflect();

	int luaItemSetBoostPercent();
	int luaItemGetBoostPercent();

	// Container
	int luaContainerCreate();

	int luaContainerGetSize();
	int luaContainerGetCapacity();
	int luaContainerGetEmptySlots();
	int luaContainerGetItems();
	int luaContainerGetItemHoldingCount();
	int luaContainerGetItemCountById();

	int luaContainerGetItem();
	int luaContainerHasItem();
	int luaContainerAddItem();
	int luaContainerAddItemEx();
	int luaContainerGetCorpseOwner();

	// Teleport
	int luaTeleportCreate();

	int luaTeleportGetDestination();
	int luaTeleportSetDestination();

	// Podium
	int luaPodiumCreate();

	int luaPodiumGetOutfit();
	int luaPodiumSetOutfit();
	int luaPodiumHasFlag();
	int luaPodiumSetFlag();
	int luaPodiumGetDirection();
	int luaPodiumSetDirection();

	// Creature
	int luaCreatureCreate();

	int luaCreatureGetEvents();
	int luaCreatureRegisterEvent();
	int luaCreatureUnregisterEvent();

	int luaCreatureIsRemoved();
	int luaCreatureIsCreature();
	int luaCreatureIsInGhostMode();
	int luaCreatureIsHealthHidden();
	int luaCreatureIsMovementBlocked();
	int luaCreatureIsImmune();

	int luaCreatureCanSee();
	int luaCreatureCanSeeCreature();
	int luaCreatureCanSeeGhostMode();
	int luaCreatureCanSeeInvisibility();

	int luaCreatureGetParent();

	int luaCreatureGetId();
	int luaCreatureGetName();

	int luaCreatureGetTarget();
	int luaCreatureSetTarget();

	int luaCreatureGetFollowCreature();
	int luaCreatureSetFollowCreature();

	int luaCreatureGetMaster();
	int luaCreatureSetMaster();

	int luaCreatureGetLight();
	int luaCreatureSetLight();

	int luaCreatureGetSpeed();
	int luaCreatureGetBaseSpeed();
	int luaCreatureChangeSpeed();

	int luaCreatureSetDropLoot();
	int luaCreatureSetSkillLoss();

	int luaCreatureGetPosition();
	int luaCreatureGetTile();
	int luaCreatureGetDirection();
	int luaCreatureSetDirection();

	int luaCreatureGetHealth();
	int luaCreatureSetHealth();
	int luaCreatureAddHealth();
	int luaCreatureGetMaxHealth();
	int luaCreatureSetMaxHealth();
	int luaCreatureSetHiddenHealth();
	int luaCreatureSetMovementBlocked();

	int luaCreatureGetSkull();
	int luaCreatureSetSkull();

	int luaCreatureGetOutfit();
	int luaCreatureSetOutfit();

	int luaCreatureGetCondition();
	int luaCreatureAddCondition();
	int luaCreatureRemoveCondition();
	int luaCreatureHasCondition();

	int luaCreatureRemove();
	int luaCreatureTeleportTo();
	int luaCreatureSay();

	int luaCreatureGetDamageMap();

	int luaCreatureGetSummons();

	int luaCreatureGetDescription();

	int luaCreatureGetPathTo();
	int luaCreatureMove();

	int luaCreatureGetZone();

	int luaCreatureHasIcon();
	int luaCreatureSetIcon();
	int luaCreatureGetIcon();
	int luaCreatureRemoveIcon();

	int luaCreatureGetStorageValue();
	int luaCreatureSetStorageValue();

	// Player
	int luaPlayerCreate();

	int luaPlayerIsPlayer();

	int luaPlayerGetGuid();
	int luaPlayerGetIp();
	int luaPlayerGetAccountId();
	int luaPlayerGetLastLoginSaved();
	int luaPlayerGetLastLogout();

	int luaPlayerGetAccountType();
	int luaPlayerSetAccountType();

	int luaPlayerGetCapacity();
	int luaPlayerSetCapacity();

	int luaPlayerGetFreeCapacity();

	int luaPlayerGetDepotChest();
	int luaPlayerGetInbox();

	int luaPlayerGetSkullTime();
	int luaPlayerSetSkullTime();
	int luaPlayerGetDeathPenalty();

	int luaPlayerGetExperience();
	int luaPlayerAddExperience();
	int luaPlayerRemoveExperience();
	int luaPlayerGetLevel();
	int luaPlayerGetLevelPercent();

	int luaPlayerGetMagicLevel();
	int luaPlayerGetMagicLevelPercent();
	int luaPlayerGetBaseMagicLevel();
	int luaPlayerGetMana();
	int luaPlayerAddMana();
	int luaPlayerGetMaxMana();
	int luaPlayerSetMaxMana();
	int luaPlayerSetManaShieldBar();
	int luaPlayerGetManaSpent();
	int luaPlayerAddManaSpent();
	int luaPlayerRemoveManaSpent();

	int luaPlayerGetBaseMaxHealth();
	int luaPlayerGetBaseMaxMana();

	int luaPlayerGetSkillLevel();
	int luaPlayerGetEffectiveSkillLevel();
	int luaPlayerGetSkillPercent();
	int luaPlayerGetSkillTries();
	int luaPlayerAddSkillTries();
	int luaPlayerRemoveSkillTries();
	int luaPlayerGetSpecialSkill();
	int luaPlayerAddSpecialSkill();

	int luaPlayerAddOfflineTrainingTime();
	int luaPlayerGetOfflineTrainingTime();
	int luaPlayerRemoveOfflineTrainingTime();

	int luaPlayerAddOfflineTrainingTries();

	int luaPlayerGetOfflineTrainingSkill();
	int luaPlayerSetOfflineTrainingSkill();

	int luaPlayerGetItemCount();
	int luaPlayerGetItemById();

	int luaPlayerGetVocation();
	int luaPlayerSetVocation();

	int luaPlayerGetSex();
	int luaPlayerSetSex();

	int luaPlayerGetTown();
	int luaPlayerSetTown();

	int luaPlayerGetGuild();
	int luaPlayerSetGuild();

	int luaPlayerGetGuildLevel();
	int luaPlayerSetGuildLevel();

	int luaPlayerGetGuildNick();
	int luaPlayerSetGuildNick();

	int luaPlayerGetGroup();
	int luaPlayerSetGroup();

	int luaPlayerGetStamina();
	int luaPlayerSetStamina();

	int luaPlayerGetSoul();
	int luaPlayerAddSoul();
	int luaPlayerGetMaxSoul();

	int luaPlayerGetBankBalance();
	int luaPlayerSetBankBalance();

	int luaPlayerAddItem();
	int luaPlayerAddItemEx();
	int luaPlayerRemoveItem();
	int luaPlayerSendSupplyUsed();

	int luaPlayerGetMoney();
	int luaPlayerAddMoney();
	int luaPlayerRemoveMoney();

	int luaPlayerShowTextDialog();

	int luaPlayerSendTextMessage();
	int luaPlayerSendChannelMessage();
	int luaPlayerSendPrivateMessage();

	int luaPlayerChannelSay();
	int luaPlayerOpenChannel();

	int luaPlayerGetSlotItem();

	int luaPlayerGetParty();

	int luaPlayerAddOutfit();
	int luaPlayerAddOutfitAddon();
	int luaPlayerRemoveOutfit();
	int luaPlayerRemoveOutfitAddon();
	int luaPlayerHasOutfit();
	int luaPlayerCanWearOutfit();
	int luaPlayerSendOutfitWindow();

	int luaPlayerSendEditPodium();

	int luaPlayerAddMount();
	int luaPlayerRemoveMount();
	int luaPlayerHasMount();
	int luaPlayerToggleMount();

	int luaPlayerGetPremiumEndsAt();
	int luaPlayerSetPremiumEndsAt();

	int luaPlayerHasBlessing();
	int luaPlayerAddBlessing();
	int luaPlayerRemoveBlessing();

	int luaPlayerCanLearnSpell();
	int luaPlayerLearnSpell();
	int luaPlayerForgetSpell();
	int luaPlayerHasLearnedSpell();

	int luaPlayerSendTutorial();
	int luaPlayerAddMapMark();

	int luaPlayerSave();
	int luaPlayerPopupFYI();

	int luaPlayerIsPzLocked();

	int luaPlayerGetClient();

	int luaPlayerGetHouse();
	int luaPlayerSendHouseWindow();
	int luaPlayerSetEditHouse();

	int luaPlayerSetGhostMode();

	int luaPlayerGetContainerId();
	int luaPlayerGetContainerById();
	int luaPlayerGetContainerIndex();

	int luaPlayerGetInstantSpells();
	int luaPlayerCanCast();

	int luaPlayerHasChaseMode();
	int luaPlayerHasSecureMode();
	int luaPlayerGetFightMode();

	int luaPlayerGetStoreInbox();

	int luaPlayerIsNearDepotBox();

	int luaPlayerGetIdleTime();
	int luaPlayerResetIdleTime();

	int luaPlayerSendCreatureSquare();

	int luaPlayerGetClientExpDisplay();
	int luaPlayerSetClientExpDisplay();

	int luaPlayerGetClientStaminaBonusDisplay();
	int luaPlayerSetClientStaminaBonusDisplay();

	int luaPlayerGetClientLowLevelBonusDisplay();
	int luaPlayerSetClientLowLevelBonusDisplay();

	int luaPlayerSendResourceBalance();

	// Monster
	int luaMonsterCreate();

	int luaMonsterIsMonster();

	int luaMonsterGetId();
	int luaMonsterGetType();

	int luaMonsterRename();

	int luaMonsterGetSpawnPosition();
	int luaMonsterIsInSpawnRange();

	int luaMonsterIsIdle();
	int luaMonsterSetIdle();

	int luaMonsterIsTarget();
	int luaMonsterIsOpponent();
	int luaMonsterIsFriend();

	int luaMonsterAddFriend();
	int luaMonsterRemoveFriend();
	int luaMonsterGetFriendList();
	int luaMonsterGetFriendCount();

	int luaMonsterAddTarget();
	int luaMonsterRemoveTarget();
	int luaMonsterGetTargetList();
	int luaMonsterGetTargetCount();

	int luaMonsterSelectTarget();
	int luaMonsterSearchTarget();

	int luaMonsterIsWalkingToSpawn();
	int luaMonsterWalkToSpawn();

	int luaMonsterHasIcon();
	int luaMonsterSetIcon();
	int luaMonsterGetIcon();
	int luaMonsterRemoveIcon();

	// Npc
	int luaNpcCreate();

	int luaNpcIsNpc();

	int luaNpcSetMasterPos();

	int luaNpcGetSpeechBubble();
	int luaNpcSetSpeechBubble();

	int luaNpcGetSpectators();

	// NpcType
	int luaNpcTypeCreate();
	int luaNpcTypeName();
	int luaNpcTypeOnCallback();
	int luaNpcTypeEventType();
	int luaNpcTypeSpeechBubble();
	int luaNpcTypeWalkTicks();
	int luaNpcTypeBaseSpeed();
	int luaNpcTypeMasterRadius();
	int luaNpcTypeFloorChange();
	int luaNpcTypeAttackable();
	int luaNpcTypeIgnoreHeight();
	int luaNpcTypeIsIdle();
	int luaNpcTypePushable();
	int luaNpcTypeDefaultOutfit();
	int luaNpcTypeParameter();
	int luaNpcTypeHealth();
	int luaNpcTypeMaxHealth();
	int luaNpcTypeSight();

	// Guild
	int luaGuildCreate();

	int luaGuildGetId();
	int luaGuildGetName();
	int luaGuildGetMembersOnline();

	int luaGuildAddRank();
	int luaGuildGetRankById();
	int luaGuildGetRankByLevel();

	int luaGuildGetMotd();
	int luaGuildSetMotd();

	// Group
	int luaGroupCreate();

	int luaGroupGetId();
	int luaGroupGetName();
	int luaGroupGetFlags();
	int luaGroupGetAccess();
	int luaGroupGetMaxDepotItems();
	int luaGroupGetMaxVipEntries();
	int luaGroupHasFlag();

	// Vocation
	int luaVocationCreate();

	int luaVocationGetId();
	int luaVocationGetClientId();
	int luaVocationGetName();
	int luaVocationGetDescription();

	int luaVocationGetRequiredSkillTries();
	int luaVocationGetRequiredManaSpent();

	int luaVocationGetCapacityGain();

	int luaVocationGetHealthGain();
	int luaVocationGetHealthGainTicks();
	int luaVocationGetHealthGainAmount();

	int luaVocationGetManaGain();
	int luaVocationGetManaGainTicks();
	int luaVocationGetManaGainAmount();

	int luaVocationGetMaxSoul();
	int luaVocationGetSoulGainTicks();

	int luaVocationGetAttackSpeed();
	int luaVocationGetBaseSpeed();

	int luaVocationGetDemotion();
	int luaVocationGetPromotion();

	int luaVocationAllowsPvp();

	// Town
	int luaTownCreate();

	int luaTownGetId();
	int luaTownGetName();
	int luaTownGetTemplePosition();

	// House
	int luaHouseCreate();

	int luaHouseGetId();
	int luaHouseGetName();
	int luaHouseGetTown();
	int luaHouseGetExitPosition();

	int luaHouseGetRent();
	int luaHouseSetRent();

	int luaHouseGetPaidUntil();
	int luaHouseSetPaidUntil();

	int luaHouseGetPayRentWarnings();
	int luaHouseSetPayRentWarnings();

	int luaHouseGetOwnerName();
	int luaHouseGetOwnerGuid();
	int luaHouseSetOwnerGuid();
	int luaHouseStartTrade();

	int luaHouseGetBeds();
	int luaHouseGetBedCount();

	int luaHouseGetDoors();
	int luaHouseGetDoorCount();
	int luaHouseGetDoorIdByPosition();

	int luaHouseGetTiles();
	int luaHouseGetItems();
	int luaHouseGetTileCount();

	int luaHouseCanEditAccessList();
	int luaHouseGetAccessList();
	int luaHouseSetAccessList();

	int luaHouseKickPlayer();

	int luaHouseSave();

	// ItemType
	int luaItemTypeCreate();

	int luaItemTypeIsCorpse();
	int luaItemTypeIsDoor();
	int luaItemTypeIsContainer();
	int luaItemTypeIsFluidContainer();
	int luaItemTypeIsMovable();
	int luaItemTypeIsRune();
	int luaItemTypeIsStackable();
	int luaItemTypeIsReadable();
	int luaItemTypeIsWritable();
	int luaItemTypeIsBlocking();
	int luaItemTypeIsGroundTile();
	int luaItemTypeIsMagicField();
	int luaItemTypeIsUseable();
	int luaItemTypeIsPickupable();
	int luaItemTypeIsRotatable();

	int luaItemTypeGetType();
	int luaItemTypeGetGroup();
	int luaItemTypeGetId();
	int luaItemTypeGetClientId();
	int luaItemTypeGetName();
	int luaItemTypeGetPluralName();
	int luaItemTypeGetRotateTo();
	int luaItemTypeGetArticle();
	int luaItemTypeGetDescription();
	int luaItemTypeGetSlotPosition();

	int luaItemTypeGetCharges();
	int luaItemTypeGetFluidSource();
	int luaItemTypeGetCapacity();
	int luaItemTypeGetWeight();
	int luaItemTypeGetWorth();

	int luaItemTypeGetHitChance();
	int luaItemTypeGetShootRange();
	int luaItemTypeGetAttack();
	int luaItemTypeGetAttackSpeed();
	int luaItemTypeGetDefense();
	int luaItemTypeGetExtraDefense();
	int luaItemTypeGetArmor();
	int luaItemTypeGetWeaponType();

	int luaItemTypeGetElementType();
	int luaItemTypeGetElementDamage();

	int luaItemTypeGetTransformEquipId();
	int luaItemTypeGetTransformDeEquipId();
	int luaItemTypeGetDestroyId();
	int luaItemTypeGetDecayId();
	int luaItemTypeGetRequiredLevel();
	int luaItemTypeGetAmmoType();
	int luaItemTypeGetCorpseType();
	int luaItemTypeGetClassification();
	int luaItemTypeHasShowCount();
	int luaItemTypeGetAbilities();
	int luaItemTypeHasShowAttributes();
	int luaItemTypeHasShowCharges();
	int luaItemTypeHasShowDuration();
	int luaItemTypeHasAllowDistRead();
	int luaItemTypeGetWieldInfo();
	int luaItemTypeGetDurationMin();
	int luaItemTypeGetDurationMax();
	int luaItemTypeGetLevelDoor();
	int luaItemTypeGetRuneSpellName();
	int luaItemTypeGetVocationString();
	int luaItemTypeGetMinReqLevel();
	int luaItemTypeGetMinReqMagicLevel();

	int luaItemTypeGetMarketBuyStatistics();
	int luaItemTypeGetMarketSellStatistics();

	int luaItemTypeHasSubType();

	int luaItemTypeIsStoreItem();

	// Combat
	int luaCombatCreate();
	int luaCombatDelete();

	int luaCombatSetParameter();
	int luaCombatGetParameter();

	int luaCombatSetFormula();

	int luaCombatSetArea();
	int luaCombatAddCondition();
	int luaCombatClearConditions();
	int luaCombatSetCallback();
	int luaCombatSetOrigin();

	int luaCombatExecute();

	// Condition
	int luaConditionCreate();
	int luaConditionDelete();

	int luaConditionGetId();
	int luaConditionGetSubId();
	int luaConditionGetType();
	int luaConditionGetIcons();
	int luaConditionGetEndTime();

	int luaConditionClone();

	int luaConditionGetTicks();
	int luaConditionSetTicks();

	int luaConditionSetParameter();
	int luaConditionGetParameter();

	int luaConditionSetFormula();
	int luaConditionSetOutfit();

	int luaConditionAddDamage();

	// Outfit
	int luaOutfitCreate();
	int luaOutfitCompare();

	// MonsterType
	int luaMonsterTypeCreate();

	int luaMonsterTypeIsAttackable();
	int luaMonsterTypeIsChallengeable();
	int luaMonsterTypeIsConvinceable();
	int luaMonsterTypeIsSummonable();
	int luaMonsterTypeIsIgnoringSpawnBlock();
	int luaMonsterTypeIsIllusionable();
	int luaMonsterTypeIsHostile();
	int luaMonsterTypeIsPushable();
	int luaMonsterTypeIsHealthHidden();
	int luaMonsterTypeIsBoss();

	int luaMonsterTypeCanPushItems();
	int luaMonsterTypeCanPushCreatures();

	int luaMonsterTypeCanWalkOnEnergy();
	int luaMonsterTypeCanWalkOnFire();
	int luaMonsterTypeCanWalkOnPoison();

	int luaMonsterTypeName();
	int luaMonsterTypeNameDescription();

	int luaMonsterTypeHealth();
	int luaMonsterTypeMaxHealth();
	int luaMonsterTypeRunHealth();
	int luaMonsterTypeExperience();
	int luaMonsterTypeSkull();

	int luaMonsterTypeCombatImmunities();
	int luaMonsterTypeConditionImmunities();

	int luaMonsterTypeGetAttackList();
	int luaMonsterTypeAddAttack();

	int luaMonsterTypeGetDefenseList();
	int luaMonsterTypeAddDefense();

	int luaMonsterTypeGetElementList();
	int luaMonsterTypeAddElement();

	int luaMonsterTypeGetVoices();
	int luaMonsterTypeAddVoice();

	int luaMonsterTypeGetLoot();
	int luaMonsterTypeAddLoot();

	int luaMonsterTypeGetCreatureEvents();
	int luaMonsterTypeRegisterEvent();

	int luaMonsterTypeEventOnCallback();
	int luaMonsterTypeEventType();

	int luaMonsterTypeGetSummonList();
	int luaMonsterTypeAddSummon();

	int luaMonsterTypeMaxSummons();

	int luaMonsterTypeArmor();
	int luaMonsterTypeDefense();
	int luaMonsterTypeOutfit();
	int luaMonsterTypeRace();
	int luaMonsterTypeCorpseId();
	int luaMonsterTypeManaCost();
	int luaMonsterTypeBaseSpeed();
	int luaMonsterTypeLight();

	int luaMonsterTypeStaticAttackChance();
	int luaMonsterTypeTargetDistance();
	int luaMonsterTypeYellChance();
	int luaMonsterTypeYellSpeedTicks();
	int luaMonsterTypeChangeTargetChance();
	int luaMonsterTypeChangeTargetSpeed();

	int luaMonsterTypeBestiaryInfo();

	// Loot
	int luaCreateLoot();
	int luaDeleteLoot();
	int luaLootSetId();
	int luaLootSetMaxCount();
	int luaLootSetSubType();
	int luaLootSetChance();
	int luaLootSetActionId();
	int luaLootSetDescription();
	int luaLootAddChildLoot();

	// MonsterSpell
	int luaCreateMonsterSpell();
	int luaDeleteMonsterSpell();
	int luaMonsterSpellSetType();
	int luaMonsterSpellSetScriptName();
	int luaMonsterSpellSetChance();
	int luaMonsterSpellSetInterval();
	int luaMonsterSpellSetRange();
	int luaMonsterSpellSetCombatValue();
	int luaMonsterSpellSetCombatType();
	int luaMonsterSpellSetAttackValue();
	int luaMonsterSpellSetNeedTarget();
	int luaMonsterSpellSetNeedDirection();
	int luaMonsterSpellSetCombatLength();
	int luaMonsterSpellSetCombatSpread();
	int luaMonsterSpellSetCombatRadius();
	int luaMonsterSpellSetCombatRing();
	int luaMonsterSpellSetConditionType();
	int luaMonsterSpellSetConditionDamage();
	int luaMonsterSpellSetConditionSpeedChange();
	int luaMonsterSpellSetConditionDuration();
	int luaMonsterSpellSetConditionDrunkenness();
	int luaMonsterSpellSetConditionTickInterval();
	int luaMonsterSpellSetCombatShootEffect();
	int luaMonsterSpellSetCombatEffect();
	int luaMonsterSpellSetOutfit();

	// Party
	int luaPartyCreate();
	int luaPartyDisband();

	int luaPartyGetLeader();
	int luaPartySetLeader();

	int luaPartyGetMembers();
	int luaPartyGetMemberCount();

	int luaPartyGetInvitees();
	int luaPartyGetInviteeCount();

	int luaPartyAddInvite();
	int luaPartyRemoveInvite();

	int luaPartyAddMember();
	int luaPartyRemoveMember();

	int luaPartyIsSharedExperienceActive();
	int luaPartyIsSharedExperienceEnabled();
	int luaPartyIsMemberSharingExp();
	int luaPartyShareExperience();
	int luaPartySetSharedExperience();

	// Spells
	int luaSpellCreate();

	int luaSpellOnCastSpell();
	int luaSpellRegister();
	int luaSpellName();
	int luaSpellId();
	int luaSpellGroup();
	int luaSpellCooldown();
	int luaSpellGroupCooldown();
	int luaSpellLevel();
	int luaSpellMagicLevel();
	int luaSpellMana();
	int luaSpellManaPercent();
	int luaSpellSoul();
	int luaSpellRange();
	int luaSpellPremium();
	int luaSpellEnabled();
	int luaSpellNeedTarget();
	int luaSpellNeedWeapon();
	int luaSpellNeedLearn();
	int luaSpellSelfTarget();
	int luaSpellBlocking();
	int luaSpellAggressive();
	int luaSpellPzLock();
	int luaSpellVocation();

	// only for InstantSpells
	int luaSpellWords();
	int luaSpellNeedDirection();
	int luaSpellHasParams();
	int luaSpellHasPlayerNameParam();
	int luaSpellNeedCasterTargetOrDirection();
	int luaSpellIsBlockingWalls();

	// only for RuneSpells
	int luaSpellRuneLevel();
	int luaSpellRuneMagicLevel();
	int luaSpellRuneId();
	int luaSpellCharges();
	int luaSpellAllowFarUse();
	int luaSpellBlockWalls();
	int luaSpellCheckFloor();

	// Actions
	int luaCreateAction();
	int luaActionOnUse();
	int luaActionRegister();
	int luaActionItemId();
	int luaActionActionId();
	int luaActionUniqueId();
	int luaActionAllowFarUse();
	int luaActionBlockWalls();
	int luaActionCheckFloor();

	// Talkactions
	int luaCreateTalkaction();
	int luaTalkactionOnSay();
	int luaTalkactionRegister();
	int luaTalkactionSeparator();
	int luaTalkactionAccess();
	int luaTalkactionAccountType();

	// CreatureEvents
	int luaCreateCreatureEvent();
	int luaCreatureEventType();
	int luaCreatureEventRegister();
	int luaCreatureEventOnCallback();

	// MoveEvents
	int luaCreateMoveEvent();
	int luaMoveEventType();
	int luaMoveEventRegister();
	int luaMoveEventOnCallback();
	int luaMoveEventLevel();
	int luaMoveEventSlot();
	int luaMoveEventMagLevel();
	int luaMoveEventPremium();
	int luaMoveEventVocation();
	int luaMoveEventTileItem();
	int luaMoveEventItemId();
	int luaMoveEventActionId();
	int luaMoveEventUniqueId();
	int luaMoveEventPosition();

	// GlobalEvents
	int luaCreateGlobalEvent();
	int luaGlobalEventType();
	int luaGlobalEventRegister();
	int luaGlobalEventOnCallback();
	int luaGlobalEventTime();
	int luaGlobalEventInterval();

	// Weapon
	int luaCreateWeapon();
	int luaWeaponId();
	int luaWeaponLevel();
	int luaWeaponMagicLevel();
	int luaWeaponMana();
	int luaWeaponManaPercent();
	int luaWeaponHealth();
	int luaWeaponHealthPercent();
	int luaWeaponSoul();
	int luaWeaponPremium();
	int luaWeaponBreakChance();
	int luaWeaponAction();
	int luaWeaponUnproperly();
	int luaWeaponVocation();
	int luaWeaponOnUseWeapon();
	int luaWeaponRegister();
	int luaWeaponElement();
	int luaWeaponAttack();
	int luaWeaponDefense();
	int luaWeaponRange();
	int luaWeaponCharges();
	int luaWeaponDuration();
	int luaWeaponDecayTo();
	int luaWeaponTransformEquipTo();
	int luaWeaponTransformDeEquipTo();
	int luaWeaponSlotType();
	int luaWeaponHitChance();
	int luaWeaponExtraElement();

	// exclusively for distance weapons
	int luaWeaponMaxHitChance();
	int luaWeaponAmmoType();

	// exclusively for wands
	int luaWeaponWandDamage();

	// exclusively for wands & distance weapons
	int luaWeaponShootType();

	// XML
	int luaCreateXmlDocument();
	int luaDeleteXmlDocument();
	int luaXmlDocumentChild();

	int luaDeleteXmlNode();
	int luaXmlNodeAttribute();
	int luaXmlNodeName();
	int luaXmlNodeFirstChild();
	int luaXmlNodeNextSibling();
};

class LuaEnvironment : public LuaScriptInterface
{
public:
	LuaEnvironment();
	~LuaEnvironment();

	// non-copyable
	LuaEnvironment(const LuaEnvironment&) = delete;
	LuaEnvironment& operator=(const LuaEnvironment&) = delete;

	bool initState() override;
	bool reInitState();
	bool closeState() override;

	LuaScriptInterface* getTestInterface();

	Combat_ptr getCombatObject(uint32_t id) const;
	Combat_ptr createCombatObject(LuaScriptInterface* interface);
	void clearCombatObjects(LuaScriptInterface* interface);

	AreaCombat* getAreaObject(uint32_t id) const;
	uint32_t createAreaObject(LuaScriptInterface* interface);
	void clearAreaObjects(LuaScriptInterface* interface);

private:
	void executeTimerEvent(uint32_t eventIndex);

	std::unordered_map<uint32_t, LuaTimerEventDesc> timerEvents;
	std::unordered_map<uint32_t, Combat_ptr> combatMap;
	std::unordered_map<uint32_t, AreaCombat*> areaMap;

	std::unordered_map<LuaScriptInterface*, std::vector<uint32_t>> combatIdMap;
	std::unordered_map<LuaScriptInterface*, std::vector<uint32_t>> areaIdMap;

	LuaScriptInterface* testInterface = nullptr;

	uint32_t lastEventTimerId = 1;
	uint32_t lastCombatId = 0;
	uint32_t lastAreaId = 0;

	friend class LuaScriptInterface;
	friend class CombatSpell;
};

namespace tfs::lua {

void removeTempItem(Item* item);

ScriptEnvironment* getScriptEnv();
bool reserveScriptEnv();
void resetScriptEnv();

void reportError(std::string_view function, std::string_view error_desc, lua_State* L = nullptr,
                 bool stack_trace = false);
#define reportErrorFunc(L, a) tfs::lua::reportError(__FUNCTION__, a, L, true)

// push/pop common structures
void pushThing(lua_State* L, Thing* thing);
void pushVariant(lua_State* L, const LuaVariant& var);
void pushString(lua_State* L, std::string_view value);
void pushCallback(lua_State* L, int32_t callback);
void pushCylinder(lua_State* L, Cylinder* cylinder);

std::string popString();
int32_t popCallback();

// Userdata
template <class T>
void pushUserdata(lua_State* L, T* value)
{
	T** userdata = static_cast<T**>(lua_newuserdata(L, sizeof(T*)));
	*userdata = value;
}

// Metatables
void setMetatable(lua_State* L, int32_t index, std::string_view name);
void setItemMetatable(lua_State* L, int32_t index, const Item* item);
void setCreatureMetatable(lua_State* L, int32_t index, const Creature* creature);

// Get
template <typename T>
typename std::enable_if_t<std::is_enum_v<T>, T> getNumber(lua_State* L, int32_t arg)
{
	return static_cast<T>(static_cast<int64_t>(lua_tonumber(L, arg)));
}

template <typename T>
typename std::enable_if_t<std::is_integral_v<T> && std::is_unsigned_v<T>, T> getNumber(lua_State* L, int32_t arg)
{
	double num = lua_tonumber(L, arg);
	if (num < static_cast<double>(std::numeric_limits<T>::lowest()) ||
	    num > static_cast<double>(std::numeric_limits<T>::max())) {
		reportErrorFunc(L, fmt::format("Argument {} has out-of-range value for {}: {}", arg, typeid(T).name(), num));
	}

	return static_cast<T>(num);
}

template <typename T>
typename std::enable_if_t<(std::is_integral_v<T> && std::is_signed_v<T>) || std::is_floating_point_v<T>, T> getNumber(
    lua_State* L, int32_t arg)
{
	double num = lua_tonumber(L, arg);
	if (num < static_cast<double>(std::numeric_limits<T>::lowest()) ||
	    num > static_cast<double>(std::numeric_limits<T>::max())) {
		reportErrorFunc(L, fmt::format("Argument {} has out-of-range value for {}: {}", arg, typeid(T).name(), num));
	}

	return static_cast<T>(num);
}

template <typename T>
T getNumber(lua_State* L, int32_t arg, T defaultValue)
{
	if (lua_isnumber(L, arg) == 0) {
		return defaultValue;
	}
	return getNumber<T>(L, arg);
}

template <class T>
T** getRawUserdata(lua_State* L, int32_t arg)
{
	return static_cast<T**>(lua_touserdata(L, arg));
}

template <class T>
T* getUserdata(lua_State* L, int32_t arg)
{
	T** userdata = getRawUserdata<T>(L, arg);
	if (!userdata) {
		return nullptr;
	}
	return *userdata;
}

bool getBoolean(lua_State* L, int32_t arg);
bool getBoolean(lua_State* L, int32_t arg, bool defaultValue);
std::string getString(lua_State* L, int32_t arg);
Position getPosition(lua_State* L, int32_t arg);
Position getPosition(lua_State* L, int32_t arg, int32_t& stackpos);
Thing* getThing(lua_State* L, int32_t arg);
Creature* getCreature(lua_State* L, int32_t arg);
Player* getPlayer(lua_State* L, int32_t arg);

template <typename T>
T getField(lua_State* L, int32_t arg, std::string_view key)
{
	lua_getfield(L, arg, key.data());
	return getNumber<T>(L, -1);
}

template <typename T, typename... Args>
T getField(lua_State* L, int32_t arg, std::string_view key, T&& defaultValue)
{
	lua_getfield(L, arg, key.data());
	return getNumber<T>(L, -1, std::forward<T>(defaultValue));
}

std::string getFieldString(lua_State* L, int32_t arg, std::string_view key);

// Push
void pushBoolean(lua_State* L, bool value);
void pushSpell(lua_State* L, const Spell& spell);
void pushPosition(lua_State* L, const Position& position, int32_t stackpos = 0);
void pushOutfit(lua_State* L, const Outfit_t& outfit);
void pushOutfit(lua_State* L, const Outfit* outfit);

//
int protectedCall(lua_State* L, int nargs, int nresults);
void registerMethod(lua_State* L, std::string_view globalName, std::string_view methodName, lua_CFunction func);
std::string getErrorDesc(ErrorCode_t code);

} // namespace tfs::lua

class LuaContext
{
public:
	LuaContext();
	~LuaContext();

	// non-copyable
	LuaContext(const LuaContext&) = delete;
	LuaContext& operator=(const LuaContext&) = delete;

	bool init();
	void close();

	// push functions
	void push_nil();
	void push_number(lua_Number n);
	void push_integer(lua_Integer n);
	void push_boolean(bool value);

	// get functions (Lua -> stack)
	void get_table(int index);
	void get_field(int index, const char* key);
	void raw_get(int index);
	void raw_geti(int index, int n);
	void create_table(int narr, int nrec);
	void* new_userdata(size_t size);
	bool get_metatable(int objindex);
	void get_fenv(int index);

	template <typename T>
	typename std::enable_if_t<std::is_enum_v<T>, T> get_number(int32_t arg)
	{
		return static_cast<T>(static_cast<int64_t>(lua_tonumber(L, arg)));
	}

	template <typename T>
	typename std::enable_if_t<std::is_integral_v<T> && std::is_unsigned_v<T>, T> get_number(int32_t arg)
	{
		double num = lua_tonumber(L, arg);
		if (num < static_cast<double>(std::numeric_limits<T>::lowest()) ||
		    num > static_cast<double>(std::numeric_limits<T>::max())) {
			reportErrorFunc(L,
			                fmt::format("Argument {} has out-of-range value for {}: {}", arg, typeid(T).name(), num));
		}

		return static_cast<T>(num);
	}

	template <typename T>
	typename std::enable_if_t<(std::is_integral_v<T> && std::is_signed_v<T>) || std::is_floating_point_v<T>, T>
	get_number(int32_t arg)
	{
		double num = lua_tonumber(L, arg);
		if (num < static_cast<double>(std::numeric_limits<T>::lowest()) ||
		    num > static_cast<double>(std::numeric_limits<T>::max())) {
			reportErrorFunc(L,
			                fmt::format("Argument {} has out-of-range value for {}: {}", arg, typeid(T).name(), num));
		}

		return static_cast<T>(num);
	}

	template <typename T>
	T get_number(int32_t arg, T defaultValue)
	{
		if (lua_isnumber(L, arg) == 0) {
			return defaultValue;
		}
		return getNumber<T>(L, arg);
	}

private:
	lua_State* state;
};

#endif // FS_LUASCRIPT_H
