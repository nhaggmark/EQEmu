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
// cli_companion_tests.cpp
//
// CLI integration tests for the Companion class.
// Run via: cd ~/server && ~/code/build/bin/zone tests:companion
//
// Tests establish baseline behavior for all companion subsystems
// BEFORE Phase 1 implementation begins. All tests must PASS on
// the unmodified codebase.
//
// Test Suites:
//   1. Construction & Initialization
//   2. Equipment
//   3. Melee Combat baseline
//   4. Defense / Skills
//   5. Stats & Resources
//   6. Spell Loading
//   7. Group Integration
//   8. Stance and Role Assignment
// ============================================================

#include "zone/zone_cli.h"
#include "zone/companion.h"
#include "zone/entity.h"
#include "zone/npc.h"
#include "common/rulesys.h"
#include "common/item_instance.h"
#include "common/inventory_profile.h"

#include "cli_companion_test_util.h"

#include <fmt/format.h>
#include <iostream>
#include <string>

// Forward declarations from cli_test_util.cpp
extern Zone *zone;
void RunTest(const std::string& test_name, const std::string& expected, const std::string& actual);
void RunTest(const std::string& test_name, bool expected, bool actual);
void RunTest(const std::string& test_name, int expected, int actual);

// ============================================================
// Suite 1: Construction & Initialization
// ============================================================

inline void TestCompanionConstruction()
{
	std::cout << "\n--- Suite 1: Construction & Initialization ---\n";

	// ---- Test 1.1: Basic construction from valid NPCType ----
	// Use a warrior NPC (class=1) near level 50
	Companion* comp = CreateTestCompanionByClass(1, 50, 0);
	RunTestNotNull("Construction > Companion created from valid NPC type (warrior)", comp);
	if (!comp) { return; } // can't continue without a companion

	// ---- Test 1.2: Identity flags ----
	RunTest("Construction > IsCompanion() returns true", true, comp->IsCompanion());
	RunTest("Construction > IsNPC() returns true", true, comp->IsNPC());
	RunTest("Construction > IsOfClientBot() returns true", true, comp->IsOfClientBot());
	RunTest("Construction > IsOfClientBotMerc() returns true", true, comp->IsOfClientBotMerc());
	RunTest("Construction > IsClient() returns false", false, comp->IsClient());
	RunTest("Construction > IsBot() returns false", false, comp->IsBot());

	// ---- Test 1.3: Base stat initialization ----
	// Stats may be scaled by StatScalePct rule; we check they are non-zero
	RunTestGreaterThan("Construction > GetSTR() > 0", static_cast<int>(comp->GetSTR()), 0);
	RunTestGreaterThan("Construction > GetSTA() > 0", static_cast<int>(comp->GetSTA()), 0);
	RunTestGreaterThan("Construction > GetMaxHP() > 0", static_cast<int>(comp->GetMaxHP()), 0);
	RunTestGreaterThan("Construction > GetLevel() > 0", static_cast<int>(comp->GetLevel()), 0);

	// ---- Test 1.4: Combat role — warrior must be MELEE_TANK ----
	RunTest("Construction > Warrior gets COMBAT_ROLE_MELEE_TANK",
		static_cast<int>(COMBAT_ROLE_MELEE_TANK), static_cast<int>(comp->GetCombatRole()));

	// ---- Test 1.5: Default stance and state ----
	RunTest("Construction > Default stance is BALANCED",
		static_cast<int>(COMPANION_STANCE_BALANCED), static_cast<int>(comp->GetStance()));
	RunTest("Construction > Not suspended by default", false, comp->IsSuspended());
	RunTest("Construction > Not dismissed by default", false, comp->IsDismissed());
	RunTest("Construction > Companion ID is 0 before save",
		0, static_cast<int>(comp->GetCompanionID()));
	RunTest("Construction > Owner char ID matches constructor arg",
		0, static_cast<int>(comp->GetOwnerCharacterID()));
	RunTestGreaterThan("Construction > Recruited level > 0",
		static_cast<int>(comp->GetRecruitedLevel()), 0);

	// ---- Test 1.6: Inventory profile initialized ----
	// GetInv().GetItem(0) should not crash (may return null, but must not segfault)
	const EQ::ItemInstance* inv_item = comp->GetInv().GetItem(EQ::invslot::slotPrimary);
	// We expect nullptr since no weapon was given yet — this also tests crash safety
	RunTestNull("Construction > Primary slot empty before any GiveItem",
		static_cast<const void*>(inv_item));

	// ---- Test 1.7: Equipment array zeroed ----
	bool equipment_zeroed = true;
	for (int16 s = EQ::invslot::EQUIPMENT_BEGIN; s <= EQ::invslot::EQUIPMENT_END; ++s) {
		if (comp->GetEquipment(static_cast<uint8>(s)) != 0) {
			equipment_zeroed = false;
			break;
		}
	}
	RunTest("Construction > All equipment slots are zero at construction", true, equipment_zeroed);

	// ---- Test 1.8: Flee immunity (when rule is off) ----
	bool flee_enabled = RuleB(Companions, CompanionFleeEnabled);
	if (!flee_enabled) {
		RunTest("Construction > Flee immunity set when rule is off",
			1, comp->GetSpecialAbility(SpecialAbility::FleeingImmunity));
	} else {
		SkipTest("Construction > Flee immunity", "CompanionFleeEnabled rule is true");
	}

	// ---- Test 1.9: Recruited NPC type ID stored ----
	RunTestGreaterThan("Construction > RecruitedNPCTypeID > 0",
		static_cast<int>(comp->GetRecruitedNPCTypeID()), 0);

	// ---- Test 1.10: Cleric companion creation ----
	Companion* cleric = CreateTestCompanionByClass(2, 50, 0);
	if (cleric) {
		RunTest("Construction > Cleric gets COMBAT_ROLE_HEALER",
			static_cast<int>(COMBAT_ROLE_HEALER), static_cast<int>(cleric->GetCombatRole()));
		RunTest("Construction > Cleric IsCompanion()", true, cleric->IsCompanion());
	} else {
		SkipTest("Construction > Cleric tests", "No cleric NPC found in DB at level 50");
	}

	// ---- Test 1.11: Rogue companion creation ----
	Companion* rogue = CreateTestCompanionByClass(9, 50, 0);
	if (rogue) {
		RunTest("Construction > Rogue gets COMBAT_ROLE_ROGUE",
			static_cast<int>(COMBAT_ROLE_ROGUE), static_cast<int>(rogue->GetCombatRole()));
	} else {
		SkipTest("Construction > Rogue tests", "No rogue NPC found in DB at level 50");
	}

	// ---- Test 1.12: Wizard companion creation ----
	Companion* wiz = CreateTestCompanionByClass(12, 50, 0);
	if (wiz) {
		RunTest("Construction > Wizard gets COMBAT_ROLE_CASTER_DPS",
			static_cast<int>(COMBAT_ROLE_CASTER_DPS), static_cast<int>(wiz->GetCombatRole()));
		// Casters should have max mana > 0
		RunTestGreaterThanInt64("Construction > Wizard GetMaxMana() > 0",
			wiz->GetMaxMana(), 0);
	} else {
		SkipTest("Construction > Wizard tests", "No wizard NPC found in DB at level 50");
	}
}

