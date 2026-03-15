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

// ============================================================
// companion_ai.cpp — Spell AI for the Companion class
//
// Implements LoadCompanionSpells() and AICastSpell() for all
// 15 Classic-Luclin player classes.  The design is intentionally
// simpler than the Bot system: companions use the SpellTypes
// bitmask system (same as Merc) rather than Bot's 58+ typed
// spell categories.  Spell lists come from companion_spell_sets
// which is populated by the data-expert with spells sourced
// from bot_spells_entries and spells_new.
//
// Architecture:
//   - LoadCompanionSpells(): queries companion_spell_sets for
//     this companion's class_id and current level, populates
//     m_companion_spells.
//   - AICastSpell(): called from AI_EngagedCastCheck() and
//     AI_IdleCastCheck() in companion.cpp.  Routes to
//     class-specific handler methods.
//   - Each archetype method iterates m_companion_spells,
//     filters by spell type and stance, selects the best
//     candidate, and calls AIDoSpellCast().
// ============================================================

#include "companion.h"

#include "common/rulesys.h"
#include "common/spdat.h"
#include "zone/client.h"
#include "zone/corpse.h"
#include "zone/entity.h"
#include "zone/groups.h"
#include "zone/merc.h"
#include "zone/zone.h"

#include <algorithm>
#include <vector>

// Forward declarations of class constants used below
// (these mirror EQEmu's Class namespace values)
static constexpr int8 CLASS_WAR = 1;
static constexpr int8 CLASS_CLR = 2;
static constexpr int8 CLASS_PAL = 3;
static constexpr int8 CLASS_RNG = 4;
static constexpr int8 CLASS_SHD = 5;
static constexpr int8 CLASS_DRU = 6;
static constexpr int8 CLASS_MNK = 7;
static constexpr int8 CLASS_BRD = 8;
static constexpr int8 CLASS_ROG = 9;
static constexpr int8 CLASS_SHM = 10;
static constexpr int8 CLASS_NEC = 11;
static constexpr int8 CLASS_WIZ = 12;
static constexpr int8 CLASS_MAG = 13;
static constexpr int8 CLASS_ENC = 14;
static constexpr int8 CLASS_BST = 15;

// ============================================================
// Internal helper: check if the companion's current stance
// matches the spell's stance requirement.
//   stance == 0  → all stances allowed
//   stance  > 0  → only when companion is in that exact stance
//   stance  < 0  → all stances EXCEPT |stance|
// ============================================================
static bool StanceMatch(int16 spell_stance, uint8 companion_stance)
{
	if (spell_stance == 0) {
		return true;
	}
	if (spell_stance > 0) {
		return static_cast<int16>(companion_stance) == spell_stance;
	}
	// negative: all except that stance
	return static_cast<int16>(companion_stance) != -spell_stance;
}

// ============================================================
// Internal helper: get all companion spells matching a spell
// type bitmask and the current stance.  Returns them sorted
// by priority (higher priority = checked first = better spell).
// ============================================================
static std::vector<CompanionSpell> GetSpellsForType(
	const std::vector<CompanionSpell>& spell_list,
	uint32 spell_type_mask,
	uint8 companion_stance)
{
	std::vector<CompanionSpell> result;

	for (const auto& cs : spell_list) {
		if (!IsValidSpell(cs.spellid)) {
			continue;
		}
		if (!(cs.type & spell_type_mask)) {
			continue;
		}
		if (!StanceMatch(cs.stance, companion_stance)) {
			continue;
		}
		result.push_back(cs);
	}

	// Sort descending by slot (slot acts as priority: lower slot = higher priority in DB)
	std::sort(result.begin(), result.end(), [](const CompanionSpell& a, const CompanionSpell& b) {
		return a.slot < b.slot;
	});

	return result;
}

// ============================================================
// Internal helper: select best healing spell for a target at
// a given HP percentage.  Considers min/max_hp_pct constraints.
// ============================================================
static uint16 SelectHealSpell(
	const std::vector<CompanionSpell>& spell_list,
	uint8 companion_stance,
	int target_hp_pct,   // widened from int8 (Issue #4 fix — no truncation)
	uint32 now_ms)
{
	auto candidates = GetSpellsForType(spell_list, SpellType_Heal, companion_stance);

	for (const auto& cs : candidates) {
		if (cs.time_cancast > now_ms) {
			continue; // on recast cooldown
		}
		// Check HP thresholds
		if (cs.min_hp_pct > 0 && target_hp_pct > cs.min_hp_pct) {
			continue;
		}
		if (cs.max_hp_pct > 0 && cs.max_hp_pct < 100 && target_hp_pct < cs.max_hp_pct) {
			continue;
		}
		return cs.spellid;
	}

	return 0;
}

// ============================================================
// Internal helper: select the most mana-efficient healing spell
// for a target at a given HP percentage.  Used when the caster
// is low on mana (< HealerManaConservePct) to preserve resources.
// Picks the valid heal spell with the lowest mana cost.
// ============================================================
static uint16 SelectEfficientHealSpell(
	const std::vector<CompanionSpell>& spell_list,
	uint8 companion_stance,
	int target_hp_pct,   // widened from int8 (Issue #4 fix — no truncation)
	uint32 now_ms)
{
	auto candidates = GetSpellsForType(spell_list, SpellType_Heal, companion_stance);

	uint16 best_spell = 0;
	int    best_mana  = INT_MAX;

	for (const auto& cs : candidates) {
		if (cs.time_cancast > now_ms) {
			continue; // on recast cooldown
		}
		// Check HP thresholds
		if (cs.min_hp_pct > 0 && target_hp_pct > cs.min_hp_pct) {
			continue;
		}
		if (cs.max_hp_pct > 0 && cs.max_hp_pct < 100 && target_hp_pct < cs.max_hp_pct) {
			continue;
		}
		// Pick the one with the lowest mana cost
		if (!IsValidSpell(cs.spellid)) {
			continue;
		}
		int spell_mana = spells[cs.spellid].mana;
		if (spell_mana < best_mana) {
			best_mana  = spell_mana;
			best_spell = cs.spellid;
		}
	}

	return best_spell;
}

// ============================================================
// Internal helper: select best HoT (heal-over-time) spell for a
// target.  Identifies HoT spells by their buff_duration > 0 on the
// SPDat_Spell_Struct (they leave a ticking buff).  Used by the
// druid to prefer HoT when target is above 50% HP (Issue #5 fix).
// Returns 0 if no HoT heal spell is available/ready.
// ============================================================
static uint16 SelectHoTSpell(
	const std::vector<CompanionSpell>& spell_list,
	uint8 companion_stance,
	int target_hp_pct,
	uint32 now_ms)
{
	auto candidates = GetSpellsForType(spell_list, SpellType_Heal, companion_stance);

	for (const auto& cs : candidates) {
		if (cs.time_cancast > now_ms) {
			continue;
		}
		// Check HP thresholds
		if (cs.min_hp_pct > 0 && target_hp_pct > cs.min_hp_pct) {
			continue;
		}
		if (cs.max_hp_pct > 0 && cs.max_hp_pct < 100 && target_hp_pct < cs.max_hp_pct) {
			continue;
		}
		// Identify as HoT: spell has buff_duration > 0 (ticking buff, not instant heal)
		if (!IsValidSpell(cs.spellid)) {
			continue;
		}
		if (spells[cs.spellid].buff_duration > 0) {
			return cs.spellid;
		}
	}

	return 0;
}

// ============================================================
// BUG-019: Returns true if spell_id has a SE_DamageShield
// (effect ID 59) in any of its effect slots.  Used by AI_WizardBuff
// to identify damage shield spells.  DS spells must only target
// melee companions AND must only be cast during combat (BUG-019 fix).
// Exposed as a public static member of Companion for testability.
// See Issue #8 and BUG-019.
// ============================================================
/*static*/ bool Companion::IsDamageShieldSpell(uint16 spell_id)
{
	if (!IsValidSpell(spell_id)) {
		return false;
	}
	const SPDat_Spell_Struct& sp = spells[spell_id];
	for (int i = 0; i < EFFECT_COUNT; ++i) {
		if (sp.effect_id[i] == SpellEffect::DamageShield) {
			return true;
		}
	}
	return false;
}

// ============================================================
// Internal helper: select first available spell for a type,
// respecting recast timers.
// ============================================================
static uint16 SelectFirstSpell(
	const std::vector<CompanionSpell>& spell_list,
	uint32 spell_type_mask,
	uint8 companion_stance,
	uint32 now_ms)
{
	auto candidates = GetSpellsForType(spell_list, spell_type_mask, companion_stance);

	for (const auto& cs : candidates) {
		if (cs.time_cancast <= now_ms) {
			return cs.spellid;
		}
	}

	return 0;
}

