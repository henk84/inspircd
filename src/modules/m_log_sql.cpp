/*
 * InspIRCd -- Internet Relay Chat Daemon
 *
 *   Copyright (C) 2022 Sadie Powell <sadie@witchery.services>
 *
 * This file is part of InspIRCd.  InspIRCd is free software: you can
 * redistribute it and/or modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation, version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "inspircd.h"
#include "modules/sql.h"

namespace
{
	Module* thismod;
}

class SQLQuery final
	: public SQL::Query
{
public:
	SQLQuery(Module* mod)
		: SQL::Query(mod)
	{
	}

	void OnResult(SQL::Result& res) override
	{
		// Nothing to do here.
	}

	void OnError(SQL::Error& error) override
	{
		ServerInstance->SNO.WriteGlobalSno('a', "Unable to write to SQL log (query error: %s).", error.ToString());
	}
};

class SQLMethod final
	: public Log::Method
{
private:
	std::string query;
	dynamic_reference<SQL::Provider> sql;

public:
	SQLMethod(const dynamic_reference<SQL::Provider>& s, const std::string& q)
		: query(q)
		, sql(s)
	{
	}

	void OnLog(Log::Level level, const std::string& type, const std::string& message) override
	{
		if (!sql)
		{
			ServerInstance->SNO.WriteGlobalSno('a', "Unable to write to SQL log (database %s not available).", sql->GetId().c_str());
			return;
		}

		SQL::ParamMap params = {
			{ "level",   ConvToStr(static_cast<uint8_t>(level)) },
			{ "message", message                                },
			{ "time",    ConvToStr(ServerInstance->Time())      },
			{ "type",    type                                   },
		};
		sql->Submit(new SQLQuery(thismod), query, params);
	}
};

class SQLEngine final
	: public Log::Engine
{
public:
	SQLEngine(Module* Creator)
		: Log::Engine(Creator, "sql")
	{
	}

public:
	Log::MethodPtr Create(std::shared_ptr<ConfigTag> tag) override
	{
		dynamic_reference<SQL::Provider> sql(creator, "SQL");
		const std::string dbid = tag->getString("dbid");
		if (!dbid.empty())
			sql.SetProvider("SQL/" + dbid);

		const std::string query = tag->getString("query", "INSERT INTO ircd_log (time, type, message) VALUES (FROM_UNIXTIME($time), '$type', '$message');", 1);
		return std::make_shared<SQLMethod>(sql, query);
	}
};

class ModuleLogSQL final
	: public Module
{
private:
	SQLEngine engine;

public:
	ModuleLogSQL()
		: Module(VF_VENDOR, "Provides the ability to write logs to an SQL database.")
		, engine(this)
	{
		thismod = this;
	}

};

MODULE_INIT(ModuleLogSQL)