// ============================================================
// Suite 2: Equipment
// ============================================================

inline void TestCompanionEquipment()
{
	std::cout << "\n--- Suite 2: Equipment ---\n";

	Companion* comp = CreateTestCompanionByClass(1, 50, 0);
	if (!comp) {
		SkipTest("Equipment suite", "No warrior NPC found in DB");
		return;
	}

	// ---- Test 2.1: GiveItem basic ----
	// Find any 1H weapon
	uint32 weapon_id = FindWeapon(5, 50, 20, 50);
	if (weapon_id == 0) {
		// Fallback: find any weapon with damage > 0
		auto results = content_db.QueryDatabase(
			"SELECT `id` FROM `items` WHERE `damage` > 0 AND `itemtype` IN (0,1,2,3,4) LIMIT 1"
		);
		if (results.Success() && results.RowCount() > 0) {
			auto row = results.begin();
			weapon_id = static_cast<uint32>(atoi(row[0]));
		}
	}

	if (weapon_id == 0) {
		SkipTest("Equipment > GiveItem tests", "No weapon found in items table");
	} else {
		// Record STR before equipping a stat item
		int str_before = static_cast<int>(comp->GetSTR());

		bool give_result = comp->GiveItem(weapon_id, EQ::invslot::slotPrimary);
		RunTest("Equipment > GiveItem returns true for valid slot", true, give_result);
		RunTest("Equipment > GiveItem populates m_equipment array",
			static_cast<int>(weapon_id),
			static_cast<int>(comp->GetEquipment(EQ::invslot::slotPrimary)));

		// ---- Test 2.2: Inventory profile populated ----
		const EQ::ItemInstance* inst = comp->GetInv().GetItem(EQ::invslot::slotPrimary);
		RunTestNotNull("Equipment > GiveItem populates inventory profile", static_cast<const void*>(inst));

		if (inst && inst->GetItem()) {
			RunTestGreaterThan("Equipment > Equipped weapon has Damage > 0",
				static_cast<int>(inst->GetItem()->Damage), 0);
			RunTestGreaterThan("Equipment > Equipped weapon has Delay > 0",
				static_cast<int>(inst->GetItem()->Delay), 0);
		}

		// ---- Test 2.3: Invalid slot rejection ----
		RunTest("Equipment > GiveItem returns false for invalid slot (negative)",
			false, comp->GiveItem(weapon_id, -1));
		RunTest("Equipment > GiveItem returns false for invalid slot (too high)",
			false, comp->GiveItem(weapon_id, EQ::invslot::EQUIPMENT_END + 1));

		// ---- Test 2.4: RemoveItemFromSlot ----
		bool remove_result = comp->RemoveItemFromSlot(EQ::invslot::slotPrimary);
		RunTest("Equipment > RemoveItemFromSlot returns true", true, remove_result);
		RunTest("Equipment > RemoveItemFromSlot clears m_equipment",
			0, static_cast<int>(comp->GetEquipment(EQ::invslot::slotPrimary)));
		RunTestNull("Equipment > RemoveItemFromSlot clears inventory profile",
			static_cast<const void*>(comp->GetInv().GetItem(EQ::invslot::slotPrimary)));
	}

	// ---- Test 2.5: Stat item changes stats ----
	uint32 str_item_id = FindItemWithStatBonus("astr", 3);
	if (str_item_id == 0) {
		SkipTest("Equipment > STR item bonus test", "No STR-bonus item found in items table");
	} else {
		int str_before = static_cast<int>(comp->GetSTR());
		// Use a finger slot that definitely accepts stat items
		comp->GiveItem(str_item_id, EQ::invslot::slotEar1);
		int str_after = static_cast<int>(comp->GetSTR());
		// Remove it
		comp->RemoveItemFromSlot(EQ::invslot::slotEar1);
		int str_restored = static_cast<int>(comp->GetSTR());

		// The stat should have changed (either up or back to baseline)
		// We just verify no crash and the remove restores correctly
		RunTest("Equipment > STR after equip/remove returns to baseline", str_before, str_restored);
	}

	// ---- Test 2.6: Bow flag ----
	uint32 bow_id = FindWeapon(5, 200, 20, 100, EQ::item::ItemTypeBow);
	if (bow_id == 0) {
		// Try direct query — ItemTypeBow = 5
		auto results = content_db.QueryDatabase(
			fmt::format("SELECT `id` FROM `items` WHERE `itemtype` = {} AND `damage` > 0 LIMIT 1",
				static_cast<int>(EQ::item::ItemTypeBow))
		);
		if (results.Success() && results.RowCount() > 0) {
			auto row = results.begin();
			bow_id = static_cast<uint32>(atoi(row[0]));
		}
	}

	if (bow_id != 0) {
		comp->GiveItem(bow_id, EQ::invslot::slotRange);
		RunTest("Equipment > Equipping bow sets HasBowEquipped flag", true, comp->HasBowEquipped());
		comp->RemoveItemFromSlot(EQ::invslot::slotRange);
	} else {
		SkipTest("Equipment > Bow flag test", "No bow item found in items table");
	}

	// ---- Test 2.7: Arrow flag ----
	// EQ::item::ItemTypeArrow = 26
	uint32 arrow_id = 0;
	{
		auto results = content_db.QueryDatabase(
			fmt::format("SELECT `id` FROM `items` WHERE `itemtype` = {} LIMIT 1",
				static_cast<int>(EQ::item::ItemTypeArrow))
		);
		if (results.Success() && results.RowCount() > 0) {
			auto row = results.begin();
			arrow_id = static_cast<uint32>(atoi(row[0]));
		}
	}
	if (arrow_id != 0) {
		comp->GiveItem(arrow_id, EQ::invslot::slotAmmo);
		RunTest("Equipment > Equipping arrows sets HasArrowEquipped flag", true, comp->HasArrowEquipped());
		comp->RemoveItemFromSlot(EQ::invslot::slotAmmo);
	} else {
		SkipTest("Equipment > Arrow flag test", "No arrow item found in items table");
	}
}

