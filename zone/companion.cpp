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
#include "common/spdat.h"
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
#include "zone/questmgr.h"
#include "zone/string_ids.h"
#include "zone/zone.h"

#include <algorithm>
#include <cmath>
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
	  m_replacement_spawn_timer(RuleI(Companions, ReplacementSpawnDelayS) * 1000),
	  m_ping_timer(5000),
	  m_mana_report_timer(15000),
	  m_sitting_regen_timer(6000)
{
	// Identity
	m_companion_id          = 0;
	m_owner_char_id         = owner_char_id;
	m_recruited_npc_type_id = d->npc_id;
	m_companion_type        = companion_type;
	m_current_stance        = COMPANION_STANCE_BALANCED;
	m_suspended             = false;
	m_is_dismissed          = false;
	m_depop                 = false;
	m_is_zoning             = false;
	m_lom_announced         = false;
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

	// Initialize inventory profile so CalcItemBonuses() can read equipped items via GetInv().GetItem().
	// MobVersion::Bot is required — not MobVersion::NPC — because the NPC version has a zero
	// PossessionsBitmask which causes InventoryProfile::PutItem() to return SLOT_INVALID for all
	// equipment slots. MobVersion::Bot has EntityLimits::Bot::invslot::POSSESSIONS_BITMASK which
	// includes all equipment slots, allowing GiveItem() and LoadEquipment() to populate the profile
	// so CalcItemBonuses() applies item stats correctly.
	GetInv().SetInventoryVersion(EQ::versions::MobVersion::Bot);
	GetInv().SetGMInventory(false);

	// Disable retention check for companion-type (only mercs need it)
	if (m_companion_type == COMPANION_TYPE_COMPANION) {
		m_retention_check_timer.Disable();
	}

	// Disable death despawn timer until death occurs
	m_death_despawn_timer.Disable();
	m_replacement_spawn_timer.Disable();
	// Ping timer starts disabled; enabled in Process() when companion is stationary
	m_ping_timer.Disable();
	// Mana report timer starts disabled; enabled when companion sits to med
	m_mana_report_timer.Disable();

	// Set flee immunity immediately in the constructor so there is no window
	// between construction and Spawn() where flee can trigger.  Spawn() and
	// Process() re-apply this based on the live rule value, but the constructor
	// guard ensures the companion is never fleable even if Spawn() is delayed.
	if (!RuleB(Companions, CompanionFleeEnabled)) {
		SetSpecialAbility(SpecialAbility::FleeingImmunity, 1);
	}

	// Determine combat role from class for positioning logic
	m_combat_role = DetermineRoleFromClass(GetClass());

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

	// Check for an existing dismissed or suspended companion record (re-recruitment path).
	// Matches both voluntary dismissal (is_dismissed=1) and death-then-re-recruitment
	// (is_suspended=1 with cur_hp=0) so equipment is always preserved across dismissal
	// and death. Without matching is_suspended, a dead companion re-recruited before
	// the despawn timer fires would fall through to the fresh-recruitment path and lose
	// all equipment. (BUG-012)
	auto existing = CompanionDataRepository::GetWhere(
		database,
		fmt::format(
			"owner_id = {} AND npc_type_id = {} AND (is_dismissed = 1 OR is_suspended = 1) LIMIT 1",
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

		// Restore HP/mana to max for re-recruited companions.
		// Load() faithfully restores cur_hp from the DB — for companions that died,
		// cur_hp=0 is stored in the DB and Load() skips SetHP() (due to the `if
		// (cd.cur_hp > 0)` guard). Without this, a re-recruited dead companion would
		// spawn with whatever HP the NPC constructor left (base NPC max_hp before
		// ScaleStatsToLevel raised max_hp), which is lower than GetMaxHP().
		// Spawn() does not call RestoreHealth(), so we must set here.
		// We use SetHP/SetMana directly (not RestoreHealth/RestoreMana) because
		// SendHPUpdate() in RestoreHealth() requires an entity ID, which is only
		// assigned when Spawn() adds the companion to the entity list.
		companion->SetHP(companion->GetMaxHP());
		companion->SetMana(companion->GetMaxMana());

		// Clear dismissed/suspended flags and activate — companion is active again.
		// Reset both C++ members and the DB record so Save() writes the correct state
		// on the next save without re-introducing stale is_dismissed/is_suspended values.
		companion->m_suspended    = false;
		companion->m_is_dismissed = false;
		database.QueryDatabase(
			fmt::format(
				"UPDATE `companion_data` SET `is_dismissed` = 0, `is_suspended` = 0 WHERE `id` = {}",
				existing[0].id
			)
		);

		// Belt-and-suspenders cooldown cleanup: delete any stale cooldown data_bucket
		// so Lua re-recruitment detection is never blocked by a leftover cooldown from
		// a prior failed first-time attempt. Lua also deletes this on success, but C++
		// ensures no stale cooldown survives if Lua is bypassed or a code path changes.
		DataBucket::DeleteData(
			&database,
			fmt::format("companion_cooldown_{}_{}", source_npc->GetNPCTypeID(), owner->CharacterID())
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

	// NPC::Death() unconditionally sets p_depop = true, which causes NPC::Process()
	// to return false on the very next tick and immediately remove the entity.
	// For companions we want the entity to persist in the world (as a dead entity)
	// until m_death_despawn_timer fires in Companion::Process(), giving the player
	// a window to resurrect them.  Reset p_depop here so NPC::Process() keeps
	// returning true until the death despawn timer triggers cleanup.
	SetDepop(false);

	// Equipment persistence on death: if the rule is false, clear all equipment and
	// return it to the owner. Default is true (equipment survives death).
	if (!RuleB(Companions, EquipmentPersistsThroughDeath)) {
		Client* equip_owner = GetCompanionOwner();
		for (int slot = EQ::invslot::EQUIPMENT_BEGIN; slot <= EQ::invslot::EQUIPMENT_END; ++slot) {
			if (m_equipment[slot] != 0) {
				uint32 item_id = m_equipment[slot];
				m_equipment[slot] = 0;
				equipment[slot] = 0;
				GetInv().DeleteItem(static_cast<int16>(slot));
				if (equip_owner) {
					equip_owner->SummonItem(item_id);
				}
			}
		}
		SaveEquipment();
		CalcBonuses();
	}

	// Save companion HP as 0 so resurrection is possible
	Client* owner = GetCompanionOwner();
	if (owner) {
		// Notify owner of companion death
		owner->Message(Chat::Red,
			"%s has fallen in battle! You have %d seconds to resurrect them, "
			"or they will return home.",
			GetCleanName(),
			RuleI(Companions, DeathDespawnS));

		// Mark companion as suspended (dead) in DB. Explicitly save equipment before
		// the entity is cleaned up so the companion_inventories rows always reflect the
		// current gear at the moment of death. (BUG-012)
		SetSuspended(true);
		m_times_died++;
		UpdateTimeActive(); // stop accruing active time at death
		SaveEquipment();
		Save();

		// Start the despawn timer — if not resurrected, auto-dismiss
		m_death_despawn_timer.Start();
	}

	// Remove from group's members[] array BEFORE the entity cleanup pass deletes us.
	// Without this, the group holds a dangling pointer that crashes on the next
	// QueueClients() call. Matches how Bot::Death() calls Zone()->MemberZoned().
	// MemberZoned() NULLs our slot without sending group-leave packets, which is
	// correct for a dead entity (dead companions stay conceptually in the group
	// for resurrection purposes, but the live pointer must be cleared).
	if (HasGroup()) {
		Group* g = GetGroup();
		if (g) {
			g->MemberZoned(this);
		}
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
	if (!other) {
		return false;
	}

	// Hard safety net: companions must never strike their owner or any member of
	// their group regardless of how the target ended up on the hate list.
	Client* atk_owner = GetCompanionOwner();
	if (atk_owner) {
		if (other == atk_owner) {
			// Target is the owner — scrub them from the hate list and abort
			RemoveFromHateList(other);
			SetTarget(nullptr);
			return false;
		}
		Group* atk_grp = GetGroup();
		if (atk_grp && atk_grp->IsGroupMember(other)) {
			// Target is a group member — scrub from hate list and abort
			RemoveFromHateList(other);
			SetTarget(nullptr);
			return false;
		}
	}

	// -------------------------------------------------------
	// Phase 1 — Weapon Damage Path
	// When UseWeaponDamage is true and a weapon is equipped in
	// the attack hand, use the weapon's damage values instead of
	// the npc_types base damage (GetBaseDamage / GetMinDamage).
	// If the rule is false, or no weapon is in the slot, fall
	// through to NPC::Attack() which uses the NPC base damage.
	// -------------------------------------------------------
	if (!RuleB(Companions, UseWeaponDamage)) {
		return NPC::Attack(other, Hand, FromRiposte, IsStrikethrough, IsFromSpell, opts);
	}

	// Retrieve the equipped weapon from the inventory profile.
	// Companions populate GetInv() via GiveItem()/LoadEquipment().
	// NPCs use equipment[] + database.GetItem() instead; we must use GetInv() here.
	const EQ::ItemInstance* weapon_inst = nullptr;
	if (Hand == EQ::invslot::slotSecondary) {
		weapon_inst = GetInv().GetItem(EQ::invslot::slotSecondary);
	} else {
		weapon_inst = GetInv().GetItem(EQ::invslot::slotPrimary);
	}

	// If no weapon is equipped (or item is not a weapon), fall back to NPC path.
	if (!weapon_inst || !weapon_inst->IsWeapon()) {
		return NPC::Attack(other, Hand, FromRiposte, IsStrikethrough, IsFromSpell, opts);
	}

	// --- Weapon-damage attack path (mirrors NPC::Attack but uses weapon->Damage) ---

	if (DivineAura()) {
		return false;
	}

	if (!GetTarget()) {
		SetTarget(other);
	}

	if (!IsAttackAllowed(other)) {
		RemoveFromHateList(other);
		RemoveFromRampageList(other);
		return false;
	}

	FaceTarget(GetTarget());

	const EQ::ItemData* weapon = weapon_inst->GetItem();

	if (Hand == EQ::invslot::slotSecondary) {
		// Secondary hand only accepts 1H weapons
		if (!weapon->IsType1HWeapon()) {
			return false;
		}
		OffHandAtk(true);
	} else {
		OffHandAtk(false);
	}

	DamageHitInfo my_hit;
	my_hit.hand         = Hand;
	my_hit.damage_done  = 1;
	my_hit.min_damage   = 0;

	// Determine skill and send attack animation (Client-style: pass ItemInstance*).
	// AttackAnimation sends the weapon swing packet to nearby clients and derives
	// the skill type from the item type (1H slash, 2H blunt, etc.).
	my_hit.skill = AttackAnimation(Hand, weapon_inst);

	// Base damage from weapon via the shared GetWeaponDamage path (Client/Bot overload).
	// This reads weapon->Damage and applies immunity checks for the target.
	int64 hate = weapon->Damage + weapon->ElemDmgAmt;
	my_hit.base_damage = GetWeaponDamage(other, weapon_inst, &hate);

	if (hate == 0 && my_hit.base_damage > 1) {
		hate = my_hit.base_damage;
	}

	if (my_hit.base_damage > 0) {
		// Apply damage caps (same as Client path: attack.cpp:1649-1650)
		if (Hand == EQ::invslot::slotPrimary || Hand == EQ::invslot::slotSecondary) {
			my_hit.base_damage = DoDamageCaps(my_hit.base_damage);
		}

		// Bane and elemental damage (same logic as NPC::Attack, attack.cpp:2332-2353)
		int eleBane = 0;
		if (RuleB(NPC, UseBaneDamage)) {
			if (weapon->BaneDmgBody == other->GetBodyType()) {
				eleBane += weapon->BaneDmgAmt;
			}
			if (weapon->BaneDmgRace == other->GetRace()) {
				eleBane += weapon->BaneDmgRaceAmt;
			}
		}
		if (weapon->ElemDmgAmt) {
			eleBane += static_cast<int>(weapon->ElemDmgAmt * other->ResistSpell(weapon->ElemDmgType, 0, this) / 100);
		}
		my_hit.base_damage += eleBane;
		hate += eleBane;

#ifndef EQEMU_NO_WEAPON_DAMAGE_BONUS
		// Damage bonus: applied to primary hand hits at level 28+ for warrior classes.
		// Mirrors Client path at attack.cpp:1674-1685.
		if (Hand == EQ::invslot::slotPrimary && GetLevel() >= 28 && IsWarriorClass()) {
			int ucDamageBonus = static_cast<int>(GetWeaponDamageBonus(weapon));
			my_hit.min_damage = ucDamageBonus;
			hate += ucDamageBonus;
		}

		// Sinister Strikes: offhand damage bonus when AA/item/spell bonus present
		if (Hand == EQ::invslot::slotSecondary &&
		    (aabonuses.SecondaryDmgInc || itembonuses.SecondaryDmgInc || spellbonuses.SecondaryDmgInc)) {
			int ucDamageBonus = static_cast<int>(GetWeaponDamageBonus(weapon, true));
			my_hit.min_damage = ucDamageBonus;
			hate += ucDamageBonus;
		}
#endif

		int hit_chance_bonus = 0;
		my_hit.offense = offense(my_hit.skill);

		if (opts) {
			my_hit.base_damage *= opts->damage_percent;
			my_hit.base_damage += opts->damage_flat;
			hate *= opts->hate_percent;
			hate += opts->hate_flat;
			hit_chance_bonus += opts->hit_chance;
		}

		my_hit.tohit = GetTotalToHit(my_hit.skill, hit_chance_bonus);

		DoAttack(other, my_hit, opts, FromRiposte);
	} else {
		my_hit.damage_done = DMG_INVULNERABLE;
	}

	other->AddToHateList(this, hate);

	if (GetHP() > 0 && !other->HasDied()) {
		other->Damage(this, my_hit.damage_done, SPELL_UNKNOWN, my_hit.skill, true, -1, false, m_specialattacks);
	} else {
		return false;
	}

	if (HasDied()) {
		return false;
	}

	MeleeLifeTap(my_hit.damage_done);
	CommonBreakInvisibleFromCombat();

	if (!GetTarget()) {
		return true; // killed them
	}

	bool has_hit = my_hit.damage_done > 0;
	if (has_hit && !FromRiposte && !other->HasDied()) {
		// TryWeaponProc expects ItemData* (NPC overload), not ItemInstance*
		TryWeaponProc(nullptr, weapon, other, Hand);

		if (!other->HasDied()) {
			TrySpellProc(nullptr, weapon, other, Hand);
		}

		if (HasSkillProcSuccess() && !other->HasDied()) {
			TrySkillProc(other, my_hit.skill, 0, true, Hand);
		}
	}

	if (GetHP() > 0 && !other->HasDied()) {
		TriggerDefensiveProcs(other, Hand, true, my_hit.damage_done);
	}

	return has_hit;
}

// ============================================================
// SetAttackTimer — Weapon Delay Path (Phase 1)
// ============================================================

void Companion::SetAttackTimer()
{
	// When the rule is off, delegate to the standard NPC path
	// (uses npc_types.attack_delay — unchanged behavior).
	if (!RuleB(Companions, UseWeaponDamage)) {
		NPC::SetAttackTimer();
		return;
	}

	// Weapon-delay path: mirrors Client::SetAttackTimer() (attack.cpp:6682-6782)
	// but reads weapons from the inventory profile (GetInv()) instead of GetBotItem().
	// Guard against division by zero: GetHaste() can return 0 under 100% melee
	// inhibition debuffs. Clamp to 0.01 (1% of normal speed) so the cast to int
	// of (delay / haste_mod) never encounters infinity or undefined behavior.
	float haste_mod = std::max(0.01f, GetHaste() * 0.01f);
	int primary_speed   = 0;
	int secondary_speed = 0;

	// Default in case of no valid weapon
	attack_timer.SetAtTrigger(4000, true);

	Timer* TimerToUse = nullptr;

	for (int i = EQ::invslot::slotRange; i <= EQ::invslot::slotSecondary; i++) {
		if (i == EQ::invslot::slotPrimary) {
			TimerToUse = &attack_timer;
		} else if (i == EQ::invslot::slotRange) {
			TimerToUse = &ranged_timer;
		} else if (i == EQ::invslot::slotSecondary) {
			TimerToUse = &attack_dw_timer;
		} else {
			continue; // hands — skip
		}

		// Dual wield check: disable offhand timer if class can't dual wield
		if (i == EQ::invslot::slotSecondary) {
			if (!CanThisClassDualWield() || HasTwoHanderEquipped()) {
				attack_dw_timer.Disable();
				continue;
			}
		}

		// Get item from companion inventory profile
		const EQ::ItemData* ItemToUse = nullptr;
		EQ::ItemInstance* ci = GetInv().GetItem(i);
		if (ci) {
			ItemToUse = ci->GetItem();
		}

		// Validate weapon: must be a common item with non-zero damage and delay
		if (ItemToUse != nullptr) {
			if (!ItemToUse->IsClassCommon() || ItemToUse->Damage == 0 || ItemToUse->Delay == 0) {
				ItemToUse = nullptr;
			} else if ((ItemToUse->ItemType > EQ::item::ItemTypeLargeThrowing) &&
			           (ItemToUse->ItemType != EQ::item::ItemTypeMartial) &&
			           (ItemToUse->ItemType != EQ::item::ItemType2HPiercing)) {
				ItemToUse = nullptr;
			}
		}

		int hhe   = itembonuses.HundredHands + spellbonuses.HundredHands;
		int delay = 0;

		if (ItemToUse == nullptr) {
			if (i == EQ::invslot::slotPrimary) {
				delay = 100 * GetHandToHandDelay(); // unarmed fallback
			} else {
				// Range/secondary with no valid weapon: keep default or disable
				if (i == EQ::invslot::slotSecondary) {
					attack_dw_timer.Disable();
				}
				continue;
			}
		} else {
			delay = 100 * ItemToUse->Delay;
		}

		int speed = static_cast<int>(delay / haste_mod);

		if (ItemToUse && ItemToUse->ItemType == EQ::item::ItemTypeBow) {
			// Companions don't carry quivers, so no quiver haste adjustment.
			// Apply the minimum delay cap directly.
			speed = std::max(speed, RuleI(Combat, QuiverHasteCap));
		} else {
			if (RuleB(Spells, Jun182014HundredHandsRevamp)) {
				speed = static_cast<int>(speed + ((hhe / 1000.0f) * speed));
			} else {
				speed = static_cast<int>(speed + ((hhe / 100.0f) * delay));
			}
		}

		bool reinit = !TimerToUse->Enabled();
		TimerToUse->SetAtTrigger(std::max(RuleI(Combat, MinHastedDelay), speed), reinit, reinit);

		if (i == EQ::invslot::slotPrimary) {
			primary_speed = speed;
		} else if (i == EQ::invslot::slotSecondary) {
			secondary_speed = speed;
		}
	}

	// Dual wield same-delay animation sync (matches Client::SetAttackTimer behavior)
	if (primary_speed == secondary_speed) {
		SetDualWieldingSameDelayWeapons(1);
	} else {
		SetDualWieldingSameDelayWeapons(0);
	}
}

// ============================================================
// CalcMaxHP — STA-to-HP Conversion (Phase 3)
// ============================================================

// CalcMaxHP: adds STA-based HP bonus on top of the standard NPC max HP.
//
// The NPC base formula (Mob::CalcMaxHP) already includes:
//   max_hp = base_hp (from npc_types.max_hp) + itembonuses.HP
//
// Base HP from npc_types already accounts for the NPC's inherent STA
// (the NPC was designed with some STA and that's baked into max_hp).
// We must NOT convert the base STA again — that would double-count.
//
// What we DO convert: bonus STA from equipped items (itembonuses.STA)
// and spell buffs (spellbonuses.STA). These are the stats gear/spells
// actually add, and they should translate to HP just as they do for players.
//
// HP-per-STA-point at level 60:
//   Tanks (Warrior, Paladin, Shadow Knight): 8 HP/STA
//   Melee DPS (Monk, Ranger, Bard, Rogue, Beastlord, Berserker): 5 HP/STA
//   Priest (Cleric, Druid, Shaman): 4 HP/STA
//   Caster DPS (Wizard, Magician, Necromancer, Enchanter): 3 HP/STA
//
// The values scale linearly with level (level/60). For example, a level 30
// warrior gets 4 HP/STA (8 * 30/60 = 4). This avoids overvaluing STA gear
// on low-level companions.
//
// The Companions::STAToHPFactor rule (default 100) scales the entire formula
// as a percentage — set to 50 to halve the STA-to-HP conversion if needed.
int64 Companion::CalcMaxHP()
{
	// Call the NPC/Mob base to populate max_hp with base_hp + itembonuses.HP
	// and apply PercentMaxHPChange / FlatMaxHPChange bonuses.
	int64 base = Mob::CalcMaxHP();

	// Only apply STA-to-HP if the rule is set to a positive factor.
	int factor = RuleI(Companions, STAToHPFactor);
	if (factor <= 0) {
		return base;
	}

	// Bonus STA from equipped items and spells only.
	// aabonuses.STA is always 0 for companions (no AAs).
	int bonus_sta = static_cast<int>(itembonuses.STA) + static_cast<int>(spellbonuses.STA);
	if (bonus_sta <= 0) {
		return base;
	}

	// HP-per-STA base value at level 60 by class archetype.
	// These match the approximate per-class values from the Client's CalcBaseHP() formula.
	int hp_per_sta_at_60 = 3; // default: caster DPS
	switch (GetClass()) {
		case Class::Warrior:
		case Class::Paladin:
		case Class::ShadowKnight:
			hp_per_sta_at_60 = 8;
			break;
		case Class::Monk:
		case Class::Ranger:
		case Class::Bard:
		case Class::Rogue:
		case Class::Beastlord:
		case Class::Berserker:
			hp_per_sta_at_60 = 5;
			break;
		case Class::Cleric:
		case Class::Druid:
		case Class::Shaman:
			hp_per_sta_at_60 = 4;
			break;
		// Wizard, Magician, Necromancer, Enchanter: default 3
		default:
			hp_per_sta_at_60 = 3;
			break;
	}

	// Scale HP-per-STA linearly with level (capped at 60 for progression purposes).
	// A level 30 warrior gets 8 * 30/60 = 4 HP/STA, not 8.
	int level = static_cast<int>(GetLevel());
	int cap_level = std::min(level, 60);

	// Compute HP bonus: bonus_sta * hp_per_sta * level/60 * factor/100
	// Use integer arithmetic with care to avoid truncation to zero.
	// Order: multiply first, divide last.
	int64 hp_bonus = static_cast<int64>(bonus_sta) * hp_per_sta_at_60 * cap_level * factor;
	hp_bonus /= 6000; // (60 levels * 100 factor)

	max_hp = base + hp_bonus;
	return max_hp;
}

// ============================================================
// CalcMaxMana — Level-Scaled Mana Preservation (BUG-017 fix)
// ============================================================

// CalcMaxMana: preserves the level-scaled max_mana set by ScaleStatsToLevel().
//
// Without this override, NPC::CalcMaxMana() runs whenever CalcBonuses() is
// called. It resets max_mana to either:
//   - npc_mana + bonuses (when npc_mana != 0): ignores level scaling entirely
//   - ((INT or WIS)/2 + 1) * level + bonuses (when npc_mana == 0): re-derives
//     from current INT/WIS which was scaled, producing an inconsistent value
//
// This override reconstructs max_mana from the level-scaled base:
//   scaled_base = m_base_mana * GetLevel() / m_recruited_level
// Then adds item and spell mana bonuses on top. Non-caster classes get 0.
//
// This mirrors the CalcMaxHP() override pattern: we bypass the NPC formula
// entirely and compute from the stored base values instead.
//
// Note: m_base_mana is the max_mana value at recruitment time (before any
// scaling). m_recruited_level is the NPC level at recruitment. Both are set
// by StoreBaseStats() during construction.
int64 Companion::CalcMaxMana()
{
	// Non-caster companions have no mana pool.
	if (!IsIntelligenceCasterClass() && !IsWisdomCasterClass()) {
		max_mana = 0;
		return 0;
	}

	// When npc_mana == 0, the NPC type stores no explicit mana value.
	// NPC::CalcMaxMana() derives mana from the INT or WIS formula:
	//   ((INT/2) + 1) * level + bonuses
	// Since ScaleStatsToLevel() already scaled both INT/WIS and level,
	// this formula scales proportionally — no fix is needed for this path.
	// Fall through to the NPC formula in this case.
	if (npc_mana == 0) {
		return NPC::CalcMaxMana();
	}

	// When npc_mana != 0, NPC::CalcMaxMana() returns npc_mana + bonuses,
	// ignoring the current level entirely. This is the broken path: after
	// ScaleStatsToLevel() sets max_mana = m_base_mana * scale, CalcBonuses()
	// calls NPC::CalcMaxMana() which resets max_mana to the unscaled npc_mana.
	//
	// Fix: reconstruct max_mana from the level-scaled base instead of npc_mana.

	// Guard against division by zero (should never happen post-construction).
	if (m_recruited_level == 0) {
		max_mana = 0;
		return 0;
	}

	// Reconstruct the level-scaled base mana from the stored recruitment values.
	// Use float division to match ScaleStatsToLevel() precision.
	float scale = static_cast<float>(GetLevel()) / static_cast<float>(m_recruited_level);
	int64 scaled_base = static_cast<int64>(static_cast<float>(m_base_mana) * scale);

	// Add item and spell mana bonuses (same as NPC::CalcMaxMana does for bonuses).
	// aabonuses.Mana is always 0 for companions (no AAs).
	max_mana = scaled_base + itembonuses.Mana + spellbonuses.Mana;

	if (max_mana < 0) {
		max_mana = 0;
	}

	// Clamp current_mana to the new max so we don't display > 100% mana.
	if (current_mana > max_mana) {
		current_mana = max_mana;
	}

	return max_mana;
}

// ============================================================
// Resist Caps — Phase 5
// ============================================================

// GetMaxResist: returns the companion's resist cap.
//
// Formula: level * 5 + ResistCapBase (default 50).
// At level 60 with default ResistCapBase=50: cap = 350.
// At level 65 with default: cap = 375.
//
// This is intentionally lower than the Client cap of 500, placing
// companion resist ceiling at ~70% of player cap — matching the
// 70-85% power target from the PRD.
//
// Setting ResistCapBase to 0 disables resist capping entirely
// (returns 32000, a value no companion resist can realistically reach).
int32 Companion::GetMaxResist() const
{
	int rule_base = RuleI(Companions, ResistCapBase);
	if (rule_base <= 0) {
		return 32000;
	}
	return static_cast<int32>(GetLevel()) * 5 + rule_base;
}

// ============================================================
// Focus Effects — Phase 5
// ============================================================

// GetFocusEffect: delegates to Mob::GetFocusEffect (the base class)
// instead of NPC::GetFocusEffect.
//
// Why: NPC::GetFocusEffect has two problems for companions:
//   1. It gates item focus behind RuleB(Spells, NPC_UseFocusFromItems),
//      which defaults to false — completely disabling item focus for all NPCs.
//   2. It reads items from the NPC equipment[] array (raw IDs from npc_types)
//      rather than the inventory profile. Companions store their equipped
//      items via GiveItem() -> GetInv().PutItem() in the inventory profile,
//      not in equipment[]. Using equipment[] returns stale/wrong items.
//
// Mob::GetFocusEffect uses GetInv().GetItem() (correct for companions)
// and has no NPC_UseFocusFromItems rule gate. Delegating here enables
// both item-derived and spell-derived focus effects with a single call.
int64 Companion::GetFocusEffect(focusType type, uint16 spell_id,
                                Mob *caster, bool from_buff_tic)
{
	return Mob::GetFocusEffect(type, spell_id, caster, from_buff_tic);
}

// ============================================================
// Triple Attack — Phase 2
// ============================================================

// CanCompanionTripleAttack: level and class eligibility check (no RNG).
// Warriors triple at 56+. Monks and Rangers triple at 60+.
// All other classes never triple attack.
// This is called from CheckTripleAttack() and is public for testing.
bool Companion::CanCompanionTripleAttack() const
{
	uint8 level = GetLevel();
	uint8 cls   = GetClass();

	switch (cls) {
		case Class::Warrior:
			return level >= 56;
		case Class::Monk:
		case Class::Ranger:
			return level >= 60;
		default:
			return false;
	}
}

// CheckTripleAttack: rolls for triple attack success.
// Uses a percent chance derived from level, scaled by the ClassicTripleAttack
// rule values if ClassicTripleAttack is true, or a skill-derived chance if
// ClassicTripleAttack is false (matching Bot::CheckTripleAttack logic).
// Returns false immediately when CanCompanionTripleAttack() is false.
bool Companion::CheckTripleAttack()
{
	if (!CanCompanionTripleAttack()) {
		return false;
	}

	int chance = 0;

	if (RuleB(Combat, ClassicTripleAttack)) {
		// Classic mode: use per-class rule values (same as Client/Bot).
		switch (GetClass()) {
			case Class::Warrior:
				chance = RuleI(Combat, ClassicTripleAttackChanceWarrior);
				break;
			case Class::Monk:
				chance = RuleI(Combat, ClassicTripleAttackChanceMonk);
				break;
			case Class::Ranger:
				chance = RuleI(Combat, ClassicTripleAttackChanceRanger);
				break;
			default:
				return false;
		}
	} else {
		// Modern mode: skill-based chance (Bot::CheckTripleAttack line 2080).
		// SkillCaps lacks SkillTripleAttack for Warriors/Monks/Rangers in this DB,
		// so we derive a reasonable chance from level (25 base + 1 per level over 56/60).
		// This gives warriors a 25% base at 56, growing to 29% at 60.
		// Monks/Rangers start at 25% at 60.
		uint8 level = GetLevel();
		uint8 cls   = GetClass();
		if (cls == Class::Warrior) {
			chance = 25 + (level > 56 ? (level - 56) : 0);
		} else {
			// Monk, Ranger
			chance = 25 + (level > 60 ? (level - 60) : 0);
		}
	}

	if (chance < 1) {
		return false;
	}

	// Apply bonus chance from item/spell bonuses (same as Bot path)
	int inc = aabonuses.TripleAttackChance + spellbonuses.TripleAttackChance + itembonuses.TripleAttackChance;
	if (inc != 0) {
		chance = static_cast<int>(chance * (1.0f + inc / 100.0f));
	}

	return zone->random.Int(1, 100) <= chance;
}

// DoAttackRounds: performs one round of melee attacks on the target.
// Mirrors Bot::DoAttackRounds (bot.cpp:2851-2947) but without AA-based
// extra attacks since companions don't have AAs.
// Called from Companion::Process() when attack_timer fires.
void Companion::DoAttackRounds(Mob* target, int hand)
{
	if (!target || target->IsCorpse()) {
		return;
	}

	// Primary attack
	Attack(target, hand, false, false, false);

	if (!target || target->HasDied()) {
		return;
	}

	// Double attack check
	bool can_double = CanThisClassDoubleAttack();

	// Off-hand double attack requires DoubleAttack skill >= 150 or bonus
	// (matching Bot behavior at bot.cpp:2861-2864)
	if (can_double && hand == EQ::invslot::slotSecondary) {
		can_double =
			GetSkill(EQ::skills::SkillDoubleAttack) > 149 ||
			(aabonuses.GiveDoubleAttack + spellbonuses.GiveDoubleAttack + itembonuses.GiveDoubleAttack) > 0;
	}

	if (can_double && CheckDoubleAttack()) {
		Attack(target, hand, false, false, false);

		if (!target || target->HasDied()) {
			return;
		}

		// Triple attack: primary hand only, class/level eligible
		if (hand == EQ::invslot::slotPrimary && CanCompanionTripleAttack()) {
			if (CheckTripleAttack()) {
				Attack(target, hand, false, false, false);

				// Flurry chance from buffs/items (no AAs on companions)
				int flurry_chance = spellbonuses.FlurryChance + itembonuses.FlurryChance;
				if (flurry_chance && zone->random.Roll(flurry_chance)) {
					if (!target || target->HasDied()) { return; }
					Attack(target, hand, false, false, false);
				}
			}
		}
	}
}

// ============================================================
// HP Regen
// ============================================================

int64 Companion::CalcHPRegen() const
{
	// Use the NPC's native hp_regen_rate if it is already a positive number
	// (some recruited NPCs have an explicit rate set in npc_types), otherwise
	// fall back to the rule floor so that every companion heals at least that
	// amount per tic regardless of their source npc_types row.
	//
	// We deliberately do NOT add itembonuses.HPRegen or spellbonuses.HPRegen
	// here; GetNPCHPRegen() (called by NPC::Process) already adds those on top
	// of the hp_regen field we are seeding.
	int64 native_regen = NPCTypedata ? NPCTypedata->hp_regen : 0;
	int64 floor_regen  = static_cast<int64>(RuleI(Companions, HPRegenPerTic));
	return std::max(native_regen, floor_regen);
}

// ============================================================
// Mana Regen
// ============================================================

int64 Companion::CalcManaRegen()
{
	// Non-mana users: warriors, rogues, berserkers have no max mana.
	if (GetMaxMana() <= 0) {
		return 0;
	}

	uint8 level    = GetLevel();
	uint8 cls      = GetClass();
	int32 regen    = 2; // flat standing base

	// Bards regenerate slowly whether sitting or standing
	if (cls == Class::Bard) {
		regen = IsSitting() ? 2 : 1;
		regen += itembonuses.ManaRegen + aabonuses.ManaRegen;
		return static_cast<int64>(regen);
	}

	// Non-melee casters use the meditate formula when sitting, or always when AlwaysMeditateRegen is on.
	// BUG-027: AlwaysMeditateRegen (default true) bypasses the sitting requirement for small-group
	// playability — companions are perpetually mana-starved otherwise. Set rule to false to restore
	// authentic sit-to-meditate behavior.
	if (RuleB(Companions, AlwaysMeditateRegen) || IsSitting()) {
		if (GetArchetype() != Archetype::Melee) {
			uint16 meditate = GetSkill(EQ::skills::SkillMeditate);
			regen = (((meditate / 10) + (level - (level / 4))) / 4) + 4;
		}
	}

	regen += spellbonuses.ManaRegen + itembonuses.ManaRegen + aabonuses.ManaRegen;

	// Apply global character regen multiplier then companion-specific multiplier
	regen = (regen * RuleI(Character, ManaRegenMultiplier)) / 100;
	regen = (regen * RuleI(Companions, CompanionManaRegenMult)) / 100;

	return static_cast<int64>(regen);
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

	// Load companion-specific spell list from companion_spell_sets.
	// NPC::AI_Start() above disabled AIautocastspell_timer when AIspells is empty
	// (which is true for most recruited NPCs that have no npc_spells_id).
	// After loading companion spells we re-enable the timer so the cast-check
	// overrides (AI_EngagedCastCheck, AI_PursueCastCheck, AI_IdleCastCheck) fire
	// on schedule.  Without this the timer stays disabled and companions stand
	// idle on initial spawn — they only cast after a level-up which calls
	// LoadCompanionSpells() directly and re-arms the timer via a separate path.
	LoadCompanionSpells();

	// Re-arm cast timer if companion spells were loaded.  The timer controls
	// how frequently Mob::AI_Process() calls the cast-check path; without it
	// the companion never casts on initial spawn.
	if (!m_companion_spells.empty() && AIautocastspell_timer) {
		AIautocastspell_timer->Start(RandomTimer(0, 500), false);
	}

	// Seed hp_regen so that companions whose npc_types row has hp_regen_rate=0
	// still regenerate HP.  CalcHPRegen() returns the greater of the NPC's
	// native rate and the Companions::HPRegenPerTic rule floor.
	hp_regen = CalcHPRegen();

	// Allow companions to use OOC regen expressed as a percentage of max HP.
	// This piggybacks on NPC::Process()'s existing ooc_regen branch which
	// computes: ooc_regen_calc = GetMaxHP() * ooc_regen / 100.
	ooc_regen = RuleI(Companions, OOCRegenPct);
}

void Companion::AI_Stop()
{
	NPC::AI_Stop();
}

// ============================================================
// Combat Role Classification
// ============================================================

CompanionCombatRole Companion::DetermineRoleFromClass(uint8 class_id)
{
	switch (class_id) {
		// Tanks: charge to melee, hold from front
		case Class::Warrior:
		case Class::Paladin:
		case Class::ShadowKnight:
			return COMBAT_ROLE_MELEE_TANK;

		// Rogue: position behind mob for backstab
		case Class::Rogue:
			return COMBAT_ROLE_ROGUE;

		// Melee DPS: charge to melee normally
		case Class::Monk:
		case Class::Berserker:
		case Class::Beastlord:
		case Class::Ranger:
		case Class::Bard:
			return COMBAT_ROLE_MELEE_DPS;

		// Caster DPS: stay at spell range
		case Class::Wizard:
		case Class::Magician:
		case Class::Necromancer:
		case Class::Enchanter:
			return COMBAT_ROLE_CASTER_DPS;

		// Healers: stay at spell range
		case Class::Cleric:
		case Class::Druid:
		case Class::Shaman:
			return COMBAT_ROLE_HEALER;

		default:
			return COMBAT_ROLE_MELEE_DPS;
	}
}

CompanionCombatRole Companion::GetCombatRole() const
{
	return m_combat_role;
}

// ============================================================
// Combat Positioning
// ============================================================

void Companion::UpdateCombatPositioning()
{
	// Reset flag every tick — will be re-set below if we want to hold
	m_hold_combat_position = false;

	if (!IsEngaged() || !GetTarget()) {
		return;
	}

	Mob* target = GetTarget();

	// Safety guard: never run combat positioning against the owner or any group
	// member.  This can happen when the hate list has a stale entry pointing at
	// the owner (e.g., because a previous tick set the target before the hate
	// list was pruned) or when NPC::AI_Process() re-sets the target from the
	// hate list to someone that should not be attacked.  If we detect the
	// target is the owner or a group member, scrub it from the hate list,
	// clear the target, and bail out so the companion returns to normal
	// formation-follow behaviour.
	Client* positioning_owner = GetCompanionOwner();
	if (positioning_owner) {
		if (target == positioning_owner) {
			RemoveFromHateList(target);
			SetTarget(nullptr);
			return;
		}
		Group* pos_grp = GetGroup();
		if (pos_grp && pos_grp->IsGroupMember(target)) {
			RemoveFromHateList(target);
			SetTarget(nullptr);
			return;
		}
	}

	// Additional guard: if we have a follow target (formation-follow) but the
	// target is NOT a genuine hostile NPC, skip combat positioning entirely.
	// This prevents the rogue (and casters) from running class-specific combat
	// movement against non-enemy entities during normal follow.
	if (GetFollowID() && (!target->IsNPC() || target->IsCompanion())) {
		return;
	}

	switch (m_combat_role) {
		case COMBAT_ROLE_MELEE_TANK:
		case COMBAT_ROLE_MELEE_DPS:
			// Default melee behavior — AI_Process handles pursuit and attacks normally
			break;

		case COMBAT_ROLE_ROGUE: {
			if (!RuleB(Companions, RogueBehindMob)) {
				break;
			}
			// BUG-023: Replace PlotPositionAroundTarget (which calculated position from the
			// rogue's location, not the target's) with a direct geometric calculation.
			// We compute a point directly behind the target using the target's heading so the
			// rogue gets a straight line rather than a wide arc around the mob.
			if (!BehindMob(target, GetX(), GetY())) {
				// Backstab offset: step behind the target accounting for its model radius.
				// Using target_size/2 + 2 units ensures we land just outside the model, not inside it.
				float target_size     = target->GetSize() > 0.0f ? target->GetSize() : 5.0f;
				float backstab_dist   = (target_size / 2.0f) + 2.0f;

				// Target's heading in EQ uses 0–512 (north = 0, clockwise). Convert to radians.
				// "Behind" the target is opposite its facing direction, so add pi.
				float target_heading_rad = target->GetHeading() / 256.0f * static_cast<float>(M_PI);
				float behind_dx = std::sin(target_heading_rad + static_cast<float>(M_PI));
				float behind_dy = std::cos(target_heading_rad + static_cast<float>(M_PI));

				// Candidate destination: directly behind the target at backstab_dist
				float dest_x = target->GetX() + behind_dx * backstab_dist;
				float dest_y = target->GetY() + behind_dy * backstab_dist;
				float dest_z = target->GetZ();

				bool valid_dest = false;
				if (CheckPositioningLosFN(target, dest_x, dest_y, dest_z)) {
					valid_dest = true;
				} else {
					// LOS failed (mob against a wall, etc.). Try +/- 30 degrees from directly behind.
					constexpr float kOffsetRad = static_cast<float>(M_PI) / 6.0f; // 30 degrees
					for (float offset : { kOffsetRad, -kOffsetRad }) {
						float alt_dx = std::sin(target_heading_rad + static_cast<float>(M_PI) + offset);
						float alt_dy = std::cos(target_heading_rad + static_cast<float>(M_PI) + offset);
						float alt_x  = target->GetX() + alt_dx * backstab_dist;
						float alt_y  = target->GetY() + alt_dy * backstab_dist;
						if (CheckPositioningLosFN(target, alt_x, alt_y, dest_z)) {
							dest_x     = alt_x;
							dest_y     = alt_y;
							valid_dest = true;
							break;
						}
					}
				}

				if (valid_dest) {
					float dist_sq = DistanceSquaredNoZ(m_Position, glm::vec4(dest_x, dest_y, dest_z, 0.0f));
					if (dist_sq > 25.0f) { // >5 units — not already at destination
						RunTo(dest_x, dest_y, dest_z);
					}
					m_hold_combat_position = true;
				}
				// If no valid LOS destination found, fall through to default melee behavior
				// (m_hold_combat_position stays false, normal pursuit takes over).
			}
			// If already behind, let normal melee AI handle attacks (don't set hold)
			break;
		}

		case COMBAT_ROLE_CASTER_DPS:
		case COMBAT_ROLE_HEALER: {
			int desired_range = RuleI(Companions, CasterCombatRange);
			if (desired_range <= 0) {
				// Rule disabled — fall back to default melee pursue
				break;
			}

			if (GetManaRatio() <= 10.0f) {
				// OOM — hold position rather than charging into melee.
				// A caster with no mana standing at range is far better than a caster
				// with no mana getting hit in melee.  They can still auto-attack from
				// their current position and will resume casting when mana regenerates.
				if (IsMoving()) {
					StopNavigation();
				}
				if (GetTarget()) {
					FaceTarget(GetTarget());
				}
				m_hold_combat_position = true;
				break;
			}

			float dist_sq = DistanceSquaredNoZ(m_Position, target->GetPosition());
			float range_f  = static_cast<float>(desired_range);
			float range_sq = range_f * range_f;

			if (dist_sq <= range_sq && dist_sq >= (range_f * 0.5f) * (range_f * 0.5f)) {
				// Within the sweet spot (50%–100% of desired range).
				// BUG-026: Also check LOS from current position. If the target has moved
				// behind an obstacle while we held position, re-evaluate by stepping closer.
				if (!CheckLosFN(target)) {
					// LOS lost while holding — fall through to "close up" logic below.
					// Recalculate at 70% of desired range toward the target.
					goto caster_reposition;
				}
				if (IsMoving()) {
					StopNavigation();
				}
				FaceTarget();
				m_hold_combat_position = true;
			} else if (dist_sq > range_sq) {
				caster_reposition:
				// Too far from target (or LOS lost in sweet spot) — close to 70% of desired range.
				// BUG-026: Validate LOS at the goal position. If blocked, step closer in 10%
				// increments until we find a position with LOS or hit the 20% minimum.
				float desired_dist = range_f * 0.7f;
				float dx = target->GetX() - GetX();
				float dy = target->GetY() - GetY();
				float len = std::sqrt(dx * dx + dy * dy);
				if (len > 0.0f) {
					float nx = dx / len;
					float ny = dy / len;
					float move_dist = std::sqrt(dist_sq) - desired_dist;
					float goal_x = GetX() + nx * move_dist;
					float goal_y = GetY() + ny * move_dist;
					float goal_z = target->GetZ();
					// LOS check at goal; if blocked step closer by 10% of desired_range each try
					bool los_ok = CheckPositioningLosFN(target, goal_x, goal_y, goal_z);
					if (!los_ok) {
						float step = range_f * 0.1f;
						for (float try_dist = desired_dist - step;
						     try_dist >= range_f * 0.2f && !los_ok;
						     try_dist -= step) {
							float try_move = std::sqrt(dist_sq) - try_dist;
							float try_x = GetX() + nx * try_move;
							float try_y = GetY() + ny * try_move;
							if (CheckPositioningLosFN(target, try_x, try_y, goal_z)) {
								goal_x = try_x;
								goal_y = try_y;
								los_ok = true;
							}
						}
					}
					if (los_ok) {
						RunTo(goal_x, goal_y, goal_z);
					} else {
						// No LOS-valid position found toward the target — hold current position
						// and face target. Better to stand in place than run behind a wall.
						if (IsMoving()) {
							StopNavigation();
						}
						FaceTarget();
					}
				}
				m_hold_combat_position = true;
			} else {
				// Too close (< 50% of desired range) — actively retreat to 70% of desired range.
				// BUG-026: LOS check the retreat destination. If blocked, stop at current position
				// (retreating behind geometry is worse than staying close to the target).
				float desired_dist = range_f * 0.7f;
				float dx = GetX() - target->GetX();  // direction AWAY from target
				float dy = GetY() - target->GetY();
				float len = std::sqrt(dx * dx + dy * dy);
				if (len > 1.0f) {
					// Move from the target's position outward along the away vector
					float nx     = dx / len;
					float ny     = dy / len;
					float goal_x = target->GetX() + nx * desired_dist;
					float goal_y = target->GetY() + ny * desired_dist;
					if (CheckPositioningLosFN(target, goal_x, goal_y, GetZ())) {
						RunTo(goal_x, goal_y, GetZ());
					}
					// If LOS fails at the retreat position, hold current position (we're close
					// to the target but can see them, which is better than retreating to blindness).
				} else {
					// Overlapping with target — retreat toward the owner as a tiebreaker
					Client* retreat_owner = GetCompanionOwner();
					if (retreat_owner) {
						float ox   = retreat_owner->GetX() - target->GetX();
						float oy   = retreat_owner->GetY() - target->GetY();
						float olen = std::sqrt(ox * ox + oy * oy);
						if (olen > 0.0f) {
							float goal_x = target->GetX() + (ox / olen) * desired_dist;
							float goal_y = target->GetY() + (oy / olen) * desired_dist;
							if (CheckPositioningLosFN(target, goal_x, goal_y, GetZ())) {
								RunTo(goal_x, goal_y, GetZ());
							}
						}
					}
				}
				m_hold_combat_position = true;
			}
			break;
		}
	}
}

bool Companion::Process()
{
	// Check death despawn timer
	if (m_death_despawn_timer.Enabled() && m_death_despawn_timer.Check()) {
		LogInfo("Companion [{}] death despawn timer fired — marking dismissed", GetName());
		// Death timer expired without resurrection. Mark the companion as dismissed so
		// the player can re-recruit and restore their companion's equipment/XP/state.
		// SoulWipe() (permanent deletion) is reserved for explicit permanent dismissal
		// via !dismiss permanent — not automatic death expiry. (BUG-012)
		Client* owner = GetCompanionOwner();
		if (owner) {
			owner->Message(Chat::Yellow,
				"%s has returned home. You can recruit them again when you find them.",
				GetCleanName());
		}
		// Safety net: ensure group slot is NULLed before we return false
		// (entity cleanup will safe_delete us; the group must not hold our pointer).
		if (HasGroup()) {
			Group* g = GetGroup();
			if (g) {
				g->MemberZoned(this);
			}
		}

		// Mark as dismissed so re-recruitment can find and restore this companion.
		SetDismissed(true);
		SetSuspended(true);
		Save();
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

	// Enforce flee immunity in sync with the rule every process tick.
	// FleeingImmunity is checked at the top of CheckFlee() (fearpath.cpp),
	// which fires inside NPC::Process() below.  Keeping the ability set
	// here (rather than only in Spawn) ensures runtime rule changes apply
	// immediately and that the ability survives any buff/debuff cycle that
	// might clear special abilities.
	if (!RuleB(Companions, CompanionFleeEnabled)) {
		SetSpecialAbility(SpecialAbility::FleeingImmunity, 1);
	} else {
		SetSpecialAbility(SpecialAbility::FleeingImmunity, 0);
	}

	Client* owner = GetCompanionOwner();

	// PASSIVE STANCE: disengage from all combat, never assist
	if (m_current_stance == COMPANION_STANCE_PASSIVE) {
		if (IsEngaged()) {
			WipeHateList();
			SetTarget(nullptr);
			// Interrupt any offensive spell currently casting
			if (IsCasting() && IsDetrimentalSpell(casting_spell_id)) {
				InterruptSpell();
			}
		}
		// Sit/stand with owner even in passive stance
		if (!IsEngaged() && !IsMoving() && owner && owner->IsClient()) {
			bool owner_sitting = (owner->GetAppearance() == eaSitting);
			if (owner_sitting && !IsSitting()) {
				Sit();
			} else if (!owner_sitting && IsSitting()) {
				Stand();
			}
		}
		// Mana report for passive casters/healers sitting
		if (IsSitting() &&
		    (m_combat_role == COMBAT_ROLE_CASTER_DPS || m_combat_role == COMBAT_ROLE_HEALER) &&
		    GetMaxMana() > 0 &&
		    m_mana_report_timer.Check()) {
			CompanionGroupSay(this, "Mana: %d%%", static_cast<int>(GetManaRatio()));
		}
		// Skip all assist logic — fall through to NPC::Process() for regen and movement
		return NPC::Process();
	}

	// BALANCED STANCE: assist only when the owner or a group member is attacked
	if (owner && m_current_stance == COMPANION_STANCE_BALANCED) {
		Group* grp = GetGroup();
		if (grp && !IsEngaged()) {
			// Scan nearby NPCs and engage any that are attacking a group member
			for (auto& [id, nearby] : GetCloseMobList(200.0f)) {
				if (!nearby || !nearby->IsNPC() || nearby->IsCompanion()) {
					continue;
				}
				for (int i = 0; i < MAX_GROUP_MEMBERS; i++) {
					Mob* member = grp->members[i];
					if (!member || member == this) {
						continue;
					}
					if (nearby->CheckAggro(member) && IsAttackAllowed(nearby)) {
						AddToHateList(nearby, 1);
						SetTarget(nearby);
						break;
					}
				}
				if (IsEngaged()) {
					break;  // found a target — stop scanning
				}
			}
		}
	}

	// AGGRESSIVE STANCE: actively seek and engage hostiles near the owner
	if (owner && m_current_stance == COMPANION_STANCE_AGGRESSIVE && !IsEngaged()) {
		float scan_range = static_cast<float>(RuleI(Companions, AggressiveScanRadius));
		Mob*  closest_hostile = nullptr;
		float closest_dist_sq = scan_range * scan_range;

		for (auto& [id, nearby] : GetCloseMobList(scan_range)) {
			if (!nearby || !nearby->IsNPC() || nearby->IsCompanion()) {
				continue;
			}
			if (!IsAttackAllowed(nearby)) {
				continue;
			}
			// "Hostile" = threatening or worse from the owner's faction perspective
			FACTION_VALUE fv = owner->GetReverseFactionCon(nearby);
			if (fv != FACTION_THREATENINGLY && fv != FACTION_SCOWLS) {
				continue;
			}
			float dist_sq = DistanceSquaredNoZ(m_Position, nearby->GetPosition());
			if (dist_sq < closest_dist_sq) {
				closest_dist_sq = dist_sq;
				closest_hostile = nearby;
			}
		}

		if (closest_hostile) {
			AddToHateList(closest_hostile, 1);
			SetTarget(closest_hostile);
		}
	}

	// For BALANCED: also assist when the owner is actively fighting (auto-attack on).
	// Merely targeting a mob is not enough — the player must have engaged.
	if (owner && m_current_stance == COMPANION_STANCE_BALANCED && !IsEngaged()) {
		if (owner->IsClient() && owner->CastToClient()->AutoAttackEnabled()) {
			Mob* owner_target = owner->GetTarget();
			if (owner_target && IsAttackAllowed(owner_target)) {
				bool target_is_safe = false;
				if (owner_target == owner) {
					target_is_safe = true;
				} else if (owner_target->IsCompanion() &&
				           static_cast<Companion*>(owner_target->CastToNPC())->GetOwnerCharacterID() == m_owner_char_id) {
					target_is_safe = true;
				} else {
					Group* grp = GetGroup();
					if (grp && grp->IsGroupMember(owner_target)) {
						target_is_safe = true;
					}
				}
				if (!target_is_safe) {
					AddToHateList(owner_target, 1);
					SetTarget(owner_target);
				}
			}
		}
	}

	// For AGGRESSIVE: assist the owner's explicit target (any target, attack-on-sight).
	if (owner && m_current_stance == COMPANION_STANCE_AGGRESSIVE) {
		Mob* owner_target = owner->GetTarget();
		if (owner_target) {
			// Safety: companions must never assist against the owner, another
			// companion belonging to the same owner, or any group member.
			bool target_is_safe = false;
			if (owner_target == owner) {
				target_is_safe = true;
			} else if (owner_target->IsCompanion() &&
			           static_cast<Companion*>(owner_target->CastToNPC())->GetOwnerCharacterID() == m_owner_char_id) {
				target_is_safe = true;
			} else {
				Group* grp = GetGroup();
				if (grp && grp->IsGroupMember(owner_target)) {
					target_is_safe = true;
				}
			}

			if (!target_is_safe && IsAttackAllowed(owner_target)) {
				if (!IsEngaged() || GetTarget() == nullptr) {
					AddToHateList(owner_target, 1);
					SetTarget(owner_target);
				}
			}
		}
	}

	// Keep-alive position packet: the Titanium client culls (stops rendering) entities
	// that have not sent a position update for ~5 seconds.  When a companion is
	// stationary (holding position or following without moving), send a zero-delta
	// position packet on a 5-second timer so the client keeps the entity visible.
	// This mirrors the same pattern used by Bot::Process() (bot.cpp ~line 1737-1748).
	if (IsMoving()) {
		m_ping_timer.Disable();
	} else {
		if (!m_ping_timer.Enabled()) {
			m_ping_timer.Start(5000);
		}
		if (m_ping_timer.Check()) {
			SentPositionPacket(0.0f, 0.0f, 0.0f, 0.0f, 0);
		}
	}

	// Sit when owner sits (med/rest synchronization).
	// Only when out of combat and the companion is not moving.
	// Casters/healers sit to regen mana; all companions sit to show idle state.
	if (!IsEngaged() && !IsMoving()) {
		if (owner && owner->IsClient()) {
			bool owner_sitting = (owner->GetAppearance() == eaSitting);
			bool companion_sitting = IsSitting();
			if (owner_sitting && !companion_sitting) {
				Sit();
			} else if (!owner_sitting && companion_sitting) {
				Stand();
			}
		}
	} else if (IsEngaged() && IsSitting()) {
		// Stand when entering combat
		Stand();
	}

	// Mana report via group say while sitting (casters/healers only).
	if (IsSitting() && !IsEngaged() &&
	    (m_combat_role == COMBAT_ROLE_CASTER_DPS || m_combat_role == COMBAT_ROLE_HEALER) &&
	    GetMaxMana() > 0 &&
	    m_mana_report_timer.Check()) {
		int mana_pct = static_cast<int>(GetManaRatio());
		CompanionGroupSay(this, "Mana: %d%%", mana_pct);
	}

	// BUG-024: LOM announcement — casters/healers announce once when mana drops to or below
	// the LOMThresholdPct rule (default 15%). The flag resets when mana recovers above the
	// threshold, so the announcement fires again if mana drops back down (one per dip).
	if (IsEngaged() &&
	    (m_combat_role == COMBAT_ROLE_CASTER_DPS || m_combat_role == COMBAT_ROLE_HEALER) &&
	    GetMaxMana() > 0) {
		float lom_threshold = static_cast<float>(RuleI(Companions, LOMThresholdPct));
		if (lom_threshold > 0.0f) {
			if (GetManaRatio() <= lom_threshold) {
				if (!m_lom_announced) {
					CompanionGroupSay(this, "LOM");
					m_lom_announced = true;
				}
			} else {
				m_lom_announced = false;
			}
		}
	}

	// Update class-based combat positioning before NPC::Process() runs AI_Process().
	// For casters/healers this sets m_hold_combat_position to prevent default melee
	// pursuit. For rogues this routes them behind the target. Must run after target
	// selection so GetTarget() is current.
	UpdateCombatPositioning();

	// Triple attack interception (Phase 2):
	// Mob::DoMainHandAttackRounds() with UseLiveCombatRounds=true only does double
	// attack — it never calls triple attack. This intercept fires the attack timers
	// here (consuming them) so NPC::Process() -> AI_Process() sees them as not ready
	// and skips its own melee call. We then perform the full DoAttackRounds() sequence
	// which includes triple attack for qualifying classes and levels.
	// Mirrors the same pattern used by Bot::Process() -> Bot::DoAttackRounds().
	if (IsEngaged() && !IsStunned() && !IsMezzed() &&
	    GetAppearance() != eaDead && !IsMeleeDisabled()) {
		Mob* atk_target = GetTarget();
		if (atk_target) {
			if (attack_timer.Check()) {
				DoAttackRounds(atk_target, EQ::invslot::slotPrimary);
				TriggerDefensiveProcs(atk_target, EQ::invslot::slotPrimary, false);
			}
			if (CanThisClassDualWield() && attack_dw_timer.Check()) {
				if (CheckDualWield()) {
					DoAttackRounds(atk_target, EQ::invslot::slotSecondary);
					TriggerDefensiveProcs(atk_target, EQ::invslot::slotSecondary, false);
				}
			}
		}
	}

	// Sitting HP regen bonus (Phase 3, corrected by audit Issue #1):
	// NPC::Process() regen code adds only 3 HP/tick for sitting (npc_sitting_regen_bonus).
	// For companions, we want 2-3x the OOC regen rate when sitting and out of combat.
	// We apply an additive sitting bonus BEFORE NPC::Process() runs, so the total
	// regen for a sitting OOC companion is:
	//   NPC::Process() OOC regen  (max(hp_regen, ooc_regen_calc) + 3)
	//   + sitting bonus (base_ooc * (SittingRegenMult - 100) / 100)
	//
	// At SittingRegenMult=200 (2x): sitting bonus equals the base OOC regen,
	// so total sitting regen = 2x the standing OOC regen rate.
	//
	// CRITICAL: This must be gated behind m_sitting_regen_timer (6-second cadence)
	// to match the tic-based regen in NPC::Process(). Without this gate, the bonus
	// fires on every Process() tick (~6-40 times per 6-second tic), causing 24-40x
	// overregen that refills HP almost instantly.
	//
	// Conditions: must be sitting, not engaged, have HP to recover, and timer fired.
	if (IsSitting() && !IsEngaged() && GetHP() < GetMaxHP()
	    && m_sitting_regen_timer.Check()) {
		int mult = RuleI(Companions, SittingRegenMult);
		if (mult > 100) {
			// Compute the OOC regen amount NPC::Process would apply
			int ooc_pct = RuleI(Companions, OOCRegenPct);
			if (ooc_pct > 0) {
				int64 base_ooc = (GetMaxHP() * ooc_pct) / 100;
				// Additive bonus beyond the base OOC regen
				int64 sitting_bonus = (base_ooc * (mult - 100)) / 100;
				if (sitting_bonus > 0) {
					SetHP(std::min(GetHP() + sitting_bonus, GetMaxHP()));
				}
			}
		}
	}

	bool npc_result = NPC::Process();
	if (!npc_result) {
		LogInfo("Companion [{}] (id {}) NPC::Process() returned false — npc_depop=[{}] companion_depop=[{}] IsEngaged=[{}] HP=[{}]",
		        GetName(), GetID(), (int)NPC::GetDepop(), (int)m_depop, (int)IsEngaged(), GetHP());
	}
	return npc_result;
}

bool Companion::AI_EngagedCastCheck()
{
	LogAIDetail("Companion [{}] AI_EngagedCastCheck: spells=[{}] target=[{}] engaged=[{}] mana=[{:.0f}%%]",
	            GetName(), m_companion_spells.size(),
	            GetTarget() ? GetTarget()->GetName() : "none",
	            IsEngaged(), GetManaRatio());
	// Delegate to the companion spell AI in companion_ai.cpp
	return AICastSpell(GetChanceToCastBySpellType(0), 0xFFFFFFFF);
}

bool Companion::AI_IdleCastCheck()
{
	// Only buff/heal when idle
	return AICastSpell(GetChanceToCastBySpellType(0), SpellType_Buff | SpellType_Heal | SpellType_Pet);
}

bool Companion::AI_PursueCastCheck()
{
	LogAIDetail("Companion [{}] AI_PursueCastCheck: hold=[{}] role=[{}] spells=[{}] target=[{}] engaged=[{}] mana=[{:.0f}%%]",
	            GetName(), m_hold_combat_position, static_cast<int>(m_combat_role),
	            m_companion_spells.size(),
	            GetTarget() ? GetTarget()->GetName() : "none",
	            IsEngaged(), GetManaRatio());
	// For casters and healers that hold position at range (m_hold_combat_position
	// is set by UpdateCombatPositioning()), we cast spells from this distance
	// rather than pursuing to melee.  Call our own AICastSpell() so the companion
	// uses the companion_spell_sets data, not the NPC spell list.
	//
	// For melee roles (TANK, MELEE_DPS, ROGUE) we return false so AI_Process()
	// falls through to the normal pursue-to-melee branch.
	if (m_hold_combat_position) {
		switch (m_combat_role) {
			case COMBAT_ROLE_CASTER_DPS:
			case COMBAT_ROLE_HEALER:
				LogAIDetail("Companion [{}] AI_PursueCastCheck: holding position, attempting AICastSpell", GetName());
				return AICastSpell(GetChanceToCastBySpellType(0), 0xFFFFFFFF);
			default:
				LogAIDetail("Companion [{}] AI_PursueCastCheck: melee role [{}], not casting from pursue", GetName(), static_cast<int>(m_combat_role));
				break;
		}
	}
	return false;
}

// Companion::AICastSpell is implemented in companion_ai.cpp

bool Companion::AIDoSpellCast(uint16 spellid, Mob* tar, int32 mana_cost,
                              uint32* oDontDoAgainBefore)
{
	LogAIDetail("Companion [{}] AIDoSpellCast: spell=[{}] ({}) target=[{}] mana_cost=[{}] currently_casting=[{}] mana=[{:.0f}%%]",
	            GetName(), spellid,
	            IsValidSpell(spellid) ? spells[spellid].name : "INVALID",
	            tar ? tar->GetName() : "none",
	            mana_cost,
	            casting_spell_id,
	            GetManaRatio());

	// Group-say combat logging: announce spell cast to the group so the player
	// can see what the companion is doing.
	if (IsValidSpell(spellid) && tar) {
		const char* spell_name = spells[spellid].name;
		int mana_pct = GetMaxMana() > 0 ? static_cast<int>(GetManaRatio()) : 100;
		if (tar == this) {
			CompanionGroupSay(this, "Casting %s on myself [Mana: %d%%]",
			                  spell_name, mana_pct);
		} else {
			CompanionGroupSay(this, "Casting %s on %s [Mana: %d%%]",
			                  spell_name, tar->GetCleanName(), mana_pct);
		}
	}

	bool result = CastSpell(spellid, tar ? tar->GetID() : 0, EQ::spells::CastingSlot::Gem2,
	                        -1, mana_cost, oDontDoAgainBefore);
	LogAIDetail("Companion [{}] AIDoSpellCast: spell=[{}] CastSpell result=[{}]",
	            GetName(), spellid, result);
	return result;
}

// ============================================================
// FillSpawnStruct
// ============================================================

void Companion::FillSpawnStruct(NewSpawn_Struct* ns, Mob* ForWho)
{
	NPC::FillSpawnStruct(ns, ForWho);

	// Override is_npc and NPC to make the companion appear player-like to the
	// Titanium client.  This mirrors Bot::FillSpawnStruct (bot.cpp:3807-3813)
	// and ensures the client treats the companion as a valid group member
	// target when the player clicks the name tile in the Group Window.
	ns->spawn.is_npc = 0;
	ns->spawn.is_pet = 0;
	ns->spawn.NPC    = 0;   // 0=player, 1=npc, 2=pc corpse, 3=npc corpse
}

// ============================================================
// Equipment appearance overrides (Bug 2)
// ============================================================

uint32 Companion::GetEquipmentMaterial(uint8 material_slot) const
{
	if (material_slot >= EQ::textures::materialCount) {
		return 0;
	}

	int16 invslot = EQ::InventoryProfile::CalcSlotFromMaterial(material_slot);
	if (invslot == INVALID_INDEX) {
		return 0;
	}

	// Check companion's own equipment first
	if (invslot >= EQ::invslot::EQUIPMENT_BEGIN &&
	    invslot <= EQ::invslot::EQUIPMENT_END &&
	    m_equipment[invslot] != 0) {
		// Item exists in companion equipment — use Mob base class to resolve material
		return Mob::GetEquipmentMaterial(material_slot);
	}

	// No companion equipment in this slot — fall back to NPC base appearance
	return NPC::GetEquipmentMaterial(material_slot);
}

uint32 Companion::GetEquippedItemFromTextureSlot(uint8 material_slot) const
{
	if (material_slot >= EQ::textures::materialCount) {
		return 0;
	}

	const int16 inventory_slot = EQ::InventoryProfile::CalcSlotFromMaterial(material_slot);
	if (inventory_slot == INVALID_INDEX) {
		return 0;
	}

	if (inventory_slot >= EQ::invslot::EQUIPMENT_BEGIN &&
	    inventory_slot <= EQ::invslot::EQUIPMENT_END) {
		return m_equipment[inventory_slot];
	}

	return 0;
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

	// --- Name normalization ---
	// Ensure the raw `name` field matches `GetCleanName()` so the spawn packet
	// name and the group window member name are identical.  The NPC constructor
	// runs MakeNameUnique which appends a 3-digit suffix (e.g. "Guard_Liben001")
	// but GetCleanName() strips underscores and digits to produce "Guard Liben".
	// The Titanium client resolves group window clicks to entity IDs by matching
	// the group member name (from GetCleanName) against spawn names — if they
	// differ the client silently fails to target.
	// This mirrors Bot::Spawn() which does the same (see bot.cpp:3605).
	//
	// Invalidate the clean_name cache first so any subsequent GetCleanName()
	// call recomputes from the new `name` value.
	clean_name[0] = '\0';
	strcpy(name, GetCleanName());

	// Add to entity list — this assigns the entity ID and sends spawn packet
	// immediately (dont_queue = true so the companion appears in the world right
	// away, not on the next spawn tick).
	entity_list.AddCompanion(this, true, true);

	if (!GetID()) {
		LogError("Companion::Spawn: failed to get entity ID for [{}]", GetName());
		return false;
	}

	// Start the NPC AI so the companion acts in combat, follows the owner,
	// and casts spells.  LoadCompanionSpells() is called inside AI_Start().
	AI_Start();

	// Strip problematic special abilities inherited from the source NPC.
	// ProcessSpecialAbilities() runs inside NPC::AI_Start() and re-applies
	// whatever is in NPCTypedata->special_abilities.  We strip after AI_Start()
	// so companions are never immune to melee/magic, and are never invulnerable,
	// regardless of what the source NPC's special_abilities field contains.
	SetSpecialAbility(SpecialAbility::MeleeImmunity, 0);
	SetSpecialAbility(SpecialAbility::MagicImmunity, 0);
	SetInvul(false);

	// Apply flee immunity based on the rule. Using FleeingImmunity (ability 21)
	// is more robust than clearing currently_fleeing in Process() because
	// FleeingImmunity is checked at the top of CheckFlee() before any flee
	// state can be set.  The rule value is re-evaluated in Process() as well
	// so runtime changes to the rule take effect immediately.
	if (!RuleB(Companions, CompanionFleeEnabled)) {
		SetSpecialAbility(SpecialAbility::FleeingImmunity, 1);
	}

	// Join the owner's group (creates a new group if the owner is not already
	// in one).  Sets follow ID so the companion trails the owner.
	CompanionJoinClientGroup();

	LogInfo("Companion::Spawn: [{}] spawned for owner [{}] (entity id: {})",
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
	// Guard against re-entrant calls. DisbandGroup() (called from
	// RemoveCompanionFromGroup inside Depop) iterates group members and calls
	// Zone() again on any companions it finds. Without this guard that results in
	// double Save() + Depop() per companion. (BUG-008)
	if (m_is_zoning) {
		return;
	}
	m_is_zoning = true;

	UpdateTimeActive(); // accrue elapsed seconds before saving
	if (zone) {
		RecordZoneVisit(zone->GetZoneID());
	}
	Save();
	Depop();
	// Note: m_is_zoning is intentionally not reset here. Once Zone() has been
	// called the companion is being depoped; the flag stays true for the
	// lifetime of the object to block any further re-entrant invocations.
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

	// Capture owner id before removal from entity list so the reassignment
	// call below only sees the companions that remain active.
	uint32 owner_char_id = m_owner_char_id;
	entity_list.RemoveCompanion(GetID());

	// Reassign formation slots for remaining companions now that this one
	// has left the entity list.
	if (owner_char_id != 0) {
		ReassignFormationSlots(owner_char_id);
	}

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
		// Voluntary dismissal: mark suspended and dismissed so re-recruitment can find
		// this record. is_dismissed=1 is the sentinel CreateFromNPC() uses to locate
		// a previously-dismissed companion and restore its equipment/XP/state. (BUG-012)
		// The +10% re-recruitment bonus is tracked via companion_data.dismissed_at
		SetSuspended(true);
		SetDismissed(true);
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
		// No group exists — create a new one following the Bot pattern exactly.
		// new Group(owner) puts the owner in members[0] and sets SetGrouped(true),
		// but does NOT send any client packets yet.
		g = new Group(owner);
		if (!g) {
			return false;
		}

		// AddMember populates members[1]/membername[1] and sends OP_GroupUpdate
		// (groupActJoin) to the owner's client so the companion appears in the
		// group window.  This must happen before entity_list.AddGroup assigns the
		// group ID; SendGroupJoinOOZ inside VerifyGroup is a no-op until then.
		if (!g->AddMember(this)) {
			delete g;
			return false;
		}

		entity_list.AddGroup(g);

		if (g->GetID() == 0) {
			delete g;
			return false;
		}

		database.SetGroupLeaderName(g->GetID(), owner->GetName());
		g->SaveGroupLeaderAA();

		// Notify the leader client that it is now a group leader and send
		// the full group update — matches what AddBotToGroup does for bots.
		if (g->GroupCount() == 2 && g->GetLeader() && g->GetLeader()->IsClient()) {
			g->UpdateGroupAAs();
			g->SendUpdate(groupActUpdate, g->GetLeader());
		}

		g->VerifyGroup();
		g->SendGroupJoinOOZ(this);

		// DB writes — persist both the owner and the companion in group_id table.
		g->AddToGroup(owner);
		g->AddToGroup(this);
		SetFollowID(owner->GetID());
		ReassignFormationSlots(m_owner_char_id);

		LogInfo("Companion [{}] joined new group with [{}]", GetName(), owner->GetName());
	} else {
		// Existing group — AddMember populates the members array and sends
		// OP_GroupUpdate to all in-zone clients so the companion appears in
		// the group window.
		if (g->AddMember(this)) {
			// Send group AA and full update to the leader, then sync group state.
			if (g->GetLeader() && g->GetLeader()->IsClient()) {
				g->UpdateGroupAAs();
				g->SendUpdate(groupActUpdate, g->GetLeader());
			}
			g->VerifyGroup();
			g->SendGroupJoinOOZ(this);

			// DB write — persist the companion in the group_id table.
			g->AddToGroup(this);

			SetFollowID(owner->GetID());
			ReassignFormationSlots(m_owner_char_id);
			LogInfo("Companion [{}] joined existing group with [{}]", GetName(), owner->GetName());
		} else {
			Suspend();
			LogInfo("Companion [{}] failed to join existing group — suspending", GetName());
		}
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
			ReassignFormationSlots(companion->GetOwnerCharacterID());
			return true;
		}
		RemoveCompanionFromGroup(companion, companion->GetGroup());
	}

	if (group->AddMember(companion) && companion->GetCompanionOwner()) {
		// Sync group AA / update and DB state, matching the Bot pattern used in
		// AddBotToGroup + its callers.
		if (group->GetLeader() && group->GetLeader()->IsClient()) {
			group->UpdateGroupAAs();
			group->SendUpdate(groupActUpdate, group->GetLeader());
		}
		group->VerifyGroup();
		group->SendGroupJoinOOZ(companion);
		group->AddToGroup(companion);

		companion->SetFollowID(companion->GetCompanionOwner()->GetID());
		ReassignFormationSlots(companion->GetOwnerCharacterID());
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
	cd.is_dismissed    = m_is_dismissed ? 1 : 0;
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
	m_is_dismissed          = (cd.is_dismissed == 1);
	m_spawn2_id             = cd.spawn2_id;
	m_spawngroupid          = cd.spawngroupid;
	m_companion_xp          = static_cast<uint32>(cd.experience);
	m_recruited_level       = cd.recruited_level;
	m_total_kills           = cd.total_kills;
	m_zones_visited         = cd.zones_visited.empty() ? "[]" : cd.zones_visited;
	m_time_active           = cd.time_active;
	m_times_died            = cd.times_died;
	m_active_since          = static_cast<uint32>(time(nullptr));

	// Apply saved level if the companion has leveled past its recruited level.
	// The constructor sets stats from npc_type->level (the original NPC's base
	// level). If the companion earned XP and leveled up, cd.level will be higher
	// than m_recruited_level. ScaleStatsToLevel() uses m_recruited_level (just
	// restored above) and m_base_* (set in the constructor from npc_type) as the
	// reference point, so it must be called after m_recruited_level is restored
	// but before equipment bonuses are applied.
	if (cd.level > 0 && static_cast<uint8>(cd.level) != GetLevel()) {
		ScaleStatsToLevel(static_cast<uint8>(cd.level));
	}

	// Restore HP/mana if not suspended at max
	if (cd.cur_hp > 0) {
		SetHP(cd.cur_hp);
	}
	if (cd.cur_mana > 0) {
		SetMana(cd.cur_mana);
	}

	// Load equipment from companion_inventories table
	LoadEquipment();

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
	equipment[slot] = item_id;  // sync to NPC::equipment[] for direct-access code paths

	// Populate inventory profile so CalcItemBonuses() finds this item when computing stats.
	// Bot system pattern: database.CreateItem → GetInv().PutItem (bot.cpp:4083).
	EQ::ItemInstance* inst = database.CreateItem(item_id);
	if (inst) {
		GetInv().PutItem(slot, *inst);

		// Mirror the bow/arrow flag logic from loot.cpp so ranged-attack AI
		// (HasBowAndArrowEquipped) works when equipment is given via GiveItem().
		if (slot == EQ::invslot::slotRange && inst->GetItem() &&
		    inst->GetItem()->ItemType == EQ::item::ItemTypeBow) {
			SetBowEquipped(true);
		}
		if (slot == EQ::invslot::slotAmmo && inst->GetItem() &&
		    inst->GetItem()->ItemType == EQ::item::ItemTypeArrow) {
			SetArrowEquipped(true);
		}

		delete inst;
	}

	uint8 mat_slot = EQ::InventoryProfile::CalcMaterialFromSlot(slot);
	if (mat_slot != EQ::textures::materialInvalid) {
		SendWearChange(mat_slot);
	}
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
	equipment[slot] = 0;  // sync to NPC::equipment[]

	// Remove from inventory profile so CalcItemBonuses() stops applying the removed item's stats.
	GetInv().DeleteItem(slot);

	uint8 mat_slot = EQ::InventoryProfile::CalcMaterialFromSlot(slot);
	if (mat_slot != EQ::textures::materialInvalid) {
		SendWearChange(mat_slot);
	}
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

			// Populate m_inv so CalcItemBonuses() applies stats from loaded equipment.
			// Called once per companion load (zone-in, re-recruitment, unsuspend).
			EQ::ItemInstance* inst = database.CreateItem(row.item_id);
			if (inst) {
				GetInv().PutItem(static_cast<int16>(row.slot_id), *inst);

				// Set ranged-attack AI flags so HasBowAndArrowEquipped() works
				// on companions that had a bow/arrows saved from a previous session.
				if (row.slot_id == EQ::invslot::slotRange && inst->GetItem() &&
				    inst->GetItem()->ItemType == EQ::item::ItemTypeBow) {
					SetBowEquipped(true);
				}
				if (row.slot_id == EQ::invslot::slotAmmo && inst->GetItem() &&
				    inst->GetItem()->ItemType == EQ::item::ItemTypeArrow) {
					SetArrowEquipped(true);
				}

				delete inst;
			}
		}
	}

	CalcBonuses();

	// Sync to NPC::equipment[] for code paths that read the inherited array
	for (int slot = EQ::invslot::EQUIPMENT_BEGIN; slot <= EQ::invslot::EQUIPMENT_END; slot++) {
		equipment[slot] = m_equipment[slot];
	}

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
// Equipment listing / retrieval
// ============================================================

// Helper: map a Lua-friendly slot name to an EQ inventory slot constant.
// Returns SLOT_INVALID (-1) if not recognised.
static int16 SlotNameToSlotID(const std::string& slot_name)
{
	// Lowercase for case-insensitive comparison
	std::string lower = slot_name;
	for (auto& c : lower) { c = static_cast<char>(tolower(static_cast<unsigned char>(c))); }

	if (lower == "charm")                                                    return EQ::invslot::slotCharm;
	if (lower == "ear1" || lower == "ear 1")                                return EQ::invslot::slotEar1;
	if (lower == "head" || lower == "helm" || lower == "helmet")            return EQ::invslot::slotHead;
	if (lower == "face" || lower == "mask")                                  return EQ::invslot::slotFace;
	if (lower == "ear2" || lower == "ear 2")                                return EQ::invslot::slotEar2;
	if (lower == "neck" || lower == "necklace")                             return EQ::invslot::slotNeck;
	if (lower == "shoulder" || lower == "shoulders")                        return EQ::invslot::slotShoulders;
	if (lower == "arms" || lower == "arm")                                  return EQ::invslot::slotArms;
	if (lower == "back" || lower == "cloak" || lower == "cape")             return EQ::invslot::slotBack;
	if (lower == "wrist1" || lower == "wrist 1" || lower == "leftwrist")    return EQ::invslot::slotWrist1;
	if (lower == "wrist2" || lower == "wrist 2" || lower == "rightwrist")   return EQ::invslot::slotWrist2;
	if (lower == "range" || lower == "ranged" || lower == "bow")            return EQ::invslot::slotRange;
	if (lower == "hands" || lower == "hand" || lower == "gloves")           return EQ::invslot::slotHands;
	if (lower == "primary" || lower == "weapon" || lower == "mainhand" || lower == "main") return EQ::invslot::slotPrimary;
	if (lower == "secondary" || lower == "offhand" || lower == "off")       return EQ::invslot::slotSecondary;
	if (lower == "finger1" || lower == "finger 1" || lower == "ring1" || lower == "leftfinger")  return EQ::invslot::slotFinger1;
	if (lower == "finger2" || lower == "finger 2" || lower == "ring2" || lower == "rightfinger") return EQ::invslot::slotFinger2;
	if (lower == "chest" || lower == "body" || lower == "torso")            return EQ::invslot::slotChest;
	if (lower == "legs" || lower == "leg")                                  return EQ::invslot::slotLegs;
	if (lower == "feet" || lower == "boots" || lower == "foot")             return EQ::invslot::slotFeet;
	if (lower == "waist" || lower == "belt")                                return EQ::invslot::slotWaist;
	if (lower == "ammo" || lower == "ammunition" || lower == "arrows")      return EQ::invslot::slotAmmo;

	return EQ::invslot::SLOT_INVALID;
}

void Companion::ShowEquipment(Client* client)
{
	if (!client) {
		return;
	}

	client->Message(Chat::Yellow, "=== %s - Equipment ===", GetCleanName());

	// 19-slot display order per PRD. Charm, Ear1, Ear2 are omitted from display
	// (still stored and apply stats, just not shown per design).
	static const struct { int16 slot; const char* label; bool is_weapon; } kDisplaySlots[] = {
		{ EQ::invslot::slotHead,      "Head",      false },
		{ EQ::invslot::slotFace,      "Face",      false },
		{ EQ::invslot::slotNeck,      "Neck",      false },
		{ EQ::invslot::slotShoulders, "Shoulders", false },
		{ EQ::invslot::slotChest,     "Chest",     false },
		{ EQ::invslot::slotBack,      "Back",      false },
		{ EQ::invslot::slotArms,      "Arms",      false },
		{ EQ::invslot::slotWrist1,    "Wrist 1",   false },
		{ EQ::invslot::slotWrist2,    "Wrist 2",   false },
		{ EQ::invslot::slotHands,     "Hands",     false },
		{ EQ::invslot::slotFinger1,   "Finger 1",  false },
		{ EQ::invslot::slotFinger2,   "Finger 2",  false },
		{ EQ::invslot::slotLegs,      "Legs",      false },
		{ EQ::invslot::slotFeet,      "Feet",      false },
		{ EQ::invslot::slotWaist,     "Waist",     false },
		{ EQ::invslot::slotPrimary,   "Primary",   true  },
		{ EQ::invslot::slotSecondary, "Secondary", true  },
		{ EQ::invslot::slotRange,     "Range",     true  },
		{ EQ::invslot::slotAmmo,      "Ammo",      false },
	};

	for (const auto& entry : kDisplaySlots) {
		uint32 item_id = m_equipment[entry.slot];
		if (item_id != 0) {
			const EQ::ItemData* item = database.GetItem(item_id);
			if (!item) {
				client->Message(Chat::White, "  %-12s (unknown item)", entry.label);
				continue;
			}
			// Generate a clickable item link via quest_manager (safe to call
			// without active quest context — varlink only uses database.CreateItem
			// and EQ::SayLinkEngine, not the quest initiator).
			std::string link = quest_manager.varlink(item_id);
			if (entry.is_weapon && item->Damage > 0) {
				client->Message(Chat::White, "  %-12s %s (Dmg: %d  Delay: %d)",
				    entry.label, link.c_str(), item->Damage, item->Delay);
			} else if (!entry.is_weapon && item->AC > 0) {
				client->Message(Chat::White, "  %-12s %s (AC: %d)",
				    entry.label, link.c_str(), item->AC);
			} else {
				client->Message(Chat::White, "  %-12s %s",
				    entry.label, link.c_str());
			}
		} else {
			client->Message(Chat::White, "  %-12s (empty)", entry.label);
		}
	}
}

// GiveSlot: return the item in the named slot to the client.
// Returns true if an item was returned, false if the slot was empty or invalid.
// The caller is responsible for telling the player when false is returned (e.g.
// the !unequip command prints "nothing equipped"; the trade handler stays silent).
bool Companion::GiveSlot(Client* client, const std::string& slot_name)
{
	if (!client) {
		return false;
	}

	int16 slot = SlotNameToSlotID(slot_name);
	if (slot == EQ::invslot::SLOT_INVALID) {
		client->Message(Chat::Red,
		    "Unknown slot name '%s'. Valid slots: head, face, neck, shoulders, chest, back, "
		    "arms, wrist1, wrist2, hands, finger1, finger2, legs, feet, waist, primary, "
		    "secondary, range, ammo",
		    slot_name.c_str());
		return false;
	}

	uint32 item_id = m_equipment[slot];
	if (item_id == 0) {
		// Slot is empty — nothing to return.
		return false;
	}

	// Remove from companion
	RemoveItemFromSlot(slot);

	// Give to client
	const EQ::ItemData* item = database.GetItem(item_id);
	const char* item_name = item ? item->Name : "(unknown item)";
	const char* slot_label = EQ::invslot::GetInvPossessionsSlotName(slot);

	client->SummonItem(item_id);

	client->Message(Chat::Yellow,
	    "%s returns %s (slot: %s) to you.",
	    GetCleanName(), item_name, slot_label);

	LogInfo("Companion [{}] gave item [{}] (slot [{}]) to client [{}]",
	    GetName(), item_id, slot, client->GetName());
	return true;
}

void Companion::GiveAll(Client* client)
{
	if (!client) {
		return;
	}

	bool any = false;
	// Iterate all equipment slots and give each occupied one to the client
	for (int slot = EQ::invslot::EQUIPMENT_BEGIN; slot <= EQ::invslot::EQUIPMENT_END; ++slot) {
		if (m_equipment[slot] == 0) {
			continue;
		}

		uint32 item_id = m_equipment[slot];

		// Remove from companion first (updates m_equipment, saves, sends wear change)
		RemoveItemFromSlot(static_cast<int16>(slot));

		// Give to client
		client->SummonItem(item_id);

		const EQ::ItemData* item = database.GetItem(item_id);
		const char* item_name = item ? item->Name : "(unknown item)";
		const char* slot_label = EQ::invslot::GetInvPossessionsSlotName(static_cast<int16>(slot));

		client->Message(Chat::Yellow,
		    "%s returns %s (%s) to you.",
		    GetCleanName(), item_name, slot_label);

		any = true;
	}

	if (!any) {
		client->Message(Chat::Yellow,
		    "%s has no equipment to return.", GetCleanName());
	}
}

// ============================================================
// XP / Leveling (Task 19)
// ============================================================

void Companion::AddExperience(uint32 xp)
{
	m_companion_xp += xp;

	// Loop to handle cascading level-ups (e.g., cap released when player levels up
	// and companion had stored XP beyond the old threshold).
	bool leveled = false;
	while (CheckForLevelUp()) {
		leveled = true;
	}

	if (leveled) {
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

	// Companion max level = player_level - MaxLevelOffset, clamped to [1, 60]
	Client* owner = GetCompanionOwner();
	if (!owner) {
		return false;
	}

	uint8 max_level = owner->GetLevel();
	int offset = RuleI(Companions, MaxLevelOffset);
	if (offset < 0) { offset = 0; }
	if (offset > 59) { offset = 59; }
	if (max_level > (uint8)offset) {
		max_level -= (uint8)offset;
	} else {
		max_level = 1;
	}

	// Absolute hard cap: companions may never exceed level 60 (Classic-Luclin era ceiling)
	if (max_level > 60) {
		max_level = 60;
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

	// Restore HP and mana to full as a level-up reward
	SetHP(GetMaxHP());
	SetMana(GetMaxMana());

	// Notify clients of HP/level changes so the group window updates correctly.
	// Matches the bot level-up pattern (bot.cpp:4015-4016).
	SendHPUpdate();
	SendAppearancePacket(AppearanceType::WhoLevel, new_level, true, true);

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

uint32 Companion::GetTimeActive() const
{
	uint32 total = m_time_active;
	if (m_active_since > 0) {
		uint32 now = static_cast<uint32>(time(nullptr));
		if (now > m_active_since) {
			total += (now - m_active_since);
		}
	}
	return total;
}

uint32 Companion::GetRecruitedZoneID() const
{
	// m_zones_visited format: "[]" (empty) or "[id1,id2,...]"
	// The first element is the recruited zone.
	if (m_zones_visited.size() <= 2) {
		return 0; // empty array "[]"
	}
	// Find first digit after the opening '['
	size_t start = m_zones_visited.find('[');
	if (start == std::string::npos) {
		return 0;
	}
	++start; // skip '['
	size_t end = m_zones_visited.find_first_of(",]", start);
	if (end == std::string::npos || end == start) {
		return 0;
	}
	const std::string id_str = m_zones_visited.substr(start, end - start);
	try {
		return static_cast<uint32>(std::stoul(id_str));
	} catch (...) {
		return 0;
	}
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

// Companion::LoadCompanionSpells is implemented in companion_ai.cpp

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
	SetAppearance(eaSitting);
	m_mana_report_timer.Start(15000);
}

void Companion::Stand()
{
	SetAppearance(eaStanding);
	m_mana_report_timer.Disable();
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
// Formation slot assignment
//
// Distributes all active companions for an owner in a 120-degree arc
// centred directly in front of the owner, with equal angular spacing.
// The narrow forward arc keeps companions visible and clickable when
// the player stops, since they fan out in front rather than behind.
//
//   N=1 companions : offset = 0 (directly in front)
//   N=2 companions : offsets at -arc/2, +arc/2  (left-front, right-front)
//   N>=2 companions: step = arc / (N-1), offset[i] = -arc/2 + i*step
//
// Offsets are stored as EQ heading units (0-512 scale).
// 120 degrees = 120/360 * 512 = 170.667 heading units.
// ============================================================

void Companion::AssignFormationSlot()
{
	Client* owner = GetCompanionOwner();
	if (!owner) {
		return;
	}

	auto companions = entity_list.GetCompanionsByOwnerCharacterID(m_owner_char_id);
	int total = static_cast<int>(companions.size());
	if (total == 0) {
		return;
	}

	// Sort by companion_id for consistent, deterministic slot ordering.
	std::sort(companions.begin(), companions.end(),
		[](const Companion* a, const Companion* b) {
			return a->GetCompanionID() < b->GetCompanionID();
		});

	// Find this companion's index in the sorted list.
	int my_slot = 0;
	for (int i = 0; i < total; i++) {
		if (companions[i] == this) {
			my_slot = i;
			break;
		}
	}

	// 120 degrees converted to EQ heading units: 120.0f / 360.0f * 512.0f
	// This fans companions out in a forward arc so they are visible and
	// clickable when the player stops.
	const float arc_width = 170.667f;

	float offset = 0.0f;
	if (total >= 2) {
		float step = arc_width / static_cast<float>(total - 1);
		offset = -arc_width / 2.0f + static_cast<float>(my_slot) * step;
	}

	SetFollowAngleOffset(offset);
	SetFollowFormationDistance(static_cast<float>(RuleI(Companions, FormationDistance)));
}

void Companion::ReassignFormationSlots(uint32 owner_char_id)
{
	auto companions = entity_list.GetCompanionsByOwnerCharacterID(owner_char_id);
	for (auto* c : companions) {
		if (c) {
			c->AssignFormationSlot();
		}
	}
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

		// Spawn() is the single entry point for spawning companions.  It
		// normalizes the name field so spawn packet and group window names
		// match, adds to the entity list, starts AI, and joins the owner's
		// group.
		if (!companion->Spawn(this)) {
			LogError(
				"SpawnCompanionsOnZone: Spawn() failed for companion id {} (owner {})",
				cd.id, char_id
			);
			delete companion;
			continue;
		}

		LogInfo(
			"SpawnCompanionsOnZone: spawned companion '{}' (id {}) for player '{}'",
			cd.name, cd.id, GetName()
		);
	}
}
