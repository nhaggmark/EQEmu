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

#pragma once

#include "zone/npc.h"
#include <algorithm>
#include <string>

class Client;
class Corpse;
class Group;
class Mob;
class Raid;
struct NPCType;
struct NewSpawn_Struct;

namespace EQ
{
	struct ItemData;
}

// Companion stance IDs
#define COMPANION_STANCE_PASSIVE    0
#define COMPANION_STANCE_BALANCED   1
#define COMPANION_STANCE_AGGRESSIVE 2

// Companion type: pure companion (loyal) vs. mercenary-type (faction-dependent retention)
#define COMPANION_TYPE_COMPANION    0
#define COMPANION_TYPE_MERCENARY    1

// Maximum AI spell range for companions
const int CompanionAISpellRange = 100;

struct CompanionSpell {
	uint16 spellid;       // <= 0 = no spell
	uint32 type;          // spell type category (heal, nuke, buff, etc.)
	int16  stance;        // 0 = all stances, positive = only this stance, negative = all except
	int16  slot;
	uint16 proc_chance;
	uint32 time_cancast;  // epoch ms when we can cast this spell next
	int16  min_hp_pct;    // don't cast if target HP below this
	int16  max_hp_pct;    // don't cast if target HP above this
};

// Combat role classification for positioning behavior
enum CompanionCombatRole : uint8 {
	COMBAT_ROLE_MELEE_TANK,  // Warrior, Paladin, Shadow Knight — charge to melee, hold front
	COMBAT_ROLE_MELEE_DPS,   // Monk, Berserker, Beastlord, Ranger, Bard — charge to melee
	COMBAT_ROLE_ROGUE,       // Rogue — circle behind mob for backstab
	COMBAT_ROLE_CASTER_DPS,  // Wizard, Magician, Necromancer, Enchanter — stay at spell range
	COMBAT_ROLE_HEALER       // Cleric, Druid, Shaman — stay at spell range
};

// ============================================================
// Companion class
//
// Inherits from NPC. Stats come directly from npc_types.
// Group integration and zone persistence follow Merc patterns.
// Spell AI is class-aware (all 15 Classic-Luclin classes).
// ============================================================
class Companion : public NPC {
public:
	// Constructor: takes same NPCType* as NPC, plus owner and companion metadata
	Companion(const NPCType* npc_type_data, float x, float y, float z, float heading,
	          uint32 owner_char_id, uint8 companion_type = COMPANION_TYPE_COMPANION);
	virtual ~Companion();

	// -------------------------------------------------------
	// Factory method: create a Companion from a live NPC in the world
	// Returns nullptr on failure (ineligible, DB error, etc.)
	// -------------------------------------------------------
	static Companion* CreateFromNPC(Client* owner, NPC* source_npc);

	// -------------------------------------------------------
	// Entity virtual overrides
	// -------------------------------------------------------
	virtual bool Death(Mob* killer_mob, int64 damage, uint16 spell_id,
	                   EQ::skills::SkillType attack_skill,
	                   KilledByTypes killed_by = KilledByTypes::Killed_NPC,
	                   bool is_buff_tic = false) override;
	virtual void Damage(Mob* from, int64 damage, uint16 spell_id,
	                    EQ::skills::SkillType attack_skill, bool avoidable = true,
	                    int8 buffslot = -1, bool iBuffTic = false,
	                    eSpecialAttacks special = eSpecialAttacks::None) override;
	virtual bool Attack(Mob* other, int Hand = EQ::invslot::slotPrimary,
	                    bool FromRiposte = false, bool IsStrikethrough = false,
	                    bool IsFromSpell = false,
	                    ExtraAttackOptions* opts = nullptr) override;
	virtual bool HasRaid() override { return false; }
	virtual bool HasGroup() override { return (GetGroup() ? true : false); }
	virtual Raid* GetRaid() override { return nullptr; }
	virtual Group* GetGroup() override { return entity_list.GetGroupByMob(this); }

	// Type identification
	virtual bool IsCompanion()       const override { return true; }
	virtual bool IsNPC()             const override { return true; }
	virtual bool IsOfClientBot()     const override { return true; }
	virtual bool IsOfClientBotMerc() const override { return true; }

	// -------------------------------------------------------
	// Combat overrides — weapon-damage path (Phase 1)
	// -------------------------------------------------------
	virtual void SetAttackTimer() override;

