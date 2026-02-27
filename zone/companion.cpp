/*	EQEMu: Everquest Server Emulator
	Copyright (C) 2001-2026 EQEMu Development Team (http://eqemulator.org)

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; version 2 of the License.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY except by those people which sell it, which
	are required to give you total support for your newly bought product;
	without even the implied warranty of MERCHANTABILITY or FITNESS FOR
	A PARTICULAR PURPOSE. See the GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "companion.h"

#include "common/data_bucket.h"
#include "common/repositories/companion_data_repository.h"
#include "common/repositories/companion_buffs_repository.h"
#include "common/repositories/companion_inventories_repository.h"
#include "common/rulesys.h"
#include "common/skill_caps.h"
#include "zone/client.h"
#include "zone/corpse.h"
#include "zone/entity.h"
#include "zone/groups.h"
#include "zone/mob.h"
#include "zone/quest_parser_collection.h"
#include "zone/string_ids.h"
#include "zone/zone.h"

#include <fmt/format.h>

extern volatile bool is_zone_loaded;

// ============================================================
// Constructor / Destructor
// ============================================================

Companion::Companion(const NPCType* d, float x, float y, float z, float heading,
                     uint32 owner_char_id, uint8 companion_type)
	: NPC(d, nullptr, glm::vec4(x, y, z, heading), GravityBehavior::Water, false),
	  m_evade_timer(500),
	  m_retention_check_timer(RuleI(Companions, MercRetentionCheckS) * 1000),
	  m_death_despawn_timer(RuleI(Companions, DeathDespawnS) * 1000),
	  m_replacement_spawn_timer(RuleI(Companions, ReplacementSpawnDelayS) * 1000)
{
	// Identity
	m_companion_id          = 0;
	m_owner_char_id         = owner_char_id;
	m_recruited_npc_type_id = d->npc_id;
	m_companion_type        = companion_type;
	m_current_stance        = COMPANION_STANCE_BALANCED;
	m_suspended             = false;
	m_depop                 = false;
	m_recruited_level       = d->level;

	// Spawn origin (set after recruitment, used for replacement NPC)
	m_spawn2_id    = 0;
	m_spawngroupid = 0;

	// XP / history
	m_companion_xp = 0;
	m_total_kills  = 0;
	m_times_died   = 0;
	m_time_active  = 0;
	m_zones_visited = "[]";
	m_active_since = static_cast<uint32>(time(nullptr));

	// Store base stats for later scaling
	m_base_str  = d->STR;
	m_base_sta  = d->STA;
	m_base_dex  = d->DEX;
	m_base_agi  = d->AGI;
	m_base_int  = d->INT;
	m_base_wis  = d->WIS;
	m_base_cha  = d->CHA;
	m_base_ac   = d->AC;
	m_base_atk  = d->ATK;
	m_base_mr   = d->MR;
	m_base_fr   = d->FR;
	m_base_dr   = d->DR;
	m_base_pr   = d->PR;
	m_base_cr   = d->CR;
	m_base_hp   = d->max_hp;
	m_base_mana = d->Mana;

	memset(m_equipment, 0, sizeof(m_equipment));

	// Disable retention check for companion-type (only mercs need it)
	if (m_companion_type == COMPANION_TYPE_COMPANION) {
		m_retention_check_timer.Disable();
	}

	// Disable death despawn timer until death occurs
	m_death_despawn_timer.Disable();
	m_replacement_spawn_timer.Disable();

	// Initialize AI based on class
	if (GetClass() == Class::Rogue) {
		m_evade_timer.Start();
	} else {
		m_evade_timer.Disable();
	}

	// Apply global stat scale percentage
	ApplyStatScalePct();

	CalcBonuses();
}

Companion::~Companion()
{
	// entity_list and group cleanup happens in Depop()
}

// ============================================================
// Factory method: create a Companion from a live world NPC
// ============================================================

Companion* Companion::CreateFromNPC(Client* owner, NPC* source_npc)
{
	if (!owner || !source_npc) {
		return nullptr;
	}

	if (!RuleB(Companions, CompanionsEnabled)) {
		return nullptr;
	}

	// Load the NPCType from the database for this NPC
	// We need to fetch fresh data so we have a stable copy independent of the
	// source NPC's lifecycle. content_db.LoadNPCTypesData returns a heap-allocated NPCType.
	const NPCType* npc_type_data = content_db.LoadNPCTypesData(source_npc->GetNPCTypeID());
	if (!npc_type_data) {
		owner->Message(Chat::Red, "Unable to load NPC data for recruitment.");
		return nullptr;
	}

	auto pos = source_npc->GetPosition();

	// Check for an existing dismissed companion record (re-recruitment path).
	// If the player previously dismissed this NPC voluntarily, restore its full
	// state (level, XP, equipment, buffs) rather than starting fresh.
	auto existing = CompanionDataRepository::GetWhere(
		database,
		fmt::format(
			"owner_id = {} AND npc_type_id = {} AND is_dismissed = 1 LIMIT 1",
			owner->CharacterID(),
			source_npc->GetNPCTypeID()
		)
	);

	if (!existing.empty()) {
		// Re-recruitment: restore saved companion state
		Companion* companion = new Companion(
			npc_type_data,
			pos.x, pos.y, pos.z, pos.w,
			owner->CharacterID(),
			existing[0].companion_type
		);

		if (!companion) {
			return nullptr;
		}

		if (!companion->Load(existing[0].id)) {
			LogError("Companion::CreateFromNPC: Load() failed for re-recruitment id [{}]", existing[0].id);
			delete companion;
			return nullptr;
		}

		// Clear dismissed flag and activate — companion is active again.
		// is_dismissed has no C++ member; update directly to avoid Save() overwriting
		// other fields with stale data before the companion is fully initialized.
		companion->m_suspended = false;
		database.QueryDatabase(
			fmt::format(
				"UPDATE `companion_data` SET `is_dismissed` = 0, `is_suspended` = 0 WHERE `id` = {}",
				existing[0].id
			)
		);

		LogInfo(
			"Companion::CreateFromNPC: re-recruiting companion id [{}] '{}' for player '{}'",
			existing[0].id, existing[0].name, owner->GetName()
		);
		return companion;
	}

	// Fresh recruitment: create new companion record
	Companion* companion = new Companion(
		npc_type_data,
		pos.x, pos.y, pos.z, pos.w,
		owner->CharacterID(),
		COMPANION_TYPE_COMPANION
	);

	if (!companion) {
		return nullptr;
	}

	companion->SetRecruitedNPCTypeID(source_npc->GetNPCTypeID());
	companion->SetSpawn2ID(source_npc->GetSpawnPointID());

	if (source_npc->GetSpawn()) {
		companion->SetSpawnGroupID(source_npc->GetSpawnGroupId());
	}

	// Store base stats for future scaling
	companion->StoreBaseStats();

	return companion;
}

// ============================================================
// Base stat storage (called once at recruitment)
// ============================================================

void Companion::StoreBaseStats()
{
	// Store raw Mob member fields (without item/spell bonuses) as base stats.
	// Called once at recruitment; used as reference for future level scaling.
	m_recruited_level = GetLevel();
	m_base_str  = STR;
	m_base_sta  = STA;
	m_base_dex  = DEX;
	m_base_agi  = AGI;
	m_base_int  = INT;
	m_base_wis  = WIS;
	m_base_cha  = CHA;
	m_base_ac   = AC;
	m_base_atk  = ATK;
	m_base_mr   = MR;
	m_base_fr   = FR;
	m_base_dr   = DR;
	m_base_pr   = PR;
	m_base_cr   = CR;
	m_base_hp   = max_hp;
	m_base_mana = max_mana;
}

// ============================================================
// Stat scaling: scale from recruited_level to current_level
// Uses float division to avoid integer truncation (per architecture)
// ============================================================

void Companion::ScaleStatsToLevel(uint8 current_level)
{
	if (m_recruited_level == 0 || current_level == 0) {
		return;
	}

	// Float division per architecture spec to avoid integer truncation
	float scale = (float)current_level / (float)m_recruited_level;

	SetLevel(current_level);

	// Scale base stats directly (Mob member fields)
	STR   = (int32)(m_base_str  * scale);
	STA   = (int32)(m_base_sta  * scale);
	DEX   = (int32)(m_base_dex  * scale);
	AGI   = (int32)(m_base_agi  * scale);
	INT   = (int32)(m_base_int  * scale);
	WIS   = (int32)(m_base_wis  * scale);
	CHA   = (int32)(m_base_cha  * scale);
	AC    = (int)(m_base_ac     * scale);
	ATK   = (int32)(m_base_atk  * scale);
	MR    = (int32)(m_base_mr   * scale);
	FR    = (int32)(m_base_fr   * scale);
	DR    = (int32)(m_base_dr   * scale);
	PR    = (int32)(m_base_pr   * scale);
	CR    = (int32)(m_base_cr   * scale);
	max_hp   = (int64)(m_base_hp   * scale);
	base_hp  = max_hp;
	max_mana = (int64)(m_base_mana * scale);

	CalcBonuses();
}

void Companion::ApplyStatScalePct()
{
	int stat_scale_pct = RuleI(Companions, StatScalePct);
	if (stat_scale_pct == 100) {
		return;
	}

	float scale = (float)stat_scale_pct / 100.0f;

	STR   = (int32)(STR   * scale);
	STA   = (int32)(STA   * scale);
	DEX   = (int32)(DEX   * scale);
	AGI   = (int32)(AGI   * scale);
	INT   = (int32)(INT   * scale);
	WIS   = (int32)(WIS   * scale);
	CHA   = (int32)(CHA   * scale);
	AC    = (int)(AC      * scale);
	ATK   = (int32)(ATK   * scale);
	MR    = (int32)(MR    * scale);
	FR    = (int32)(FR    * scale);
	DR    = (int32)(DR    * scale);
	PR    = (int32)(PR    * scale);
	CR    = (int32)(CR    * scale);
	max_hp   = (int64)(max_hp   * scale);
	base_hp  = max_hp;
	max_mana = (int64)(max_mana * scale);
}

// ============================================================
// Entity virtual overrides
// ============================================================

bool Companion::Death(Mob* killer_mob, int64 damage, uint16 spell_id,
                      EQ::skills::SkillType attack_skill,
                      KilledByTypes killed_by, bool is_buff_tic)
{
	// Let the base NPC death handling run first (creates corpse, etc.)
	bool result = NPC::Death(killer_mob, damage, spell_id, attack_skill, killed_by, is_buff_tic);

	// Save companion HP as 0 so resurrection is possible
	Client* owner = GetCompanionOwner();
	if (owner) {
		// Notify owner of companion death
		owner->Message(Chat::Red,
			"%s has fallen in battle! You have %d seconds to resurrect them, "
			"or they will return home.",
			GetCleanName(),
			RuleI(Companions, DeathDespawnS));

		// Mark companion as suspended (dead) in DB
		SetSuspended(true);
		m_times_died++;
		UpdateTimeActive(); // stop accruing active time at death
		Save();

		// Start the despawn timer — if not resurrected, auto-dismiss
		m_death_despawn_timer.Start();
	}

	return result;
}

void Companion::Damage(Mob* from, int64 damage, uint16 spell_id,
                       EQ::skills::SkillType attack_skill, bool avoidable,
                       int8 buffslot, bool iBuffTic,
                       eSpecialAttacks special)
{
	// Check self-preservation behavior
	if (ShouldUseDefensiveBehavior() && from) {
		// Switch to passive stance to disengage from combat
		if (m_current_stance == COMPANION_STANCE_AGGRESSIVE) {
			SetStance(COMPANION_STANCE_BALANCED);
		}
	}

	NPC::Damage(from, damage, spell_id, attack_skill, avoidable, buffslot, iBuffTic, special);
}

bool Companion::Attack(Mob* other, int Hand, bool FromRiposte, bool IsStrikethrough,
                       bool IsFromSpell, ExtraAttackOptions* opts)
{
	return NPC::Attack(other, Hand, FromRiposte, IsStrikethrough, IsFromSpell, opts);
}

// ============================================================
// AI Overrides
// ============================================================

void Companion::AI_Init()
{
	NPC::AI_Init();
}

void Companion::AI_Start(uint32 iMoveDelay)
{
	NPC::AI_Start(iMoveDelay);

	// Load companion-specific spell list
	LoadCompanionSpells();
}

void Companion::AI_Stop()
{
	NPC::AI_Stop();
}

bool Companion::Process()
{
	// Check death despawn timer
	if (m_death_despawn_timer.Enabled() && m_death_despawn_timer.Check()) {
		LogInfo("Companion [{}] death despawn timer fired — auto-dismissing", GetName());
		// Permanent death — soul wipe for unresurrected companions
		Client* owner = GetCompanionOwner();
		if (owner) {
			owner->Message(Chat::Yellow,
				"%s has been lost forever. They waited too long to be resurrected.",
				GetCleanName());
		}
		// Trigger soul wipe on permanent death
		SoulWipe();
		return false;
	}

	// Check mercenary retention
	if (m_retention_check_timer.Enabled() && m_retention_check_timer.Check()) {
		CheckMercenaryRetention();
	}

	// Check replacement NPC spawn timer
	if (m_replacement_spawn_timer.Enabled() && m_replacement_spawn_timer.Check()) {
		// Spawn a generic replacement NPC at the original spawn point
		// This is handled by the zone's spawn system — we just signal readiness
		m_replacement_spawn_timer.Disable();
	}

	// Follow owner or engage owner's target depending on stance
	Client* owner = GetCompanionOwner();
	if (owner && m_current_stance != COMPANION_STANCE_PASSIVE) {
		// If owner is in combat and we don't have a target, pick up owner's target
		if (owner->GetTarget() && owner->GetTarget()->IsAttackAllowed(this)) {
			if (!GetTarget() || GetTarget() == owner) {
				SetTarget(owner->GetTarget());
			}
		}
	}

	return NPC::Process();
}

bool Companion::AI_EngagedCastCheck()
{
	// Delegate to the companion spell AI in companion_ai.cpp
	return AICastSpell(GetChanceToCastBySpellType(0), 0xFFFFFFFF);
}

bool Companion::AI_IdleCastCheck()
{
	// Only buff/heal when idle
	return AICastSpell(GetChanceToCastBySpellType(0), SpellType_Buff | SpellType_Heal | SpellType_Pet);
}

bool Companion::AICastSpell(int8 iChance, uint32 iSpellTypes)
{
	// Full implementation is in companion_ai.cpp
	// This base version falls back to NPC spell AI
	return NPC::AI_EngagedCastCheck();
}

bool Companion::AIDoSpellCast(uint16 spellid, Mob* tar, int32 mana_cost,
                              uint32* oDontDoAgainBefore)
{
	return CastSpell(spellid, tar ? tar->GetID() : 0, EQ::spells::CastingSlot::Gem2,
	                 -1, mana_cost, oDontDoAgainBefore);
}

// ============================================================
// FillSpawnStruct
// ============================================================

void Companion::FillSpawnStruct(NewSpawn_Struct* ns, Mob* ForWho)
{
	NPC::FillSpawnStruct(ns, ForWho);
	// Mark as NPC spawn type — Titanium doesn't have a special companion spawn type
	// The companion appears as a regular NPC in the group window
}

// ============================================================
// Lifecycle: Spawn
// ============================================================

bool Companion::Spawn(Client* owner)
{
	if (!owner) {
		return false;
	}

	m_owner_char_id = owner->CharacterID();

	// Add to entity list — this assigns the entity ID and sends spawn packet
	entity_list.AddCompanion(this, true, true);

	if (!GetID()) {
		LogError("Companion::Spawn: failed to get entity ID for [{}]", GetName());
		return false;
	}

	LogInfo("Companion [{}] spawned for owner [{}] (entity id: {})",
	        GetName(), owner->GetName(), GetID());

	return true;
}

// ============================================================
// Lifecycle: Suspend (save to DB, depop from zone)
// ============================================================

bool Companion::Suspend()
{
	SetSuspended(true);
	UpdateTimeActive(); // accrue elapsed seconds before saving

	if (!Save()) {
		LogError("Companion::Suspend: Save() failed for companion id [{}]", m_companion_id);
	}

	if (SaveBuffs()) {
		// Buffs saved
	}

	Depop();

	return true;
}

// ============================================================
// Lifecycle: Unsuspend (restore from DB, spawn in new zone)
// ============================================================

bool Companion::Unsuspend(bool set_max_stats)
{
	if (!GetID()) {
		// Not yet in entity list — caller must spawn first
		return false;
	}

	SetSuspended(false);
	LoadBuffs();

	if (CompanionJoinClientGroup()) {
		if (set_max_stats) {
			RestoreHealth();
			RestoreMana();
		}
	}

	LogInfo("Companion [{}] unsuspended for owner char id [{}]", GetName(), m_owner_char_id);
	return true;
}

// ============================================================
// Lifecycle: Zone (called when owner zones out)
// ============================================================

void Companion::Zone()
{
	UpdateTimeActive(); // accrue elapsed seconds before saving
	if (zone) {
		RecordZoneVisit(zone->GetZoneID());
	}
	Save();
	Depop();
}

// ============================================================
// Lifecycle: Depop
// ============================================================

void Companion::Depop(bool start_spawn_timer)
{
	WipeHateList();

	if (IsCasting()) {
		InterruptSpell();
	}

	entity_list.RemoveFromHateLists(this);

	if (GetGroup()) {
		RemoveCompanionFromGroup(this, GetGroup());
	}

	entity_list.RemoveCompanion(GetID());

	if (HasPet()) {
		GetPet()->Depop();
	}

	m_depop = true;

	NPC::Depop(false);
}

// ============================================================
// Lifecycle: Dismiss
// ============================================================

void Companion::Dismiss(bool permanent)
{
	Client* owner = GetCompanionOwner();

	if (permanent) {
		// Permanent dismissal: delete DB record, wipe soul
		SoulWipe();
	} else {
		// Voluntary dismissal: mark suspended and preserve state
		// The +10% re-recruitment bonus is tracked via companion_data.dismissed_at
		SetSuspended(true);
		Save();
	}

	// Notify owner
	if (owner) {
		if (permanent) {
			owner->Message(Chat::Yellow,
				"%s bids you farewell forever.", GetCleanName());
		} else {
			owner->Message(Chat::White,
				"%s says 'Farewell for now. You know where to find me.'",
				GetCleanName());
		}

		// TODO (Task 9): call owner->RemoveCompanion(this) once client.h
		// companion tracking is implemented.
	}

	Depop();
}

// ============================================================
// Lifecycle: ProcessClientZoneChange
// ============================================================

void Companion::ProcessClientZoneChange(Client* companion_owner)
{
	if (companion_owner) {
		Zone();
	}
}

// ============================================================
// Group management
// ============================================================

bool Companion::CompanionJoinClientGroup()
{
	Client* owner = GetCompanionOwner();
	if (!owner) {
		Suspend();
		return false;
	}

	if (!GetID()) {
		return false;
	}

	if (HasGroup()) {
		RemoveCompanionFromGroup(this, GetGroup());
	}

	Group* g = entity_list.GetGroupByClient(owner);

	if (!g) {
		// No group exists — create one with the owner
		g = new Group(owner);
		if (!g) {
			return false;
		}

		entity_list.AddGroup(g);

		if (g->GetID() == 0) {
			delete g;
			return false;
		}

		if (AddCompanionToGroup(this, g)) {
			g->AddToGroup(owner);
			database.SetGroupLeaderName(g->GetID(), owner->GetName());
			database.RefreshGroupFromDB(owner);
			g->SaveGroupLeaderAA();
			LogInfo("Companion [{}] joined new group with [{}]", GetName(), owner->GetName());
		} else {
			g->DisbandGroup();
			Suspend();
			LogInfo("Companion [{}] failed to join new group — suspending", GetName());
		}
	} else if (AddCompanionToGroup(this, owner->GetGroup())) {
		database.RefreshGroupFromDB(owner);
		GetGroup()->SendGroupJoinOOZ(this);
		LogInfo("Companion [{}] joined existing group with [{}]", GetName(), owner->GetName());
	} else {
		Suspend();
		LogInfo("Companion [{}] failed to join existing group — suspending", GetName());
	}

	return true;
}

bool Companion::AddCompanionToGroup(Companion* companion, Group* group)
{
	if (!companion || !group) {
		return false;
	}

	if (companion->HasGroup()) {
		if (companion->GetGroup() == group && companion->GetCompanionOwner()) {
			companion->SetFollowID(companion->GetCompanionOwner()->GetID());
			return true;
		}
		RemoveCompanionFromGroup(companion, companion->GetGroup());
	}

	if (group->AddMember(companion) && companion->GetCompanionOwner()) {
		companion->SetFollowID(companion->GetCompanionOwner()->GetID());
		return true;
	}

	return false;
}

bool Companion::RemoveCompanionFromGroup(Companion* companion, Group* group)
{
	if (!companion || !group) {
		return false;
	}

	if (!companion->HasGroup()) {
		return false;
	}

	if (!group->IsLeader(companion)) {
		companion->SetFollowID(0);

		if (group->GroupCount() <= 2 && companion->GetGroup() == group && is_zone_loaded) {
			group->DisbandGroup();
		} else if (group->DelMember(companion, true)) {
			if (companion->GetOwnerCharacterID() != 0) {
				Group::RemoveFromGroup(companion);
			}
		}
	} else {
		// Companion is group leader — disband and re-group
		for (int i = 0; i < MAX_GROUP_MEMBERS; i++) {
			if (!group->members[i])
				continue;
			if (!group->members[i]->IsClient())
				continue;
			Client* member = group->members[i]->CastToClient();
			member->LeaveGroup();
		}

		uint32 group_id = group->GetID();
		Group* old_group = entity_list.GetGroupByID(group_id);
		if (old_group) {
			old_group->DisbandGroup();
		}
	}

	return true;
}

void Companion::CompanionGroupSay(Mob* speaker, const char* msg, ...)
{
	if (!speaker || !msg) {
		return;
	}

	va_list args;
	va_start(args, msg);
	char buf[4096];
	vsnprintf(buf, sizeof(buf), msg, args);
	va_end(args);

	Group* g = entity_list.GetGroupByMob(speaker);
	if (g) {
		g->GroupMessage(speaker, Language::CommonTongue, Language::MaxValue, buf);
	}
}

// ============================================================
// Persistence: Save
// ============================================================

bool Companion::Save()
{
	if (!m_owner_char_id) {
		return false;
	}

	auto cd = CompanionDataRepository::NewEntity();
	cd.owner_id        = m_owner_char_id;
	cd.npc_type_id     = m_recruited_npc_type_id;
	cd.name            = GetCleanName();
	cd.companion_type  = m_companion_type;
	cd.level           = GetLevel();
	cd.class_id        = GetClass();
	cd.race_id         = GetRace();
	cd.gender          = GetGender();
	cd.zone_id         = zone ? zone->GetZoneID() : 0;
	cd.x               = GetX();
	cd.y               = GetY();
	cd.z               = GetZ();
	cd.heading         = GetHeading();
	cd.cur_hp          = GetHP();
	cd.cur_mana        = GetMana();
	cd.is_suspended    = m_suspended ? 1 : 0;
	cd.stance          = m_current_stance;
	cd.spawn2_id       = m_spawn2_id;
	cd.spawngroupid    = m_spawngroupid;
	cd.experience      = m_companion_xp;
	cd.recruited_level = m_recruited_level;
	cd.total_kills     = static_cast<uint32_t>(m_total_kills);
	cd.zones_visited   = m_zones_visited;
	cd.time_active     = m_time_active;
	cd.times_died      = m_times_died;

	if (m_companion_id == 0) {
		// New companion — insert
		auto inserted = CompanionDataRepository::InsertOne(database, cd);
		if (inserted.id > 0) {
			m_companion_id = inserted.id;
			LogInfo("Companion [{}] saved (new id: {})", GetName(), m_companion_id);
			return true;
		}
		LogError("Companion [{}] failed to insert into companion_data", GetName());
		return false;
	} else {
		// Existing companion — update
		cd.id = m_companion_id;
		bool ok = CompanionDataRepository::UpdateOne(database, cd);
		if (!ok) {
			LogError("Companion [{}] failed to update companion_data id [{}]",
			         GetName(), m_companion_id);
		}
		return ok;
	}
}

bool Companion::Load(uint32 companion_id)
{
	auto cd = CompanionDataRepository::FindOne(database, companion_id);
	if (cd.id == 0) {
		LogError("Companion::Load: no record found for companion_id [{}]", companion_id);
		return false;
	}

	m_companion_id          = cd.id;
	m_owner_char_id         = cd.owner_id;
	m_recruited_npc_type_id = cd.npc_type_id;
	m_companion_type        = cd.companion_type;
	m_current_stance        = cd.stance;
	m_suspended             = (cd.is_suspended == 1);
	m_spawn2_id             = cd.spawn2_id;
	m_spawngroupid          = cd.spawngroupid;
	m_companion_xp          = static_cast<uint32>(cd.experience);
	m_recruited_level       = cd.recruited_level;
	m_total_kills           = cd.total_kills;
	m_zones_visited         = cd.zones_visited.empty() ? "[]" : cd.zones_visited;
	m_time_active           = cd.time_active;
	m_times_died            = cd.times_died;
	m_active_since          = static_cast<uint32>(time(nullptr));

	// Restore HP/mana if not suspended at max
	if (cd.cur_hp > 0) {
		SetHP(cd.cur_hp);
	}
	if (cd.cur_mana > 0) {
		SetMana(cd.cur_mana);
	}

	return true;
}

// ============================================================
// Persistence: Buffs
// ============================================================

bool Companion::SaveBuffs()
{
	if (m_companion_id == 0) {
		return false;
	}

	// Delete old buff rows
	CompanionBuffsRepository::DeleteWhere(database,
		fmt::format("`companion_id` = {}", m_companion_id));

	auto* buffs = GetBuffs();
	if (!buffs) {
		return true; // nothing to save
	}

	std::vector<CompanionBuffsRepository::CompanionBuffs> rows;

	for (int i = 0; i < BUFF_COUNT; i++) {
		if (!IsValidSpell(buffs[i].spellid)) {
			continue;
		}

		auto row = CompanionBuffsRepository::NewEntity();
		row.companion_id     = m_companion_id;
		row.spell_id         = buffs[i].spellid;
		row.caster_level     = buffs[i].casterlevel;
		row.duration_formula = spells[buffs[i].spellid].buff_duration_formula;
		row.ticks_remaining  = buffs[i].ticsremaining;
		row.dot_rune         = buffs[i].dot_rune;
		row.persistent       = buffs[i].persistant_buff ? 1 : 0;
		row.counters         = buffs[i].counters;
		row.num_hits         = buffs[i].hit_number;
		row.melee_rune       = buffs[i].melee_rune;
		row.magic_rune       = buffs[i].magic_rune;
		row.instrument_mod   = buffs[i].instrument_mod;
		row.caston_x         = buffs[i].caston_x;
		row.caston_y         = buffs[i].caston_y;
		row.caston_z         = buffs[i].caston_z;
		row.extra_di_chance  = buffs[i].ExtraDIChance;

		rows.emplace_back(row);
	}

	if (!rows.empty()) {
		CompanionBuffsRepository::InsertMany(database, rows);
	}

	return true;
}

bool Companion::LoadBuffs()
{
	if (m_companion_id == 0) {
		return false;
	}

	auto buff_rows = CompanionBuffsRepository::GetWhere(database,
		fmt::format("`companion_id` = {}", m_companion_id));

	auto* buffs = GetBuffs();
	if (!buffs) {
		return false;
	}

	// Clear all buff slots first
	uint32 max_slots = GetMaxBuffSlots();
	for (uint32 i = 0; i < max_slots; i++) {
		buffs[i].spellid = SPELL_UNKNOWN;
	}

	uint32 slot = 0;
	for (const auto& row : buff_rows) {
		if (slot >= BUFF_COUNT) {
			break;
		}
		if (row.spell_id == 0 || !IsValidSpell(row.spell_id)) {
			continue;
		}

		buffs[slot].spellid        = row.spell_id;
		buffs[slot].casterlevel    = row.caster_level;
		buffs[slot].ticsremaining  = row.ticks_remaining;
		buffs[slot].dot_rune       = row.dot_rune;
		buffs[slot].persistant_buff = (row.persistent != 0);
		buffs[slot].counters       = row.counters;
		buffs[slot].hit_number     = row.num_hits;
		buffs[slot].melee_rune     = row.melee_rune;
		buffs[slot].magic_rune     = row.magic_rune;
		buffs[slot].instrument_mod = row.instrument_mod;
		buffs[slot].caston_x       = row.caston_x;
		buffs[slot].caston_y       = row.caston_y;
		buffs[slot].caston_z       = row.caston_z;
		buffs[slot].ExtraDIChance  = row.extra_di_chance;
		buffs[slot].casterid       = 0;

		++slot;
	}

	CalcBonuses();
	return true;
}

// ============================================================
// Equipment (Task 21)
// ============================================================

bool Companion::GiveItem(uint32 item_id, int16 slot)
{
	if (slot < EQ::invslot::EQUIPMENT_BEGIN || slot > EQ::invslot::EQUIPMENT_END) {
		return false;
	}
	m_equipment[slot] = item_id;
	SendWearChange(slot);
	SaveEquipment();
	CalcBonuses();
	return true;
}

bool Companion::RemoveItemFromSlot(int16 slot)
{
	if (slot < EQ::invslot::EQUIPMENT_BEGIN || slot > EQ::invslot::EQUIPMENT_END) {
		return false;
	}
	m_equipment[slot] = 0;
	SendWearChange(slot);
	SaveEquipment();
	CalcBonuses();
	return true;
}

bool Companion::LoadEquipment()
{
	if (m_companion_id == 0) {
		return false;
	}

	auto rows = CompanionInventoriesRepository::GetWhere(database,
		fmt::format("`companion_id` = {}", m_companion_id));

	memset(m_equipment, 0, sizeof(m_equipment));

	for (auto& row : rows) {
		if (row.slot_id >= EQ::invslot::EQUIPMENT_BEGIN && row.slot_id <= EQ::invslot::EQUIPMENT_END) {
			m_equipment[row.slot_id] = row.item_id;
		}
	}

	CalcBonuses();
	return true;
}

bool Companion::SaveEquipment()
{
	if (m_companion_id == 0) {
		return false;
	}

	// Delete old rows
	CompanionInventoriesRepository::DeleteWhere(database,
		fmt::format("`companion_id` = {}", m_companion_id));

	// Insert current equipment
	for (int slot = EQ::invslot::EQUIPMENT_BEGIN; slot <= EQ::invslot::EQUIPMENT_END; slot++) {
		if (m_equipment[slot] == 0) {
			continue;
		}
		auto row = CompanionInventoriesRepository::NewEntity();
		row.companion_id = m_companion_id;
		row.slot_id      = static_cast<uint16_t>(slot);
		row.item_id      = m_equipment[slot];
		CompanionInventoriesRepository::InsertOne(database, row);
	}

	return true;
}

void Companion::SendWearChange(uint8 material_slot)
{
	// Delegate to Mob::SendWearChange which handles OP_WearChange packet building
	Mob::SendWearChange(material_slot);
}

uint32 Companion::GetEquipment(uint8 slot) const
{
	if (slot >= EQ::invslot::EQUIPMENT_COUNT) {
		return 0;
	}
	return m_equipment[slot];
}

void Companion::SetEquipment(uint8 slot, uint32 item_id)
{
	if (slot < EQ::invslot::EQUIPMENT_COUNT) {
		m_equipment[slot] = item_id;
	}
}

// ============================================================
// XP / Leveling (Task 19)
// ============================================================

void Companion::AddExperience(uint32 xp)
{
	m_companion_xp += xp;

	// Check if we can level up
	if (CheckForLevelUp()) {
		Client* owner = GetCompanionOwner();
		if (owner) {
			owner->Message(Chat::Yellow,
				"%s has grown stronger! They are now level %d.",
				GetCleanName(), GetLevel());
		}
	}
}

bool Companion::CheckForLevelUp()
{
	uint8 current_level = GetLevel();

	// Companion max level = player_level - MaxLevelOffset
	Client* owner = GetCompanionOwner();
	if (!owner) {
		return false;
	}

	uint8 max_level = owner->GetLevel();
	int offset = RuleI(Companions, MaxLevelOffset);
	if (offset > 0 && max_level > (uint8)offset) {
		max_level -= (uint8)offset;
	}

	if (current_level >= max_level) {
		return false; // Already at cap
	}

	uint32 xp_needed = GetXPForNextLevel();
	if (m_companion_xp < xp_needed) {
		return false; // Not enough XP
	}

	// Level up!
	m_companion_xp -= xp_needed;
	uint8 new_level = current_level + 1;

	// Scale stats to new level
	ScaleStatsToLevel(new_level);

	// Reload spell list for new level
	LoadCompanionSpells();

	// Save progress
	Save();

	LogInfo("Companion [{}] leveled up to [{}]", GetName(), new_level);
	return true;
}

uint32 Companion::GetXPForNextLevel() const
{
	// Same formula EQEmu uses for players: level * level * 1000 (simplified)
	uint8 level = GetLevel();
	return (uint32)(level * level * 1000);
}

// ============================================================
// History tracking (Task 22)
// ============================================================

void Companion::RecordKill(uint32 /*npc_type_id*/)
{
	m_total_kills++;
	// Save() is called on zone/suspend; no per-kill DB write needed
}

