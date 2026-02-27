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
	// Experience
	// -------------------------------------------------------
	void AddExperience(uint32 xp);

	// -------------------------------------------------------
	// Soul wipe (permanent death + ChromaDB clear signal)
	// -------------------------------------------------------
	void SoulWipe();

	// -------------------------------------------------------
	// Equipment listing / retrieval
	// -------------------------------------------------------
	void ShowEquipment(Lua_Client client);
	void GiveSlot(Lua_Client client, std::string slot_name);
	void GiveAll(Lua_Client client);
};

#endif // LUA_EQEMU
