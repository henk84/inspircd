/*
 * InspIRCd -- Internet Relay Chat Daemon
 *
 *   Copyright (C) 2021 Molly Miller
 *   Copyright (C) 2020 Matt Schatz <genius3000@g3k.solutions>
 *   Copyright (C) 2019 linuxdaemon <linuxdaemon.irc@gmail.com>
 *   Copyright (C) 2013, 2017-2023 Sadie Powell <sadie@witchery.services>
 *   Copyright (C) 2012-2016 Attila Molnar <attilamolnar@hush.com>
 *   Copyright (C) 2012 Robby <robby@chatbelgie.be>
 *   Copyright (C) 2010 Adam <Adam@anope.org>
 *   Copyright (C) 2009-2010 Daniel De Graaf <danieldg@inspircd.org>
 *   Copyright (C) 2007 Dennis Friis <peavey@inspircd.org>
 *   Copyright (C) 2006-2007, 2009 Craig Edwards <brain@inspircd.org>
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
#include "extension.h"
#include "modules/ssl.h"
#include "modules/webirc.h"
#include "modules/who.h"
#include "modules/whois.h"
#include "numerichelper.h"
#include "timeutils.h"

class SSLCertExt final
	: public ExtensionItem
{
public:
	SSLCertExt(Module* parent)
		: ExtensionItem(parent, "ssl_cert", ExtensionType::USER)
	{
	}

	ssl_cert* Get(const User* user) const
	{
		return static_cast<ssl_cert*>(GetRaw(user));
	}

	void Set(User* user, ssl_cert* value, bool sync = true)
	{
		value->refcount_inc();
		ssl_cert* old = static_cast<ssl_cert*>(SetRaw(user, value));
		if (old && old->refcount_dec())
			delete old;

		if (sync)
			Sync(user, value);
	}

	void Unset(User* user)
	{
		Delete(user, UnsetRaw(user));
	}

	std::string ToInternal(const Extensible* container, void* item) const noexcept override
	{
		return ToNetwork(container, item);
	}

	std::string ToNetwork(const Extensible* container, void* item) const noexcept override
	{
		const ssl_cert* cert = static_cast<ssl_cert*>(item);
		std::stringstream value;
		value
			<< (cert->IsInvalid() ? "v" : "V")
			<< (cert->IsTrusted() ? "T" : "t")
			<< (cert->IsRevoked() ? "R" : "r")
			<< (cert->IsUnknownSigner() ? "s" : "S")
			<< (cert->GetError().empty() ? "e" : "E")
			<< " ";

		if (cert->GetError().empty())
			value << cert->GetFingerprint() << " " << cert->GetDN() << " " << cert->GetIssuer();
		else
			value << cert->GetError();

		return value.str();
	}

	void FromInternal(Extensible* container, const std::string& value) noexcept override
	{
		FromNetwork(container, value);
	}

	void FromNetwork(Extensible* container, const std::string& value) noexcept override
	{
		if (container->extype != this->extype)
			return;

		auto* cert = new ssl_cert();
		Set(static_cast<User*>(container), cert, false);

		std::stringstream s(value);
		std::string v;
		getline(s, v, ' ');

		cert->invalid = (v.find('v') != std::string::npos);
		cert->trusted = (v.find('T') != std::string::npos);
		cert->revoked = (v.find('R') != std::string::npos);
		cert->unknownsigner = (v.find('s') != std::string::npos);
		if (v.find('E') != std::string::npos)
		{
			getline(s, cert->error, '\n');
		}
		else
		{
			getline(s, cert->fingerprint, ' ');
			getline(s, cert->dn, ' ');
			getline(s, cert->issuer, '\n');
		}
	}

	void Delete(Extensible* container, void* item) override
	{
		ssl_cert* old = static_cast<ssl_cert*>(item);
		if (old && old->refcount_dec())
			delete old;
	}
};

class UserCertificateAPIImpl final
	: public UserCertificateAPIBase
{
public:
	BoolExtItem nosslext;
	SSLCertExt sslext;
	bool localsecure;

	UserCertificateAPIImpl(Module* mod)
		: UserCertificateAPIBase(mod)
		, nosslext(mod, "no-ssl-cert", ExtensionType::USER)
		, sslext(mod)
	{
	}

	ssl_cert* GetCertificate(User* user) override
	{
		ssl_cert* cert = sslext.Get(user);
		if (cert)
			return cert;

		LocalUser* luser = IS_LOCAL(user);
		if (!luser || nosslext.Get(luser))
			return nullptr;

		cert = SSLClientCert::GetCertificate(&luser->eh);
		if (!cert)
			return nullptr;

		SetCertificate(user, cert);
		return cert;
	}

	bool IsSecure(User* user) override
	{
		auto* cert = GetCertificate(user);
		if (cert)
			return !!cert;

		if (localsecure)
			return user->client_sa.is_local();

		return false;
	}

	void SetCertificate(User* user, ssl_cert* cert) override
	{
		ServerInstance->Logs.Debug(MODNAME, "Setting TLS client certificate for {}: {}",
			user->GetMask(), sslext.ToNetwork(user, cert));
		sslext.Set(user, cert);
	}
};

class CommandSSLInfo final
	: public SplitCommand
{
private:
	ChanModeReference sslonlymode;

	void HandleUserInternal(LocalUser* source, User* target, bool verbose)
	{
		ssl_cert* cert = sslapi.GetCertificate(target);
		if (!cert)
		{
			source->WriteNotice(INSP_FORMAT("*** {} is not connected using TLS.", target->nick));
		}
		else if (cert->GetError().length())
		{
			source->WriteNotice(INSP_FORMAT("*** {} is connected using TLS but has not specified a valid client certificate ({}).",
				target->nick, cert->GetError()));
		}
		else if (!verbose)
		{
			source->WriteNotice(INSP_FORMAT("*** {} is connected using TLS with a valid client certificate ({}).",
				target->nick, cert->GetFingerprint()));
		}
		else
		{
			source->WriteNotice("*** Distinguished Name: " + cert->GetDN());
			source->WriteNotice("*** Issuer:             " + cert->GetIssuer());
			source->WriteNotice("*** Key Fingerprint:    " + cert->GetFingerprint());
		}
	}

	CmdResult HandleUser(LocalUser* source, const std::string& nick)
	{
		auto* target = ServerInstance->Users.FindNick(nick, true);
		if (!target)
		{
			source->WriteNumeric(Numerics::NoSuchNick(nick));
			return CmdResult::FAILURE;
		}

		if (operonlyfp && !source->IsOper() && source != target)
		{
			source->WriteNumeric(ERR_NOPRIVILEGES, "You must be a server operator to view TLS client certificate information for other users.");
			return CmdResult::FAILURE;
		}

		HandleUserInternal(source, target, true);
		return CmdResult::SUCCESS;
	}

	CmdResult HandleChannel(LocalUser* source, const std::string& channel)
	{
		auto* chan = ServerInstance->Channels.Find(channel);
		if (!chan)
		{
			source->WriteNumeric(Numerics::NoSuchChannel(channel));
			return CmdResult::FAILURE;
		}

		if (operonlyfp && !source->IsOper())
		{
			source->WriteNumeric(ERR_NOPRIVILEGES, "You must be a server operator to view TLS client certificate information for channels.");
			return CmdResult::FAILURE;
		}

		if (!source->IsOper() && chan->GetPrefixValue(source) < OP_VALUE)
		{
			source->WriteNumeric(Numerics::ChannelPrivilegesNeeded(chan, OP_VALUE, "view TLS (SSL) client certificate information"));
			return CmdResult::FAILURE;
		}

		if (sslonlymode)
		{
			source->WriteNotice(INSP_FORMAT("*** {} {} have channel mode +{} ({}) set.",
				chan->name, chan->IsModeSet(sslonlymode) ? "does" : "does not",
				sslonlymode->GetModeChar(), sslonlymode->name));
		}

		for (const auto& [u, _] : chan->GetUsers())
		{
			if (!u->server->IsService())
				HandleUserInternal(source, u, false);
		}

		return CmdResult::SUCCESS;
	}

public:
	UserCertificateAPIImpl sslapi;
	bool operonlyfp;

	CommandSSLInfo(Module* Creator)
		: SplitCommand(Creator, "SSLINFO", 1)
		, sslonlymode(Creator, "sslonly")
		, sslapi(Creator)
	{
		syntax = { "<channel|nick>" };
	}

	CmdResult HandleLocal(LocalUser* user, const Params& parameters) override
	{
		if (ServerInstance->Channels.IsPrefix(parameters[0][0]))
			return HandleChannel(user, parameters[0]);
		else
			return HandleUser(user, parameters[0]);
	}
};

class ModuleSSLInfo final
	: public Module
	, public WebIRC::EventListener
	, public Whois::EventListener
	, public Who::EventListener
{
private:
	CommandSSLInfo cmd;
	std::string hash;
	bool spkifp;
	unsigned long warnexpiring;

	static bool MatchFP(ssl_cert* const cert, const std::string& fp)
	{
		return irc::spacesepstream(fp).Contains(cert->GetFingerprint());
	}

public:
	ModuleSSLInfo()
		: Module(VF_VENDOR, "Adds user facing TLS information, various TLS configuration options, and the /SSLINFO command to look up TLS certificate information for other users.")
		, WebIRC::EventListener(this)
		, Whois::EventListener(this)
		, Who::EventListener(this)
		, cmd(this)
	{
	}

	void ReadConfig(ConfigStatus& status) override
	{
		const auto& tag = ServerInstance->Config->ConfValue("sslinfo");
		cmd.operonlyfp = tag->getBool("operonly");
		cmd.sslapi.localsecure = tag->getBool("localsecure", true);
		hash = tag->getString("hash");
		spkifp = tag->getBool("spkifp");
		warnexpiring = tag->getDuration("warnexpiring", 0, 0, 60*60*24*365);
	}

	void OnWhois(Whois::Context& whois) override
	{
		if (cmd.sslapi.IsSecure(whois.GetTarget()))
			whois.SendLine(RPL_WHOISSECURE, "is using a secure connection");

		ssl_cert* cert = cmd.sslapi.GetCertificate(whois.GetTarget());
		if (cert)
		{
			if ((!cmd.operonlyfp || whois.IsSelfWhois() || whois.GetSource()->IsOper()) && !cert->fingerprint.empty())
				whois.SendLine(RPL_WHOISCERTFP, INSP_FORMAT("has TLS client certificate fingerprint {}", cert->fingerprint));
		}
	}

	ModResult OnWhoLine(const Who::Request& request, LocalUser* source, User* user, Membership* memb, Numeric::Numeric& numeric) override
	{
		size_t flag_index;
		if (!request.GetFieldIndex('f', flag_index))
			return MOD_RES_PASSTHRU;

		ssl_cert* cert = cmd.sslapi.GetCertificate(user);
		if (cert)
			numeric.GetParams()[flag_index].push_back('s');

		return MOD_RES_PASSTHRU;
	}

	ModResult OnPreOperLogin(LocalUser* user, const std::shared_ptr<OperAccount>& oper, bool automatic) override
	{
		auto* cert = cmd.sslapi.GetCertificate(user);
		if (oper->GetConfig()->getBool("sslonly") && !cert)
		{
			if (!automatic)
			{
				ServerInstance->SNO.WriteGlobalSno('o', "{} ({}) [{}] failed to log into the \x02{}\x02 oper account because they are not connected using TLS.",
					user->nick, user->GetRealUserHost(), user->GetAddress(), oper->GetName());
			}
			return MOD_RES_DENY;
		}

		const std::string fingerprint = oper->GetConfig()->getString("fingerprint");
		if (!fingerprint.empty() && (!cert || !MatchFP(cert, fingerprint)))
		{
			if (!automatic)
			{
				ServerInstance->SNO.WriteGlobalSno('o', "{} ({}) [{}] failed to log into the \x02{}\x02 oper account because they are not using the correct TLS client certificate.",
					user->nick, user->GetRealUserHost(), user->GetAddress(), oper->GetName());
			}
			return MOD_RES_DENY;
		}

		return MOD_RES_PASSTHRU;
	}

	void OnPostConnect(User* user) override
	{
		LocalUser* const localuser = IS_LOCAL(user);
		if (!localuser)
			return;

		const SSLIOHook* const ssliohook = SSLIOHook::IsSSL(&localuser->eh);
		if (!ssliohook || cmd.sslapi.nosslext.Get(localuser))
			return;

		ssl_cert* const cert = ssliohook->GetCertificate();

		std::string text = "*** You are connected to ";
		if (!ssliohook->GetServerName(text))
			text.append(ServerInstance->Config->GetServerName());
		text.append(" using TLS cipher '");
		ssliohook->GetCiphersuite(text);
		text.push_back('\'');
		if (cert && !cert->GetFingerprint().empty())
			text.append(" and your TLS client certificate fingerprint is ").append(cert->GetFingerprint());
		user->WriteNotice(text);

		if (!cert || !warnexpiring || !cert->GetExpirationTime())
			return;

		if (ServerInstance->Time() > cert->GetExpirationTime())
		{
			user->WriteNotice("*** Your TLS (SSL) client certificate has expired.");
		}
		else if (static_cast<time_t>(ServerInstance->Time() + warnexpiring) > cert->GetExpirationTime())
		{
			const std::string duration = Duration::ToString(cert->GetExpirationTime() - ServerInstance->Time());
			user->WriteNotice("*** Your TLS (SSL) client certificate expires in " + duration + ".");
		}
	}

	ModResult OnPreChangeConnectClass(LocalUser* user, const std::shared_ptr<ConnectClass>& klass, std::optional<Numeric::Numeric>& errnum) override
	{
		ssl_cert* cert = cmd.sslapi.GetCertificate(user);
		const char* error = nullptr;
		const std::string requiressl = klass->config->getString("requiressl");
		if (stdalgo::string::equalsci(requiressl, "trusted"))
		{
			if (!cert || !cert->IsCAVerified())
				error = "a trusted TLS client certificate";
		}
		else if (klass->config->getBool("requiressl"))
		{
			if (!cert)
				error = "a TLS connection";
		}

		if (error)
		{
			ServerInstance->Logs.Debug("CONNECTCLASS", "The {} connect class is not suitable as it requires {}.",
				klass->GetName(), error);
			return MOD_RES_DENY;
		}

		return MOD_RES_PASSTHRU;
	}

	void OnWebIRCAuth(LocalUser* user, const WebIRC::FlagMap* flags) override
	{
		// We are only interested in connection flags. If none have been
		// given then we have nothing to do.
		if (!flags)
			return;

		// We only care about the tls connection flag if the connection
		// between the gateway and the server is secure.
		if (!cmd.sslapi.GetCertificate(user))
			return;

		WebIRC::FlagMap::const_iterator iter = flags->find("secure");
		if (iter == flags->end())
		{
			// If this is not set then the connection between the client and
			// the gateway is not secure.
			cmd.sslapi.nosslext.Set(user);
			cmd.sslapi.sslext.Unset(user);
			return;
		}

		// Create a fake ssl_cert for the user.
		auto* cert = new ssl_cert();
		if (!hash.empty())
		{
			iter = flags->find(spkifp ? "spkifp-" : "certfp-" + hash);
			if (iter != flags->end() && !iter->second.empty())
			{
				// If the gateway specifies this flag we put all trust onto them
				// for having validated the client certificate. This is probably
				// ill-advised but there's not much else we can do.
				cert->fingerprint = iter->second;
				cert->dn = "(unknown)";
				cert->invalid = false;
				cert->issuer = "(unknown)";
				cert->trusted = true;
				cert->unknownsigner = false;
			}
		}

		if (cert->fingerprint.empty())
		{
			cert->error = "WebIRC gateway did not send a client fingerprint";
			cert->revoked = true;
		}

		cmd.sslapi.SetCertificate(user, cert);
	}
};

MODULE_INIT(ModuleSSLInfo)