// ============================================================
// LoadCompanionSpells
//
// Queries companion_spell_sets for this companion's class and
// level range.  On success, populates m_companion_spells.
// Returns true if any spells were loaded.
//
// Falls back to an empty list if the table has no entries for
// this class/level — the companion will use melee-only AI.
// ============================================================
bool Companion::LoadCompanionSpells()
{
	m_companion_spells.clear();

	uint8 comp_level = GetLevel();
	uint8 comp_class = GetClass();

	// Query the companion_spell_sets table
	// Columns: id, class_id, min_level, max_level, spell_id, spell_type,
	//          stance, priority, min_hp_pct, max_hp_pct
	auto results = database.QueryDatabase(
		fmt::format(
			"SELECT `spell_id`, `spell_type`, `stance`, `priority`, `min_hp_pct`, `max_hp_pct` "
			"FROM `companion_spell_sets` "
			"WHERE `class_id` = {} AND `min_level` <= {} AND `max_level` >= {} "
			"ORDER BY `priority` ASC, `id` ASC",
			static_cast<uint32>(comp_class),
			static_cast<uint32>(comp_level),
			static_cast<uint32>(comp_level)
		)
	);

	if (!results.Success()) {
		LogError("Companion::LoadCompanionSpells: DB query failed for class [{}] level [{}]",
		         comp_class, comp_level);
		return false;
	}

	uint32 now_ms = Timer::GetCurrentTime();
	int16 slot = 0;

	for (auto row = results.begin(); row != results.end(); ++row) {
		uint16 spellid    = static_cast<uint16>(atoi(row[0]));
		uint32 spell_type = static_cast<uint32>(strtoul(row[1], nullptr, 10));
		int16  stance     = static_cast<int16>(atoi(row[2]));
		// priority is ordering; use slot counter for precedence
		int16  min_hp_pct = static_cast<int16>(atoi(row[4]));
		int16  max_hp_pct = static_cast<int16>(atoi(row[5]));

		if (!IsValidSpell(spellid)) {
			continue;
		}

		CompanionSpell cs;
		cs.spellid       = spellid;
		cs.type          = spell_type;
		cs.stance        = stance;
		cs.slot          = slot++;
		cs.proc_chance   = 100;
		cs.time_cancast  = now_ms; // available immediately
		cs.min_hp_pct    = min_hp_pct;
		cs.max_hp_pct    = max_hp_pct;

		m_companion_spells.push_back(cs);
	}

	LogInfo("Companion [{}] (class {} level {}) loaded [{}] spells from companion_spell_sets",
	        GetName(), comp_class, comp_level, m_companion_spells.size());

	return !m_companion_spells.empty();
}

// ============================================================
// AICastSpell — main entry point for companion spell AI
//
// Called from AI_EngagedCastCheck() and AI_IdleCastCheck()
// in companion.cpp.  Routes to class-specific archetypes.
//
// iChance:     0-100, probability to even attempt casting
// iSpellTypes: bitmask of SpellTypes to consider
// ============================================================
bool Companion::AICastSpell(int8 iChance, uint32 iSpellTypes)
{
	LogAIDetail("Companion [{}] AICastSpell: spells=[{}] chance=[{}] types=[{}] target=[{}] engaged=[{}] mana=[{:.0f}%%] class=[{}]",
	            GetName(), m_companion_spells.size(), iChance, iSpellTypes,
	            GetTarget() ? GetTarget()->GetName() : "none",
	            IsEngaged(), GetManaRatio(), GetClass());

	// Never cast while sitting/meditating (BUG-020).
	// Companions sit with the owner to recover mana; casting while sitting
	// is both incorrect and disrupts the meditation cycle.
	if (IsSitting()) {
		LogAIDetail("Companion [{}] AICastSpell: sitting — skipping all spell casting", GetName());
		return false;
	}

	if (m_companion_spells.empty()) {
		LogAIDetail("Companion [{}] AICastSpell: no companion spells, falling back to NPC::AI_EngagedCastCheck", GetName());
		// No companion spells loaded — fall back to base NPC AI
		// (NPC uses its native npc_spells_id entries)
		return NPC::AI_EngagedCastCheck();
	}

	// Chance roll (like Merc: 0-100 integer check)
	if (iChance < 100) {
		int roll = zone->random.Int(0, 100);
		if (roll > iChance) {
			LogAIDetail("Companion [{}] AICastSpell: chance roll failed (rolled [{}] > chance [{}])", GetName(), roll, iChance);
			return false;
		}
	}

	// Check mana — companions don't cast when OOM (< 10% mana)
	// Pure melee classes (WAR, MNK, ROG) have no mana and are exempt
	uint8 comp_class = GetClass();
	bool has_mana = (GetMaxMana() > 0);
	if (has_mana && GetManaRatio() < 10.0f) {
		LogAIDetail("Companion [{}] AICastSpell: OOM bail (mana=[{:.0f}%%] < 10%%)", GetName(), GetManaRatio());
		return false;
	}

	// Self-preservation: in defensive state, heavily favor heals
	bool is_defensive = ShouldUseDefensiveBehavior();

	// Route to class-specific handler
	switch (comp_class) {
		// Tank archetypes — warrior, paladin, shadow knight
		case CLASS_WAR:
			return AI_Tank(iSpellTypes, is_defensive);

		case CLASS_PAL:
			return AI_Paladin(iSpellTypes, is_defensive);

		case CLASS_SHD:
			return AI_ShadowKnight(iSpellTypes, is_defensive);

		// Healer archetypes — cleric, druid, shaman
		case CLASS_CLR:
			return AI_Cleric(iSpellTypes, is_defensive);

		case CLASS_DRU:
			return AI_Druid(iSpellTypes, is_defensive);

		case CLASS_SHM:
			return AI_Shaman(iSpellTypes, is_defensive);

		// Melee DPS archetypes — rogue, monk, ranger, beastlord
		case CLASS_ROG:
			return AI_Rogue(iSpellTypes, is_defensive);

		case CLASS_MNK:
			return AI_Monk(iSpellTypes, is_defensive);

		case CLASS_RNG:
			return AI_Ranger(iSpellTypes, is_defensive);

		case CLASS_BST:
			return AI_Beastlord(iSpellTypes, is_defensive);

		// Caster DPS archetypes — wizard, magician, necromancer
		case CLASS_WIZ:
			return AI_Wizard(iSpellTypes, is_defensive);

		case CLASS_MAG:
			return AI_Magician(iSpellTypes, is_defensive);

		case CLASS_NEC:
			return AI_Necromancer(iSpellTypes, is_defensive);

		// Utility archetypes — enchanter, bard
		case CLASS_ENC:
			return AI_Enchanter(iSpellTypes, is_defensive);

		case CLASS_BRD:
			return AI_Bard(iSpellTypes, is_defensive);

		default:
			// Unknown class — attempt a generic heal/buff/nuke sequence
			return AI_Generic(iSpellTypes, is_defensive);
	}
}