void Companion::RecordZoneVisit(uint32 zone_id)
{
	if (zone_id == 0) {
		return;
	}

	// m_zones_visited is a JSON array: [id1, id2, ...]
	// Append zone_id if not already present (cap at 100 entries to prevent unbounded growth)
	const std::string id_str = std::to_string(zone_id);

	// Simple string search to avoid a JSON parser dependency
	// Format: "[" or ",id," or ",id]" or "[id]"
	bool found = false;
	if (m_zones_visited.find('[' + id_str + ']') != std::string::npos ||
	    m_zones_visited.find('[' + id_str + ',') != std::string::npos ||
	    m_zones_visited.find(',' + id_str + ',') != std::string::npos ||
	    m_zones_visited.find(',' + id_str + ']') != std::string::npos) {
		found = true;
	}

	if (!found) {
		// Count existing entries (number of commas + 1, or 0 if "[]")
		size_t entry_count = 0;
		if (m_zones_visited.size() > 2) {
			entry_count = std::count(m_zones_visited.begin(), m_zones_visited.end(), ',') + 1;
		}

		if (entry_count >= 100) {
			// Drop the oldest entry (remove from front of array)
			size_t first_comma = m_zones_visited.find(',');
			if (first_comma != std::string::npos) {
				m_zones_visited = '[' + m_zones_visited.substr(first_comma + 1);
			}
		}

		// Append new zone_id
		if (m_zones_visited == "[]") {
			m_zones_visited = '[' + id_str + ']';
		} else {
			// Replace trailing ']' with ',id]'
			m_zones_visited.back() = ',';
			m_zones_visited += id_str + ']';
		}
	}
}

