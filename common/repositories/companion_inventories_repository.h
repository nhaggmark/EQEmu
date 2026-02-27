/**
 * NPC Recruitment — companion_inventories repository
 * Hand-written for the feature/npc-recruitment branch.
 * Follows the same pattern as auto-generated repositories in base/.
 * Schema mirrors companion_inventories table (Task 20).
 */

#pragma once

#include "common/database.h"
#include "common/strings.h"
#include <vector>

class CompanionInventoriesRepository {
public:
	struct CompanionInventories {
		uint32_t    id;
		uint32_t    companion_id;
		uint16_t    slot_id;
		uint32_t    item_id;
		uint8_t     charges;
		uint32_t    aug_slot_1;
		uint32_t    aug_slot_2;
		uint32_t    aug_slot_3;
		uint32_t    aug_slot_4;
		uint32_t    aug_slot_5;
	};

	static std::string PrimaryKey() { return "id"; }
	static std::string TableName() { return "companion_inventories"; }

	static CompanionInventories NewEntity()
	{
		CompanionInventories e{};
		e.id          = 0;
		e.companion_id = 0;
		e.slot_id     = 0;
		e.item_id     = 0;
		e.charges     = 0;
		e.aug_slot_1  = 0;
		e.aug_slot_2  = 0;
		e.aug_slot_3  = 0;
		e.aug_slot_4  = 0;
		e.aug_slot_5  = 0;
		return e;
	}

	static std::vector<CompanionInventories> GetWhere(Database& db, const std::string& where_filter)
	{
		std::vector<CompanionInventories> all;
		auto results = db.QueryDatabase(
			fmt::format(
				"SELECT id, companion_id, slot_id, item_id, charges, "
				"aug_slot_1, aug_slot_2, aug_slot_3, aug_slot_4, aug_slot_5 "
				"FROM companion_inventories WHERE {}",
				where_filter
			)
		);

		for (auto row = results.begin(); row != results.end(); ++row) {
			CompanionInventories e{};
			int i = 0;
			e.id           = row[i++] ? static_cast<uint32_t>(strtoul(row[i-1], nullptr, 10)) : 0;
			e.companion_id = row[i++] ? static_cast<uint32_t>(strtoul(row[i-1], nullptr, 10)) : 0;
			e.slot_id      = row[i++] ? static_cast<uint16_t>(strtoul(row[i-1], nullptr, 10)) : 0;
			e.item_id      = row[i++] ? static_cast<uint32_t>(strtoul(row[i-1], nullptr, 10)) : 0;
			e.charges      = row[i++] ? static_cast<uint8_t>(strtoul(row[i-1], nullptr, 10)) : 0;
			e.aug_slot_1   = row[i++] ? static_cast<uint32_t>(strtoul(row[i-1], nullptr, 10)) : 0;
			e.aug_slot_2   = row[i++] ? static_cast<uint32_t>(strtoul(row[i-1], nullptr, 10)) : 0;
			e.aug_slot_3   = row[i++] ? static_cast<uint32_t>(strtoul(row[i-1], nullptr, 10)) : 0;
			e.aug_slot_4   = row[i++] ? static_cast<uint32_t>(strtoul(row[i-1], nullptr, 10)) : 0;
			e.aug_slot_5   = row[i++] ? static_cast<uint32_t>(strtoul(row[i-1], nullptr, 10)) : 0;
			all.push_back(e);
		}

		return all;
	}

	static int DeleteOne(Database& db, uint32_t inventory_id)
	{
		auto results = db.QueryDatabase(
			fmt::format("DELETE FROM companion_inventories WHERE id = {}", inventory_id)
		);
		return results.Success() ? results.RowsAffected() : 0;
	}

	static int DeleteWhere(Database& db, const std::string& where_filter)
	{
		auto results = db.QueryDatabase(
			fmt::format("DELETE FROM companion_inventories WHERE {}", where_filter)
		);
		return results.Success() ? results.RowsAffected() : 0;
	}

	static CompanionInventories InsertOne(Database& db, CompanionInventories e)
	{
		auto results = db.QueryDatabase(
			fmt::format(
				"INSERT INTO companion_inventories "
				"(companion_id, slot_id, item_id, charges, "
				"aug_slot_1, aug_slot_2, aug_slot_3, aug_slot_4, aug_slot_5) "
				"VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {})",
				e.companion_id,
				e.slot_id,
				e.item_id,
				e.charges,
				e.aug_slot_1,
				e.aug_slot_2,
				e.aug_slot_3,
				e.aug_slot_4,
				e.aug_slot_5
			)
		);

		if (!results.Success()) {
			return NewEntity();
		}

		e.id = results.LastInsertedID();
		return e;
	}
};
