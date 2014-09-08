/*
 * Copyright (C) 2013 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) version 3.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */

#include <config.h>

#ifdef USE_GOA

#include "goa.h"
#include <syncevo/IdentityProvider.h>
#include <gdbus-cxx-bridge.h>

#include <boost/algorithm/string/join.hpp>
#include <boost/foreach.hpp>

#include <algorithm>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

/*
 * We call the GOA D-Bus API directly. This is easier than using
 * libgoa because our own D-Bus wrapper gives us data in C++ data
 * structures. It also avoids another library dependency.
 */

static const char GOA_BUS_NAME[] = "org.gnome.OnlineAccounts";
static const char GOA_PATH[] = "/org/gnome/OnlineAccounts";

static const char OBJECT_MANAGER_INTERFACE[] = "org.freedesktop.DBus.ObjectManager";
static const char OBJECT_MANAGER_GET_MANAGED_OBJECTS[] = "GetManagedObjects";

static const char GOA_ACCOUNT_INTERFACE[] = "org.gnome.OnlineAccounts.Account";
static const char GOA_ACCOUNT_ENSURE_CREDENTIALS[] = "EnsureCredentials";
static const char GOA_ACCOUNT_PRESENTATION_IDENTITY[] = "PresentationIdentity";
static const char GOA_ACCOUNT_ID[] = "Id";
static const char GOA_ACCOUNT_PROVIDER_NAME[] = "ProviderName";

static const char GOA_OAUTH2_INTERFACE[] = "org.gnome.OnlineAccounts.OAuth2Based";
static const char GOA_OAUTH2_GET_ACCESS_TOKEN[] = "GetAccessToken";

class GOAAccount;

class GOAManager : private GDBusCXX::DBusRemoteObject
{
    typedef std::map<std::string, // property name
                     boost::variant<std::string> // property value - we only care about strings
                     > Properties;
    typedef std::map<std::string, // interface name
                     Properties
                     > Interfaces;
    typedef std::map<GDBusCXX::DBusObject_t, Interfaces> ManagedObjects;
    GDBusCXX::DBusClientCall1<ManagedObjects> m_getManagedObjects;

 public:
    GOAManager(const GDBusCXX::DBusConnectionPtr &conn);

    /**
     * Find a particular account, identified by its representation ID
     * (the unique user visible string). The account must support OAuth2,
     * otherwise an error is thrown.
     */
    boost::shared_ptr<GOAAccount> lookupAccount(const std::string &representationID);
};

class GOAAccount
{
    GDBusCXX::DBusRemoteObject m_account;
    GDBusCXX::DBusRemoteObject m_oauth2;


public:
    GOAAccount(const GDBusCXX::DBusConnectionPtr &conn,
               const std::string &path);

    GDBusCXX::DBusClientCall1<int32_t> m_ensureCredentials;
    GDBusCXX::DBusClientCall1<std::string> m_getAccessToken;
};

GOAManager::GOAManager(const GDBusCXX::DBusConnectionPtr &conn) :
    GDBusCXX::DBusRemoteObject(conn, GOA_PATH, OBJECT_MANAGER_INTERFACE, GOA_BUS_NAME),
    m_getManagedObjects(*this, OBJECT_MANAGER_GET_MANAGED_OBJECTS)
{
}