void Companion::UpdateTimeActive()
{
	if (m_active_since == 0) {
		return;
	}
	uint32 now = static_cast<uint32>(time(nullptr));
	if (now > m_active_since) {
		m_time_active += (now - m_active_since);
	}
	m_active_since = 0; // mark as suspended
}

// ============================================================
// Re-recruitment (Task 23): get the re-recruit bonus
// The bonus is a multiplier on the persuasion roll.
// Returned as a float fraction (e.g., 0.10 = +10%).
// Called from Lua when calculating the persuasion roll.
// ============================================================

// The re-recruitment bonus is read from the rule by Lua.
// No C++ method needed here — Lua reads RuleR(Companions, ReRecruitBonus).

// ============================================================
// Soul wipe: permanent death (Task 24)
// ============================================================

bool Companion::SoulWipe()
{
	if (m_companion_id == 0) {
		return false;
	}

	return SoulWipeByCompanionID(m_companion_id, m_owner_char_id);
}

bool Companion::SoulWipeByCompanionID(uint32 companion_id, uint32 owner_char_id)
{
	if (companion_id == 0) {
		return false;
	}

	LogInfo("Soul wipe triggered for companion_id [{}] owner [{}]", companion_id, owner_char_id);

	// Delete companion buffs
	CompanionBuffsRepository::DeleteWhere(database,
		fmt::format("`companion_id` = {}", companion_id));

	// Delete companion inventory
	CompanionInventoriesRepository::DeleteWhere(database,
		fmt::format("`companion_id` = {}", companion_id));

	// Delete companion data record
	CompanionDataRepository::DeleteOne(database, companion_id);

	// Signal Lua to clear ChromaDB soul data for this companion
	// This is done by the lua-expert's soul wipe Lua handler calling
	// a ChromaDB delete on the NPC's soul collection.
	// We signal via a quest event — the Lua script listens for this.
	// For now we set a data bucket so Lua can detect the wipe on next NPC interaction.
	std::string wipe_key = fmt::format("soul_wipe_{}_{}", owner_char_id, companion_id);
	DataBucket::SetData(&database, wipe_key, "1");

	return true;
}