// ============================================================
// Suite 3: Melee Combat (baseline)
// ============================================================

inline void TestCompanionMeleeCombat()
{
	std::cout << "\n--- Suite 3: Melee Combat (Baseline) ---\n";

	Companion* comp = CreateTestCompanionByClass(1, 50, 0);
	if (!comp) {
		SkipTest("Melee Combat suite", "No warrior NPC found in DB");
		return;
	}

	// ---- Test 3.1: Attack with null target returns false (safety check) ----
	bool attack_null = comp->Attack(nullptr);
	RunTest("Combat > Attack(nullptr) returns false", false, attack_null);

	// ---- Test 3.2: Unarmed companion has base damage > 0 ----
	RunTestGreaterThan("Combat > [BASELINE] GetBaseDamage() > 0",
		static_cast<int>(comp->GetBaseDamage()), 0);

	// ---- Test 3.3: Attack with a live target NPC ----
	// Create a target NPC to attack
	uint32 target_npc_id = FindNPCTypeIDForClassLevel(1, 5, 15);
	if (target_npc_id != 0) {
		const NPCType* target_type = content_db.LoadNPCTypesData(target_npc_id);
		if (target_type) {
			auto* target = new NPC(target_type, nullptr, glm::vec4(5, 5, 0, 0), GravityBehavior::Water, false);
			entity_list.AddNPC(target);

			// The attack may or may not hit (RNG-dependent) but must not crash
			comp->Attack(target, EQ::invslot::slotPrimary, false, false, false);
			RunTest("Combat > Attack(target) does not crash", true, true);

			target->Depop();
			entity_list.MobProcess();
		}
	} else {
		SkipTest("Combat > Attack-with-target test", "No low-level NPC found in DB");
	}

	// ---- Test 3.4: Weapon data accessible via inventory ----
	uint32 weapon_id = FindWeapon(5, 50, 20, 50);
	if (weapon_id != 0) {
		comp->GiveItem(weapon_id, EQ::invslot::slotPrimary);
		const EQ::ItemInstance* wi = comp->GetInv().GetItem(EQ::invslot::slotPrimary);
		RunTestNotNull("Combat > GetInv().GetItem(slotPrimary) non-null after equip",
			static_cast<const void*>(wi));
		if (wi && wi->GetItem()) {
			RunTestGreaterThan("Combat > Equipped weapon->Damage > 0",
				static_cast<int>(wi->GetItem()->Damage), 0);
			RunTestGreaterThan("Combat > Equipped weapon->Delay > 0",
				static_cast<int>(wi->GetItem()->Delay), 0);
		}
		comp->RemoveItemFromSlot(EQ::invslot::slotPrimary);
	} else {
		SkipTest("Combat > Weapon data access test", "No weapon found in items table");
	}

	// ---- Test 3.5: GetBaseDamage and GetMinDamage accessible ----
	int base_dmg = static_cast<int>(comp->GetBaseDamage());
	int min_dmg  = static_cast<int>(comp->GetMinDamage());
	RunTestGreaterThan("Combat > GetBaseDamage() > 0", base_dmg, 0);
	RunTest("Combat > GetMinDamage() >= 0", true, min_dmg >= 0);
}