	// -------------------------------------------------------
	// Stat overrides — survivability path (Phase 3)
	// -------------------------------------------------------
	// CalcMaxHP adds STA-to-HP conversion on top of the NPC base calculation.
	// Only bonus STA from items and spells contributes — base NPC STA is already
	// reflected in npc_types.max_hp and must not be double-counted.
	// The conversion factor is scaled by level, class archetype, and the
	// Companions::STAToHPFactor rule.
	virtual int64 CalcMaxHP() override;

	// CalcMaxMana preserves the level-scaled max_mana set by ScaleStatsToLevel().
	// Without this override, NPC::CalcMaxMana() resets max_mana to npc_mana
	// (the original recruited NPC's value), discarding the level-scaled result.
	// This override reconstructs the level-scaled base from m_base_mana and
	// adds item/spell mana bonuses on top. Non-caster classes return 0.
	// (BUG-017 fix)
	virtual int64 CalcMaxMana() override;

	// -------------------------------------------------------
	// Resist cap overrides — Phase 5
	// -------------------------------------------------------
	// Companions use Client-style resist caps to prevent immunity stacking.
	// Base cap formula: level * 5 + ResistCapBase (default 50).
	// GetMaxResist() returns the cap; Get{X}R() enforce it.
	// Setting ResistCapBase to 0 disables capping (returns 32000).
	int32 GetMaxResist() const;
	inline int32 GetMR() const override {
		return std::min(MR + itembonuses.MR + spellbonuses.MR, GetMaxResist());
	}
	inline int32 GetFR() const override {
		return std::min(FR + itembonuses.FR + spellbonuses.FR, GetMaxResist());
	}
	inline int32 GetDR() const override {
		return std::min(DR + itembonuses.DR + spellbonuses.DR, GetMaxResist());
	}
	inline int32 GetPR() const override {
		return std::min(PR + itembonuses.PR + spellbonuses.PR, GetMaxResist());
	}
	inline int32 GetCR() const override {
		return std::min(CR + itembonuses.CR + spellbonuses.CR, GetMaxResist());
	}

	// -------------------------------------------------------
	// Focus effect override — Phase 5
	// -------------------------------------------------------
	// Companions use the Mob base class GetFocusEffect() instead of the
	// NPC override. The NPC override (a) gates item focus behind
	// NPC_UseFocusFromItems rule (default false), and (b) reads items
	// from the NPC equipment[] array instead of the inventory profile.
	// Both are wrong for companions. The Mob base implementation uses
	// GetInv().GetItem() (correct for companions) and has no rule gate.
	int64 GetFocusEffect(focusType type, uint16 spell_id,
	                     Mob *caster = nullptr,
	                     bool from_buff_tic = false) override;

	// -------------------------------------------------------
	// Combat overrides — triple attack (Phase 2)
	// -------------------------------------------------------
	// Returns true when this companion's class and level qualify
	// for triple attack. Does NOT roll — this is the eligibility check.
	// Warriors: level 56+. Monks, Rangers: level 60+.
	bool CanCompanionTripleAttack() const;

	// Rolls for a triple attack success. Returns true if this attack
	// round should include a third main-hand swing. Uses a chance value
	// derived from the companion's level and class, matching the EQ
	// damage bonus table approach (no SkillTripleAttack in DB for these
	// classes, so we compute the chance directly).
	bool CheckTripleAttack();

	// Performs one round of melee attacks for this companion on the given
	// target and hand (slotPrimary or slotSecondary). Includes double attack
	// and triple attack (when eligible) — mirrors Bot::DoAttackRounds().
	// Called from Companion::Process() instead of Mob::DoMainHandAttackRounds()
	// when the companion is engaged and the attack timer fires.
	void DoAttackRounds(Mob* target, int hand);

	// -------------------------------------------------------
	// AI Virtual Overrides
	// -------------------------------------------------------
	virtual void AI_Start(uint32 iMoveDelay = 0) override;
	virtual void AI_Stop() override;
	virtual void AI_Init() override;
	virtual bool Process() override;
	virtual bool AI_PursueCastCheck() override;
	virtual bool AI_EngagedCastCheck() override;
	virtual bool AI_IdleCastCheck() override;
	virtual bool AICastSpell(int8 iChance, uint32 iSpellTypes);
	virtual bool AIDoSpellCast(uint16 spellid, Mob* tar, int32 mana_cost,
	                           uint32* oDontDoAgainBefore = nullptr);

	// -------------------------------------------------------
	// Combat Positioning
	// -------------------------------------------------------
	// Returns the combat role for this companion based on its class.
	CompanionCombatRole GetCombatRole() const;
	// Classifies this companion by class and caches to m_combat_role.
	static CompanionCombatRole DetermineRoleFromClass(uint8 class_id);
	// Called each tick from Process() when engaged. Sets m_hold_combat_position
	// and calls RunTo/StopNavigation as appropriate for the class role.
	void UpdateCombatPositioning();

