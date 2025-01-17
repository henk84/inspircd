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
#include "clientprotocolmsg.h"
#include "timeutils.h"

const char* Log::LevelToString(Log::Level level)
{
	switch (level)
	{
		case Log::Level::ERROR:
			return "error";

		case Log::Level::WARNING:
			return "warning";

		case Log::Level::NORMAL:
			return "normal";

		case Log::Level::DEBUG:
			return "debug";

		case Log::Level::RAWIO:
			return "rawio";
	}

	// Should never happen.
	return "unknown";
}

void Log::NotifyRawIO(LocalUser* user, MessageType type)
{
	ClientProtocol::Messages::Privmsg msg(ServerInstance->FakeClient, user, "*** Raw I/O logging is enabled on this server. All messages, passwords, and commands are being recorded.", type);
	user->Send(ServerInstance->GetRFCEvents().privmsg, msg);
}

Log::FileMethod::FileMethod(const std::string& n, FILE* fh, unsigned long fl, bool ac)
	: Timer(15*60, true)
	, autoclose(ac)
	, file(fh)
	, flush(fl)
	, name(n)
{
	if (flush > 1)
		ServerInstance->Timers.AddTimer(this);
}

Log::FileMethod::~FileMethod()
{
	if (autoclose)
		fclose(file);
}

void Log::FileMethod::OnLog(time_t time, Level level, const std::string& type, const std::string& message)
{
	static time_t prevtime = 0;
	static std::string timestr;
	if (prevtime != time)
	{
		prevtime = time;
		timestr = Time::ToString(prevtime);
	}

	fputs(timestr.c_str(), file);
	fputs(" ", file);
	fputs(type.c_str(), file);
	fputs(": ", file);
	fputs(message.c_str(), file);
#if defined _WIN32
	fputs("\r\n", file);
#else
	fputs("\n", file);
#endif

	if (!(++lines % flush))
		fflush(file);

	if (ferror(file))
		throw CoreException(INSP_FORMAT("Unable to write to {}: {}", name, strerror(errno)));
}

bool Log::FileMethod::Tick()
{
	fflush(file);
	return true;
}

Log::Engine::Engine(Module* Creator, const std::string& Name)
	: DataProvider(Creator, "log/" + Name)
{
}

Log::Engine::~Engine()
{
	if (creator)
		ServerInstance->Logs.UnloadEngine(this);
}

Log::FileEngine::FileEngine(Module* Creator)
	: Engine(Creator, "file")
{
}

Log::MethodPtr Log::FileEngine::Create(const std::shared_ptr<ConfigTag>& tag)
{
	const std::string target = tag->getString("target");
	if (target.empty())
		throw CoreException("<log:target> must be specified for file logger at " + tag->source.str());

	const std::string fulltarget = ServerInstance->Config->Paths.PrependLog(Time::ToString(ServerInstance->Time(), target.c_str()));
	auto* fh = fopen(fulltarget.c_str(), "a");
	if (!fh)
	{
		throw CoreException(INSP_FORMAT("Unable to open {} for file logger at {}: {}", fulltarget,
			tag->source.str(), strerror(errno)));
	}

	const unsigned long flush = tag->getNum<unsigned long>("flush", 20, 1);
	return std::make_shared<FileMethod>(fulltarget, fh, flush, true);
}

Log::StreamEngine::StreamEngine(Module* Creator, const std::string& Name, FILE* fh)
	: Engine(Creator, Name)
	, file(fh)
{
}

Log::MethodPtr Log::StreamEngine::Create(const std::shared_ptr<ConfigTag>& tag)
{
	return std::make_shared<FileMethod>(name, file, 1, false);
}

Log::Manager::CachedMessage::CachedMessage(time_t ts, Level l, const std::string& t, const std::string& m)
	: time(ts)
	, level(l)
	, type(t)
	, message(m)
{
}

Log::Manager::Info::Info(Level l, TokenList t, MethodPtr m, bool c, const Engine* e)
	: config(c)
	, level(l)
	, types(std::move(t))
	, method(std::move(m))
	, engine(e)
{
}

Log::Manager::Manager()
	: filelog(nullptr)
	, stderrlog(nullptr, "stderr", stderr)
	, stdoutlog(nullptr, "stdout", stdout)
{
}

