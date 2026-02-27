/**
 * NPC Recruitment — companion_data repository
 * Hand-written for the feature/npc-recruitment branch.
 * Follows the same pattern as auto-generated repositories in base/.
 */

#pragma once

#include "common/database.h"
#include "common/strings.h"
#include <ctime>
#include <vector>

class CompanionDataRepository {
public:
	struct CompanionData {
		uint32_t    id;
		uint32_t    owner_id;
		uint32_t    npc_type_id;
		std::string name;
		uint8_t     companion_type;
		uint8_t     level;
		uint8_t     class_id;
		uint16_t    race_id;
		uint8_t     gender;
		uint32_t    zone_id;
		float       x;
		float       y;
		float       z;
		float       heading;
		int64_t     cur_hp;
		int64_t     cur_mana;
		int64_t     cur_endurance;
		uint8_t     is_suspended;
		uint8_t     stance;
		uint32_t    spawn2_id;
		uint32_t    spawngroupid;
		uint64_t    experience;
		uint8_t     recruited_level;
		uint8_t     is_dismissed;
		uint32_t    total_kills;
		std::string zones_visited;  // JSON array of zone IDs visited
		uint32_t    time_active;    // cumulative seconds active
		uint32_t    times_died;
	};

	static std::string PrimaryKey() { return "id"; }
	static std::string TableName() { return "companion_data"; }

	static CompanionData NewEntity()
	{
		CompanionData e{};
		e.id             = 0;
		e.owner_id       = 0;
		e.npc_type_id    = 0;
		e.name           = "";
		e.companion_type = 0;
		e.level          = 1;
		e.class_id       = 0;
		e.race_id        = 0;
		e.gender         = 0;
		e.zone_id        = 0;
		e.x              = 0.0f;
		e.y              = 0.0f;
		e.z              = 0.0f;
		e.heading        = 0.0f;
		e.cur_hp         = 0;
		e.cur_mana       = 0;
		e.cur_endurance  = 0;
		e.is_suspended   = 1;
		e.stance         = 1;
		e.spawn2_id      = 0;
		e.spawngroupid   = 0;
		e.experience     = 0;
		e.recruited_level = 1;
		e.is_dismissed   = 0;
		e.total_kills    = 0;
		e.zones_visited  = "[]";
		e.time_active    = 0;
		e.times_died     = 0;
		return e;
	}

	static CompanionData FindOne(Database& db, uint32_t companion_id)
	{
		auto results = db.QueryDatabase(
			fmt::format(
				"SELECT id, owner_id, npc_type_id, name, companion_type, level, class_id, race_id, "
				"gender, zone_id, x, y, z, heading, cur_hp, cur_mana, cur_endurance, "
				"is_suspended, stance, spawn2_id, spawngroupid, experience, recruited_level, "
				"is_dismissed, total_kills, zones_visited, time_active, times_died "
				"FROM companion_data WHERE id = {} LIMIT 1",
				companion_id
			)
		);

		if (results.RowCount() != 1) {
			return NewEntity();
		}

		auto row = results.begin();
		CompanionData e{};
		int i = 0;
		e.id              = row[i++] ? static_cast<uint32_t>(strtoul(row[i-1], nullptr, 10)) : 0;
		e.owner_id        = row[i++] ? static_cast<uint32_t>(strtoul(row[i-1], nullptr, 10)) : 0;
		e.npc_type_id     = row[i++] ? static_cast<uint32_t>(strtoul(row[i-1], nullptr, 10)) : 0;
		e.name            = row[i++] ? row[i-1] : "";
		e.companion_type  = row[i++] ? static_cast<uint8_t>(strtoul(row[i-1], nullptr, 10)) : 0;
		e.level           = row[i++] ? static_cast<uint8_t>(strtoul(row[i-1], nullptr, 10)) : 1;
		e.class_id        = row[i++] ? static_cast<uint8_t>(strtoul(row[i-1], nullptr, 10)) : 0;
		e.race_id         = row[i++] ? static_cast<uint16_t>(strtoul(row[i-1], nullptr, 10)) : 0;
		e.gender          = row[i++] ? static_cast<uint8_t>(strtoul(row[i-1], nullptr, 10)) : 0;
		e.zone_id         = row[i++] ? static_cast<uint32_t>(strtoul(row[i-1], nullptr, 10)) : 0;
		e.x               = row[i++] ? strtof(row[i-1], nullptr) : 0.0f;
		e.y               = row[i++] ? strtof(row[i-1], nullptr) : 0.0f;
		e.z               = row[i++] ? strtof(row[i-1], nullptr) : 0.0f;
		e.heading         = row[i++] ? strtof(row[i-1], nullptr) : 0.0f;
		e.cur_hp          = row[i++] ? static_cast<int64_t>(strtoll(row[i-1], nullptr, 10)) : 0;
		e.cur_mana        = row[i++] ? static_cast<int64_t>(strtoll(row[i-1], nullptr, 10)) : 0;
		e.cur_endurance   = row[i++] ? static_cast<int64_t>(strtoll(row[i-1], nullptr, 10)) : 0;
		e.is_suspended    = row[i++] ? static_cast<uint8_t>(strtoul(row[i-1], nullptr, 10)) : 1;
		e.stance          = row[i++] ? static_cast<uint8_t>(strtoul(row[i-1], nullptr, 10)) : 1;
		e.spawn2_id       = row[i++] ? static_cast<uint32_t>(strtoul(row[i-1], nullptr, 10)) : 0;
		e.spawngroupid    = row[i++] ? static_cast<uint32_t>(strtoul(row[i-1], nullptr, 10)) : 0;
		e.experience      = row[i++] ? static_cast<uint64_t>(strtoull(row[i-1], nullptr, 10)) : 0;
		e.recruited_level = row[i++] ? static_cast<uint8_t>(strtoul(row[i-1], nullptr, 10)) : 1;
		e.is_dismissed    = row[i++] ? static_cast<uint8_t>(strtoul(row[i-1], nullptr, 10)) : 0;
		e.total_kills     = row[i++] ? static_cast<uint32_t>(strtoul(row[i-1], nullptr, 10)) : 0;
		e.zones_visited   = row[i++] ? row[i-1] : "[]";
		e.time_active     = row[i++] ? static_cast<uint32_t>(strtoul(row[i-1], nullptr, 10)) : 0;
		e.times_died      = row[i++] ? static_cast<uint32_t>(strtoul(row[i-1], nullptr, 10)) : 0;
		return e;
	}