	// -------------------------------------------------------
	// Class-specific AI handlers (implemented in companion_ai.cpp)
	// -------------------------------------------------------
	bool AI_Tank(uint32 iSpellTypes, bool is_defensive);
	bool AI_Paladin(uint32 iSpellTypes, bool is_defensive);
	bool AI_ShadowKnight(uint32 iSpellTypes, bool is_defensive);
	bool AI_Cleric(uint32 iSpellTypes, bool is_defensive);
	bool AI_Druid(uint32 iSpellTypes, bool is_defensive);
	bool AI_Shaman(uint32 iSpellTypes, bool is_defensive);
	bool AI_Rogue(uint32 iSpellTypes, bool is_defensive);
	bool AI_Monk(uint32 iSpellTypes, bool is_defensive);
	bool AI_Ranger(uint32 iSpellTypes, bool is_defensive);
	bool AI_Beastlord(uint32 iSpellTypes, bool is_defensive);
	bool AI_Wizard(uint32 iSpellTypes, bool is_defensive);
	bool AI_Magician(uint32 iSpellTypes, bool is_defensive);
	bool AI_Necromancer(uint32 iSpellTypes, bool is_defensive);
	bool AI_Enchanter(uint32 iSpellTypes, bool is_defensive);
	bool AI_Bard(uint32 iSpellTypes, bool is_defensive);
	bool AI_Generic(uint32 iSpellTypes, bool is_defensive);

	// Shared spell selection helpers (companion_ai.cpp)
	bool AI_HealGroupMember(bool engaged);
	bool AI_BuffGroupMember();
	bool AI_WizardBuff();           // Issue #8: wizard-specific buff — DS only to melee targets, OOC only non-DS
	bool AI_CureGroupMember();

	// BUG-019: DS spell identification helper — exposed as public static for testing.
	// Returns true if spell_id has a SE_DamageShield (effect 59) in any effect slot.
	static bool IsDamageShieldSpell(uint16 spell_id);
	bool AI_NukeTarget(uint32 nuke_types);
	bool AI_SlowDebuff(Mob* target);
	bool AI_MezTarget();
	bool AI_SummonPet();
	bool AI_InCombatBuff();

	// Issue #6: Find the best Cannibalize spell (SE_CurrentMana self-spell)
	// Returns 0 if no suitable spell is found in m_companion_spells.
	uint16 FindCannibalizeSpell();

	virtual void FillSpawnStruct(NewSpawn_Struct* ns, Mob* ForWho) override;
	uint32 GetEquipmentMaterial(uint8 material_slot) const override;
	uint32 GetEquippedItemFromTextureSlot(uint8 material_slot) const override;

	// -------------------------------------------------------
	// Lifecycle
	// -------------------------------------------------------
	bool Spawn(Client* owner);
	bool Suspend();
	bool Unsuspend(bool set_max_stats = true);
	void Zone();
	void Dismiss(bool permanent = false);
	virtual void Depop(bool start_spawn_timer = false) override;
	void ProcessClientZoneChange(Client* companion_owner);

	static bool AddCompanionToGroup(Companion* companion, Group* group);
	static bool RemoveCompanionFromGroup(Companion* companion, Group* group);
	bool CompanionJoinClientGroup();
	static void CompanionGroupSay(Mob* speaker, const char* msg, ...);

	// Formation slot assignment: distributes companions in a 120-degree arc behind the owner.
	// AssignFormationSlot() recalculates all active companions for this companion's owner.
	// ReassignFormationSlots() is the static entry point used when the group composition changes.
	void AssignFormationSlot();
	static void ReassignFormationSlots(uint32 owner_char_id);

	// -------------------------------------------------------
	// Persistence
	// -------------------------------------------------------
	virtual bool Save() override;
	bool Load(uint32 companion_id);

	// Buff save/restore
	bool SaveBuffs();
	bool LoadBuffs();

	// -------------------------------------------------------
	// Spell AI
	// -------------------------------------------------------
	bool LoadCompanionSpells();
	std::vector<CompanionSpell> GetCompanionSpells() const { return m_companion_spells; }
	int8 GetChanceToCastBySpellType(uint32 spell_type);
	void SetSpellTimeCanCast(uint16 spellid, uint32 recast_delay);
	bool CheckSpellRecastTimers(uint16 spellid);