// ============================================================
// AI_HealGroupMember — shared healer logic
//
// Iterates group members to find the most injured, then selects
// and casts an appropriate heal spell.  Returns true if a spell
// was cast.
// ============================================================
bool Companion::AI_HealGroupMember(bool engaged)
{
	Group* g = GetGroup();
	if (!g) {
		// Solo: only heal self
		// Issue #4 fix: use int (not int8) to avoid overflow when HP ratio > 127
		int self_hp = static_cast<int>(GetHPRatio());
		if (self_hp < 95) {
			uint16 heal_spell = SelectHealSpell(
				m_companion_spells, m_current_stance, self_hp,
				Timer::GetCurrentTime());
			if (heal_spell) {
				return AIDoSpellCast(heal_spell, this, spells[heal_spell].mana);
			}
		}
		return false;
	}

	// Find most injured group member (owner prioritized, then tanks).
	// When engaged, use the rule-configured threshold (default 80%).
	// When idle/OOC, always heal below 99% (keep group topped off).
	//
	// Issue #4 fix: use int (not int8) for HP ratio comparisons.
	// int8 overflows at 128+, causing silent negative values:
	//   static_cast<int8>(200) = -56  → healer never heals anyone (no one is at -56% HP)
	//   static_cast<int8>(130) = -126 → same pathological result
	// Using int prevents this. Valid HP ratio (0-100) and rule values (0-200) fit safely.
	Mob* best_target = nullptr;
	int  lowest_hp   = engaged ? RuleI(Companions, HealThresholdPct) : 99;

	Client* owner = GetCompanionOwner();

	for (int i = 0; i < MAX_GROUP_MEMBERS; i++) {
		if (!g->members[i] || g->members[i]->GetHP() <= 0) {
			continue;
		}

		int hpr = static_cast<int>(g->members[i]->GetHPRatio());

		if (hpr >= lowest_hp) {
			continue;
		}

		// Prioritize owner, then any member lower HP
		if (g->members[i] == owner) {
			best_target = g->members[i];
			lowest_hp   = hpr;
		} else if (!best_target || hpr < lowest_hp) {
			best_target = g->members[i];
			lowest_hp   = hpr;
		}
	}

	// Also consider healing self
	int self_hp = static_cast<int>(GetHPRatio());
	if (self_hp < lowest_hp) {
		best_target = this;
		lowest_hp   = self_hp;
	}

	if (!best_target) {
		return false;
	}

	uint32 now_ms = Timer::GetCurrentTime();

	// When mana is below HealerManaConservePct, use the most efficient heal
	// spell to preserve resources. Above the threshold, use the best/fastest heal.
	uint16 heal_spell = 0;
	bool   mana_low   = (GetMaxMana() > 0 &&
	                     GetManaRatio() < static_cast<float>(RuleI(Companions, HealerManaConservePct)));
	if (mana_low) {
		heal_spell = SelectEfficientHealSpell(
			m_companion_spells, m_current_stance, lowest_hp, now_ms);
	}
	if (heal_spell == 0) {
		// Fall back to standard heal selection (best priority spell)
		heal_spell = SelectHealSpell(
			m_companion_spells, m_current_stance, lowest_hp, now_ms);
	}

	if (heal_spell == 0) {
		return false;
	}

	bool cast_ok = AIDoSpellCast(heal_spell, best_target, spells[heal_spell].mana);
	if (cast_ok) {
		SetSpellTimeCanCast(heal_spell, spells[heal_spell].recast_time);
	}
	return cast_ok;
}

// ============================================================
// AI_BuffGroupMember — shared buff logic
//
// Iterates group members and applies any missing buffs.
// Returns true if a spell was cast.
// ============================================================
bool Companion::AI_BuffGroupMember()
{
	// Don't buff if mana is below 30% (conserve for heals/combat)
	if (GetMaxMana() > 0 && GetManaRatio() < 30.0f) {
		return false;
	}

	uint32 now_ms = Timer::GetCurrentTime();
	auto buff_spells = GetSpellsForType(m_companion_spells, SpellType_Buff | SpellType_PreCombatBuff, m_current_stance);

	if (buff_spells.empty()) {
		return false;
	}

	Group* g = GetGroup();

	// Build target list: self, then group members, then their pets
	std::vector<Mob*> targets;
	targets.push_back(this);

	if (g) {
		for (int i = 0; i < MAX_GROUP_MEMBERS; i++) {
			if (g->members[i] && g->members[i] != this) {
				targets.push_back(g->members[i]);
				if (g->members[i]->HasPet()) {
					targets.push_back(g->members[i]->GetPet());
				}
			}
		}
	}

	uint8 comp_level = GetLevel();

	for (const auto& cs : buff_spells) {
		if (cs.time_cancast > now_ms) {
			continue;
		}

		const auto& sp = spells[cs.spellid];

		for (Mob* tgt : targets) {
			if (!tgt || tgt->GetHP() <= 0) {
				continue;
			}

			// Self-only spells
			if (sp.target_type == ST_Self && tgt != this) {
				continue;
			}

			// Skip if already buffed
			if (tgt->IsImmuneToSpell(cs.spellid, this)) {
				continue;
			}
			if (tgt->CanBuffStack(cs.spellid, comp_level, true) < 0) {
				continue;
			}

			uint32 dont_buff_before = tgt->DontBuffMeBefore();
			bool cast_ok = AIDoSpellCast(cs.spellid, tgt, spells[cs.spellid].mana, &dont_buff_before);
			if (cast_ok) {
				tgt->SetDontBuffMeBefore(dont_buff_before);
				SetSpellTimeCanCast(cs.spellid, spells[cs.spellid].recast_time);
				return true;
			}
		}
	}

	return false;
}

// ============================================================
// AI_WizardBuff — wizard-specific idle buff logic (Issue #8 fix)
//
// The wizard has damage shield (DS) spells that are only useful on
// melee companions (tanks, DPS) that get hit in melee. Casting DS
// on casters/healers who stand at range wastes mana.
//
// This method works like AI_BuffGroupMember() but with DS spell
// filtering: DS spells are only cast on melee-role group members.
// Non-DS buffs (fire resist, other utility) are cast on everyone.
//
// Clients (the player) are always eligible for DS since their
// positioning is player-controlled.
// ============================================================
bool Companion::AI_WizardBuff()
{
	// Don't buff if mana is below 30% (conserve for nukes)
	if (GetMaxMana() > 0 && GetManaRatio() < 30.0f) {
		return false;
	}

	uint32 now_ms   = Timer::GetCurrentTime();
	uint8  comp_level = GetLevel();

	auto buff_spells = GetSpellsForType(m_companion_spells, SpellType_Buff | SpellType_PreCombatBuff, m_current_stance);

	if (buff_spells.empty()) {
		return false;
	}

	Group* g = GetGroup();

	// Build full target list
	std::vector<Mob*> all_targets;
	all_targets.push_back(this);
	if (g) {
		for (int i = 0; i < MAX_GROUP_MEMBERS; i++) {
			if (g->members[i] && g->members[i] != this) {
				all_targets.push_back(g->members[i]);
				if (g->members[i]->HasPet()) {
					all_targets.push_back(g->members[i]->GetPet());
				}
			}
		}
	}

	// Build melee-only target list for DS spells:
	// Companions with melee roles + Clients (whose positioning is player-controlled).
	std::vector<Mob*> melee_targets;
	for (Mob* tgt : all_targets) {
		if (!tgt) {
			continue;
		}
		if (tgt->IsClient()) {
			// Always include the player (they control their own positioning)
			melee_targets.push_back(tgt);
		} else if (tgt->IsCompanion()) {
			Companion* ctgt = static_cast<Companion*>(tgt);
			CompanionCombatRole role = ctgt->GetCombatRole();
			if (role == COMBAT_ROLE_MELEE_TANK ||
			    role == COMBAT_ROLE_MELEE_DPS   ||
			    role == COMBAT_ROLE_ROGUE) {
				melee_targets.push_back(tgt);
			}
		}
		// Pets are melee — include them
		else if (tgt->IsPet()) {
			melee_targets.push_back(tgt);
		}
	}

	bool engaged = IsEngaged();

	for (const auto& cs : buff_spells) {
		if (cs.time_cancast > now_ms) {
			continue;
		}

		// BUG-019: DS spells must ONLY be cast during combat.
		// Casting DS out of combat wastes mana and serves no purpose —
		// the DS only triggers when the target is hit by melee attacks.
		if (IsDamageShieldSpell(cs.spellid) && !engaged) {
			LogAIDetail("Companion [{}] AI_WizardBuff: skipping DS spell [{}] ({}) — not engaged (BUG-019 fix)",
			            GetName(), cs.spellid,
			            IsValidSpell(cs.spellid) ? spells[cs.spellid].name : "INVALID");
			continue;
		}

		const auto& sp = spells[cs.spellid];

		// Choose which target list to use based on spell type:
		// DS spells target only melee roles; non-DS buffs target everyone.
		const std::vector<Mob*>& targets = IsDamageShieldSpell(cs.spellid)
		                                   ? melee_targets
		                                   : all_targets;

		for (Mob* tgt : targets) {
			if (!tgt || tgt->GetHP() <= 0) {
				continue;
			}

			// Self-only spells
			if (sp.target_type == ST_Self && tgt != this) {
				continue;
			}

			// Skip if already buffed
			if (tgt->IsImmuneToSpell(cs.spellid, this)) {
				continue;
			}
			if (tgt->CanBuffStack(cs.spellid, comp_level, true) < 0) {
				continue;
			}

			uint32 dont_buff_before = tgt->DontBuffMeBefore();
			bool cast_ok = AIDoSpellCast(cs.spellid, tgt, spells[cs.spellid].mana, &dont_buff_before);
			if (cast_ok) {
				tgt->SetDontBuffMeBefore(dont_buff_before);
				SetSpellTimeCanCast(cs.spellid, spells[cs.spellid].recast_time);
				return true;
			}
		}
	}

	return false;
}

