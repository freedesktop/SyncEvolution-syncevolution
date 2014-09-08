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

#include <syncevo/IdentityProvider.h>

#ifdef USE_SIGNON
#ifdef USE_GSSO
#include "libgsignon-glib/signon-auth-service.h"
#include "libgsignon-glib/signon-identity.h"
#elif defined USE_UOA
#include "libsignon-glib/signon-auth-service.h"
#include "libsignon-glib/signon-identity.h"
#endif // USE_GSSO
#ifdef USE_ACCOUNTS
#include "libaccounts-glib/ag-account.h"
#include "libaccounts-glib/ag-account-service.h"
#include <libaccounts-glib/ag-auth-data.h>
#include <libaccounts-glib/ag-service.h>
#include <libaccounts-glib/ag-manager.h>
#endif

#include <syncevo/GLibSupport.h>
#include <syncevo/GVariantSupport.h>
#include <pcrecpp.h>

#include <boost/lambda/core.hpp>

SE_GOBJECT_TYPE(SignonAuthService)
SE_GOBJECT_TYPE(SignonAuthSession)
SE_GOBJECT_TYPE(SignonIdentity)

#ifdef USE_ACCOUNTS
SE_GOBJECT_TYPE(AgAccount)
SE_GOBJECT_TYPE(AgAccountService)
SE_GOBJECT_TYPE(AgManager)
SE_GLIB_TYPE(AgService, ag_service)
SE_GLIB_TYPE(AgAuthData, ag_auth_data)
#endif

#endif // USE_SIGNON

#include <syncevo/declarations.h>
SE_BEGIN_CXX

#ifdef USE_SIGNON

#ifdef USE_ACCOUNTS
typedef GListCXX<AgService, GList, ag_service_unref> ServiceListCXX;
#endif

class SignonAuthProvider : public AuthProvider
{
    SignonAuthSessionCXX m_authSession;
    GHashTableCXX m_sessionData;
    std::string m_mechanism;

public:
    SignonAuthProvider(const SignonAuthSessionCXX &authSession,
                       const GHashTableCXX &sessionData,
                       const std::string &mechanism) :
        m_authSession(authSession),
        m_sessionData(sessionData),
        m_mechanism(mechanism)
    {}

    virtual bool methodIsSupported(AuthMethod method) const { return method == AUTH_METHOD_OAUTH2; }

    virtual Credentials getCredentials() const { SE_THROW("only OAuth2 is supported"); }

    virtual std::string getOAuth2Bearer(int failedTokens,
                                        const PasswordUpdateCallback &passwordUpdateCallback) const
    {
        SE_LOG_DEBUG(NULL, "retrieving OAuth2 token, attempt %d", failedTokens);

        // Retry login if even the refreshed token failed.
        g_hash_table_insert(m_sessionData, g_strdup("UiPolicy"),
                            g_variant_ref_sink(g_variant_new_uint32(failedTokens >= 2 ? SIGNON_POLICY_REQUEST_PASSWORD : 0)));
        // We get assigned a plain pointer to an instance that we'll own,
        // so we have to use the "steal" variant to enable that assignment.
        GVariantStealCXX resultDataVar;
        GErrorCXX gerror;
        // Enforce normal reference counting via _ref_sink.
        GVariantCXX sessionDataVar(g_variant_ref_sink(HashTable2Variant(m_sessionData)),
                                   TRANSFER_REF);
        PlainGStr buffer(g_variant_print(sessionDataVar, true));
        SE_LOG_DEBUG(NULL, "asking for OAuth2 token with method %s, mechanism %s and parameters %s",
                     signon_auth_session_get_method(m_authSession),
                     m_mechanism.c_str(),
                     buffer.get());

#define signon_auth_session_process_async_finish signon_auth_session_process_finish
        SYNCEVO_GLIB_CALL_SYNC(resultDataVar, gerror, signon_auth_session_process_async,
                               m_authSession, sessionDataVar, m_mechanism.c_str(), NULL);
        buffer.reset(resultDataVar ? g_variant_print(resultDataVar, true) : NULL);
        SE_LOG_DEBUG(NULL, "OAuth2 token result: %s, %s",
                     buffer.get() ? buffer.get() : "<<null>>",
                     gerror ? gerror->message : "???");
        if (!resultDataVar || gerror) {
            SE_THROW_EXCEPTION_STATUS(StatusException,
                                      StringPrintf("could not obtain OAuth2 token: %s", gerror ? gerror->message : "???"),
                                      STATUS_FORBIDDEN);
        }
        GHashTableCXX resultData(Variant2HashTable(resultDataVar));
        GVariant *tokenVar = static_cast<GVariant *>(g_hash_table_lookup(resultData, (gpointer)"AccessToken"));
        if (!tokenVar) {
            SE_THROW("no AccessToken in OAuth2 response");
        }
        const char *token = g_variant_get_string(tokenVar, NULL);
        if (!token) {
            SE_THROW("AccessToken did not contain a string value");
        }
        return token;
    }