// ============================================================
// Mercenary retention check
// ============================================================

void Companion::CheckMercenaryRetention()
{
	if (m_companion_type != COMPANION_TYPE_MERCENARY) {
		return;
	}

	Client* owner = GetCompanionOwner();
	if (!owner) {
		Suspend();
		return;
	}

	// Check faction — if faction has dropped below MinFaction, dismiss
	// Get the companion's original faction from npc_types via m_recruited_npc_type_id
	// This is a simplified check: if faction < MinFaction, auto-dismiss
	// Full implementation would query character's current faction standing
	// For Phase 1, we just log and continue
	LogInfo("Companion [{}] mercenary retention check for owner [{}]",
	        GetName(), owner->GetName());
}

// ============================================================
// Self-preservation
// ============================================================

bool Companion::ShouldUseDefensiveBehavior() const
{
	float hp_pct = (float)GetHP() / (float)GetMaxHP();
	float threshold = (m_companion_type == COMPANION_TYPE_MERCENARY)
		? RuleR(Companions, MercSelfPreservePct)
		: RuleR(Companions, CompanionSelfPreservePct);

	return (hp_pct < threshold);
}

// ============================================================
// Getters
// ============================================================

Client* Companion::GetCompanionOwner() const
{
	return entity_list.GetClientByCharID(m_owner_char_id);
}

