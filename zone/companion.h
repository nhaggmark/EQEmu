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
	virtual bool IsCompanion() const override { return true; }
	virtual bool IsNPC()       const override { return true; }

	// -------------------------------------------------------
	// AI Virtual Overrides
	// -------------------------------------------------------
	virtual void AI_Start(uint32 iMoveDelay = 0) override;
	virtual void AI_Stop() override;
	virtual void AI_Init() override;
	virtual bool Process() override;
	virtual bool AI_EngagedCastCheck() override;
	virtual bool AI_IdleCastCheck() override;
	virtual bool AICastSpell(int8 iChance, uint32 iSpellTypes);
	virtual bool AIDoSpellCast(uint16 spellid, Mob* tar, int32 mana_cost,
	                           uint32* oDontDoAgainBefore = nullptr);

	virtual void FillSpawnStruct(NewSpawn_Struct* ns, Mob* ForWho) override;

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
	void RecordKill(uint32 npc_type_id);
	void RecordZoneVisit(uint32 zone_id);
	void UpdateTimeActive(uint32 seconds);

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

	bool     GetDepop() const { return m_depop; }

	uint32   GetRecruitedNPCTypeID() const { return m_recruited_npc_type_id; }
	void     SetRecruitedNPCTypeID(uint32 id) { m_recruited_npc_type_id = id; }

	uint32   GetSpawn2ID() const { return m_spawn2_id; }
	void     SetSpawn2ID(uint32 id) { m_spawn2_id = id; }

	uint32   GetSpawnGroupID() const { return m_spawngroupid; }
	void     SetSpawnGroupID(uint32 id) { m_spawngroupid = id; }

	uint8    GetRecruitedLevel() const { return m_recruited_level; }
	void     SetRecruitedLevel(uint8 level) { m_recruited_level = level; }

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

	// Companion behavior
	uint8    m_companion_type;        // COMPANION_TYPE_COMPANION or COMPANION_TYPE_MERCENARY
	uint8    m_current_stance;        // COMPANION_STANCE_PASSIVE/BALANCED/AGGRESSIVE
	bool     m_suspended;             // true when saved to DB but not spawned
	bool     m_depop;                 // true when this entity should be cleaned up

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
	uint64   m_total_kills;           // history: number of kills

	// Replacement NPC spawn delay timer
	Timer    m_replacement_spawn_timer;
};