void Log::Manager::CloseLogs()
{
	logging = true; // Prevent writing to dying loggers.
	loggers.erase(std::remove_if(loggers.begin(), loggers.end(), [](const Info& info) { return info.config; }), loggers.end());
	logging = false;
}

void Log::Manager::EnableDebugMode()
{
	TokenList types = std::string("*");
	MethodPtr method = stdoutlog.Create(ServerInstance->Config->EmptyTag);
	loggers.emplace_back(Level::RAWIO, std::move(types), std::move(method), false, &stdoutlog);
	ServerInstance->Config->RawLog = true;
}

void Log::Manager::OpenLogs(bool requiremethods)
{
	// If the server is started in debug mode we don't write logs.
	if (ServerInstance->Config->CommandLine.forcedebug)
	{
		Normal("LOG", "Not opening loggers because we were started with --debug");
		return;
	}

	// If the server is started with logging disabled we don't write logs.
	if (!ServerInstance->Config->CommandLine.writelog)
	{
		Normal("LOG", "Not opening loggers because we were started with --nolog");
		return;
	}

	for (const auto& [_, tag] : ServerInstance->Config->ConfTags("log"))
	{
		const std::string methodstr = tag->getString("method", "file", 1);
		Log::Engine* engine = ServerInstance->Modules.FindDataService<Log::Engine>("log/" + methodstr);
		if (!engine)
		{
			if (!requiremethods)
				continue; // We will open this later.

			throw CoreException(methodstr + " is not a valid logging method at " + tag->source.str());
		}

		const Level level = tag->getEnum("level", Level::NORMAL, {
			{ "error",   Level::ERROR   },
			{ "warning", Level::WARNING },
			{ "normal",  Level::NORMAL  },
			{ "debug",   Level::DEBUG   },
			{ "rawio",   Level::RAWIO   },

			// Deprecated v3 names.
			{ "sparse",  Level::ERROR },
			{ "verbose", Level::WARNING },
			{ "default", Level::NORMAL },

		});
		TokenList types = tag->getString("type", "*", 1);
		MethodPtr method = engine->Create(tag);
		loggers.emplace_back(level, std::move(types), method, true, engine);
	}

	if (requiremethods && caching)
	{
		// The server has finished starting up so we can write out any cached log messages.
		for (const auto& logger : loggers)
		{
			if (!logger.method->AcceptsCachedMessages())
				continue; // Does not support logging.

			for (const auto& message : cache)
			{
				if (logger.level >= message.level && logger.types.Contains(message.type))
					logger.method->OnLog(message.time, message.level, message.type, message.message);
			}
		}

		cache.clear();
		cache.shrink_to_fit();
		caching = false;
	}

	// There might be a logger not from the config so we need to check this outside of the creation loop.
	ServerInstance->Config->RawLog = std::any_of(loggers.begin(), loggers.end(), [](const auto& logger) {
		return logger.level >= Level::RAWIO;
	});
}

void Log::Manager::RegisterServices()
{
	ServiceProvider* coreloggers[] = { &filelog, &stderrlog, &stdoutlog };
	ServerInstance->Modules.AddServices(coreloggers, sizeof(coreloggers)/sizeof(ServiceProvider*));
}

void Log::Manager::UnloadEngine(const Engine* engine)
{
	logging = true; // Prevent writing to dying loggers.
	size_t logger_count = loggers.size();
	loggers.erase(std::remove_if(loggers.begin(), loggers.end(), [&engine](const Info& info) { return info.engine == engine; }), loggers.end());
	logging = false;

	Normal("LOG", "The {} log engine is unloading; removed {}/{} loggers.", engine->name.c_str(), logger_count - loggers.size(), logger_count);
}

void Log::Manager::Write(Level level, const std::string& type, const std::string& message)
{
	if (logging)
		return; // Avoid log loops.

	logging = true;
	time_t time = ServerInstance->Time();
	for (const auto& logger : loggers)
	{
		if (logger.level >= level && logger.types.Contains(type))
			logger.method->OnLog(time, level, type, message);
	}

	if (caching)
		cache.emplace_back(time, level, type, message);
	logging = false;
}