// ============================================================
// AI_CureGroupMember — cure debuffs from group members
// ============================================================
bool Companion::AI_CureGroupMember()
{
	uint32 now_ms = Timer::GetCurrentTime();
	uint16 cure_spell = SelectFirstSpell(m_companion_spells, SpellType_Cure, m_current_stance, now_ms);

	if (!cure_spell) {
		return false;
	}

	Group* g = GetGroup();
	Mob*   cure_target = nullptr;

	if (g) {
		for (int i = 0; i < MAX_GROUP_MEMBERS; i++) {
			if (g->members[i] && Merc::GetNeedsCured(g->members[i]) &&
			    g->members[i]->DontCureMeBefore() < now_ms) {
				cure_target = g->members[i];
				break;
			}
		}
	} else if (Merc::GetNeedsCured(this) && DontCureMeBefore() < now_ms) {
		cure_target = this;
	}

	if (!cure_target) {
		return false;
	}

	bool cast_ok = AIDoSpellCast(cure_spell, cure_target, spells[cure_spell].mana);
	if (cast_ok) {
		cure_target->SetDontCureMeBefore(now_ms + 4000);
		SetSpellTimeCanCast(cure_spell, spells[cure_spell].recast_time);
	}
	return cast_ok;
}

// ============================================================
// AI_NukeTarget — cast a damage spell on the current target
// ============================================================
bool Companion::AI_NukeTarget(uint32 nuke_types)
{
	Mob* target = GetTarget();
	if (!target || target->GetHP() <= 0) {
		LogAIDetail("Companion [{}] AI_NukeTarget: no valid target (target=[{}] HP=[{}])",
		            GetName(), target ? target->GetName() : "none",
		            target ? (int64)target->GetHP() : 0);
		return false;
	}

	uint32 now_ms = Timer::GetCurrentTime();
	uint16 nuke_spell = SelectFirstSpell(m_companion_spells, nuke_types, m_current_stance, now_ms);

	if (!nuke_spell) {
		LogAIDetail("Companion [{}] AI_NukeTarget: no spell available for types=[{}] stance=[{}]",
		            GetName(), nuke_types, m_current_stance);
		return false;
	}

	// Range check
	float dist2 = DistanceSquared(m_Position, target->GetPosition());
	float range  = GetActSpellRange(nuke_spell, spells[nuke_spell].range);
	LogAIDetail("Companion [{}] AI_NukeTarget: spell=[{}] ({}) dist2=[{:.1f}] range=[{:.1f}] range2=[{:.1f}]",
	            GetName(), nuke_spell, spells[nuke_spell].name,
	            dist2, range, range * range);
	if (dist2 > range * range) {
		LogAIDetail("Companion [{}] AI_NukeTarget: out of range for spell [{}]", GetName(), nuke_spell);
		return false;
	}

	bool cast_ok = AIDoSpellCast(nuke_spell, target, spells[nuke_spell].mana);
	if (cast_ok) {
		SetSpellTimeCanCast(nuke_spell, spells[nuke_spell].recast_time);
	}
	return cast_ok;
}

// ============================================================
// AI_SlowDebuff — apply a slow or debuff to target
// ============================================================
bool Companion::AI_SlowDebuff(Mob* target)
{
	if (!target || target->GetHP() <= 0) {
		return false;
	}

	uint32 now_ms = Timer::GetCurrentTime();
	uint16 slow_spell = SelectFirstSpell(
		m_companion_spells, SpellType_Slow | SpellType_Debuff,
		m_current_stance, now_ms);

	if (!slow_spell) {
		return false;
	}

	if (target->CanBuffStack(slow_spell, GetLevel(), true) < 0) {
		return false;
	}

	bool cast_ok = AIDoSpellCast(slow_spell, target, spells[slow_spell].mana);
	if (cast_ok) {
		SetSpellTimeCanCast(slow_spell, spells[slow_spell].recast_time);
	}
	return cast_ok;
}

// ============================================================
// AI_MezTarget — mez a secondary target to reduce mob count
// ============================================================
bool Companion::AI_MezTarget()
{
	uint32 now_ms = Timer::GetCurrentTime();
	uint16 mez_spell = SelectFirstSpell(m_companion_spells, SpellType_Mez, m_current_stance, now_ms);

	if (!mez_spell) {
		return false;
	}

	Mob* mez_target = entity_list.GetTargetForMez(this);
	if (!mez_target) {
		return false;
	}

	if (mez_target->CanBuffStack(mez_spell, GetLevel(), true) < 0) {
		return false;
	}

	bool cast_ok = AIDoSpellCast(mez_spell, mez_target, spells[mez_spell].mana);
	if (cast_ok) {
		SetSpellTimeCanCast(mez_spell, spells[mez_spell].recast_time);
	}
	return cast_ok;
}

// ============================================================
// AI_SummonPet — summon a pet if we don't have one
// ============================================================
bool Companion::AI_SummonPet()
{
	if (HasPet()) {
		return false;
	}

	uint32 now_ms = Timer::GetCurrentTime();
	uint16 pet_spell = SelectFirstSpell(m_companion_spells, SpellType_Pet, m_current_stance, now_ms);

	if (!pet_spell) {
		return false;
	}

	bool cast_ok = AIDoSpellCast(pet_spell, this, spells[pet_spell].mana);
	if (cast_ok) {
		SetSpellTimeCanCast(pet_spell, spells[pet_spell].recast_time);
	}
	return cast_ok;
}

// ============================================================
// AI_InCombatBuff — cast a self-only combat buff (discipline
// equivalent or proc buff like WAR disciplines)
// ============================================================
bool Companion::AI_InCombatBuff()
{
	uint32 now_ms = Timer::GetCurrentTime();
	uint16 icb_spell = SelectFirstSpell(
		m_companion_spells, SpellType_InCombatBuff, m_current_stance, now_ms);

	if (!icb_spell) {
		return false;
	}

	if (CanBuffStack(icb_spell, GetLevel(), true) < 0) {
		return false;
	}

	bool cast_ok = AIDoSpellCast(icb_spell, this, spells[icb_spell].mana);
	if (cast_ok) {
		SetSpellTimeCanCast(icb_spell, spells[icb_spell].recast_time);
	}
	return cast_ok;
}

// ============================================================
// CLASS-SPECIFIC AI HANDLERS
// ============================================================

// -------------------------------------------------------
// Warrior: Pure melee — no mana spells.
// Only uses combat disciplines (InCombatBuff) if available.
// Passive stance: does not engage.
// -------------------------------------------------------
bool Companion::AI_Tank(uint32 iSpellTypes, bool is_defensive)
{
	if (m_current_stance == COMPANION_STANCE_PASSIVE) {
		return false;
	}

	bool engaged = IsEngaged();

	if (engaged) {
		// Attempt in-combat buff (disciplines) only
		if (iSpellTypes & SpellType_InCombatBuff) {
			return AI_InCombatBuff();
		}
	} else {
		// Idle: no warrior-specific spells; buffs are handled by the group healer
		if (iSpellTypes & SpellType_Buff) {
			return AI_BuffGroupMember();
		}
	}

	return false;
}

// -------------------------------------------------------
// Paladin: Tank/healer hybrid.
// Engaged: heal low-HP members, use stuns, lay on hands equivalent.
// Idle: buffs (armor of faith, guard, etc.), cure.
// -------------------------------------------------------
bool Companion::AI_Paladin(uint32 iSpellTypes, bool is_defensive)
{
	if (m_current_stance == COMPANION_STANCE_PASSIVE) {
		return false;
	}

	bool engaged = IsEngaged();

	if (engaged) {
		// Defensive or low HP: prioritize heals
		if ((iSpellTypes & SpellType_Heal) && (is_defensive || zone->random.Roll(40))) {
			if (AI_HealGroupMember(true)) {
				return true;
			}
		}
		// Cure conditions
		if ((iSpellTypes & SpellType_Cure) && zone->random.Roll(25)) {
			if (AI_CureGroupMember()) {
				return true;
			}
		}
		// In-combat buffs (holy might, etc.)
		if (iSpellTypes & SpellType_InCombatBuff) {
			if (AI_InCombatBuff()) {
				return true;
			}
		}
		// Nukes (stuns, undead nukes for PAL)
		if ((iSpellTypes & SpellType_Nuke) && m_current_stance == COMPANION_STANCE_AGGRESSIVE) {
			return AI_NukeTarget(SpellType_Nuke);
		}
	} else {
		// Idle: cure > heal > resurrect dead > buff
		if (iSpellTypes & SpellType_Cure) {
			if (AI_CureGroupMember()) {
				return true;
			}
		}
		if (iSpellTypes & SpellType_Heal) {
			if (AI_HealGroupMember(false)) {
				return true;
			}
		}
		if (iSpellTypes & SpellType_Resurrect) {
			if (AI_ResurrectDeadGroupMember()) {
				return true;
			}
		}
		if (iSpellTypes & SpellType_Buff) {
			return AI_BuffGroupMember();
		}
	}

	return false;
}