	// -------------------------------------------------------
	// HP Regen
	// -------------------------------------------------------
	// Computes the per-tic HP regeneration amount for this companion.
	// Used both in AI_Start (to seed hp_regen) and on each tic in Process().
	// Returns the higher of the NPC's native hp_regen_rate (from npc_types) and
	// the Companions::HPRegenPerTic rule floor so companions with hp_regen_rate=0
	// still heal between combats.
	int64 CalcHPRegen() const;

	// -------------------------------------------------------
	// Mana Regen
	// -------------------------------------------------------
	// Computes the per-tic mana regeneration amount for this companion.
	// Mirrors Bot::CalcManaRegen(): sitting non-melee casters use the meditate
	// formula; standing uses a flat base rate.  Spell/item bonuses are included.
	// Returns 0 for non-mana-using classes (warriors, rogues, etc.).
	// Scaled by Character:ManaRegenMultiplier then Companions:CompanionManaRegenMult.
	int64 CalcManaRegen();

	// -------------------------------------------------------
	// Stat Scaling
	// -------------------------------------------------------
	// Scales stats from recruited_level to current_level using float division:
	// scaled = (int)(base_stat * (float)current_level / (float)recruited_level)
	void ScaleStatsToLevel(uint8 current_level);
	void ApplyStatScalePct();

	// -------------------------------------------------------
	// Equipment (Task 21)
	// -------------------------------------------------------
	bool GiveItem(uint32 item_id, int16 slot);
	bool RemoveItemFromSlot(int16 slot);
	bool LoadEquipment();
	bool SaveEquipment();
	void SendWearChange(uint8 material_slot);
	uint32 GetEquipment(uint8 slot) const;
	void SetEquipment(uint8 slot, uint32 item_id);

	// Expose inventory profile as public so tests and external code can read equipped item data.
	// Mob::GetInv() is protected; this public override makes it accessible without modifying mob.h.
	virtual EQ::InventoryProfile& GetInv() override { return Mob::GetInv(); }

	// Expose item bonuses as public for tests and external code.
	// Mob::itembonuses is protected; this accessor avoids modifying mob.h.
	const StatBonuses& GetItemBonuses() const { return itembonuses; }

	// Equipment listing / retrieval (called from Lua companion.lua)
	void ShowEquipment(Client* client);
	bool GiveSlot(Client* client, const std::string& slot_name);
	void GiveAll(Client* client);

	// -------------------------------------------------------
	// XP / Leveling (Task 19)
	// -------------------------------------------------------
	void AddExperience(uint32 xp);
	bool CheckForLevelUp();
	uint32 GetCompanionXP() const { return m_companion_xp; }
	uint32 GetXPForNextLevel() const;

	// -------------------------------------------------------
	// History (Task 22)
	// -------------------------------------------------------
	void RecordKill(uint32 npc_type_id);    // call from AI kill credit path
	void RecordZoneVisit(uint32 zone_id);  // call on zone-in; updates JSON array
	void UpdateTimeActive();               // accumulates elapsed seconds into m_time_active

	// -------------------------------------------------------
	// Re-recruitment (Task 23)
	// -------------------------------------------------------
	bool IsEverRecruited() const { return m_companion_id > 0; }
	uint32 GetCompanionID() const { return m_companion_id; }
	void SetCompanionID(uint32 id) { m_companion_id = id; }

	// -------------------------------------------------------
	// Soul wipe / permanent death (Task 24)
	// -------------------------------------------------------
	bool SoulWipe();
	static bool SoulWipeByCompanionID(uint32 companion_id, uint32 owner_char_id);

	// -------------------------------------------------------
	// Getters / Setters
	// -------------------------------------------------------
	Client*  GetCompanionOwner() const;
	uint32   GetOwnerCharacterID() const { return m_owner_char_id; }
	void     SetOwnerCharacterID(uint32 id) { m_owner_char_id = id; }

	uint8    GetStance() const { return m_current_stance; }
	void     SetStance(uint8 stance) { m_current_stance = stance; }

	uint8    GetCompanionType() const { return m_companion_type; }
	void     SetCompanionType(uint8 type) { m_companion_type = type; }

	bool     IsSuspended() const { return m_suspended; }
	void     SetSuspended(bool suspended) { m_suspended = suspended; }

	bool     IsDismissed() const { return m_is_dismissed; }
	void     SetDismissed(bool dismissed) { m_is_dismissed = dismissed; }

	bool     GetDepop() const { return m_depop; }

	uint32   GetRecruitedNPCTypeID() const { return m_recruited_npc_type_id; }
	void     SetRecruitedNPCTypeID(uint32 id) { m_recruited_npc_type_id = id; }