	static std::vector<CompanionData> GetWhere(Database& db, const std::string& where_filter)
	{
		std::vector<CompanionData> all;
		auto results = db.QueryDatabase(
			fmt::format(
				"SELECT id, owner_id, npc_type_id, name, companion_type, level, class_id, race_id, "
				"gender, zone_id, x, y, z, heading, cur_hp, cur_mana, cur_endurance, "
				"is_suspended, stance, spawn2_id, spawngroupid, experience, recruited_level, "
				"is_dismissed, total_kills, zones_visited, time_active, times_died "
				"FROM companion_data WHERE {}",
				where_filter
			)
		);

		for (auto row = results.begin(); row != results.end(); ++row) {
			CompanionData e{};
			int i = 0;
			e.id              = row[i++] ? static_cast<uint32_t>(strtoul(row[i-1], nullptr, 10)) : 0;
			e.owner_id        = row[i++] ? static_cast<uint32_t>(strtoul(row[i-1], nullptr, 10)) : 0;
			e.npc_type_id     = row[i++] ? static_cast<uint32_t>(strtoul(row[i-1], nullptr, 10)) : 0;
			e.name            = row[i++] ? row[i-1] : "";
			e.companion_type  = row[i++] ? static_cast<uint8_t>(strtoul(row[i-1], nullptr, 10)) : 0;
			e.level           = row[i++] ? static_cast<uint8_t>(strtoul(row[i-1], nullptr, 10)) : 1;
			e.class_id        = row[i++] ? static_cast<uint8_t>(strtoul(row[i-1], nullptr, 10)) : 0;
			e.race_id         = row[i++] ? static_cast<uint16_t>(strtoul(row[i-1], nullptr, 10)) : 0;
			e.gender          = row[i++] ? static_cast<uint8_t>(strtoul(row[i-1], nullptr, 10)) : 0;
			e.zone_id         = row[i++] ? static_cast<uint32_t>(strtoul(row[i-1], nullptr, 10)) : 0;
			e.x               = row[i++] ? strtof(row[i-1], nullptr) : 0.0f;
			e.y               = row[i++] ? strtof(row[i-1], nullptr) : 0.0f;
			e.z               = row[i++] ? strtof(row[i-1], nullptr) : 0.0f;
			e.heading         = row[i++] ? strtof(row[i-1], nullptr) : 0.0f;
			e.cur_hp          = row[i++] ? static_cast<int64_t>(strtoll(row[i-1], nullptr, 10)) : 0;
			e.cur_mana        = row[i++] ? static_cast<int64_t>(strtoll(row[i-1], nullptr, 10)) : 0;
			e.cur_endurance   = row[i++] ? static_cast<int64_t>(strtoll(row[i-1], nullptr, 10)) : 0;
			e.is_suspended    = row[i++] ? static_cast<uint8_t>(strtoul(row[i-1], nullptr, 10)) : 1;
			e.stance          = row[i++] ? static_cast<uint8_t>(strtoul(row[i-1], nullptr, 10)) : 1;
			e.spawn2_id       = row[i++] ? static_cast<uint32_t>(strtoul(row[i-1], nullptr, 10)) : 0;
			e.spawngroupid    = row[i++] ? static_cast<uint32_t>(strtoul(row[i-1], nullptr, 10)) : 0;
			e.experience      = row[i++] ? static_cast<uint64_t>(strtoull(row[i-1], nullptr, 10)) : 0;
			e.recruited_level = row[i++] ? static_cast<uint8_t>(strtoul(row[i-1], nullptr, 10)) : 1;
			e.is_dismissed    = row[i++] ? static_cast<uint8_t>(strtoul(row[i-1], nullptr, 10)) : 0;
			e.total_kills     = row[i++] ? static_cast<uint32_t>(strtoul(row[i-1], nullptr, 10)) : 0;
			e.zones_visited   = row[i++] ? row[i-1] : "[]";
			e.time_active     = row[i++] ? static_cast<uint32_t>(strtoul(row[i-1], nullptr, 10)) : 0;
			e.times_died      = row[i++] ? static_cast<uint32_t>(strtoul(row[i-1], nullptr, 10)) : 0;
			all.push_back(e);
		}

		return all;
	}