    virtual std::string getUsername() const { return ""; }
};

#ifdef USE_ACCOUNTS
class StoreIdentityData
{
public:
    StoreIdentityData() : m_running(true) {}

    bool m_running;
    guint32 m_id;
    GErrorCXX m_gerror;
};

static void StoreIdentityCB(SignonIdentity *self,
                            guint32 id,
                            const GError *error,
                            gpointer userData)
{
    StoreIdentityData *data = reinterpret_cast<StoreIdentityData *>(userData);
    data->m_running = false;
    data->m_id = id;
    data->m_gerror = error;
}

boost::shared_ptr<AuthProvider> createSignonAuthProvider(const InitStateString &username,
                                                         const InitStateString &password)
{
    boost::shared_ptr<AuthProvider> provider;

    // Split username into <account ID> and <service name>.
    // Be flexible and allow leading/trailing white space.
    // Comma is optional.
    static const pcrecpp::RE re("^\\s*(\\d+)\\s*,?\\s*(.*)\\s*$");
    AgAccountId accountID;
    std::string serviceName;
    if (!re.FullMatch(username, &accountID, &serviceName)) {
        SE_THROW(StringPrintf("username must have the format gsso:<account ID>,<service name>: %s",
                              username.c_str()));
    }
    SE_LOG_DEBUG(NULL, "looking up account ID %d and service '%s'",
                 accountID,
                 serviceName.c_str());
    AgManagerCXX manager(ag_manager_new(), TRANSFER_REF);
    GErrorCXX gerror;
    AgAccountCXX account(ag_manager_load_account(manager, accountID, gerror), TRANSFER_REF);
    if (!account) {
        gerror.throwError(SE_HERE, StringPrintf("loading account with ID %d from %s failed",
                                       accountID,
                                       username.c_str()));
    }
    if (!ag_account_get_enabled(account)) {
        SE_THROW(StringPrintf("account with ID %d from %s is disabled, refusing to use it",
                              accountID,
                              username.c_str()));
    }
    AgAccountServiceCXX accountService;
    if (serviceName.empty()) {
        accountService = AgAccountServiceCXX::steal(ag_account_service_new(account, NULL));
    } else {
        ServiceListCXX services(ag_account_list_enabled_services(account));
        BOOST_FOREACH (AgService *service, services) {
            const char *name = ag_service_get_name(service);
            SE_LOG_DEBUG(NULL, "enabled service: %s", name);
            if (serviceName == name) {
                accountService = AgAccountServiceCXX::steal(ag_account_service_new(account, service));
                // Do *not* select the service for reading/writing properties.
                // AgAccountService does this internally, and when we create
                // a new identity below, we want it to be shared by all
                // services so that the user only needs to log in once.
                // ag_account_select_service(account, service);
                break;
            }
        }
    }
    if (!accountService) {
        SE_THROW(StringPrintf("service '%s' in account with ID %d not found or not enabled",
                              serviceName.c_str(),
                              accountID));
    }
    AgAuthDataCXX authData(ag_account_service_get_auth_data(accountService), TRANSFER_REF);

    // SignonAuthServiceCXX authService(signon_auth_service_new(), TRANSFER_REF);
    guint signonID = ag_auth_data_get_credentials_id(authData);
    const char *method = ag_auth_data_get_method(authData);
    const char *mechanism = ag_auth_data_get_mechanism(authData);

    GVariantCXX sessionDataVar(ag_auth_data_get_login_parameters(authData, NULL), TRANSFER_REF);
    GHashTableCXX sessionData(Variant2HashTable(sessionDataVar));
#ifdef USE_GSSO
    GVariant *realmsVariant = (GVariant *)g_hash_table_lookup(sessionData, "Realms");
    PlainGStrArray realms(g_variant_dup_strv(realmsVariant, NULL));
#endif

    // Check that the service has a credentials ID. If not, create it and
    // store its ID permanently.
    if (!signonID) {
        SE_LOG_DEBUG(NULL, "have to create signon identity");
        SignonIdentityCXX identity(signon_identity_new(), TRANSFER_REF);
        boost::shared_ptr<SignonIdentityInfo> identityInfo(signon_identity_info_new(), signon_identity_info_free);
        signon_identity_info_set_caption(identityInfo.get(),
                                         StringPrintf("created by SyncEvolution for account #%d and service %s",
                                                      accountID,
                                                      serviceName.empty() ? "<<none>>" : serviceName.c_str()).c_str());
        const gchar *mechanisms[] = { mechanism ? mechanism : "*", NULL };
        signon_identity_info_set_method(identityInfo.get(), method, mechanisms);
#ifdef USE_GSSO
        if (realms) {
            signon_identity_info_set_realms(identityInfo.get(), realms);
        }
#endif
        StoreIdentityData data;
        signon_identity_store_credentials_with_info(identity, identityInfo.get(),
                                                    StoreIdentityCB, &data);
        GRunWhile(boost::lambda::var(data.m_running));
        if (!data.m_id || data.m_gerror) {
            SE_THROW(StringPrintf("failed to create signon identity: %s",
                                  data.m_gerror ? data.m_gerror->message : "???"));
        }

        // Now store in account.
        static const char CREDENTIALS_ID[] = "CredentialsId";
        ag_account_set_variant(account, CREDENTIALS_ID, g_variant_new_uint32(data.m_id));
#define ag_account_store_async_finish ag_account_store_finish
        gboolean res;
        SYNCEVO_GLIB_CALL_SYNC(res, gerror, ag_account_store_async,
                               account, NULL);
        if (!res) {
            gerror.throwError(SE_HERE, "failed to store account");
        }

        authData = AgAuthDataCXX::steal(ag_account_service_get_auth_data(accountService));
        signonID = ag_auth_data_get_credentials_id(authData);
        if (!signonID) {
            SE_THROW("still no signonID?!");
        }
        method = ag_auth_data_get_method(authData);
        mechanism = ag_auth_data_get_mechanism(authData);
    }

    SignonIdentityCXX identity(signon_identity_new_from_db(signonID), TRANSFER_REF);
    SE_LOG_DEBUG(NULL, "using signond identity %d", signonID);
    SignonAuthSessionCXX authSession(signon_identity_create_session(identity, method, gerror), TRANSFER_REF);

    // TODO (?): retrieve start URL from account system

    provider.reset(new SignonAuthProvider(authSession, sessionData, mechanism));

    return provider;
}