	uint32   GetSpawn2ID() const { return m_spawn2_id; }
	void     SetSpawn2ID(uint32 id) { m_spawn2_id = id; }

	uint32   GetSpawnGroupID() const { return m_spawngroupid; }
	void     SetSpawnGroupID(uint32 id) { m_spawngroupid = id; }

	uint8    GetRecruitedLevel() const { return m_recruited_level; }
	void     SetRecruitedLevel(uint8 level) { m_recruited_level = level; }

	// Returns cumulative seconds active, including elapsed time since last unsuspend.
	// When active (m_active_since > 0), adds live elapsed seconds to m_time_active.
	// When suspended (m_active_since == 0), returns m_time_active as-is.
	uint32   GetTimeActive() const;

	// Returns the zone ID from the first entry of m_zones_visited JSON array.
	// This is the zone where the companion was first recruited.
	// Returns 0 if m_zones_visited is empty or unparseable.
	uint32   GetRecruitedZoneID() const;

	// Mercenary retention check (for COMPANION_TYPE_MERCENARY)
	void     CheckMercenaryRetention();

	// Stats at recruitment (for scaling)
	void     StoreBaseStats();

	// Self-preservation behavior
	bool     ShouldUseDefensiveBehavior() const;

	// Depop flag setter
	void     SetDepop(bool depop) { m_depop = depop; }

	void     Signal(int signal_id);

	// Meditate / rest
	void     Sit();
	void     Stand();
	bool     IsSitting() const override;
	bool     IsStanding() const;

protected:
	// Spell AI storage
	std::vector<CompanionSpell> m_companion_spells;
	Timer m_evade_timer;
	Timer m_retention_check_timer;
	Timer m_death_despawn_timer;
	Timer m_ping_timer;              // keep-alive: prevents client culling idle entities
	Timer m_mana_report_timer;       // mana % report via group say while sitting (15s interval)
	Timer m_sitting_regen_timer;     // 6-second cadence for sitting HP bonus (Issue #1 fix)

	// Equipment: array of item IDs indexed by EQ::invslot::EQUIPMENT slots
	uint32 m_equipment[EQ::invslot::EQUIPMENT_COUNT];

private:
	// -------------------------------------------------------
	// Private members
	// -------------------------------------------------------

	// Identity / ownership
	uint32   m_companion_id;          // primary key in companion_data table (0 = not yet saved)
	uint32   m_owner_char_id;         // character_data.id of owning player
	uint32   m_recruited_npc_type_id; // npc_types.id of the original recruited NPC

	// Combat role (assigned at spawn, used by UpdateCombatPositioning)
	CompanionCombatRole m_combat_role;

	// Companion behavior
	uint8    m_companion_type;        // COMPANION_TYPE_COMPANION or COMPANION_TYPE_MERCENARY
	uint8    m_current_stance;        // COMPANION_STANCE_PASSIVE/BALANCED/AGGRESSIVE
	bool     m_suspended;             // true when saved to DB but not spawned
	bool     m_is_dismissed;         // true when voluntarily dismissed; enables re-recruitment lookup
	bool     m_depop;                 // true when this entity should be cleaned up
	bool     m_is_zoning;             // re-entrancy guard: true during Zone() to prevent double-depop (BUG-008)
	bool     m_lom_announced;         // BUG-024: true once LOM is announced; reset when mana recovers above threshold

	// Spawn origin (for replacement NPC, dismissal return)
	uint32   m_spawn2_id;
	uint32   m_spawngroupid;

	// Stats at time of recruitment (used for scaling after level-up)
	uint8    m_recruited_level;       // NPC level when recruited
	int32    m_base_str;
	int32    m_base_sta;
	int32    m_base_dex;
	int32    m_base_agi;
	int32    m_base_int;
	int32    m_base_wis;
	int32    m_base_cha;
	int32    m_base_ac;
	int32    m_base_atk;
	int32    m_base_mr;
	int32    m_base_fr;
	int32    m_base_dr;
	int32    m_base_pr;
	int32    m_base_cr;
	int64    m_base_hp;
	int64    m_base_mana;

	// XP tracking (Task 19)
	uint32   m_companion_xp;          // accumulated experience

	// History tracking (Task 22)
	uint64      m_total_kills;        // total kills attributed to this companion
	uint32      m_times_died;         // total deaths (not including soul wipes)
	uint32      m_time_active;        // cumulative seconds active (unsuspended)
	std::string m_zones_visited;      // JSON array of zone IDs visited
	uint32      m_active_since;       // epoch seconds when last unsuspended (0 = suspended)

	// Replacement NPC spawn delay timer
	Timer    m_replacement_spawn_timer;
};