// ============================================================
// Spell AI helpers
// ============================================================

int8 Companion::GetChanceToCastBySpellType(uint32 spell_type)
{
	// Default chance based on stance
	switch (m_current_stance) {
		case COMPANION_STANCE_PASSIVE:
			return 20; // Minimal casting — only heals/buffs
		case COMPANION_STANCE_BALANCED:
			return 50; // Normal casting
		case COMPANION_STANCE_AGGRESSIVE:
			return 80; // Heavy casting
		default:
			return 50;
	}
}

bool Companion::LoadCompanionSpells()
{
	// Full implementation is in companion_ai.cpp (Task 7).
	// This stub clears the list so the companion starts with no stored spells.
	m_companion_spells.clear();
	return true;
}

void Companion::SetSpellTimeCanCast(uint16 spellid, uint32 recast_delay)
{
	uint32 now_ms = Timer::GetCurrentTime();
	for (auto& spell : m_companion_spells) {
		if (spell.spellid == spellid) {
			spell.time_cancast = now_ms + recast_delay;
			break;
		}
	}
}

bool Companion::CheckSpellRecastTimers(uint16 spellid)
{
	uint32 now_ms = Timer::GetCurrentTime();
	for (const auto& spell : m_companion_spells) {
		if (spell.spellid == spellid) {
			return (now_ms >= spell.time_cancast);
		}
	}
	return true; // Not in list = no recast restriction
}