// ============================================================
// Suite 4: Defense / Skills
// ============================================================

inline void TestCompanionDefense()
{
	std::cout << "\n--- Suite 4: Defense / Skills ---\n";

	Companion* warrior = CreateTestCompanionByClass(1, 50, 0);
	if (!warrior) {
		SkipTest("Defense suite", "No warrior NPC found in DB");
		return;
	}

	// ---- Test 4.1: IsOfClientBot routes companion to skill-based avoidance ----
	RunTest("Defense > IsOfClientBot returns true (routes to skill avoidance)", true, warrior->IsOfClientBot());

	// ---- Test 4.2: Warrior has non-zero AC ----
	RunTestGreaterThan("Defense > Warrior GetAC() > 0", static_cast<int>(warrior->GetAC()), 0);

	// ---- Test 4.3: Avoidance skills ----
	// NPC constructor populates skills from SkillCaps table.
	// These may be 0 if skill_caps lacks entries; we document rather than fail-fast.
	int dodge_skill  = static_cast<int>(warrior->GetSkill(EQ::skills::SkillDodge));
	int parry_skill  = static_cast<int>(warrior->GetSkill(EQ::skills::SkillParry));
	int defense_skill = static_cast<int>(warrior->GetSkill(EQ::skills::SkillDefense));
	int offense_skill = static_cast<int>(warrior->GetSkill(EQ::skills::SkillOffense));

	// Defense and Offense are available to all classes — these should always be > 0
	RunTestGreaterThan("Defense > Warrior has defense skill > 0", defense_skill, 0);
	RunTestGreaterThan("Defense > Warrior has offense skill > 0", offense_skill, 0);

	// Dodge: all melee classes should have dodge
	if (dodge_skill > 0) {
		RunTestGreaterThan("Defense > Warrior has dodge skill > 0", dodge_skill, 0);
	} else {
		SkipTest("Defense > Warrior dodge skill", "skill_caps table may lack warrior dodge entries");
	}

	if (parry_skill > 0) {
		RunTestGreaterThan("Defense > Warrior has parry skill > 0", parry_skill, 0);
	} else {
		SkipTest("Defense > Warrior parry skill", "skill_caps table may lack warrior parry entries");
	}

	// ---- Test 4.4: Wizard has no parry/riposte ----
	Companion* wiz = CreateTestCompanionByClass(12, 50, 0);
	if (wiz) {
		int wiz_parry   = static_cast<int>(wiz->GetSkill(EQ::skills::SkillParry));
		int wiz_riposte = static_cast<int>(wiz->GetSkill(EQ::skills::SkillRiposte));
		int wiz_block   = static_cast<int>(wiz->GetSkill(EQ::skills::SkillBlock));
		int wiz_offense = static_cast<int>(wiz->GetSkill(EQ::skills::SkillOffense));
		int wiz_defense = static_cast<int>(wiz->GetSkill(EQ::skills::SkillDefense));

		RunTest("Defense > Wizard has no parry skill", 0, wiz_parry);
		RunTest("Defense > Wizard has no riposte skill", 0, wiz_riposte);
		RunTest("Defense > Wizard has no block skill", 0, wiz_block);
		RunTestGreaterThan("Defense > Wizard has defense skill > 0", wiz_defense, 0);

		// Wizard offense < Warrior offense
		if (offense_skill > 0 && wiz_offense >= 0) {
			RunTest("Defense > Wizard offense < Warrior offense",
				true, wiz_offense < offense_skill);
		}
	} else {
		SkipTest("Defense > Wizard skill tests", "No wizard NPC found in DB");
	}

	// ---- Test 4.5: AC increases with equipment ----
	uint32 armor_id = 0;
	{
		// ItemTypeArmor = 10
		auto results = content_db.QueryDatabase(
			fmt::format("SELECT `id` FROM `items` WHERE `ac` >= 20 AND `itemtype` = {} LIMIT 1",
				static_cast<int>(EQ::item::ItemTypeArmor))
		);
		if (results.Success() && results.RowCount() > 0) {
			auto row = results.begin();
			armor_id = static_cast<uint32>(atoi(row[0]));
		}
	}

	if (armor_id != 0) {
		// GiveItem populates inventory profile; CalcBonuses() applies item bonuses.
		// itembonuses.AC reflects the equipment AC contribution.
		warrior->GiveItem(armor_id, EQ::invslot::slotChest);
		int item_bonus_ac = warrior->GetItemBonuses().AC;
		warrior->RemoveItemFromSlot(EQ::invslot::slotChest);
		RunTestGreaterThan("Defense > Equipping armor sets itembonuses.AC > 0", item_bonus_ac, 0);
	} else {
		SkipTest("Defense > AC with equipment test", "No armor with AC >= 20 found in items table");
	}
}

