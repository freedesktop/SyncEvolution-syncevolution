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
#ifndef INCL_SYNC_EVOLUTION_IDENTITY_PROVIDER
# define INCL_SYNC_EVOLUTION_IDENTITY_PROVIDER

#include <syncevo/util.h>

#include <string>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

extern const char USER_IDENTITY_PLAIN_TEXT[];
extern const char USER_IDENTITY_SYNC_CONFIG[];

struct UserIdentity; // from SyncConfig.h

struct Credentials
{
    std::string m_username;
    std::string m_password;
};

/**
 * Returns username/password for an identity. The password is the
 * string configured for it inside SyncEvolution. It may be empty and/or unset if
 * the plain text password comes from the identity provider.
 *
 * If the credentials cannot be retrieved, an error is thrown, so don't use this
 * in cases where a different authentication method might also work.
 */
Credentials IdentityProviderCredentials(const UserIdentity &identity,
                                        const InitStateString &password);

/**
 * Supports multiple different ways of authorizing the user.
 * Actual implementations are IdentityProvider specific.
 */
class AuthProvider
{
 public:
    /**
     * Creates an AuthProvider matching the identity.m_provider value
     * or throws an exception if that fails. Never returns NULL.
     */
    static boost::shared_ptr<AuthProvider> create(const UserIdentity &identity,
                                                  const InitStateString &password);

    enum AuthMethod {
        AUTH_METHOD_NONE,
        AUTH_METHOD_CREDENTIALS,
        AUTH_METHOD_OAUTH2,
        AUTH_METHOD_MAX
    };

    /**
     * Return true if some kind of credentials were configured by the user.
     * They don't have to be usable.
     */
    virtual bool wasConfigured() const { return true; }

    /**
     * Returns true if the given method is supported and currently possible.
     */
    virtual bool methodIsSupported(AuthMethod method) const = 0;

    /**
     * Returns username/password credentials. Throws an error if not supported.
     */
    virtual Credentials getCredentials() const = 0;

    /**
     * Returns the 'Bearer b64token' string required for logging into
     * services supporting OAuth2 or throws an exception when we don't
     * have a valid token. Internally this will refresh tokens
     * automatically.
     *
     * See http://tools.ietf.org/html/draft-ietf-oauth-v2-bearer-20#section-2.1
     *
     * An application should:
     * - request a token and try to use it
     * - in case of failure try to request a token again, and try to use it
     *   again (in case the first token has expired just before using it)
     * - if the second token also fails, request a third token with full
     *   re-authentication as above.
     * - if that fails, then give up.
     *
     * To achieve that, the caller must count how often he got a token that
     * did not work.
     *
     * @param failedTokens           zero when asking for initial token, one for refresh, two for full re-authorization
     * @param passwordUpdateCallback callback function to be called when stored refresh token need to be updated.
     *                               Only parameter of this callback function is new value of refresh token.
     *
     * @return a base64 encoded token, ready to be used in "Authorization: Bearer %s"
     */
    typedef boost::function<void (const std::string &newPassword)> PasswordUpdateCallback;
    virtual std::string getOAuth2Bearer(int failedTokens,
                                        const PasswordUpdateCallback &passwordUpdateCallback) const = 0;

    /**
     * Returns username at the remote service. Works for
     * username/password credentials and may be made to work for
     * OAuth2. At the moment, code should not depend on it when using
     * OAuth2.
     */
    virtual std::string getUsername() const = 0;
};

/**
 * Instantiating this class adds a new provider.
 * Deleting it removes it.
 */
class IdentityProvider
{
 public:
    /** returns NULL if disabled, valid AuthProvider if possible, and throws error if something goes wrong */
    virtual boost::shared_ptr<AuthProvider> create(const InitStateString &username,
                                                   const InitStateString &password) = 0;

    /**
     * All known providers.
     */
    static std::list<IdentityProvider *> &getRegistry();

    /**
     * @param key            short, unique word without colons used to select this provider
     *                       in an identity string, for example "gsso"
     * @param descr          one or more lines describing the provider and its syntax,
     *                       for example
     *                       "gsso:<account ID>\n"
     *                       "   authentication using libgsignond + libaccounts\n"
     */
    IdentityProvider(const std::string &key,
                     const std::string &descr);
    virtual ~IdentityProvider();

    const std::string m_key;
    const std::string m_descr;
};

SE_END_CXX
#endif
