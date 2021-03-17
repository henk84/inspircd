/*
 * InspIRCd -- Internet Relay Chat Daemon
 *
 *   Copyright (C) 2013, 2018-2019 Sadie Powell <sadie@witchery.services>
 *   Copyright (C) 2012, 2014-2015 Attila Molnar <attilamolnar@hush.com>
 *   Copyright (C) 2012 Robby <robby@chatbelgie.be>
 *   Copyright (C) 2010 Craig Edwards <brain@inspircd.org>
 *   Copyright (C) 2009 Daniel De Graaf <danieldg@inspircd.org>
 *   Copyright (C) 2007 Dennis Friis <peavey@inspircd.org>
 *   Copyright (C) 2006 Oliver Lupton <om@inspircd.org>
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
#include "base.h"

// This trick detects heap allocations of refcountbase objects
static void* last_heap = NULL;

void* refcountbase::operator new(size_t size)
{
	last_heap = ::operator new(size);
	return last_heap;
}

void refcountbase::operator delete(void* obj)
{
	if (last_heap == obj)
		last_heap = NULL;
	::operator delete(obj);
}

refcountbase::refcountbase()
{
	if (this != last_heap)
		throw CoreException("Reference allocate on the stack!");
}

refcountbase::~refcountbase()
{
	if (refcount && ServerInstance)
		ServerInstance->Logs.Log("CULLLIST", LOG_DEBUG, "refcountbase::~ @%p with refcount %d",
			(void*)this, refcount);
}

usecountbase::~usecountbase()
{
	if (usecount && ServerInstance)
		ServerInstance->Logs.Log("CULLLIST", LOG_DEBUG, "usecountbase::~ @%p with refcount %d",
			(void*)this, usecount);
}

void ServiceProvider::RegisterService()
{
}

ExtensionItem::ExtensionItem(Module* mod, const std::string& Key, ExtensibleType exttype)
	: ServiceProvider(mod, Key, SERVICE_METADATA)
	, type(exttype)
{
}

void* ExtensionItem::GetRaw(const Extensible* container) const
{
	Extensible::ExtensibleStore::const_iterator i =
		container->extensions.find(const_cast<ExtensionItem*>(this));
	if (i == container->extensions.end())
		return NULL;
	return i->second;
}

void* ExtensionItem::SetRaw(Extensible* container, void* value)
{
	std::pair<Extensible::ExtensibleStore::iterator,bool> rv =
		container->extensions.insert(std::make_pair(this, value));
	if (rv.second)
	{
		return NULL;
	}
	else
	{
		void* old = rv.first->second;
		rv.first->second = value;
		return old;
	}
}

void* ExtensionItem::UnsetRaw(Extensible* container)
{
	Extensible::ExtensibleStore::iterator i = container->extensions.find(this);
	if (i == container->extensions.end())
		return NULL;
	void* rv = i->second;
	container->extensions.erase(i);
	return rv;
}

void ExtensionItem::RegisterService()
{
	if (!ServerInstance->Extensions.Register(this))
		throw ModuleException("Extension already exists: " + name);
}

bool ExtensionManager::Register(ExtensionItem* item)
{
	return types.insert(std::make_pair(item->name, item)).second;
}

void ExtensionManager::BeginUnregister(Module* module, std::vector<reference<ExtensionItem> >& list)
{
	ExtMap::iterator i = types.begin();
	while (i != types.end())
	{
		ExtMap::iterator me = i++;
		ExtensionItem* item = me->second;
		if (item->creator == module)
		{
			list.push_back(item);
			types.erase(me);
		}
	}
}

ExtensionItem* ExtensionManager::GetItem(const std::string& name)
{
	ExtMap::iterator i = types.find(name);
	if (i == types.end())
		return NULL;
	return i->second;
}

void Extensible::UnhookExtensions(const std::vector<reference<ExtensionItem>>& toRemove)
{
	for(std::vector<reference<ExtensionItem> >::const_iterator i = toRemove.begin(); i != toRemove.end(); ++i)
	{
		ExtensionItem* item = *i;
		ExtensibleStore::iterator e = extensions.find(item);
		if (e != extensions.end())
		{
			item->Delete(this, e->second);
			extensions.erase(e);
		}
	}
}

Extensible::Extensible()
	: culled(false)
{
}

Cullable::Result Extensible::Cull()
{
	FreeAllExtItems();
	culled = true;
	return Cullable::Cull();
}

void Extensible::FreeAllExtItems()
{
	for(ExtensibleStore::iterator i = extensions.begin(); i != extensions.end(); ++i)
	{
		i->first->Delete(this, i->second);
	}
	extensions.clear();
}

Extensible::~Extensible()
{
	if ((!extensions.empty() || !culled) && ServerInstance)
		ServerInstance->Logs.Log("CULLLIST", LOG_DEBUG, "Extensible destructor called without cull @%p", (void*)this);
}

void ExtensionItem::FromInternal(Extensible* container, const std::string& value) noexcept
{
	FromNetwork(container, value);
}

void ExtensionItem::FromNetwork(Extensible* container, const std::string& value) noexcept
{
}

std::string ExtensionItem::ToHuman(const Extensible* container, void* item) const noexcept
{
	// Try to use the network form by default.
	std::string ret = ToNetwork(container, item);

	// If there's no network form then fall back to the internal form.
	if (ret.empty())
		ret = ToInternal(container, item);

	return ret;
}

std::string ExtensionItem::ToInternal(const Extensible* container, void* item) const noexcept
{
	return ToNetwork(container, item);
}

std::string ExtensionItem::ToNetwork(const Extensible* container, void* item) const noexcept
{
	return std::string();
}

ModuleException::ModuleException(const std::string &message, Module* who)
	: CoreException(message, who ? who->ModuleSourceFile : "A Module")
{
}
