#ifdef LUA_EQEMU

#include "lua_companion.h"

#include "zone/client.h"
#include "zone/companion.h"
#include "zone/entity.h"
#include "zone/lua_client.h"
#include "zone/lua_mob.h"
#include "zone/masterentity.h"

#include "lua.hpp"
#include "luabind/luabind.hpp"

extern EntityList entity_list;

// -------------------------------------------------------
// Identification
// -------------------------------------------------------

uint32 Lua_Companion::GetCompanionID()
{
	Lua_Safe_Call_Int();
	return self->GetCompanionID();
}

uint32 Lua_Companion::GetOwnerCharacterID()
{
	Lua_Safe_Call_Int();
	return self->GetOwnerCharacterID();
}

uint8 Lua_Companion::GetCompanionType()
{
	Lua_Safe_Call_Int();
	return self->GetCompanionType();
}

uint32 Lua_Companion::GetRecruitedNPCTypeID()
{
	Lua_Safe_Call_Int();
	return self->GetRecruitedNPCTypeID();
}

uint8 Lua_Companion::GetStance()
{
	Lua_Safe_Call_Int();
	return self->GetStance();
}

uint32 Lua_Companion::GetCompanionXP()
{
	Lua_Safe_Call_Int();
	return self->GetCompanionXP();
}

uint8 Lua_Companion::GetRecruitedLevel()
{
	Lua_Safe_Call_Int();
	return self->GetRecruitedLevel();
}

uint32 Lua_Companion::GetTimeActive()
{
	Lua_Safe_Call_Int();
	return self->GetTimeActive();
}

uint32 Lua_Companion::GetRecruitedZoneID()
{
	Lua_Safe_Call_Int();
	return self->GetRecruitedZoneID();
}

// -------------------------------------------------------
// Owner
// -------------------------------------------------------

Lua_Client Lua_Companion::GetOwner()
{
	Lua_Safe_Call_Class(Lua_Client);
	// Look up the owning Client from the entity_list by character ID
	Client* owner = entity_list.GetClientByCharID(self->GetOwnerCharacterID());
	return Lua_Client(owner);
}

// -------------------------------------------------------
// Lifecycle
// -------------------------------------------------------

bool Lua_Companion::Suspend()
{
	Lua_Safe_Call_Bool();
	return self->Suspend();
}

bool Lua_Companion::Unsuspend()
{
	Lua_Safe_Call_Bool();
	return self->Unsuspend();
}

void Lua_Companion::Dismiss(bool voluntary)
{
	Lua_Safe_Call_Void();
	self->Dismiss(voluntary);
}

bool Lua_Companion::Save()
{
	Lua_Safe_Call_Bool();
	return self->Save();
}

// -------------------------------------------------------
// Stance and AI
// -------------------------------------------------------

void Lua_Companion::SetStance(int stance)
{
	Lua_Safe_Call_Void();
	self->SetStance(static_cast<uint8>(stance));
}

// -------------------------------------------------------
// Experience
// -------------------------------------------------------

void Lua_Companion::AddExperience(uint32 xp)
{
	Lua_Safe_Call_Void();
	self->AddExperience(xp);
}

uint32 Lua_Companion::GetXPForNextLevel()
{
	Lua_Safe_Call_Int();
	return self->GetXPForNextLevel();
}

// -------------------------------------------------------
// Soul wipe
// -------------------------------------------------------

void Lua_Companion::SoulWipe()
{
	Lua_Safe_Call_Void();
	self->SoulWipe();
}

// -------------------------------------------------------
// Equipment listing / retrieval
// -------------------------------------------------------

void Lua_Companion::ShowEquipment(Lua_Client client)
{
	Lua_Safe_Call_Void();
	self->ShowEquipment(client);
}

void Lua_Companion::GiveSlot(Lua_Client client, std::string slot_name)
{
	Lua_Safe_Call_Void();
	self->GiveSlot(client, slot_name);
}

void Lua_Companion::GiveAll(Lua_Client client)
{
	Lua_Safe_Call_Void();
	self->GiveAll(client);
}

bool Lua_Companion::GiveItem(uint32 item_id, int16 slot)
{
	Lua_Safe_Call_Bool();
	return self->GiveItem(item_id, slot);
}