// -------------------------------------------------------
// Shadow Knight: Tank/nuker hybrid.
// Engaged: lifetap to sustain, DoT, harm touch equivalent.
// Idle: buffs, summon pet.
// -------------------------------------------------------
bool Companion::AI_ShadowKnight(uint32 iSpellTypes, bool is_defensive)
{
	if (m_current_stance == COMPANION_STANCE_PASSIVE) {
		return false;
	}

	bool engaged = IsEngaged();

	if (engaged) {
		// Low HP: use lifetap
		if ((iSpellTypes & SpellType_Lifetap) && is_defensive) {
			if (AI_NukeTarget(SpellType_Lifetap)) {
				return true;
			}
		}
		// In-combat buff (hate proc, strength buffs)
		if (iSpellTypes & SpellType_InCombatBuff) {
			if (AI_InCombatBuff()) {
				return true;
			}
		}
		// DoT if aggressive
		if ((iSpellTypes & SpellType_DOT) && m_current_stance == COMPANION_STANCE_AGGRESSIVE) {
			if (AI_NukeTarget(SpellType_DOT)) {
				return true;
			}
		}
		// Nuke (harm touch equivalent, spear of pain)
		if ((iSpellTypes & SpellType_Nuke) && zone->random.Roll(50)) {
			return AI_NukeTarget(SpellType_Nuke);
		}
	} else {
		// Idle: pet first, then buffs
		if (iSpellTypes & SpellType_Pet) {
			if (AI_SummonPet()) {
				return true;
			}
		}
		if (iSpellTypes & SpellType_Buff) {
			return AI_BuffGroupMember();
		}
	}

	return false;
}

// -------------------------------------------------------
// Cleric: Pure healer.
// Engaged: heal, cure, resurrect dead members.
// Idle: buff, cure, heal.
// -------------------------------------------------------
bool Companion::AI_Cleric(uint32 iSpellTypes, bool is_defensive)
{
	if (m_current_stance == COMPANION_STANCE_PASSIVE) {
		// Passive cleric: only heal critically injured owner
		Client* owner = GetCompanionOwner();
		if (owner && owner->GetHPRatio() < 25) {
			return AI_HealGroupMember(true);
		}
		return false;
	}

	bool engaged = IsEngaged();

	if (engaged) {
		// Highest priority: cure conditions
		if (iSpellTypes & SpellType_Cure) {
			if (AI_CureGroupMember()) {
				return true;
			}
		}
		// Heal group members
		if (iSpellTypes & SpellType_Heal) {
			if (AI_HealGroupMember(true)) {
				return true;
			}
		}
		// Standard buffs (SpellType_Buff, SpellType_PreCombatBuff) are NOT cast
		// during combat. Only SpellType_InCombatBuff is allowed in combat.
		// (Pre-combat buffs are applied when idle; re-buffing waits until combat ends.)
		if (iSpellTypes & SpellType_InCombatBuff) {
			if (AI_InCombatBuff()) {
				return true;
			}
		}
	} else {
		// Idle: cure > heal > resurrect dead > buff
		if (iSpellTypes & SpellType_Cure) {
			if (AI_CureGroupMember()) {
				return true;
			}
		}
		if (iSpellTypes & SpellType_Heal) {
			if (AI_HealGroupMember(false)) {
				return true;
			}
		}
		if (iSpellTypes & SpellType_Resurrect) {
			if (AI_ResurrectDeadGroupMember()) {
				return true;
			}
		}
		if (iSpellTypes & SpellType_Buff) {
			return AI_BuffGroupMember();
		}
	}

	return false;
}

// -------------------------------------------------------
// Druid: Healer/nuker hybrid.
// Engaged: heal, nuke (sun bolt etc.), roots.
// Idle: buff (spirit of wolf, skin), heal, cure.
// -------------------------------------------------------
bool Companion::AI_Druid(uint32 iSpellTypes, bool is_defensive)
{
	if (m_current_stance == COMPANION_STANCE_PASSIVE) {
		Client* owner = GetCompanionOwner();
		if (owner && owner->GetHPRatio() < 30) {
			return AI_HealGroupMember(true);
		}
		return false;
	}

	bool engaged = IsEngaged();

	if (engaged) {
		if (iSpellTypes & SpellType_Heal) {
			// Druid HoT preference (Issue #5 fix):
			// When the most injured group member is above 50% HP, prefer HoT for
			// efficiency. Below 50% HP, switch to direct heals for immediate effect.
			// This mirrors druid playstyle: HoTs for maintenance, direct heals for urgency.
			Group*  g           = GetGroup();
			Mob*    heal_target = nullptr;
			int     target_hp   = 100;

			if (g) {
				// Find most injured group member
				for (int i = 0; i < MAX_GROUP_MEMBERS; i++) {
					if (!g->members[i] || g->members[i]->GetHP() <= 0) {
						continue;
					}
					int threshold = RuleI(Companions, HealThresholdPct);
					int hpr = static_cast<int>(g->members[i]->GetHPRatio());
					if (hpr < threshold && (!heal_target || hpr < target_hp)) {
						heal_target = g->members[i];
						target_hp   = hpr;
					}
				}
			}
			// Also check self
			{
				int threshold = RuleI(Companions, HealThresholdPct);
				int self_hpr  = static_cast<int>(GetHPRatio());
				if (self_hpr < threshold && (!heal_target || self_hpr < target_hp)) {
					heal_target = this;
					target_hp   = self_hpr;
				}
			}

			if (heal_target) {
				uint32  now_ms    = Timer::GetCurrentTime();
				uint16  heal_spell = 0;

				if (target_hp > 50) {
					// Above 50% HP: prefer HoT for mana efficiency
					heal_spell = SelectHoTSpell(
						m_companion_spells, m_current_stance, target_hp, now_ms);
				}
				if (!heal_spell) {
					// Below 50% HP or no HoT available: use direct heal
					heal_spell = SelectHealSpell(
						m_companion_spells, m_current_stance, target_hp, now_ms);
				}
				if (heal_spell) {
					bool cast_ok = AIDoSpellCast(heal_spell, heal_target, spells[heal_spell].mana);
					if (cast_ok) {
						SetSpellTimeCanCast(heal_spell, spells[heal_spell].recast_time);
						return true;
					}
				}
			}
		}
		if (iSpellTypes & SpellType_Cure) {
			if (AI_CureGroupMember()) {
				return true;
			}
		}
		// Root if balanced/aggressive
		if ((iSpellTypes & SpellType_Root) && m_current_stance != COMPANION_STANCE_PASSIVE) {
			uint32 now_ms = Timer::GetCurrentTime();
			uint16 root_spell = SelectFirstSpell(m_companion_spells, SpellType_Root, m_current_stance, now_ms);
			Mob* target = GetTarget();
			if (root_spell && target && !target->IsRooted() && zone->random.Roll(30)) {
				bool cast_ok = AIDoSpellCast(root_spell, target, spells[root_spell].mana);
				if (cast_ok) {
					SetSpellTimeCanCast(root_spell, spells[root_spell].recast_time);
					return true;
				}
			}
		}
		// DoT (flame lick, drones of doom)
		if ((iSpellTypes & SpellType_DOT) && m_current_stance == COMPANION_STANCE_AGGRESSIVE) {
			if (AI_NukeTarget(SpellType_DOT)) {
				return true;
			}
		}
		// Nuke only when aggressive and mana > 30%
		if ((iSpellTypes & SpellType_Nuke) &&
		    m_current_stance == COMPANION_STANCE_AGGRESSIVE &&
		    GetManaRatio() > 30.0f) {
			return AI_NukeTarget(SpellType_Nuke);
		}
	} else {
		if (iSpellTypes & SpellType_Cure) {
			if (AI_CureGroupMember()) {
				return true;
			}
		}
		if (iSpellTypes & SpellType_Heal) {
			if (AI_HealGroupMember(false)) {
				return true;
			}
		}
		if (iSpellTypes & SpellType_Buff) {
			return AI_BuffGroupMember();
		}
	}

	return false;
}

