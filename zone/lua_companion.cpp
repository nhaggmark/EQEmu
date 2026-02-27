#ifdef LUA_EQEMU

#include "lua_companion.h"

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
	.def("GetStance",              &Lua_Companion::GetStance)
	.def("GiveAll",                &Lua_Companion::GiveAll)
	.def("GiveSlot",               &Lua_Companion::GiveSlot)
	.def("Save",                   &Lua_Companion::Save)
	.def("SetStance",              &Lua_Companion::SetStance)
	.def("ShowEquipment",          &Lua_Companion::ShowEquipment)
	.def("SoulWipe",               &Lua_Companion::SoulWipe)
	.def("Suspend",                &Lua_Companion::Suspend)
	.def("Unsuspend",              &Lua_Companion::Unsuspend);
}

#endif
