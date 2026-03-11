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
// cli_companion_test_util.h
//
// Header-only helper library for companion CLI integration tests.
// Provides zone setup, companion factory, item lookup, and extended
// assertion utilities. All functions are inline to keep this header-only.
// ============================================================

#pragma once

#include "zone/companion.h"
#include "zone/zone.h"
#include "zone/entity.h"
#include "common/item_data.h"
#include "common/strings.h"

#include <cstdlib>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

// Forward declarations from cli_test_util.cpp
extern Zone *zone;
void RunTest(const std::string &test_name, const std::string &expected, const std::string &actual);
void RunTest(const std::string &test_name, bool expected, bool actual);
void RunTest(const std::string &test_name, int expected, int actual);
void SetupZone(std::string zone_short_name, uint32 instance_id = 0);

// ============================================================
// Zone Setup
// ============================================================

inline void SetupCompanionTestZone()
{
	SetupZone("arena");  // small zone, fast boot, few spawns
	zone->Process();

	// Depop the zone controller if present so it doesn't interfere
	auto controller = entity_list.GetNPCByNPCTypeID(ZONE_CONTROLLER_NPC_ID);
	if (controller != nullptr) {
		controller->Depop();
	}
	entity_list.MobProcess(); // process the depop
}

// ============================================================
// NPC Type Lookup (dynamic — avoids hardcoding IDs)
// ============================================================

inline uint32 FindNPCTypeIDForClassLevel(uint8 npc_class, uint8 min_level, uint8 max_level)
{
	auto results = content_db.QueryDatabase(
		fmt::format(
			"SELECT `id` FROM `npc_types` "
			"WHERE `class` = {} AND `level` BETWEEN {} AND {} "
			"AND `bodytype` != 11 "
			"ORDER BY `level` DESC LIMIT 1",
			static_cast<uint32>(npc_class),
			static_cast<uint32>(min_level),
			static_cast<uint32>(max_level)
		)
	);
	if (results.Success() && results.RowCount() > 0) {
		auto row = results.begin();
		return static_cast<uint32>(atoi(row[0]));
	}
	return 0;
}

// ============================================================
// Companion Factory
// ============================================================

inline Companion* CreateTestCompanion(
	uint32 npc_type_id,
	uint32 owner_char_id = 0,
	uint8 companion_type = COMPANION_TYPE_COMPANION)
{
	if (npc_type_id == 0) {
		return nullptr;
	}
	const NPCType* npc_type = content_db.LoadNPCTypesData(npc_type_id);
	if (!npc_type) {
		return nullptr;
	}
	auto* comp = new Companion(npc_type, 0.0f, 0.0f, 0.0f, 0.0f, owner_char_id, companion_type);
	entity_list.AddNPC(comp);
	return comp;
}

inline Companion* CreateTestCompanionByClass(
	uint8 npc_class,
	uint8 desired_level = 50,
	uint32 owner_char_id = 0)
{
	uint8 lo = (desired_level > 5) ? (desired_level - 5) : 1;
	uint8 hi = desired_level + 5;
	uint32 npc_id = FindNPCTypeIDForClassLevel(npc_class, lo, hi);
	if (npc_id == 0) {
		std::cerr << "[SKIP] No NPC of class " << static_cast<int>(npc_class)
		          << " near level " << static_cast<int>(desired_level) << " found in DB\n";
		return nullptr;
	}
	return CreateTestCompanion(npc_id, owner_char_id);
}

// ============================================================
// Cleanup
// ============================================================

inline void CleanupTestCompanions()
{
	std::vector<NPC*> to_remove;
	for (auto& e : entity_list.GetNPCList()) {
		if (e.second && e.second->IsCompanion()) {
			to_remove.push_back(e.second);
		}
	}
	for (auto* npc : to_remove) {
		npc->Depop();
	}
	entity_list.MobProcess(); // process depops
}

// Clean up test companion DB rows (owner_id = 0 means test data)
inline void CleanupTestCompanionDB()
{
	database.QueryDatabase("DELETE FROM `companion_data` WHERE `owner_id` = 0");
}

// ============================================================
// Item Lookup Helpers
// ============================================================

inline uint32 FindItemByName(const std::string& partial_name)
{
	auto results = content_db.QueryDatabase(
		fmt::format(
			"SELECT `id` FROM `items` WHERE `name` LIKE '%{}%' LIMIT 1",
			Strings::Escape(partial_name)
		)
	);
	if (results.Success() && results.RowCount() > 0) {
		auto row = results.begin();
		return static_cast<uint32>(atoi(row[0]));
	}
	return 0;
}

