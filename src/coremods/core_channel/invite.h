/*
 * InspIRCd -- Internet Relay Chat Daemon
 *
 *   Copyright (C) 2018-2019 Sadie Powell <sadie@witchery.services>
 *   Copyright (C) 2015 Attila Molnar <attilamolnar@hush.com>
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


#pragma once

#include "modules/invite.h"

namespace Invite
{
	template<typename T>
	struct Store
	{
		typedef insp::intrusive_list<Invite, T> List;

		/** List of pending Invites
		 */
		List invites;
	};

	template<typename T, ExtensionItem::ExtensibleType ExtType>
	class ExtItem;

	class APIImpl;
}

extern void RemoveInvite(Invite::Invite* inv, bool remove_user, bool remove_chan);
extern void UnserializeInvite(LocalUser* user, const std::string& value);

template<typename T, ExtensionItem::ExtensibleType ExtType>
class Invite::ExtItem : public ExtensionItem
{
 private:
	static std::string ToString(void* item, bool human)
	{
		std::string ret;
		Store<T>* store = static_cast<Store<T>*>(item);
		for (typename insp::intrusive_list<Invite, T>::iterator i = store->invites.begin(); i != store->invites.end(); ++i)
		{
			Invite* inv = *i;
			inv->Serialize(human, (ExtType == ExtensionItem::EXT_USER), ret);
		}
		if (!ret.empty())
			ret.erase(ret.length()-1);
		return ret;
	}

 public:
	ExtItem(Module* owner, const char* extname)
		: ExtensionItem(owner, extname, ExtType)
	{
	}

	Store<T>* Get(Extensible* ext, bool create = false)
	{
		Store<T>* store = static_cast<Store<T>*>(GetRaw(ext));
		if ((create) && (!store))
		{
			store = new Store<T>;
			SetRaw(ext, store);
		}
		return store;
	}

	void Unset(Extensible* ext)
	{
		void* store = UnsetRaw(ext);
		if (store)
			Delete(ext, store);
	}

	void Delete(Extensible* container, void* item) override
	{
		Store<T>* store = static_cast<Store<T>*>(item);
		for (typename Store<T>::List::iterator i = store->invites.begin(); i != store->invites.end(); )
		{
			Invite* inv = *i;
			// Destructing the Invite invalidates the iterator, so move it now
			++i;
			RemoveInvite(inv, (ExtType != ExtensionItem::EXT_USER), (ExtType == ExtensionItem::EXT_USER));
		}

		delete store;
	}

	std::string ToHuman(const Extensible* container, void* item) const noexcept override
	{
		return ToString(item, true);
	}

	std::string ToInternal(const Extensible* container, void* item) const noexcept override
	{
		return ToString(item, false);
	}

	void FromInternal(Extensible* container, const std::string& value) noexcept override
	{
		if (ExtType != ExtensionItem::EXT_CHANNEL)
			UnserializeInvite(static_cast<LocalUser*>(container), value);
	}
};

class Invite::APIImpl : public APIBase
{
	ExtItem<LocalUser, ExtensionItem::EXT_USER> userext;
	ExtItem<Channel, ExtensionItem::EXT_CHANNEL> chanext;

 public:
	APIImpl(Module* owner);

	void Create(LocalUser* user, Channel* chan, time_t timeout) override;
	Invite* Find(LocalUser* user, Channel* chan) override;
	bool Remove(LocalUser* user, Channel* chan) override;
	const List* GetList(LocalUser* user) override;

	void RemoveAll(LocalUser* user) { userext.Unset(user); }
	void RemoveAll(Channel* chan) { chanext.Unset(chan); }
	void Destruct(Invite* inv, bool remove_chan = true, bool remove_user = true);
	void Unserialize(LocalUser* user, const std::string& value);
};