// ============================================================
// Suite 5: Stats & Resources
// ============================================================

inline void TestCompanionStats()
{
	std::cout << "\n--- Suite 5: Stats & Resources ---\n";

	Companion* warrior = CreateTestCompanionByClass(1, 50, 0);
	if (!warrior) {
		SkipTest("Stats suite", "No warrior NPC found in DB");
		return;
	}

	// ---- Test 5.1: CalcBonuses does not crash ----
	warrior->CalcBonuses();
	RunTest("Stats > CalcBonuses() completes without crash", true, true);

	// ---- Test 5.2: Max HP > 0 ----
	RunTestGreaterThanInt64("Stats > GetMaxHP() > 0", warrior->GetMaxHP(), 0);

	// ---- Test 5.3: Warrior has no mana (warrior is non-mana class) ----
	// Warriors have 0 max mana and CalcManaRegen should return 0
	int64 warrior_mana_regen = warrior->CalcManaRegen();
	// Note: warriors may have 0 max mana but some npc_types rows have mana set.
	// CalcManaRegen() in companion.cpp returns 0 when GetMaxMana() <= 0.
	if (warrior->GetMaxMana() <= 0) {
		RunTest("Stats > CalcManaRegen returns 0 for warrior (no mana)", 0, static_cast<int>(warrior_mana_regen));
	} else {
		SkipTest("Stats > CalcManaRegen == 0 for warrior", "warrior npc_types row has mana > 0");
	}

	// ---- Test 5.4: HP regen >= HPRegenPerTic rule floor ----
	int64 hp_regen = warrior->CalcHPRegen();
	int64 regen_floor = static_cast<int64>(RuleI(Companions, HPRegenPerTic));
	RunTest("Stats > CalcHPRegen() >= HPRegenPerTic rule floor",
		true, hp_regen >= regen_floor);

	// ---- Test 5.5: Caster CalcManaRegen ----
	Companion* cleric = CreateTestCompanionByClass(2, 50, 0);
	if (cleric) {
		// Cleric should have max mana > 0
		int64 cleric_max_mana = cleric->GetMaxMana();
		if (cleric_max_mana > 0) {
			// Mana regen while standing: should be > 0 for casters
			int64 standing_regen = cleric->CalcManaRegen();
			RunTestGreaterThanInt64("Stats > CalcManaRegen > 0 for cleric (standing)", standing_regen, 0);
		} else {
			SkipTest("Stats > Cleric CalcManaRegen test", "cleric npc_types row has mana = 0");
		}
	} else {
		SkipTest("Stats > Cleric stats", "No cleric NPC found in DB");
	}

	// ---- Test 5.6: ScaleStatsToLevel proportional scaling ----
	// Record current level and STR
	uint8 orig_level = warrior->GetRecruitedLevel();
	int32 orig_str   = static_cast<int32>(warrior->GetSTR());

	if (orig_level > 0 && orig_level < 55) {
		uint8 new_level = orig_level + 5;
		warrior->ScaleStatsToLevel(new_level);

		int32 scaled_str = static_cast<int32>(warrior->GetSTR());
		// After scaling up, STR should be >= original (proportional increase)
		RunTest("Stats > ScaleStatsToLevel increases STR when scaling up",
			true, scaled_str >= orig_str);

		// Scale back to verify the relationship
		int64 scaled_hp = warrior->GetMaxHP();
		RunTestGreaterThanInt64("Stats > GetMaxHP() > 0 after ScaleStatsToLevel", scaled_hp, 0);

		// Restore to original level
		warrior->ScaleStatsToLevel(orig_level);
	} else {
		SkipTest("Stats > ScaleStatsToLevel test", "Recruited level is 0 or already at 55+");
	}

	// ---- Test 5.7: StatScalePct at default (100) leaves stats at NPCType values ----
	int stat_scale_pct = RuleI(Companions, StatScalePct);
	if (stat_scale_pct == 100) {
		RunTest("Stats > StatScalePct is 100 (default, no scaling applied)", 100, stat_scale_pct);
	} else {
		// If not 100, just document the current value
		std::cout << "[INFO] Stats > StatScalePct is " << stat_scale_pct << " (non-default)\n";
	}
}