boost::shared_ptr<GOAAccount> GOAManager::lookupAccount(const std::string &username)
{
    SE_LOG_DEBUG(NULL, "Looking up all accounts in GNOME Online Accounts, searching for '%s'.", username.c_str());
    ManagedObjects objects = m_getManagedObjects();

    GDBusCXX::DBusObject_t accountPath;
    bool unique = true;
    bool hasOAuth2 = false;
    std::vector<std::string> accounts;
    BOOST_FOREACH (const ManagedObjects::value_type &object, objects) {
        const GDBusCXX::DBusObject_t &path = object.first;
        const Interfaces &interfaces = object.second;
        // boost::adaptors::keys() would be nicer, but is not available on Ubuntu Lucid.
        std::list<std::string> interfaceKeys;
        BOOST_FOREACH (const Interfaces::value_type &entry, interfaces) {
            interfaceKeys.push_back(entry.first);
        }
        SE_LOG_DEBUG(NULL, "GOA object %s implements %s", path.c_str(),
                     boost::join(interfaceKeys, ", ").c_str());
        Interfaces::const_iterator it = interfaces.find(GOA_ACCOUNT_INTERFACE);
        if (it != interfaces.end()) {
            const Properties &properties = it->second;
            Properties::const_iterator id = properties.find(GOA_ACCOUNT_ID);
            Properties::const_iterator presentationID = properties.find(GOA_ACCOUNT_PRESENTATION_IDENTITY);
            if (id != properties.end() &&
                presentationID != properties.end()) {
                const std::string &idStr = boost::get<std::string>(id->second);
                const std::string &presentationIDStr = boost::get<std::string>(presentationID->second);
                Properties::const_iterator provider = properties.find(GOA_ACCOUNT_PROVIDER_NAME);
                std::string description = StringPrintf("%s, %s = %s",
                                                       provider == properties.end() ? "???" : boost::get<std::string>(provider->second).c_str(),
                                                       presentationIDStr.c_str(),
                                                       idStr.c_str());
                SE_LOG_DEBUG(NULL, "GOA account %s", description.c_str());
                accounts.push_back(description);
                // The assumption here is that ID and presentation
                // identifier are so different that there can be
                // no overlap. Otherwise we would have to know
                // whether the user gave us an ID or presentation
                // identifier.
                if (idStr == username ||
                    presentationIDStr == username) {
                    if (accountPath.empty()) {
                        accountPath = path;
                        hasOAuth2 = interfaces.find(GOA_OAUTH2_INTERFACE) != interfaces.end();
                        SE_LOG_DEBUG(NULL, "found matching GNOME Online Account for '%s': %s", username.c_str(), description.c_str());
                    } else {
                        unique = false;
                    }
                }
            } else {
                SE_LOG_DEBUG(NULL, "ignoring %s, lacks expected properties",
                             path.c_str());
            }
        }
    }

    std::sort(accounts.begin(), accounts.end());
    if (accountPath.empty()) {
        if (accounts.empty()) {
            SE_THROW(StringPrintf("GNOME Online Account '%s' not found. You must set up the account in GNOME Control Center/Online Accounts first.", username.c_str()));
        } else {
            SE_THROW(StringPrintf("GNOME Online Account '%s' not found. Choose one of the following:\n%s",
                                  username.c_str(),
                                  boost::join(accounts, "\n").c_str()));
        }
    } else if (!unique) {
        SE_THROW(StringPrintf("GNOME Online Account '%s' is not unique. Choose one of the following, using the unique ID instead of the more ambiguous representation name:\n%s",
                              username.c_str(),
                              boost::join(accounts, "\n").c_str()));
    } else if (!hasOAuth2) {
        SE_THROW(StringPrintf("Found GNOME Online Account '%s', but it does not support OAuth2. Are you sure that you picked the right account and that you are using GNOME Online Accounts >= 3.8?",
                              username.c_str()));
    }

    boost::shared_ptr<GOAAccount> account(new GOAAccount(getConnection(), accountPath));
    return account;
}

GOAAccount::GOAAccount(const GDBusCXX::DBusConnectionPtr &conn,
                       const std::string &path) :
    m_account(conn, path, GOA_ACCOUNT_INTERFACE, GOA_BUS_NAME),
    m_oauth2(conn, path, GOA_OAUTH2_INTERFACE, GOA_BUS_NAME),
    m_ensureCredentials(m_account, GOA_ACCOUNT_ENSURE_CREDENTIALS),
    m_getAccessToken(m_oauth2, GOA_OAUTH2_GET_ACCESS_TOKEN)
{
}

class GOAAuthProvider : public AuthProvider
{
    boost::shared_ptr<GOAAccount> m_account;

public:
    GOAAuthProvider(const boost::shared_ptr<GOAAccount> &account) :
        m_account(account)
    {}

    virtual bool methodIsSupported(AuthMethod method) const { return method == AUTH_METHOD_OAUTH2; }

    virtual Credentials getCredentials() const { SE_THROW("only OAuth2 is supported"); }

    virtual std::string getOAuth2Bearer(int failedTokens,
                                        const PasswordUpdateCallback &passwordUpdateCallback) const
    {
        m_account->m_ensureCredentials();
        std::string token = m_account->m_getAccessToken();
        return token;
    }

    virtual std::string getUsername() const { return ""; }
};

boost::shared_ptr<AuthProvider> createGOAAuthProvider(const InitStateString &username,
                                                      const InitStateString &password)
{
    // Because we share the connection, hopefully this won't be too expensive.
    GDBusCXX::DBusErrorCXX err;
    GDBusCXX::DBusConnectionPtr conn = dbus_get_bus_connection("SESSION",
                                                               NULL,
                                                               false,
                                                               &err);
    if (!conn) {
        err.throwFailure("connecting to session bus");
    }

    GOAManager manager(conn);
    boost::shared_ptr<GOAAccount> account = manager.lookupAccount(username);
    boost::shared_ptr<AuthProvider> provider(new GOAAuthProvider(account));
    return provider;
}

SE_END_CXX

#endif // USE_GOA