// ============================================================
// Sit/Stand
// ============================================================

void Companion::Sit()
{
	SetAppearance(eaStanding); // Will use correct appearance
}

void Companion::Stand()
{
	SetAppearance(eaStanding);
}

bool Companion::IsSitting() const
{
	return GetAppearance() == eaSitting;
}

bool Companion::IsStanding() const
{
	return GetAppearance() == eaStanding;
}

// ============================================================
// Signal
// ============================================================

void Companion::Signal(int signal_id)
{
	// TODO (Task 17/18): wire up Lua companion quest event dispatch once
	// parse->EventCompanion() is implemented in lua_companion.cpp.
	// For now signals are silently consumed.
	LogInfo("Companion [{}] received signal [{}]", GetName(), signal_id);
}

// ============================================================
// EntityList methods for Companion (follows Bot pattern)
// ============================================================

void EntityList::AddCompanion(Companion* new_companion, bool send_spawn_packet, bool dont_queue)
{
	if (!new_companion) {
		return;
	}

	new_companion->SetID(GetFreeID());
	companion_list.emplace(std::pair<uint16, Companion*>(new_companion->GetID(), new_companion));
	mob_list.emplace(std::pair<uint16, Mob*>(new_companion->GetID(), new_companion));
	// Note: companions are also NPCs, but we do NOT add to npc_list to avoid
	// double-processing. The mob_list handles all processing.

	new_companion->SetSpawned();

	if (send_spawn_packet) {
		if (dont_queue) {
			auto outapp = new EQApplicationPacket();
			new_companion->CreateSpawnPacket(outapp);
			outapp->priority = 6;
			QueueClients(new_companion, outapp, true);
			safe_delete(outapp);
		} else {
			auto ns = new NewSpawn_Struct;
			memset(ns, 0, sizeof(NewSpawn_Struct));
			new_companion->FillSpawnStruct(ns, new_companion);
			AddToSpawnQueue(new_companion->GetID(), &ns);
			safe_delete(ns);
		}
	}
}

