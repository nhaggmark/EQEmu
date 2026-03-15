#pragma once

#ifdef LUA_EQEMU

#include "zone/lua_mob.h"

#include <string>

class Companion;
class Lua_Client;
class Lua_Mob;
class Lua_NPC;

namespace luabind {
	struct scope;
}

luabind::scope lua_register_companion();

class Lua_Companion : public Lua_Mob
{
	typedef Companion NativeType;
public:
	Lua_Companion() { SetLuaPtrData(nullptr); }
	Lua_Companion(Companion *d) { SetLuaPtrData(reinterpret_cast<Entity*>(d)); }
	virtual ~Lua_Companion() { }

	operator Companion*() {
		return reinterpret_cast<Companion*>(GetLuaPtrData());
	}

	// -------------------------------------------------------
	// Identification
	// -------------------------------------------------------
	uint32 GetCompanionID();
	uint32 GetOwnerCharacterID();
	uint8  GetCompanionType();   // 0=companion, 1=mercenary
	uint32 GetRecruitedNPCTypeID();
	uint8  GetStance();          // 0=passive, 1=balanced, 2=aggressive
	uint32 GetCompanionXP();
	uint8  GetRecruitedLevel();
	uint32 GetTimeActive();      // cumulative seconds active (live, includes current session)
	uint32 GetRecruitedZoneID(); // zone ID of first zone visited (recruited zone)

	// -------------------------------------------------------
	// Owner
	// -------------------------------------------------------
	Lua_Client GetOwner();

	// -------------------------------------------------------
	// Lifecycle
	// -------------------------------------------------------
	bool   Suspend();
	bool   Unsuspend();
	void   Dismiss(bool voluntary);
	bool   Save();

	// -------------------------------------------------------
	// Stance and AI
	// -------------------------------------------------------
	void SetStance(int stance);  // 0=passive, 1=balanced, 2=aggressive

	// -------------------------------------------------------
	// Follow / Guard (exposed from NPC/Mob base on Lua_Companion
	// because Lua_Companion does not inherit Lua_NPC)
	// -------------------------------------------------------
	void SetFollowDistance(int dist);
	void SetFollowID(int id);
	void SetFollowCanRun(bool v);
	int  GetFollowID();
	int  GetFollowDistance();
	bool GetFollowCanRun();
	bool IsGuarding();
	// SetGuardMode(true)  — hold current position (clears follow ID, sets guard point)
	// SetGuardMode(false) — resume following owner (clears guard point, restores follow ID)
	void SetGuardMode(bool enabled);

	// -------------------------------------------------------
	// NPC methods exposed on Lua_Companion
	// (Lua_Companion inherits Lua_Mob, not Lua_NPC, so methods
	//  registered on Lua_NPC are not inherited. These must be
	//  explicitly re-registered here.)
	// -------------------------------------------------------
	int  GetPrimaryFaction();

	// -------------------------------------------------------
	// Experience
	// -------------------------------------------------------
	void   AddExperience(uint32 xp);
	uint32 GetXPForNextLevel();

	// -------------------------------------------------------
	// Soul wipe (permanent death + ChromaDB clear signal)
	// -------------------------------------------------------
	void SoulWipe();

	// -------------------------------------------------------
	// Combat stats (exposed from NPC base on Lua_Companion
	// because Lua_Companion does not inherit Lua_NPC)
	// -------------------------------------------------------
	uint32 GetMinDMG();
	uint32 GetMaxDMG();
	uint8  GetCombatRole();

	// -------------------------------------------------------
	// Equipment listing / retrieval
	// -------------------------------------------------------
	void ShowEquipment(Lua_Client client);
	void GiveSlot(Lua_Client client, std::string slot_name);
	void GiveAll(Lua_Client client);
	// Equip item into companion slot (slot = EQ::invslot::EQUIPMENT slot ID 0-22)
	bool GiveItem(uint32 item_id, int16 slot);
	// Returns item ID equipped in the given slot, or 0 if the slot is empty
	uint32 GetEquipment(int slot);
};

#endif // LUA_EQEMU