// -------------------------------------------------------
// Follow / Guard
// -------------------------------------------------------

// SetFollowDistance, SetFollowID, SetFollowCanRun are on Mob (and therefore
// available on Companion via C++ inheritance), but are exposed through Lua_NPC,
// not Lua_Mob. Since Lua_Companion inherits Lua_Mob (not Lua_NPC), they must be
// explicitly added here so Lua scripts can call them on companion objects.

void Lua_Companion::SetFollowDistance(int dist)
{
	Lua_Safe_Call_Void();
	self->SetFollowDistance(static_cast<uint32>(dist));
}

void Lua_Companion::SetFollowID(int id)
{
	Lua_Safe_Call_Void();
	self->SetFollowID(static_cast<uint32>(id));
}

void Lua_Companion::SetFollowCanRun(bool v)
{
	Lua_Safe_Call_Void();
	self->SetFollowCanRun(v);
}

// SetGuardMode(true)  — stop following and hold current position via NPC guard point.
// SetGuardMode(false) — clear guard point and resume following the companion's owner.
//
// NPC::SaveGuardSpot(false) sets m_GuardPoint to current position, making
// IsGuarding() return true so NPC::AI_DoMovement() holds position.
// SaveGuardSpot(true) clears m_GuardPoint (sets it to zero-vec), cancelling guard.
void Lua_Companion::SetGuardMode(bool enabled)
{
	Lua_Safe_Call_Void();
	if (enabled) {
		// Hold current position: set guard point and stop following
		self->SaveGuardSpot(false);           // false = set guard (not clear)
		self->SetFollowID(0);
		self->StopMoving();
	} else {
		// Resume following: clear guard point and re-attach to owner
		self->SaveGuardSpot(true);            // true = clear guard point
		Client* owner = entity_list.GetClientByCharID(self->GetOwnerCharacterID());
		if (owner) {
			self->SetFollowID(owner->GetID());
			self->SetFollowDistance(100);
			self->SetFollowCanRun(true);
		}
	}
}

// -------------------------------------------------------
// Registration
// -------------------------------------------------------

luabind::scope lua_register_companion() {
	return luabind::class_<Lua_Companion, Lua_Mob>("Companion")
	.def(luabind::constructor<>())
	.def("AddExperience",          &Lua_Companion::AddExperience)
	.def("Dismiss",                &Lua_Companion::Dismiss)
	.def("GetCompanionID",         &Lua_Companion::GetCompanionID)
	.def("GetCompanionType",       &Lua_Companion::GetCompanionType)
	.def("GetCompanionXP",         &Lua_Companion::GetCompanionXP)
	.def("GetOwner",               &Lua_Companion::GetOwner)
	.def("GetOwnerCharacterID",    &Lua_Companion::GetOwnerCharacterID)
	.def("GetRecruitedLevel",      &Lua_Companion::GetRecruitedLevel)
	.def("GetRecruitedNPCTypeID",  &Lua_Companion::GetRecruitedNPCTypeID)
	.def("GetRecruitedZoneID",     &Lua_Companion::GetRecruitedZoneID)
	.def("GetStance",              &Lua_Companion::GetStance)
	.def("GetTimeActive",          &Lua_Companion::GetTimeActive)
	.def("GetXPForNextLevel",      &Lua_Companion::GetXPForNextLevel)
	.def("GiveAll",                &Lua_Companion::GiveAll)
	.def("GiveItem",               &Lua_Companion::GiveItem)
	.def("GiveSlot",               &Lua_Companion::GiveSlot)
	.def("Save",                   &Lua_Companion::Save)
	.def("SetFollowCanRun",        &Lua_Companion::SetFollowCanRun)
	.def("SetFollowDistance",      &Lua_Companion::SetFollowDistance)
	.def("SetFollowID",            &Lua_Companion::SetFollowID)
	.def("SetGuardMode",           &Lua_Companion::SetGuardMode)
	.def("SetStance",              &Lua_Companion::SetStance)
	.def("ShowEquipment",          &Lua_Companion::ShowEquipment)
	.def("SoulWipe",               &Lua_Companion::SoulWipe)
	.def("Suspend",                &Lua_Companion::Suspend)
	.def("Unsuspend",              &Lua_Companion::Unsuspend);
}

#endif