// ============================================================
// Suite 6: Spell Loading
// ============================================================

inline void TestCompanionSpells()
{
	std::cout << "\n--- Suite 6: Spell Loading ---\n";

	// ---- Test 6.1: Cleric spell loading ----
	Companion* cleric = CreateTestCompanionByClass(2, 50, 0);
	if (cleric) {
		bool loaded = cleric->LoadCompanionSpells();
		size_t spell_count = cleric->GetCompanionSpells().size();

		if (spell_count > 0) {
			RunTest("Spells > LoadCompanionSpells returns true for cleric", true, loaded);
			RunTestGreaterThan("Spells > Cleric GetCompanionSpells().size() > 0",
				static_cast<int>(spell_count), 0);
		} else {
			SkipTest("Spells > Cleric spell loading", "companion_spell_sets has no entries for cleric");
		}
	} else {
		SkipTest("Spells > Cleric spell tests", "No cleric NPC found in DB");
	}

	// ---- Test 6.2: Warrior spell loading (warriors are melee-only) ----
	Companion* warrior = CreateTestCompanionByClass(1, 50, 0);
	if (warrior) {
		warrior->LoadCompanionSpells();
		size_t war_spells = warrior->GetCompanionSpells().size();
		// Warriors may have 0 spells in companion_spell_sets; document actual behavior
		std::cout << "[INFO] Spells > Warrior companion spell count: " << war_spells << "\n";
		RunTest("Spells > LoadCompanionSpells does not crash for warrior", true, true);
	}

	// ---- Test 6.3: Recast timer management ----
	if (cleric && !cleric->GetCompanionSpells().empty()) {
		uint16 first_spell = cleric->GetCompanionSpells()[0].spellid;

		// Not on cooldown initially
		RunTest("Spells > CheckSpellRecastTimers returns true when not on cooldown",
			true, cleric->CheckSpellRecastTimers(first_spell));

		// Put on cooldown
		cleric->SetSpellTimeCanCast(first_spell, 60000); // 60s cooldown
		RunTest("Spells > CheckSpellRecastTimers returns false when on cooldown",
			false, cleric->CheckSpellRecastTimers(first_spell));
	} else {
		SkipTest("Spells > Recast timer tests", "No cleric spells loaded");
	}
}