// -------------------------------------------------------
// Shaman: Healer/slower.
// Engaged: slow enemy (highest value), heal, canni if OOM.
// Idle: buff (haste, str, agi), heal, cure.
// -------------------------------------------------------
bool Companion::AI_Shaman(uint32 iSpellTypes, bool is_defensive)
{
	if (m_current_stance == COMPANION_STANCE_PASSIVE) {
		Client* owner = GetCompanionOwner();
		if (owner && owner->GetHPRatio() < 40) {
			return AI_HealGroupMember(true);
		}
		return false;
	}

	bool engaged = IsEngaged();

	if (engaged) {
		// Slow the target — shaman's most valuable contribution.
		// Always attempt slow on the first opportunity (no random chance).
		// The slow will only fire if a spell is available and the target
		// is not immune.
		if (iSpellTypes & SpellType_Slow) {
			Mob* target = GetTarget();
			if (target && !target->GetSpecialAbility(SpecialAbility::SlowImmunity)) {
				if (AI_SlowDebuff(target)) {
					return true;
				}
			}
		}
		// Heal
		if (iSpellTypes & SpellType_Heal) {
			if (AI_HealGroupMember(true)) {
				return true;
			}
		}
		// Cure
		if (iSpellTypes & SpellType_Cure) {
			if (AI_CureGroupMember()) {
				return true;
			}
		}
		// DoT when aggressive and mana above cutoff
		if ((iSpellTypes & SpellType_DOT) && m_current_stance == COMPANION_STANCE_AGGRESSIVE &&
		    GetManaRatio() > static_cast<float>(RuleI(Companions, ManaCutoffPct))) {
			if (AI_NukeTarget(SpellType_DOT)) {
				return true;
			}
		}
		// Cannibalize (Issue #6 fix): convert HP to mana when mana is low and HP healthy.
		// Conditions: mana < 40%, HP > 80%, and a Cannibalize spell is available.
		// This is the shaman's signature mana recovery ability in Classic-Luclin.
		// Note: Cannibalize spells must be present in companion_spell_sets for shaman
		// (data-expert task). FindCannibalizeSpell() identifies them by spell effects.
		if (GetMaxMana() > 0 && GetManaRatio() < 40.0f && GetHPRatio() > 80.0f) {
			uint16 canni_spell = FindCannibalizeSpell();
			if (canni_spell) {
				bool cast_ok = AIDoSpellCast(canni_spell, this, spells[canni_spell].mana);
				if (cast_ok) {
					SetSpellTimeCanCast(canni_spell, spells[canni_spell].recast_time);
					return true;
				}
			}
		}
	} else {
		if (iSpellTypes & SpellType_Cure) {
			if (AI_CureGroupMember()) {
				return true;
			}
		}
		if (iSpellTypes & SpellType_Heal) {
			if (AI_HealGroupMember(false)) {
				return true;
			}
		}
		// Cannibalize when idle and mana is low — refuel faster while sitting
		if (GetMaxMana() > 0 && GetManaRatio() < 40.0f && GetHPRatio() > 80.0f) {
			uint16 canni_spell = FindCannibalizeSpell();
			if (canni_spell) {
				bool cast_ok = AIDoSpellCast(canni_spell, this, spells[canni_spell].mana);
				if (cast_ok) {
					SetSpellTimeCanCast(canni_spell, spells[canni_spell].recast_time);
					return true;
				}
			}
		}
		if (iSpellTypes & SpellType_Buff) {
			return AI_BuffGroupMember();
		}
	}

	return false;
}

// ============================================================
// FindCannibalizeSpell — identify Cannibalize spells by effect data
//
// Scans m_companion_spells for self-only spells with SE_CurrentMana
// (effect 15) and a positive base value (mana gain). Returns the
// highest-level (last slot) match for maximum mana conversion.
//
// This identifies Cannibalize I-IV without requiring a special spell
// type tag in companion_spell_sets — the spell effects tell the story.
// ============================================================
uint16 Companion::FindCannibalizeSpell()
{
	uint16 best_spell = 0;
	int    best_slot  = -1;
	uint32 now_ms     = Timer::GetCurrentTime();

	for (const auto& cs : m_companion_spells) {
		if (!IsValidSpell(cs.spellid)) {
			continue;
		}
		if (cs.time_cancast > now_ms) {
			continue; // on recast cooldown
		}
		if (!StanceMatch(cs.stance, m_current_stance)) {
			continue;
		}

		const SPDat_Spell_Struct& sp = spells[cs.spellid];

		// Must be self-only (Cannibalize only affects the caster)
		if (sp.target_type != ST_Self) {
			continue;
		}

		// Look for SE_CurrentMana (effect 15) with positive base value (mana gain)
		bool has_mana_gain = false;
		for (int i = 0; i < EFFECT_COUNT; ++i) {
			if (sp.effect_id[i] == SpellEffect::CurrentMana && sp.base_value[i] > 0) {
				has_mana_gain = true;
				break;
			}
		}

		if (!has_mana_gain) {
			continue;
		}

		// Pick the highest-slot (best level) Cannibalize available
		if (best_slot < cs.slot) {
			best_spell = cs.spellid;
			best_slot  = cs.slot;
		}
	}

	return best_spell;
}

// -------------------------------------------------------
// Rogue: Pure melee — no spells in Classic/Luclin.
// Uses evade (a discipline / ability, not a spell).
// -------------------------------------------------------
bool Companion::AI_Rogue(uint32 iSpellTypes, bool is_defensive)
{
	// Rogues have no meaningful spell AI — melee only
	// InCombatBuff captures any disciplines if the data-expert adds them
	if ((iSpellTypes & SpellType_InCombatBuff) && IsEngaged()) {
		return AI_InCombatBuff();
	}
	return false;
}

// -------------------------------------------------------
// Monk: Pure melee — disciplines only.
// -------------------------------------------------------
bool Companion::AI_Monk(uint32 iSpellTypes, bool is_defensive)
{
	if ((iSpellTypes & SpellType_InCombatBuff) && IsEngaged()) {
		return AI_InCombatBuff();
	}
	return false;
}

// -------------------------------------------------------
// Ranger: Melee with support spells.
// Engaged: snare (slow movement), nuke (firestrike etc.), bow disciplines.
// Idle: buff (endure elements, camouflage).
// -------------------------------------------------------
bool Companion::AI_Ranger(uint32 iSpellTypes, bool is_defensive)
{
	if (m_current_stance == COMPANION_STANCE_PASSIVE) {
		return false;
	}

	bool engaged = IsEngaged();

	if (engaged) {
		// Snare to prevent fleeing enemies
		if ((iSpellTypes & SpellType_Snare) && zone->random.Roll(30)) {
			Mob* target = GetTarget();
			if (target && !target->GetSpecialAbility(SpecialAbility::SnareImmunity) &&
			    target->GetSnaredAmount() >= 0) {
				uint32 now_ms = Timer::GetCurrentTime();
				uint16 snare_spell = SelectFirstSpell(m_companion_spells, SpellType_Snare, m_current_stance, now_ms);
				if (snare_spell) {
					bool cast_ok = AIDoSpellCast(snare_spell, target, spells[snare_spell].mana);
					if (cast_ok) {
						SetSpellTimeCanCast(snare_spell, spells[snare_spell].recast_time);
						return true;
					}
				}
			}
		}
		// Nuke (firestrike, flame arrow)
		if ((iSpellTypes & SpellType_Nuke) && m_current_stance == COMPANION_STANCE_AGGRESSIVE) {
			if (AI_NukeTarget(SpellType_Nuke)) {
				return true;
			}
		}
		// In-combat buff (trueshot discipline, etc.)
		if (iSpellTypes & SpellType_InCombatBuff) {
			return AI_InCombatBuff();
		}
	} else {
		if (iSpellTypes & SpellType_Buff) {
			return AI_BuffGroupMember();
		}
	}

	return false;
}

// -------------------------------------------------------
// Beastlord: Pet summoner + melee.
// Engaged: pet maintained, slow (spirit strikes), self-buffs.
// Idle: summon/maintain pet, buffs.
// -------------------------------------------------------
bool Companion::AI_Beastlord(uint32 iSpellTypes, bool is_defensive)
{
	if (m_current_stance == COMPANION_STANCE_PASSIVE) {
		return false;
	}

	bool engaged = IsEngaged();

	// Pet is highest priority when not engaged (maintain pet).
	// Guard with 25% mana threshold (Issue #7 fix) — consistent with Necromancer/Magician.
	if (!engaged && (iSpellTypes & SpellType_Pet) && GetManaRatio() > 25.0f) {
		if (AI_SummonPet()) {
			return true;
		}
	}

	if (engaged) {
		// Keep pet alive if it exists
		if (HasPet() && (iSpellTypes & SpellType_Pet) && GetPet()->GetHPRatio() < 30) {
			// Re-summon pet if dead — for now just fall through
		}
		// Slow/debuff
		if ((iSpellTypes & SpellType_Slow) && zone->random.Roll(50)) {
			Mob* target = GetTarget();
			if (target && !target->GetSpecialAbility(SpecialAbility::SlowImmunity)) {
				if (AI_SlowDebuff(target)) {
					return true;
				}
			}
		}
		// In-combat buff (warder's gift, etc.)
		if (iSpellTypes & SpellType_InCombatBuff) {
			if (AI_InCombatBuff()) {
				return true;
			}
		}
		// DoT when aggressive
		if ((iSpellTypes & SpellType_DOT) && m_current_stance == COMPANION_STANCE_AGGRESSIVE) {
			return AI_NukeTarget(SpellType_DOT);
		}
	} else {
		if (iSpellTypes & SpellType_Buff) {
			return AI_BuffGroupMember();
		}
	}

	return false;
}

