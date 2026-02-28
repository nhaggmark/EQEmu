/**
 * NPC Recruitment — companion_buffs repository
 * Hand-written for the feature/npc-recruitment branch.
 * Follows the same pattern as auto-generated repositories in base/.
 * Schema mirrors merc_buffs / bot_buffs.
 */

#pragma once

#include "common/database.h"
#include "common/strings.h"
#include <vector>

class CompanionBuffsRepository {
public:
	struct CompanionBuffs {
		uint32_t    id;
		uint32_t    companion_id;
		uint32_t    spell_id;
		uint8_t     caster_level;
		uint8_t     duration_formula;
		int32_t     ticks_remaining;
		int32_t     dot_rune;
		int8_t      persistent;
		int32_t     counters;
		int32_t     num_hits;
		int32_t     melee_rune;
		int32_t     magic_rune;
		int32_t     instrument_mod;
		int32_t     buff_tics;
		int32_t     caston_x;
		int32_t     caston_y;
		int32_t     caston_z;
		int32_t     extra_di_chance;
	};

	static std::string PrimaryKey() { return "id"; }
	static std::string TableName() { return "companion_buffs"; }

	static CompanionBuffs NewEntity()
	{
		CompanionBuffs e{};
		e.id              = 0;
		e.companion_id    = 0;
		e.spell_id        = 0;
		e.caster_level    = 0;
		e.duration_formula = 0;
		e.ticks_remaining = 0;
		e.dot_rune        = 0;
		e.persistent      = 0;
		e.counters        = 0;
		e.num_hits        = 0;
		e.melee_rune      = 0;
		e.magic_rune      = 0;
		e.instrument_mod  = 10;
		e.buff_tics       = 0;
		e.caston_x        = 0;
		e.caston_y        = 0;
		e.caston_z        = 0;
		e.extra_di_chance = 0;
		return e;
	}

	static std::vector<CompanionBuffs> GetWhere(Database& db, const std::string& where_filter)
	{
		std::vector<CompanionBuffs> all;
		auto results = db.QueryDatabase(
			fmt::format(
				"SELECT id, companion_id, spell_id, caster_level, duration_formula, "
				"ticks_remaining, dot_rune, persistent, counters, num_hits, "
				"melee_rune, magic_rune, instrument_mod, buff_tics, "
				"caston_x, caston_y, caston_z, extra_di_chance "
				"FROM companion_buffs WHERE {}",
				where_filter
			)
		);

		for (auto row = results.begin(); row != results.end(); ++row) {
			CompanionBuffs e{};
			int i = 0;
			e.id              = row[i++] ? static_cast<uint32_t>(strtoul(row[i-1], nullptr, 10)) : 0;
			e.companion_id    = row[i++] ? static_cast<uint32_t>(strtoul(row[i-1], nullptr, 10)) : 0;
			e.spell_id        = row[i++] ? static_cast<uint32_t>(strtoul(row[i-1], nullptr, 10)) : 0;
			e.caster_level    = row[i++] ? static_cast<uint8_t>(strtoul(row[i-1], nullptr, 10)) : 0;
			e.duration_formula = row[i++] ? static_cast<uint8_t>(strtoul(row[i-1], nullptr, 10)) : 0;
			e.ticks_remaining = row[i++] ? static_cast<int32_t>(strtol(row[i-1], nullptr, 10)) : 0;
			e.dot_rune        = row[i++] ? static_cast<int32_t>(strtol(row[i-1], nullptr, 10)) : 0;
			e.persistent      = row[i++] ? static_cast<int8_t>(strtol(row[i-1], nullptr, 10)) : 0;
			e.counters        = row[i++] ? static_cast<int32_t>(strtol(row[i-1], nullptr, 10)) : 0;
			e.num_hits        = row[i++] ? static_cast<int32_t>(strtol(row[i-1], nullptr, 10)) : 0;
			e.melee_rune      = row[i++] ? static_cast<int32_t>(strtol(row[i-1], nullptr, 10)) : 0;
			e.magic_rune      = row[i++] ? static_cast<int32_t>(strtol(row[i-1], nullptr, 10)) : 0;
			e.instrument_mod  = row[i++] ? static_cast<int32_t>(strtol(row[i-1], nullptr, 10)) : 10;
			e.buff_tics       = row[i++] ? static_cast<int32_t>(strtol(row[i-1], nullptr, 10)) : 0;
			e.caston_x        = row[i++] ? static_cast<int32_t>(strtol(row[i-1], nullptr, 10)) : 0;
			e.caston_y        = row[i++] ? static_cast<int32_t>(strtol(row[i-1], nullptr, 10)) : 0;
			e.caston_z        = row[i++] ? static_cast<int32_t>(strtol(row[i-1], nullptr, 10)) : 0;
			e.extra_di_chance = row[i++] ? static_cast<int32_t>(strtol(row[i-1], nullptr, 10)) : 0;
			all.push_back(e);
		}

		return all;
	}

	static int DeleteWhere(Database& db, const std::string& where_filter)
	{
		auto results = db.QueryDatabase(
			fmt::format("DELETE FROM companion_buffs WHERE {}", where_filter)
		);
		return results.Success() ? results.RowsAffected() : 0;
	}

	static CompanionBuffs InsertOne(Database& db, CompanionBuffs e)
	{
		auto results = db.QueryDatabase(
			fmt::format(
				"INSERT INTO companion_buffs "
				"(companion_id, spell_id, caster_level, duration_formula, "
				"ticks_remaining, dot_rune, persistent, counters, num_hits, "
				"melee_rune, magic_rune, instrument_mod, buff_tics, "
				"caston_x, caston_y, caston_z, extra_di_chance) "
				"VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {})",
				e.companion_id,
				e.spell_id,
				e.caster_level,
				e.duration_formula,
				e.ticks_remaining,
				e.dot_rune,
				e.persistent,
				e.counters,
				e.num_hits,
				e.melee_rune,
				e.magic_rune,
				e.instrument_mod,
				e.buff_tics,
				e.caston_x,
				e.caston_y,
				e.caston_z,
				e.extra_di_chance
			)
		);

		if (!results.Success()) {
			return NewEntity();
		}

		e.id = results.LastInsertedID();
		return e;
	}

	static int InsertMany(Database& db, const std::vector<CompanionBuffs>& entries)
	{
		if (entries.empty()) {
			return 0;
		}

		std::vector<std::string> value_rows;
		value_rows.reserve(entries.size());

		for (const auto& e : entries) {
			value_rows.emplace_back(
				fmt::format(
					"({}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {})",
					e.companion_id,
					e.spell_id,
					e.caster_level,
					e.duration_formula,
					e.ticks_remaining,
					e.dot_rune,
					e.persistent,
					e.counters,
					e.num_hits,
					e.melee_rune,
					e.magic_rune,
					e.instrument_mod,
					e.buff_tics,
					e.caston_x,
					e.caston_y,
					e.caston_z,
					e.extra_di_chance
				)
			);
		}

		auto results = db.QueryDatabase(
			fmt::format(
				"INSERT INTO companion_buffs "
				"(companion_id, spell_id, caster_level, duration_formula, "
				"ticks_remaining, dot_rune, persistent, counters, num_hits, "
				"melee_rune, magic_rune, instrument_mod, buff_tics, "
				"caston_x, caston_y, caston_z, extra_di_chance) "
				"VALUES {}",
				Strings::Join(value_rows, ", ")
			)
		);

		return results.Success() ? results.RowsAffected() : 0;
	}
};