	static int DeleteOne(Database& db, uint32_t companion_id)
	{
		auto results = db.QueryDatabase(
			fmt::format("DELETE FROM companion_data WHERE id = {}", companion_id)
		);
		return results.Success() ? results.RowsAffected() : 0;
	}

	static int DeleteWhere(Database& db, const std::string& where_filter)
	{
		auto results = db.QueryDatabase(
			fmt::format("DELETE FROM companion_data WHERE {}", where_filter)
		);
		return results.Success() ? results.RowsAffected() : 0;
	}

	static CompanionData InsertOne(Database& db, CompanionData e)
	{
		auto results = db.QueryDatabase(
			fmt::format(
				"INSERT INTO companion_data "
				"(owner_id, npc_type_id, name, companion_type, level, class_id, race_id, "
				"gender, zone_id, x, y, z, heading, cur_hp, cur_mana, cur_endurance, "
				"is_suspended, stance, spawn2_id, spawngroupid, experience, recruited_level, "
				"is_dismissed, total_kills, zones_visited, time_active, times_died) "
				"VALUES ({}, {}, '{}', {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, "
				"{}, {}, {}, {}, {}, {}, {}, {}, '{}', {}, {})",
				e.owner_id,
				e.npc_type_id,
				Strings::Escape(e.name),
				e.companion_type,
				e.level,
				e.class_id,
				e.race_id,
				e.gender,
				e.zone_id,
				e.x, e.y, e.z, e.heading,
				e.cur_hp, e.cur_mana, e.cur_endurance,
				e.is_suspended,
				e.stance,
				e.spawn2_id,
				e.spawngroupid,
				e.experience,
				e.recruited_level,
				e.is_dismissed,
				e.total_kills,
				Strings::Escape(e.zones_visited),
				e.time_active,
				e.times_died
			)
		);

		if (!results.Success()) {
			return NewEntity();
		}

		e.id = results.LastInsertedID();
		return e;
	}

	static bool UpdateOne(Database& db, const CompanionData& e)
	{
		auto results = db.QueryDatabase(
			fmt::format(
				"UPDATE companion_data SET "
				"owner_id = {}, npc_type_id = {}, name = '{}', companion_type = {}, "
				"level = {}, class_id = {}, race_id = {}, gender = {}, zone_id = {}, "
				"x = {}, y = {}, z = {}, heading = {}, cur_hp = {}, cur_mana = {}, "
				"cur_endurance = {}, is_suspended = {}, stance = {}, spawn2_id = {}, "
				"spawngroupid = {}, experience = {}, recruited_level = {}, "
				"is_dismissed = {}, total_kills = {}, zones_visited = '{}', "
				"time_active = {}, times_died = {} "
				"WHERE id = {}",
				e.owner_id,
				e.npc_type_id,
				Strings::Escape(e.name),
				e.companion_type,
				e.level,
				e.class_id,
				e.race_id,
				e.gender,
				e.zone_id,
				e.x, e.y, e.z, e.heading,
				e.cur_hp, e.cur_mana, e.cur_endurance,
				e.is_suspended,
				e.stance,
				e.spawn2_id,
				e.spawngroupid,
				e.experience,
				e.recruited_level,
				e.is_dismissed,
				e.total_kills,
				Strings::Escape(e.zones_visited),
				e.time_active,
				e.times_died,
				e.id
			)
		);

		return results.Success();
	}
};