// -------------------------------------------------------
// Wizard: Pure caster DPS.
// Engaged: nuke aggressively.  Ports and escape when OOM/defensive.
// Idle: buff (not many wizard buffs), gate.
// -------------------------------------------------------
bool Companion::AI_Wizard(uint32 iSpellTypes, bool is_defensive)
{
	if (m_current_stance == COMPANION_STANCE_PASSIVE) {
		return false;
	}

	bool engaged = IsEngaged();

	if (engaged) {
		// Self-preservation: escape spell if very low HP
		if (is_defensive && (iSpellTypes & SpellType_Escape)) {
			uint32 now_ms = Timer::GetCurrentTime();
			uint16 escape_spell = SelectFirstSpell(m_companion_spells, SpellType_Escape, m_current_stance, now_ms);
			if (escape_spell && GetHPRatio() < 20.0f) {
				bool cast_ok = AIDoSpellCast(escape_spell, this, spells[escape_spell].mana);
				if (cast_ok) {
					SetSpellTimeCanCast(escape_spell, spells[escape_spell].recast_time);
					return true;
				}
			}
		}
		// Nuke: primary wizard role — stop nuking below ManaCutoffPct to preserve
		// mana for emergencies (gate, emergency CC).
		if ((iSpellTypes & SpellType_Nuke) &&
		    GetManaRatio() > static_cast<float>(RuleI(Companions, ManaCutoffPct))) {
			return AI_NukeTarget(SpellType_Nuke);
		}
	} else {
		// Idle: use AI_WizardBuff instead of AI_BuffGroupMember.
		// Wizard DS spells should only target melee companions (Issue #8 fix).
		if (iSpellTypes & SpellType_Buff) {
			return AI_WizardBuff();
		}
	}

	return false;
}

// -------------------------------------------------------
// Magician: Pet + nuke.
// Engaged: maintain pet, nuke.
// Idle: summon pet, buff.
// -------------------------------------------------------
bool Companion::AI_Magician(uint32 iSpellTypes, bool is_defensive)
{
	if (m_current_stance == COMPANION_STANCE_PASSIVE) {
		return false;
	}

	bool engaged = IsEngaged();

	// Maintain pet — guard with 25% mana threshold (Issue #7 fix).
	// Pet summon spells are expensive (200-600 mana); don't attempt at critically
	// low mana so that mana is preserved for nukes/lifetaps instead.
	if ((iSpellTypes & SpellType_Pet) && GetManaRatio() > 25.0f) {
		if (AI_SummonPet()) {
			return true;
		}
	}

	if (engaged) {
		// Nuke when mana available — use ManaCutoffPct rule (Issue #2 fix).
		// Previously hardcoded to 20.0f; now respects server operator settings.
		if ((iSpellTypes & SpellType_Nuke) &&
		    GetManaRatio() > static_cast<float>(RuleI(Companions, ManaCutoffPct))) {
			return AI_NukeTarget(SpellType_Nuke);
		}
	} else {
		if (iSpellTypes & SpellType_Buff) {
			return AI_BuffGroupMember();
		}
	}

	return false;
}

// -------------------------------------------------------
// Necromancer: Pet + DoT + lifetap.
// Engaged: maintain pet, apply DoTs, lifetap to sustain.
// Idle: summon pet, buff.
// -------------------------------------------------------
bool Companion::AI_Necromancer(uint32 iSpellTypes, bool is_defensive)
{
	if (m_current_stance == COMPANION_STANCE_PASSIVE) {
		return false;
	}

	bool engaged = IsEngaged();

	// Maintain pet — guard with 25% mana threshold (Issue #7 fix).
	// Pet summon spells are expensive; save mana for DoTs and lifetap when low.
	if ((iSpellTypes & SpellType_Pet) && GetManaRatio() > 25.0f) {
		if (AI_SummonPet()) {
			return true;
		}
	}

	if (engaged) {
		// DoTs are necro's main tool — stop when mana is low
		if ((iSpellTypes & SpellType_DOT) && zone->random.Roll(70) &&
		    GetManaRatio() > static_cast<float>(RuleI(Companions, ManaCutoffPct))) {
			if (AI_NukeTarget(SpellType_DOT)) {
				return true;
			}
		}
		// Lifetap for self-sustain when low HP (always allow regardless of mana,
		// since lifetap restores HP and is critical for survival)
		if ((iSpellTypes & SpellType_Lifetap) && GetHPRatio() < 60.0f) {
			if (AI_NukeTarget(SpellType_Lifetap)) {
				return true;
			}
		}
		// Direct nuke — stop nuking below ManaCutoffPct to reserve for DoTs/lifetap
		if ((iSpellTypes & SpellType_Nuke) && zone->random.Roll(40) &&
		    GetManaRatio() > static_cast<float>(RuleI(Companions, ManaCutoffPct))) {
			return AI_NukeTarget(SpellType_Nuke);
		}
	} else {
		// Idle: resurrect dead > buff (necromancer always keeps bones walking)
		if (iSpellTypes & SpellType_Resurrect) {
			if (AI_ResurrectDeadGroupMember()) {
				return true;
			}
		}
		if (iSpellTypes & SpellType_Buff) {
			return AI_BuffGroupMember();
		}
	}

	return false;
}

// -------------------------------------------------------
// Enchanter: CC utility.
// Engaged: mez additional mobs, slow primary target, haste group.
// Idle: buff (haste, clarity), mez stragglers.
// -------------------------------------------------------
bool Companion::AI_Enchanter(uint32 iSpellTypes, bool is_defensive)
{
	if (m_current_stance == COMPANION_STANCE_PASSIVE) {
		return false;
	}

	bool engaged = IsEngaged();

	if (engaged) {
		// Mez additional mobs (highest utility for enchanter)
		if (iSpellTypes & SpellType_Mez) {
			if (AI_MezTarget()) {
				return true;
			}
		}
		// Slow primary target
		if ((iSpellTypes & SpellType_Slow) && zone->random.Roll(60)) {
			Mob* target = GetTarget();
			if (target && !target->GetSpecialAbility(SpecialAbility::SlowImmunity)) {
				if (AI_SlowDebuff(target)) {
					return true;
				}
			}
		}
		// In-combat buff (rune, haste)
		if (iSpellTypes & SpellType_InCombatBuff) {
			if (AI_InCombatBuff()) {
				return true;
			}
		}
		// Nuke when aggressive — reserve extra mana for emergency mez (Issue #3 fix).
		// Enchanters are CC utility, not DPS. Reserve mana above ManaCutoffPct+10
		// (default 30%) so mez remains available even when nuking aggressively.
		// This prevents the enchanter from nuking to 10% OOM bail, leaving no mana
		// for a critical mez when adds appear.
		if ((iSpellTypes & SpellType_Nuke) && m_current_stance == COMPANION_STANCE_AGGRESSIVE &&
		    GetManaRatio() > static_cast<float>(RuleI(Companions, ManaCutoffPct) + 10)) {
			return AI_NukeTarget(SpellType_Nuke);
		}
	} else {
		if (iSpellTypes & SpellType_Buff) {
			return AI_BuffGroupMember();
		}
		// Idle mez to keep things under control
		if (iSpellTypes & SpellType_Mez) {
			return AI_MezTarget();
		}
	}

	return false;
}

