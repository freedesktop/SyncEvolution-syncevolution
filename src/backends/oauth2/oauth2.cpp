/*
 * Copyright (C) 2014 Intel Corporation
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

#include <syncevo/IdentityProvider.h>
#include <syncevo/GLibSupport.h>
#include <syncevo/GVariantSupport.h>
#include <syncevo/SoupTransportAgent.h>
#include <json.h>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

class RefreshTokenAuthProvider : public AuthProvider
{
    boost::shared_ptr<HTTPTransportAgent> m_agent;
    std::string m_tokenHost;
    std::string m_tokenPath;
    std::string m_scope;
    std::string m_clientID;
    std::string m_clientSecret;
    std::string m_refreshToken;
    mutable std::string m_accessToken;

public:
    RefreshTokenAuthProvider(const char* tokenHost,
                             const char* tokenPath,
                             const char* scope,
                             const char* clientID,
                             const char* clientSecret,
                             const char* refreshToken) :
        m_tokenHost(tokenHost),
        m_tokenPath(tokenPath),
        m_scope(scope),
        m_clientID(clientID),
        m_clientSecret(clientSecret),
        m_refreshToken(refreshToken)
    {
#ifdef ENABLE_LIBSOUP
        boost::shared_ptr<SoupTransportAgent> agent(new SoupTransportAgent(static_cast<GMainLoop *>(NULL)));
        m_agent = agent;
#elif defined(ENABLE_LIBCURL)
        m_agent = new CurlTransportAgent();
#endif
    }

    virtual bool methodIsSupported(AuthMethod method) const { return method == AUTH_METHOD_OAUTH2; }

    virtual Credentials getCredentials() const { SE_THROW("only OAuth2 is supported"); }

    virtual std::string getOAuth2Bearer(int failedTokens,
                                        const PasswordUpdateCallback &passwordUpdateCallback) const
    {
        SE_LOG_DEBUG(NULL, "retrieving OAuth2 token, attempt %d", failedTokens);
        //in case of retry do not use cached access token, request it again
        if (1 >= failedTokens) {
            m_accessToken.clear();
        }

        if (m_accessToken.empty()) {
            const char *reply;
            size_t replyLen;
            std::string contentType;

            m_agent->setURL(m_tokenHost + m_tokenPath);
            m_agent->setContentType("application/x-www-form-urlencoded");

            std::ostringstream oss;
            oss<<"grant_type=refresh_token&client_id="<<m_clientID;
            oss<<"&client_secret="<<m_clientSecret<<"&scope="<<m_scope;
            oss<<"&refresh_token="<<m_refreshToken;

            std::string requestBody = oss.str();
            m_agent->send(requestBody.c_str(), requestBody.length());

            switch (m_agent->wait()) {
            case TransportAgent::ACTIVE:
                SE_LOG_DEBUG(NULL, "retrieving OAuth2 token - agent active");
                break;
            case TransportAgent::GOT_REPLY:
            {
                SE_LOG_DEBUG(NULL, "retrieving OAuth2 token - agent got reply");
                m_agent->getReply(reply, replyLen, contentType);

                json_object *jobj = json_tokener_parse(reply);
                if (jobj) {
                    json_object_object_foreach(jobj, key, val) {
                        if (strcmp("access_token", key) == 0) {
                            m_accessToken = json_object_get_string(val);
                        }
                        if (strcmp("refresh_token", key) == 0) {
                            std::string newRefreshToken = json_object_get_string(val);
                            SE_LOG_INFO(NULL, "refresh token invalidated - updating refresh token to %s", newRefreshToken.c_str());
                            if (passwordUpdateCallback) {
                                passwordUpdateCallback(newRefreshToken);
                            }
                        }
                    }
                    json_object_put(jobj);
                }
                else {
                    SE_THROW("OAuth2 misformatted response");
                }
            }
                break;
            case TransportAgent::TIME_OUT:
                SE_LOG_DEBUG(NULL, "retrieving OAuth2 token - agent time out");
                SE_THROW("OAuth2 request timed out");
                break;
            case TransportAgent::INACTIVE:
            case TransportAgent::CLOSED:
            case TransportAgent::FAILED: {
                std::string errorString;
                m_agent->getReply(reply, replyLen, contentType);

                json_object *jobj = json_tokener_parse(reply);
                if (jobj) {
                    json_object_object_foreach(jobj, key, val) {
                        if (strcmp ("error", key) == 0) {
                            errorString = json_object_get_string(val);
                        }
                    }
                    json_object_put(jobj);
                }

                SE_THROW("OAuth2 request failed with error: " + errorString);
                break;
            }
            case TransportAgent::CANCELED:
                SE_LOG_DEBUG(NULL, "retrieving OAuth2 token - agent cancelled");
                SE_THROW("OAuth2 request cancelled");
                break;
            }
        }
        return m_accessToken;
    }
    virtual std::string getUsername() const { return ""; }
};

boost::shared_ptr<AuthProvider> createOAuth2AuthProvider(const InitStateString &username,
                                                         const InitStateString &password)
{
    // Expected content of parameter GVariant.
    boost::shared_ptr<GVariantType> hashtype(g_variant_type_new("a{sv}"), g_variant_type_free);

    // 'username' is the part after refresh_token: which we can parse directly.
    GErrorCXX gerror;
    GVariantStealCXX parametersVar(g_variant_parse(hashtype.get(), username.c_str(), NULL, NULL, gerror));
    if (!parametersVar) {
        gerror.throwError(SE_HERE, "parsing 'refresh_token:' username");
    }
    GHashTableCXX parameters(Variant2HashTable(parametersVar));

    // Extract the values that we expect in the parameters hash.
    const char *tokenHost;
    const char *tokenPath;
    const char *scope;
    const char *clientID;
    const char *clientSecret;

    GVariant *value;

    value = (GVariant *)g_hash_table_lookup(parameters, "TokenHost");
    if (!value ||
        !g_variant_type_equal(G_VARIANT_TYPE_STRING, g_variant_get_type(value))) {
        SE_THROW("need 'TokenHost: <string>' in 'refresh_token:' parameters");
    }
    tokenHost = g_variant_get_string(value, NULL);

    value = (GVariant *)g_hash_table_lookup(parameters, "TokenPath");
    if (!value ||
        !g_variant_type_equal(G_VARIANT_TYPE_STRING, g_variant_get_type(value))) {
        SE_THROW("need 'TokenPath: <string>' in 'refresh_token:' parameters");
    }
    tokenPath = g_variant_get_string(value, NULL);

    value = (GVariant *)g_hash_table_lookup(parameters, "Scope");
    if (!value ||
        !g_variant_type_equal(G_VARIANT_TYPE_STRING, g_variant_get_type(value))) {
        SE_THROW("need 'Scope: <string>' in 'refresh_token:' parameters");
    }
    scope = g_variant_get_string(value, NULL);

    value = (GVariant *)g_hash_table_lookup(parameters, "ClientID");
    if (!value ||
        !g_variant_type_equal(G_VARIANT_TYPE_STRING, g_variant_get_type(value))) {
        SE_THROW("need 'ClientID: <string>' in 'refresh_token:' parameters");
    }
    clientID = g_variant_get_string(value, NULL);

    value = (GVariant *)g_hash_table_lookup(parameters, "ClientSecret");
    if (!value ||
        !g_variant_type_equal(G_VARIANT_TYPE_STRING, g_variant_get_type(value))) {
        SE_THROW("need 'ClientSecret: <string>' in 'refresh_token:' parameters");
    }
    clientSecret = g_variant_get_string(value, NULL);
    
    if (password.empty()) {
        SE_THROW("need refresh token provided as password");
    }
    boost::shared_ptr<AuthProvider> provider(new RefreshTokenAuthProvider(tokenHost, tokenPath, scope, clientID, clientSecret, password.c_str()));
    return provider;
}

SE_END_CXX