#else // USE_ACCOUNTS

boost::shared_ptr<AuthProvider> createSignonAuthProvider(const InitStateString &username,
                                                         const InitStateString &password)
{
    // Expected content of parameter GVariant.
    boost::shared_ptr<GVariantType> hashtype(g_variant_type_new("a{sv}"), g_variant_type_free);

    // 'username' is the part after signon: which we can parse directly.
    GErrorCXX gerror;
    GVariantCXX parametersVar(g_variant_parse(hashtype.get(), username.c_str(), NULL, NULL, gerror),
                              TRANSFER_REF);
    if (!parametersVar) {
        gerror.throwError(SE_HERE, "parsing 'signon:' username");
    }
    GHashTableCXX parameters(Variant2HashTable(parametersVar));

    // Extract the values that we expect in the parameters hash.
    guint32 signonID;
    const char *method;
    const char *mechanism;

    GVariant *value;
    value = (GVariant *)g_hash_table_lookup(parameters, "identity");
    if (!value ||
        !g_variant_type_equal(G_VARIANT_TYPE_UINT32, g_variant_get_type(value))) {
        SE_THROW("need 'identity: <numeric ID>' in 'signon:' parameters");
    }
    signonID = g_variant_get_uint32(value);

    value = (GVariant *)g_hash_table_lookup(parameters, "method");
    if (!value ||
        !g_variant_type_equal(G_VARIANT_TYPE_STRING, g_variant_get_type(value))) {
        SE_THROW("need 'method: <string>' in 'signon:' parameters");
    }
    method = g_variant_get_string(value, NULL);

    value = (GVariant *)g_hash_table_lookup(parameters, "mechanism");
    if (!value ||
        !g_variant_type_equal(G_VARIANT_TYPE_STRING, g_variant_get_type(value))) {
        SE_THROW("need 'mechanism: <string>' in 'signon:' parameters");
    }
    mechanism = g_variant_get_string(value, NULL);

    value = (GVariant *)g_hash_table_lookup(parameters, "session");
    if (!value ||
        !g_variant_type_equal(hashtype.get(), g_variant_get_type(value))) {
        SE_THROW("need 'session: <hash>' in 'signon:' parameters");
    }
    GHashTableCXX sessionData(Variant2HashTable(value));

    SE_LOG_DEBUG(NULL, "using identity %u, method %s, mechanism %s",
                 signonID, method, mechanism);
    SignonIdentityCXX identity(signon_identity_new_from_db(signonID), TRANSFER_REF);
    SE_LOG_DEBUG(NULL, "using signond identity %d", signonID);
    SignonAuthSessionCXX authSession(signon_identity_create_session(identity, method, gerror), TRANSFER_REF);

    boost::shared_ptr<AuthProvider> provider(new SignonAuthProvider(authSession, sessionData, mechanism));
    return provider;
}

#endif // USE_ACCOUNTS


#endif // USE_SIGNON

SE_END_CXX