// Find a weapon with specific damage/delay characteristics
inline uint32 FindWeapon(int min_damage, int max_damage, int min_delay, int max_delay, int item_type = -1)
{
	std::string type_filter;
	if (item_type >= 0) {
		type_filter = fmt::format(" AND `itemtype` = {}", item_type);
	}
	auto results = content_db.QueryDatabase(
		fmt::format(
			"SELECT `id` FROM `items` "
			"WHERE `damage` BETWEEN {} AND {} "
			"AND `delay` BETWEEN {} AND {} "
			"{} "
			"ORDER BY `damage` ASC LIMIT 1",
			min_damage, max_damage, min_delay, max_delay, type_filter
		)
	);
	if (results.Success() && results.RowCount() > 0) {
		auto row = results.begin();
		return static_cast<uint32>(atoi(row[0]));
	}
	return 0;
}

// Find an item with a stat bonus (e.g. astr > 0)
// Searches armor, jewelry, and common equippable item types
inline uint32 FindItemWithStatBonus(const std::string& stat_col, int min_val = 1)
{
	auto results = content_db.QueryDatabase(
		fmt::format(
			"SELECT `id` FROM `items` "
			"WHERE `{}` >= {} "
			"AND `itemtype` IN (0,1,2,3,4,8,10,28) "
			"LIMIT 1",
			stat_col, min_val
		)
	);
	if (results.Success() && results.RowCount() > 0) {
		auto row = results.begin();
		return static_cast<uint32>(atoi(row[0]));
	}
	return 0;
}

// ============================================================
// Extended Assertion Helpers
// ============================================================

inline void SkipTest(const std::string& name, const std::string& reason)
{
	std::cout << "[SKIP] " << name << " (" << reason << ")\n";
}

inline void RunTestFloat(const std::string& test_name, float expected, float actual, float tolerance = 0.01f)
{
	if (std::abs(expected - actual) <= tolerance) {
		std::cout << "[✅] " << test_name << " PASSED\n";
	} else {
		std::cerr << "[❌] " << test_name << " FAILED\n";
		std::cerr << "   📌 Expected: " << expected << "\n";
		std::cerr << "   ❌ Got:      " << actual << "\n";
		std::cerr << "   Tolerance: " << tolerance << "\n";
		std::exit(1);
	}
}

inline void RunTestRange(const std::string& test_name, int actual, int min_val, int max_val)
{
	if (actual >= min_val && actual <= max_val) {
		std::cout << "[✅] " << test_name << " PASSED (value=" << actual << ")\n";
	} else {
		std::cerr << "[❌] " << test_name << " FAILED\n";
		std::cerr << "   Value: " << actual << "\n";
		std::cerr << "   Expected range: [" << min_val << ", " << max_val << "]\n";
		std::exit(1);
	}
}

inline void RunTestGreaterThan(const std::string& test_name, int actual, int threshold)
{
	if (actual > threshold) {
		std::cout << "[✅] " << test_name << " PASSED (value=" << actual << " > " << threshold << ")\n";
	} else {
		std::cerr << "[❌] " << test_name << " FAILED\n";
		std::cerr << "   Value: " << actual << "\n";
		std::cerr << "   Expected: > " << threshold << "\n";
		std::exit(1);
	}
}

inline void RunTestNotNull(const std::string& test_name, const void* ptr)
{
	if (ptr != nullptr) {
		std::cout << "[✅] " << test_name << " PASSED\n";
	} else {
		std::cerr << "[❌] " << test_name << " FAILED (was null)\n";
		std::exit(1);
	}
}

inline void RunTestNull(const std::string& test_name, const void* ptr)
{
	if (ptr == nullptr) {
		std::cout << "[✅] " << test_name << " PASSED\n";
	} else {
		std::cerr << "[❌] " << test_name << " FAILED (expected null, was non-null)\n";
		std::exit(1);
	}
}

inline void RunTestGreaterThanInt64(const std::string& test_name, int64 actual, int64 threshold)
{
	if (actual > threshold) {
		std::cout << "[✅] " << test_name << " PASSED (value=" << actual << " > " << threshold << ")\n";
	} else {
		std::cerr << "[❌] " << test_name << " FAILED\n";
		std::cerr << "   Value: " << actual << "\n";
		std::cerr << "   Expected: > " << threshold << "\n";
		std::exit(1);
	}
}