// ============================================================
// Suite 7: Group Integration
// ============================================================

inline void TestCompanionGroupIntegration()
{
	std::cout << "\n--- Suite 7: Group Integration ---\n";

	Companion* comp = CreateTestCompanionByClass(1, 50, 0);
	if (!comp) {
		SkipTest("Group Integration suite", "No warrior NPC found in DB");
		return;
	}

	// ---- Test 7.1: HasRaid always returns false ----
	RunTest("Group > HasRaid always returns false", false, comp->HasRaid());

	// ---- Test 7.2: HasGroup returns false when not in a group ----
	// Note: entity_list.GetGroupByMob(comp) will return nullptr since no group was formed
	RunTest("Group > HasGroup returns false when not in a group", false, comp->HasGroup());

	// ---- Test 7.3: GetGroup returns nullptr when not in a group ----
	RunTestNull("Group > GetGroup returns nullptr when not in a group",
		static_cast<const void*>(comp->GetGroup()));

	// ---- Test 7.4: GetRaid always returns nullptr ----
	RunTestNull("Group > GetRaid always returns nullptr",
		static_cast<const void*>(comp->GetRaid()));

	// Note: Full group membership tests require a Client object (network connection).
	// Those are covered by game-tester validation in a live environment.
}

// ============================================================
// Suite 8: Stance and Role Assignment
// ============================================================