bool EntityList::RemoveCompanion(uint16 entity_id)
{
	auto it = companion_list.find(entity_id);
	if (it != companion_list.end()) {
		companion_list.erase(it);
		return true;
	}
	return false;
}

Companion* EntityList::GetCompanionByOwnerCharacterID(uint32 character_id)
{
	for (auto& [id, companion] : companion_list) {
		if (companion && companion->GetOwnerCharacterID() == character_id) {
			return companion;
		}
	}
	return nullptr;
}

std::vector<Companion*> EntityList::GetCompanionsByOwnerCharacterID(uint32 character_id)
{
	std::vector<Companion*> result;
	for (auto& [id, companion] : companion_list) {
		if (companion && companion->GetOwnerCharacterID() == character_id) {
			result.push_back(companion);
		}
	}
	return result;
}

// ============================================================
// Client::SpawnCompanionsOnZone
// Called from client_packet.cpp when a player completes zone-in.
// Loads all non-dismissed, non-suspended companion records for this
// player and spawns each one near the player's current position.
// Mirrors the Merc pattern (SpawnMercOnZone) and Bot pattern
// (Bot::LoadAndSpawnAllZonedBots) for lifecycle consistency.
// ============================================================

void Client::SpawnCompanionsOnZone()
{
	if (!RuleB(Companions, CompanionsEnabled)) {
		return;
	}

	uint32 char_id = CharacterID();

	// Load all active (non-dismissed) companion records for this player
	auto companion_records = CompanionDataRepository::GetWhere(
		database,
		fmt::format("owner_id = {} AND is_dismissed = 0", char_id)
	);

	if (companion_records.empty()) {
		return;
	}

	glm::vec4 owner_pos = GetPosition();

	for (auto& cd : companion_records) {
		// Skip suspended companions — they exist in DB but are not spawned
		if (cd.is_suspended) {
			continue;
		}

		// Load the source NPC type for this companion
		auto* npc_type = database.LoadNPCTypesData(cd.npc_type_id);
		if (!npc_type) {
			LogInfo(
				"SpawnCompanionsOnZone: npc_type {} not found for companion {} (owner {})",
				cd.npc_type_id, cd.id, char_id
			);
			continue;
		}

		// Spawn near the player, offset so they don't stack
		float spawn_x = owner_pos.x + 5.0f;
		float spawn_y = owner_pos.y;
		float spawn_z = owner_pos.z;
		float spawn_h = owner_pos.w;

		auto* companion = new Companion(
			npc_type,
			spawn_x, spawn_y, spawn_z, spawn_h,
			char_id,
			cd.companion_type
		);

		// Restore saved state (HP, mana, XP, stance, etc.) from DB record
		if (!companion->Load(cd.id)) {
			LogError(
				"SpawnCompanionsOnZone: Load() failed for companion id {} (owner {})",
				cd.id, char_id
			);
			delete companion;
			continue;
		}

		// Record this zone visit in history
		if (zone) {
			companion->RecordZoneVisit(zone->GetZoneID());
		}

		entity_list.AddCompanion(companion);

		LogInfo(
			"SpawnCompanionsOnZone: spawned companion '{}' (id {}) for player '{}'",
			cd.name, cd.id, GetName()
		);
	}
}