// -------------------------------------------------------
// Bard: Ongoing song buffs + crowd control.
// Bards twist songs; for companions we approximate by cycling
// through available InCombatBuffSong / Buff spells.
// Engaged: haste song, resist song, mez additional mobs.
// Idle: movement speed, buff songs.
// -------------------------------------------------------
bool Companion::AI_Bard(uint32 iSpellTypes, bool is_defensive)
{
	if (m_current_stance == COMPANION_STANCE_PASSIVE) {
		return false;
	}

	bool engaged = IsEngaged();

	if (engaged) {
		// Mez additional mobs
		if (iSpellTypes & SpellType_Mez) {
			if (AI_MezTarget()) {
				return true;
			}
		}
		// InCombatBuffSong (haste, attack songs)
		if (iSpellTypes & SpellType_InCombatBuffSong) {
			uint32 now_ms = Timer::GetCurrentTime();
			uint16 song_spell = SelectFirstSpell(
				m_companion_spells,
				SpellType_InCombatBuffSong | SpellType_InCombatBuff,
				m_current_stance, now_ms);
			if (song_spell && CanBuffStack(song_spell, GetLevel(), true) >= 0) {
				bool cast_ok = AIDoSpellCast(song_spell, this, spells[song_spell].mana);
				if (cast_ok) {
					SetSpellTimeCanCast(song_spell, spells[song_spell].recast_time);
					return true;
				}
			}
		}
		// Snare fleeing targets
		if ((iSpellTypes & SpellType_Snare) && zone->random.Roll(20)) {
			Mob* target = GetTarget();
			if (target && !target->GetSpecialAbility(SpecialAbility::SnareImmunity)) {
				uint32 now_ms = Timer::GetCurrentTime();
				uint16 snare_spell = SelectFirstSpell(m_companion_spells, SpellType_Snare, m_current_stance, now_ms);
				if (snare_spell) {
					bool cast_ok = AIDoSpellCast(snare_spell, target, spells[snare_spell].mana);
					if (cast_ok) {
						SetSpellTimeCanCast(snare_spell, spells[snare_spell].recast_time);
						return true;
					}
				}
			}
		}
	} else {
		// Idle: out-of-combat buff songs
		if (iSpellTypes & (SpellType_Buff | SpellType_OutOfCombatBuffSong)) {
			return AI_BuffGroupMember();
		}
	}

	return false;
}

// -------------------------------------------------------
// Generic fallback for unrecognized classes
// -------------------------------------------------------
bool Companion::AI_Generic(uint32 iSpellTypes, bool is_defensive)
{
	bool engaged = IsEngaged();

	if (engaged) {
		if (iSpellTypes & SpellType_Heal) {
			if (AI_HealGroupMember(true)) {
				return true;
			}
		}
		if (iSpellTypes & SpellType_Nuke) {
			if (AI_NukeTarget(SpellType_Nuke | SpellType_Lifetap | SpellType_DOT)) {
				return true;
			}
		}
	} else {
		if (iSpellTypes & SpellType_Buff) {
			return AI_BuffGroupMember();
		}
	}

	return false;
}

// ============================================================
// Task 9: Resurrection AI helpers
//
// FindDeadGroupMemberCorpse() — scan corpse_list for the
// closest unrezzed companion corpse owned by any member of
// this companion's group, within RezRange.  Only corpsps
// that belong to this companion's group owner are eligible
// (we don't rez strangers from other groups).
//
// AI_ResurrectDeadGroupMember() — full rez AI pipeline:
//   1. Check RezEnabled rule
//   2. Check post-combat delay timer is expired
//   3. Task 12: multi-healer coordination — skip if another
//      companion in the zone is already casting a rez
//   4. FindDeadGroupMemberCorpse()
//   5. Select best rez spell (mana-aware: pick cheapest if
//      mana is below 50%)
//   6. Mana check: if OOM, sit and announce meditation once
//   7. AIDoSpellCast() on the corpse
// ============================================================

Corpse* Companion::FindDeadGroupMemberCorpse()
{
	Client* owner = GetCompanionOwner();
	if (!owner) {
		return nullptr;
	}

	// Only search within RezRange
	int rez_range = RuleI(Companions, RezRange);

	// We look for any companion corpse owned by this owner's character ID
	// (each player can have one companion; if two companions are dead, we find
	// the closest one — extension to multi-companion groups can sort by priority later)
	return entity_list.GetCompanionCorpseByOwnerWithinRange(
		owner->CharacterID(), this, rez_range);
}

// -------------------------------------------------------
// Task 12: check whether any other companion in the zone is
// actively casting a rez spell on a companion corpse.
// Returns true if another rez is already in flight, meaning
// this companion should skip its own rez attempt this tick.
// -------------------------------------------------------
static bool AnotherCompanionIsRezzing(Companion* self)
{
	const auto& comp_list = entity_list.GetCompanionList();

	for (const auto& kv : comp_list) {
		Companion* other = kv.second;
		if (!other || other == self) {
			continue;
		}
		if (!other->IsCasting()) {
			continue;
		}
		// Check if the spell being cast is a SpellType_Resurrect spell
		// by scanning the other companion's spell list
		uint16 casting_id = other->CastingSpellID();
		for (const auto& cs : other->GetCompanionSpells()) {
			if (cs.spellid == casting_id && (cs.type & SpellType_Resurrect)) {
				return true;
			}
		}
	}
	return false;
}

// -------------------------------------------------------
// Deity-flavored rez meditation line lookup.
// Returns a class-themed message when the companion sits
// to meditate before casting a rez spell.
// -------------------------------------------------------
static const char* GetRezMeditationLine(uint8 class_id)
{
	switch (class_id) {
	case Class::Cleric:
		return "I need to meditate before I can call upon Tunare's grace.";
	case Class::Paladin:
		return "I must rest my spirit before I can invoke the light.";
	case Class::Necromancer:
		return "I need to recover mana to commune with the dead.";
	default:
		return "I need to recover mana before I can attempt a resurrection.";
	}
}

bool Companion::AI_ResurrectDeadGroupMember()
{
	if (!RuleB(Companions, RezEnabled)) {
		return false;
	}

	// Post-combat delay: rez_delay_timer must have fired (i.e. combat just ended
	// AND the delay has elapsed).  The timer is started on engaged->idle transition
	// in Process() and disabled once it fires.  While still counting down, skip rez.
	// Also skip if we never engaged (timer was never started — stays Disabled).
	// We use Enabled() to mean "counting down, not yet fired"; if it's disabled
	// the timer was never started or has already been consumed.
	if (m_rez_delay_timer.Enabled()) {
		// Timer is still counting down — not ready yet
		return false;
	}

	// Task 12: multi-healer coordination — don't pile on if another companion
	// is already channeling a rez spell
	if (AnotherCompanionIsRezzing(this)) {
		return false;
	}

	// Find a dead group member corpse we can rez
	Corpse* target_corpse = FindDeadGroupMemberCorpse();
	if (!target_corpse) {
		return false;
	}

	// Select rez spell — prefer highest-quality rez when mana is healthy,
	// fall back to cheaper / lower-% rez when below 50% mana
	uint32 now_ms = Timer::GetCurrentTime();
	uint16 rez_spell = 0;

	float mana_ratio = GetManaRatio();
	if (mana_ratio >= 50.0f) {
		// Healthy mana: pick best rez (first by slot priority = highest quality)
		rez_spell = SelectFirstSpell(m_companion_spells, SpellType_Resurrect, m_current_stance, now_ms);
	} else {
		// Low mana: pick the cheapest rez spell available (sort by mana cost ascending)
		auto candidates = GetSpellsForType(m_companion_spells, SpellType_Resurrect, m_current_stance);
		uint16 cheapest = 0;
		int32  cheapest_mana = 0x7FFFFFFF;
		for (const auto& cs : candidates) {
			if (cs.time_cancast > now_ms) {
				continue;
			}
			int32 spell_mana = spells[cs.spellid].mana;
			if (spell_mana < cheapest_mana) {
				cheapest_mana = spell_mana;
				cheapest      = cs.spellid;
			}
		}
		rez_spell = cheapest;
	}

	if (!rez_spell) {
		return false;
	}

	// Mana check: if we can't afford the cheapest rez, sit and recover
	int32 spell_mana_cost = spells[rez_spell].mana;
	if (GetMana() < spell_mana_cost) {
		// Announce once and sit to meditate
		if (!m_rez_meditation_announced) {
			m_rez_meditation_announced = true;
			CompanionGroupSay(this, "%s", GetRezMeditationLine(GetClass()));
			Sit();
		}
		return false;
	}

	// We have enough mana — stand if we were sitting to meditate
	if (m_rez_meditation_announced) {
		m_rez_meditation_announced = false;
		Stand();
	}

	// Cast on the corpse
	bool cast_ok = AIDoSpellCast(rez_spell, target_corpse, spell_mana_cost);
	if (cast_ok) {
		SetSpellTimeCanCast(rez_spell, spells[rez_spell].recast_time);
	}
	return cast_ok;
}