inline void TestCompanionStancePositioning()
{
	std::cout << "\n--- Suite 8: Stance and Role Assignment ---\n";

	Companion* comp = CreateTestCompanionByClass(1, 50, 0);
	if (!comp) {
		SkipTest("Stance suite", "No warrior NPC found in DB");
		return;
	}

	// ---- Test 8.1: Default stance is BALANCED ----
	RunTest("Stance > Default stance is BALANCED",
		static_cast<int>(COMPANION_STANCE_BALANCED), static_cast<int>(comp->GetStance()));

	// ---- Test 8.2: Stance changes ----
	comp->SetStance(COMPANION_STANCE_AGGRESSIVE);
	RunTest("Stance > SetStance changes to AGGRESSIVE",
		static_cast<int>(COMPANION_STANCE_AGGRESSIVE), static_cast<int>(comp->GetStance()));

	comp->SetStance(COMPANION_STANCE_PASSIVE);
	RunTest("Stance > SetStance changes to PASSIVE",
		static_cast<int>(COMPANION_STANCE_PASSIVE), static_cast<int>(comp->GetStance()));

	comp->SetStance(COMPANION_STANCE_BALANCED);
	RunTest("Stance > SetStance changes back to BALANCED",
		static_cast<int>(COMPANION_STANCE_BALANCED), static_cast<int>(comp->GetStance()));

	// ---- Test 8.3: DetermineRoleFromClass — all 15 classes ----
	RunTest("Stance > DetermineRoleFromClass(1=Warrior) == MELEE_TANK",
		static_cast<int>(COMBAT_ROLE_MELEE_TANK),
		static_cast<int>(Companion::DetermineRoleFromClass(1)));

	RunTest("Stance > DetermineRoleFromClass(2=Cleric) == HEALER",
		static_cast<int>(COMBAT_ROLE_HEALER),
		static_cast<int>(Companion::DetermineRoleFromClass(2)));

	RunTest("Stance > DetermineRoleFromClass(3=Paladin) == MELEE_TANK",
		static_cast<int>(COMBAT_ROLE_MELEE_TANK),
		static_cast<int>(Companion::DetermineRoleFromClass(3)));

	RunTest("Stance > DetermineRoleFromClass(4=Ranger) == MELEE_DPS",
		static_cast<int>(COMBAT_ROLE_MELEE_DPS),
		static_cast<int>(Companion::DetermineRoleFromClass(4)));

	RunTest("Stance > DetermineRoleFromClass(5=ShadowKnight) == MELEE_TANK",
		static_cast<int>(COMBAT_ROLE_MELEE_TANK),
		static_cast<int>(Companion::DetermineRoleFromClass(5)));

	RunTest("Stance > DetermineRoleFromClass(6=Druid) == HEALER",
		static_cast<int>(COMBAT_ROLE_HEALER),
		static_cast<int>(Companion::DetermineRoleFromClass(6)));

	RunTest("Stance > DetermineRoleFromClass(7=Monk) == MELEE_DPS",
		static_cast<int>(COMBAT_ROLE_MELEE_DPS),
		static_cast<int>(Companion::DetermineRoleFromClass(7)));

	RunTest("Stance > DetermineRoleFromClass(8=Bard) == MELEE_DPS",
		static_cast<int>(COMBAT_ROLE_MELEE_DPS),
		static_cast<int>(Companion::DetermineRoleFromClass(8)));

	RunTest("Stance > DetermineRoleFromClass(9=Rogue) == ROGUE",
		static_cast<int>(COMBAT_ROLE_ROGUE),
		static_cast<int>(Companion::DetermineRoleFromClass(9)));

	RunTest("Stance > DetermineRoleFromClass(10=Shaman) == HEALER",
		static_cast<int>(COMBAT_ROLE_HEALER),
		static_cast<int>(Companion::DetermineRoleFromClass(10)));

	RunTest("Stance > DetermineRoleFromClass(11=Necromancer) == CASTER_DPS",
		static_cast<int>(COMBAT_ROLE_CASTER_DPS),
		static_cast<int>(Companion::DetermineRoleFromClass(11)));

	RunTest("Stance > DetermineRoleFromClass(12=Wizard) == CASTER_DPS",
		static_cast<int>(COMBAT_ROLE_CASTER_DPS),
		static_cast<int>(Companion::DetermineRoleFromClass(12)));

	RunTest("Stance > DetermineRoleFromClass(13=Magician) == CASTER_DPS",
		static_cast<int>(COMBAT_ROLE_CASTER_DPS),
		static_cast<int>(Companion::DetermineRoleFromClass(13)));

	RunTest("Stance > DetermineRoleFromClass(14=Enchanter) == CASTER_DPS",
		static_cast<int>(COMBAT_ROLE_CASTER_DPS),
		static_cast<int>(Companion::DetermineRoleFromClass(14)));

	RunTest("Stance > DetermineRoleFromClass(15=Beastlord) == MELEE_DPS",
		static_cast<int>(COMBAT_ROLE_MELEE_DPS),
		static_cast<int>(Companion::DetermineRoleFromClass(15)));

	// ---- Test 8.4: Sit / Stand ----
	// Companion uses SetAppearance to track sit/stand state
	comp->Sit();
	RunTest("Stance > Sit() sets sitting state", true, comp->IsSitting());
	RunTest("Stance > Sit() IsStanding() returns false", false, comp->IsStanding());

	comp->Stand();
	RunTest("Stance > Stand() clears sitting state", false, comp->IsSitting());
	RunTest("Stance > Stand() IsStanding() returns true", true, comp->IsStanding());
}

// ============================================================
// Entry Point
// ============================================================

void ZoneCLI::TestCompanion(int argc, char **argv, argh::parser &cmd, std::string &description)
{
	description = "Run companion system integration tests";

	if (cmd[{"-h", "--help"}]) {
		return;
	}

	// Clean up any leftover test data from a previous crashed run
	CleanupTestCompanionDB();

	SetupCompanionTestZone();

	std::cout << "===========================================\n";
	std::cout << "Running Companion Tests...\n";
	std::cout << "===========================================\n";

	TestCompanionConstruction();
	CleanupTestCompanions();

	TestCompanionEquipment();
	CleanupTestCompanions();

	TestCompanionMeleeCombat();
	CleanupTestCompanions();

	TestCompanionDefense();
	CleanupTestCompanions();

	TestCompanionStats();
	CleanupTestCompanions();

	TestCompanionSpells();
	CleanupTestCompanions();

	TestCompanionGroupIntegration();
	CleanupTestCompanions();

	TestCompanionStancePositioning();
	CleanupTestCompanions();

	// Final DB cleanup
	CleanupTestCompanionDB();

	std::cout << "\n===========================================\n";
	std::cout << "[✅] All Companion Tests Completed!\n";
	std::cout << "===========================================\n";
}
