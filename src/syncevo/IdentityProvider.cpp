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

#include <syncevo/IdentityProvider.h>
#include <syncevo/SyncConfig.h>
#include <syncevo/Exception.h>

#include <algorithm>

SE_BEGIN_CXX

const char USER_IDENTITY_PLAIN_TEXT[] = "user";
const char USER_IDENTITY_SYNC_CONFIG[] = "id";

Credentials IdentityProviderCredentials(const UserIdentity &identity,
                                        const InitStateString &password)
{
    Credentials cred;

    if (identity.m_provider == USER_IDENTITY_PLAIN_TEXT) {
        cred.m_username = identity.m_identity;
        cred.m_password = password;
    } else {
        // We could use the gSSO password plugin to request
        // username/password. But it is uncertain whether that is useful,
        // therefore that is not implemented at the moment.
        SE_THROW(StringPrintf("%s: need username+password as credentials", identity.toString().c_str()));
    }

    return cred;
}

class CredentialsProvider : public AuthProvider
{
    Credentials m_creds;

public:
    CredentialsProvider(const std::string &username,
                        const std::string &password)
    {
        m_creds.m_username = username;
        m_creds.m_password = password;
    }

    virtual bool wasConfigured() const { return !m_creds.m_username.empty() || !m_creds.m_password.empty(); }
    virtual bool methodIsSupported(AuthMethod method) const { return method == AUTH_METHOD_CREDENTIALS; }
    virtual Credentials getCredentials() const { return m_creds; }
    virtual std::string getOAuth2Bearer(int failedTokens, const PasswordUpdateCallback &passwordUpdateCallback) const { SE_THROW("OAuth2 not supported"); return ""; }
    virtual std::string getUsername() const { return m_creds.m_username; }
};

boost::shared_ptr<AuthProvider> AuthProvider::create(const UserIdentity &identity,
                                                     const InitStateString &password)
{
    boost::shared_ptr<AuthProvider> authProvider;

    if (identity.m_provider == USER_IDENTITY_PLAIN_TEXT) {
        SE_LOG_DEBUG(NULL, "using plain username/password for %s", identity.toString().c_str());
        authProvider.reset(new CredentialsProvider(identity.m_identity, password));
    } else {
        SE_LOG_DEBUG(NULL, "looking for identity provider for %s", identity.toString().c_str());
        BOOST_FOREACH (IdentityProvider *idProvider, IdentityProvider::getRegistry()) {
            if (boost::iequals(idProvider->m_key, identity.m_provider)) {
                authProvider = idProvider->create(identity.m_identity, password);
                if (!authProvider) {
                    SE_THROW(StringPrintf("identity provider for '%s' is disabled in this installation",
                                          identity.m_provider.c_str()));
                }
                break;
            }
        }

        if (!authProvider) {
            SE_THROW(StringPrintf("unknown identity provider '%s' in '%s'",
                                  identity.m_provider.c_str(),
                                  identity.toString().c_str()));
        }
    }

    return authProvider;
}

std::list<IdentityProvider *> &IdentityProvider::getRegistry()
{
    static std::list<IdentityProvider *> providers;
    return providers;
}

IdentityProvider::IdentityProvider(const std::string &key,
                                   const std::string &descr) :
    m_key(key),
    m_descr(descr)
{
    getRegistry().push_back(this);
}

IdentityProvider::~IdentityProvider()
{
    getRegistry().erase(std::find(getRegistry().begin(),
                                  getRegistry().end(),
                                  this));
}

SE_END_CXX
