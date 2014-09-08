/*
 * Copyright (C) 2010 Patrick Ohly <patrick.ohly@gmx.de>
 */

#include <config.h>

#ifdef ENABLE_DAV

#include "NeonCXX.h"
#include <ne_socket.h>
#include <ne_auth.h>
#include <ne_xmlreq.h>
#include <ne_string.h>

#include <list>
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/bind.hpp>

#include <syncevo/util.h>
#include <syncevo/Logging.h>
#include <syncevo/LogRedirect.h>
#include <syncevo/SmartPtr.h>
#include <syncevo/SuspendFlags.h>
#include <syncevo/IdentityProvider.h>

#include <sstream>

#include <dlfcn.h>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

namespace Neon {
#if 0
}
#endif

std::string features()
{
    std::list<std::string> res;

    if (ne_has_support(NE_FEATURE_SSL)) { res.push_back("SSL"); }
    if (ne_has_support(NE_FEATURE_ZLIB)) { res.push_back("ZLIB"); }
    if (ne_has_support(NE_FEATURE_IPV6)) { res.push_back("IPV6"); }
    if (ne_has_support(NE_FEATURE_LFS)) { res.push_back("LFS"); }
    if (ne_has_support(NE_FEATURE_SOCKS)) { res.push_back("SOCKS"); }
    if (ne_has_support(NE_FEATURE_TS_SSL)) { res.push_back("TS_SSL"); }
    if (ne_has_support(NE_FEATURE_I18N)) { res.push_back("I18N"); }
    return boost::join(res, ", ");
}

URI URI::parse(const std::string &url, bool collection)
{
    ne_uri uri;
    int error = ne_uri_parse(url.c_str(), &uri);
    URI res = fromNeon(uri, collection);
    if (!res.m_port) {
        res.m_port = ne_uri_defaultport(res.m_scheme.c_str());
    }
    ne_uri_free(&uri);
    if (error) {
        SE_THROW_EXCEPTION(TransportException,
                           StringPrintf("invalid URL '%s' (parsed as '%s')",
                                        url.c_str(),
                                        res.toURL().c_str()));
    }
    return res;
}

URI URI::fromNeon(const ne_uri &uri, bool collection)
{
    URI res;

    if (uri.scheme) { res.m_scheme = uri.scheme; }
    if (uri.host) { res.m_host = uri.host; }
    if (uri.userinfo) { res.m_userinfo = uri.userinfo; }
    if (uri.path) { res.m_path = normalizePath(uri.path, collection); }
    if (uri.query) { res.m_query = uri.query; }
    if (uri.fragment) { res.m_fragment = uri.fragment; }
    res.m_port = uri.port;

    return res;
}

URI URI::resolve(const std::string &path) const
{
    ne_uri tmp[2];
    ne_uri full;
    memset(tmp, 0, sizeof(tmp));
    tmp[0].path = const_cast<char *>(m_path.c_str());
    tmp[1].path = const_cast<char *>(path.c_str());
    ne_uri_resolve(tmp + 0, tmp + 1, &full);
    URI res(*this);
    res.m_path = full.path;
    ne_uri_free(&full);
    return res;
}

std::string URI::toURL() const
{
    std::ostringstream buffer;

    buffer << m_scheme << "://";
    if (!m_userinfo.empty()) {
        buffer << m_userinfo << "@";
    }
    buffer << m_host;
    if (m_port) {
        buffer << ":" << m_port;
    }
    buffer << m_path;
    if (!m_query.empty()) {
        buffer << "?" << m_query;
    }
    if (!m_fragment.empty()) {
        buffer << "#" << m_fragment;
    }
    return buffer.str();
}

std::string URI::escape(const std::string &text)
{
    SmartPtr<char *> tmp(ne_path_escape(text.c_str()));
    // Fail gracefully. I have observed ne_path_escape returning NULL
    // a couple of times, with input "%u". It makes sense, if the
    // escaping fails, to just return the same string, because, well,
    // it couldn't be escaped.
    return tmp ? tmp.get() : text;
}

std::string URI::unescape(const std::string &text)
{
    SmartPtr<char *> tmp(ne_path_unescape(text.c_str()));
    // Fail gracefully. See also the similar comment for the escape() method.
    return tmp ? tmp.get() : text;
}

std::string URI::normalizePath(const std::string &path, bool collection)
{
    std::string res;
    res.reserve(path.size() * 150 / 100);

    // always start with one leading slash
    res = "/";

    typedef boost::split_iterator<string::const_iterator> string_split_iterator;
    string_split_iterator it =
        boost::make_split_iterator(path, boost::first_finder("/", boost::is_iequal()));
    while (!it.eof()) {
        if (it->begin() == it->end()) {
            // avoid adding empty path components
            ++it;
        } else {
            std::string split(it->begin(), it->end());
            // Let's have an exception here for "%u", since we use that to replace the
            // actual username into the path. It's safe to ignore "%u" because it
            // couldn't be in a valid URI anyway.
            // TODO: we should find a neat way to remove the awareness of "%u" from
            // NeonCXX.
            std::string normalizedSplit = split;
            if (split != "%u") {
                normalizedSplit = escape(unescape(split));
            }
            res += normalizedSplit;
            ++it;
            if (!it.eof()) {
                res += '/';
            }
        }
    }
    if (collection && !boost::ends_with(res, "/")) {
        res += '/';
    }
    return res;
}

std::string Status2String(const ne_status *status)
{
    if (!status) {
        return "<NULL status>";
    }
    return StringPrintf("<status %d.%d, code %d, class %d, %s>",
                        status->major_version,
                        status->minor_version,
                        status->code,
                        status->klass,
                        status->reason_phrase ? status->reason_phrase : "\"\"");
}

Session::Session(const boost::shared_ptr<Settings> &settings) :
    m_forceAuthorizationOnce(false),
    m_credentialsSent(false),
    m_oauthTokenRejections(0),
    m_settings(settings),
    m_debugging(false),
    m_session(NULL),
    m_attempt(0)
{
    int logLevel = m_settings->logLevel();
    if (logLevel >= 3) {
        ne_debug_init(stderr,
                      NE_DBG_FLUSH|NE_DBG_HTTP|NE_DBG_HTTPAUTH|
                      (logLevel >= 4 ? NE_DBG_HTTPBODY : 0) |
                      (logLevel >= 5 ? (NE_DBG_LOCKS|NE_DBG_SSL) : 0)|
                      (logLevel >= 6 ? (NE_DBG_XML|NE_DBG_XMLPARSE) : 0)|
                      (logLevel >= 11 ? (NE_DBG_HTTPPLAIN) : 0));
        m_debugging = true;
    } else {
        ne_debug_init(NULL, 0);
    }

    ne_sock_init();
    m_uri = URI::parse(settings->getURL());
    m_session = ne_session_create(m_uri.m_scheme.c_str(),
                                  m_uri.m_host.c_str(),
                                  m_uri.m_port);
    ne_set_server_auth(m_session, getCredentials, this);
    if (m_uri.m_scheme == "https") {
        // neon only initializes session->ssl_context if
        // using https and segfaults in ne_ssl_trust_default_ca()
        // of ne_gnutls.c if ne_ssl_trust_default_ca()
        // is called for non-https. So better call these
        // functions only when needed.
        ne_ssl_set_verify(m_session, sslVerify, this);
        ne_ssl_trust_default_ca(m_session);

        // hack for Yahoo: need a client certificate
        ne_ssl_client_cert *cert = ne_ssl_clicert_read("client.p12");
        SE_LOG_DEBUG(NULL, "client cert is %s", !cert ? "missing" : ne_ssl_clicert_encrypted(cert) ? "encrypted" : "unencrypted");
        if (cert) {
            if (ne_ssl_clicert_encrypted(cert)) {
                if (ne_ssl_clicert_decrypt(cert, "meego")) {
                    SE_LOG_DEBUG(NULL, "decryption failed");
                }
            }
            ne_ssl_set_clicert(m_session, cert);
        }
    }

    m_proxyURL = settings->proxy();
    if (m_proxyURL.empty()) {
#ifdef HAVE_LIBNEON_SYSTEM_PROXY
        // hard compile-time dependency
        ne_session_system_proxy(m_session, 0);
#else
        // compiled against older libneon, but might run with more recent neon
        typedef void (*session_system_proxy_t)(ne_session *sess, unsigned int flags);
        session_system_proxy_t session_system_proxy =
            (session_system_proxy_t)dlsym(RTLD_DEFAULT, "ne_session_system_proxy");
        if (session_system_proxy) {
            session_system_proxy(m_session, 0);
        }
#endif
    } else {
        URI proxyuri = URI::parse(m_proxyURL);
        ne_session_proxy(m_session, proxyuri.m_host.c_str(), proxyuri.m_port);
    }

    int seconds = settings->timeoutSeconds();
    if (seconds < 0) {
        seconds = 5 * 60;
    }
    ne_set_read_timeout(m_session, seconds);
    ne_set_connect_timeout(m_session, seconds);
    ne_hook_pre_send(m_session, preSendHook, this);
}

Session::~Session()
{
    if (m_session) {
        ne_session_destroy(m_session);
    }
    ne_sock_exit();
}

boost::shared_ptr<Session> Session::m_cachedSession;

boost::shared_ptr<Session> Session::create(const boost::shared_ptr<Settings> &settings)
{
    URI uri = URI::parse(settings->getURL());
    if (m_cachedSession &&
        m_cachedSession->m_uri == uri &&
        m_cachedSession->m_proxyURL == settings->proxy()) {
        // reuse existing session with new settings pointer
        m_cachedSession->m_settings = settings;
        return m_cachedSession;
    }
    // create new session
    m_cachedSession.reset(new Session(settings));
    return m_cachedSession;
}


int Session::getCredentials(void *userdata, const char *realm, int attempt, char *username, char *password) throw()
{
    try {
        Session *session = static_cast<Session *>(userdata);
        boost::shared_ptr<AuthProvider> authProvider = session->m_settings->getAuthProvider();
        if (authProvider && authProvider->methodIsSupported(AuthProvider::AUTH_METHOD_OAUTH2)) {
            // We have to fail here because we cannot provide neon
            // with a username/password combination. Instead we rely
            // on the "retry request" mechanism to resend the request
            // with a fresh token.
            SE_LOG_DEBUG(NULL, "giving up on request, try again with new OAuth2 token");
            return 1;
        } else if (!attempt) {
            // try again with credentials
            std::string user, pw;
            session->m_settings->getCredentials(realm, user, pw);
            SyncEvo::Strncpy(username, user.c_str(), NE_ABUFSIZ);
            SyncEvo::Strncpy(password, pw.c_str(), NE_ABUFSIZ);
            session->m_credentialsSent = true;
            SE_LOG_DEBUG(NULL, "retry request with credentials");
            return 0;
        } else {
            // give up
            return 1;
        }
    } catch (...) {
        Exception::handle();
        SE_LOG_ERROR(NULL, "no credentials for %s", realm);
        return 1;
    }
}

void Session::forceAuthorization(const boost::shared_ptr<AuthProvider> &authProvider)
{
    m_forceAuthorizationOnce = true;
    m_authProvider = authProvider;
}

void Session::preSendHook(ne_request *req, void *userdata, ne_buffer *header) throw()
{
    try {
        static_cast<Session *>(userdata)->preSend(req, header);
    } catch (...) {
        Exception::handle();
    }
}

void Session::preSend(ne_request *req, ne_buffer *header)
{
    // sanity check: startOperation must have been called
    if (m_operation.empty()) {
        SE_THROW("internal error: startOperation() not called");
    }

    bool haveUserAgentHeader = boost::starts_with(header->data, "User-Agent:") ||
        strstr(header->data, "\nUser-Agent:");
    if (!haveUserAgentHeader) {
        ne_buffer_concat(header, "User-Agent: SyncEvolution\r\n", (const char *)NULL);
    }

    // Only do this once when using normal username/password.
    // Always do it when using OAuth2.
    bool useOAuth2 = m_authProvider && m_authProvider->methodIsSupported(AuthProvider::AUTH_METHOD_OAUTH2);
    if (m_forceAuthorizationOnce || useOAuth2) {
        m_forceAuthorizationOnce = false;
        bool haveAuthorizationHeader = boost::starts_with(header->data, "Authorization:") ||
            strstr(header->data, "\nAuthorization:");

        if (useOAuth2) {
            if (haveAuthorizationHeader) {
                SE_THROW("internal error: already have Authorization header when about to add OAuth2");
            }
            // Token was obtained by Session::run().
            SE_LOG_DEBUG(NULL, "using OAuth2 token '%s' to authenticate", m_oauth2Bearer.c_str());
            m_credentialsSent = true;
            // SmartPtr<char *> blob(ne_base64((const unsigned char *)m_oauth2Bearer.c_str(), m_oauth2Bearer.size()));
            ne_buffer_concat(header, "Authorization: Bearer ", m_oauth2Bearer.c_str() /* blob.get() */, "\r\n", (const char *)NULL);
        } else if (m_uri.m_scheme == "https") {
            // append "Authorization: Basic" header if not present already
            if (!haveAuthorizationHeader) {
                Credentials creds = m_authProvider->getCredentials();
                std::string credentials = creds.m_username + ":" + creds.m_password;
                SmartPtr<char *> blob(ne_base64((const unsigned char *)credentials.c_str(), credentials.size()));
                ne_buffer_concat(header, "Authorization: Basic ", blob.get(), "\r\n", (const char *)NULL);
            }

            // check for acceptance of credentials later
            m_credentialsSent = true;
            SE_LOG_DEBUG(NULL, "forced sending credentials");
        } else {
            SE_LOG_DEBUG(NULL, "skipping forced sending credentials because not using https");
        }
    }
}


int Session::sslVerify(void *userdata, int failures, const ne_ssl_certificate *cert) throw()
{
    try {
        Session *session = static_cast<Session *>(userdata);

        static const Flag descr[] = {
            { NE_SSL_NOTYETVALID, "certificate not yet valid" },
            { NE_SSL_EXPIRED, "certificate has expired" },
            { NE_SSL_IDMISMATCH, "hostname mismatch" },
            { NE_SSL_UNTRUSTED, "untrusted certificate" },
            { 0, NULL }
        };

        SE_LOG_DEBUG(NULL,
                     "%s: SSL verification problem: %s",
                     session->getURL().c_str(),
                     Flags2String(failures, descr).c_str());
        if (!session->m_settings->verifySSLCertificate()) {
            SE_LOG_DEBUG(NULL, "ignoring bad certificate");
            return 0;
        }
        if (failures == NE_SSL_IDMISMATCH &&
            !session->m_settings->verifySSLHost()) {
            SE_LOG_DEBUG(NULL, "ignoring hostname mismatch");
            return 0;
        }
        return 1;
    } catch (...) {
        Exception::handle();
        return 1;
    }
}

#ifdef HAVE_LIBNEON_OPTIONS
unsigned int Session::options(const std::string &path)
{
    unsigned int caps;
    checkError(ne_options2(m_session, path.c_str(), &caps));
    return caps;
}
#endif // HAVE_LIBNEON_OPTIONS

class PropFindDeleter
{
public:
    void operator () (ne_propfind_handler *handler) { if (handler) { ne_propfind_destroy(handler); } }
};

void Session::propfindURI(const std::string &path, int depth,
                          const ne_propname *props,
                          const PropfindURICallback_t &callback,
                          const Timespec &deadline)
{
    startOperation("PROPFIND", deadline);

 retry:
    boost::shared_ptr<ne_propfind_handler> handler;
    int error;

    checkAuthorization();
    handler = boost::shared_ptr<ne_propfind_handler>(ne_propfind_create(m_session, path.c_str(), depth),
                                                     PropFindDeleter());
    if (props != NULL) {
	error = ne_propfind_named(handler.get(), props,
                                  propsResult, const_cast<void *>(static_cast<const void *>(&callback)));
    } else {
	error = ne_propfind_allprop(handler.get(),
                                    propsResult, const_cast<void *>(static_cast<const void *>(&callback)));
    }

    // remain valid as long as "handler" is valid
    ne_request *req = ne_propfind_get_request(handler.get());
    const ne_status *status = ne_get_status(req);
    const char *tmp = ne_get_response_header(req, "Location");
    std::string location(tmp ? tmp : "");

    if (!checkError(error, status->code, status, location, path)) {
        goto retry;
    }
}

void Session::propsResult(void *userdata, const ne_uri *uri,
                          const ne_prop_result_set *results) throw()
{
    try {
        PropfindURICallback_t *callback = static_cast<PropfindURICallback_t *>(userdata);
        (*callback)(URI::fromNeon(*uri), results);
    } catch (...) {
        Exception::handle();
    }
}

void Session::propfindProp(const std::string &path, int depth,
                           const ne_propname *props,
                           const PropfindPropCallback_t &callback,
                           const Timespec &deadline)
{
    propfindURI(path, depth, props,
                boost::bind(&Session::propsIterate, _1, _2, boost::cref(callback)),
                deadline);
}

void Session::propsIterate(const URI &uri, const ne_prop_result_set *results,
                           const PropfindPropCallback_t &callback)
{
    PropIteratorUserdata_t data(&uri, &callback);
    ne_propset_iterate(results,
                       propIterator,
                       &data);
}

int Session::propIterator(void *userdata,
                           const ne_propname *pname,
                           const char *value,
                           const ne_status *status) throw()
{
    try {
        const PropIteratorUserdata_t *data = static_cast<const PropIteratorUserdata_t *>(userdata);
        (*data->second)(*data->first, pname, value, status);
        return 0;
    } catch (...) {
        Exception::handle();
        return 1; // abort iterating
    }
}

void Session::startOperation(const string &operation, const Timespec &deadline)
{
    SE_LOG_DEBUG(NULL, "starting %s, credentials %s, %s",
                 operation.c_str(),
                 m_settings->getCredentialsOkay() ? "okay" : "unverified",
                 deadline ? StringPrintf("deadline in %.1lfs",
                                         (deadline - Timespec::monotonic()).duration()).c_str() :
                 "no deadline");

    // now is a good time to check for user abort
    SuspendFlags::getSuspendFlags().checkForNormal();

    // remember current operation attributes
    m_operation = operation;
    m_deadline = deadline;

    // no credentials set yet for next request
    m_credentialsSent = false;
    // first attempt at request
    m_attempt = 0;
}

void Session::flush()
{
    if (m_debugging &&
        LogRedirect::redirectingStderr()) {
        // flush stderr and wait a bit: this might help to get
        // the redirected output via LogRedirect
        fflush(stderr);
        Sleep(0.001);
    }
}

bool Session::checkError(int error, int code, const ne_status *status,
                         const std::string &newLocation,
                         const std::string &oldLocation,
                         const std::set<int> *expectedCodes)
{
    flush();
    SuspendFlags &s = SuspendFlags::getSuspendFlags();

    // unset operation, set it again only if the same operation is going to be retried
    string operation = m_operation;
    m_operation = "";

    // determine error description, may be made more specific below
    string descr;
    if (code) {
        descr = StringPrintf("%s: Neon error code %d, HTTP status %d: %s",
                             operation.c_str(),
                             error, code,
                             ne_get_error(m_session));
        
    } else {
        descr = StringPrintf("%s: Neon error code %d, no HTTP status: %s",
                             operation.c_str(),
                             error,
                             ne_get_error(m_session));
    }
    // true for specific errors which might go away after a retry
    bool retry = false;

    // detect redirect
    if ((error == NE_ERROR || error == NE_OK) &&
        (code >= 300 && code <= 399)) {
        // Special case Google: detect redirect to temporary error page
        // and retry; same for redirect to login page. Only do that for
        // "real" URLs, not for the root or /calendar/ that we run into
        // when scanning, because the login there will always fail.
        if (oldLocation != "/" &&
            oldLocation != "/calendar/" &&
            (boost::starts_with(newLocation, "http://www.google.com/googlecalendar/unavailable.html") ||
             boost::starts_with(newLocation, "https://www.google.com/googlecalendar/unavailable.html") ||
             boost::starts_with(newLocation, "https://accounts.google.com/ServiceLogin"))) {
            retry = true;
        } else {
            SE_THROW_EXCEPTION_2(RedirectException,
                                 StringPrintf("%s: %d status: %s redirected to %s",
                                              operation.c_str(),
                                              code,
                                              oldLocation.c_str(),
                                              newLocation.c_str()),
                                 code,
                                 newLocation);
        }
    }

    switch (error) {
    case NE_OK:
        // request itself completed, but might still have resulted in bad status
        if (expectedCodes &&
            expectedCodes->find(code) != expectedCodes->end()) {
            // return to caller immediately as if we had succeeded,
            // without throwing an exception and without retrying
            return true;
        }
        if (code &&
            (code < 200 || code >= 300)) {
            if (status) {
                descr = StringPrintf("%s: bad HTTP status: %s",
                                     operation.c_str(),
                                     Status2String(status).c_str());
            } else {
                descr = StringPrintf("%s: bad HTTP status: %d",
                                     operation.c_str(),
                                     code);
            }
            if (code >= 500 && code <= 599 &&
                 // not implemented
                code != 501 &&
                // HTTP version not supported
                code != 505) {
                // potentially temporary server failure, may try again
                retry = true;
            }
        } else {
            // all fine, no retry necessary: clean up

            // remember completion time of request
            m_lastRequestEnd = Timespec::monotonic();

            // assume that credentials were valid, if sent
            if (m_credentialsSent) {
                SE_LOG_DEBUG(NULL, "credentials accepted");
                m_settings->setCredentialsOkay(true);
            }
            m_oauthTokenRejections = 0;

            return true;
        }
        break;
    case NE_AUTH: {
        // Retry OAuth2-based request if we still have a valid token.
        bool useOAuth2 = m_authProvider && m_authProvider->methodIsSupported(AuthProvider::AUTH_METHOD_OAUTH2);
        if (useOAuth2) {
            // Try again with new token? Need to restore the counter,
            // because it is relevant for getOAuth2Bearer() in preSend().
            if (m_oauthTokenRejections < 2) {
                if (!m_oauth2Bearer.empty() && m_credentialsSent) {
                    SE_LOG_DEBUG(NULL, "discarding used and rejected OAuth2 token '%s'", m_oauth2Bearer.c_str());
                    m_oauthTokenRejections++;
                    m_oauth2Bearer.clear();
                } else {
                    SE_LOG_DEBUG(NULL, "OAuth2 token '%s' not used?!", m_oauth2Bearer.c_str());
                }
                retry = true;
                SE_LOG_DEBUG(NULL, "OAuth2 retry after %d failed tokens", m_oauthTokenRejections);
            } else {
                SE_LOG_DEBUG(NULL, "too many failed OAuth2 tokens, giving up");
            }
        }

        // tell caller what kind of transport error occurred
        code = STATUS_UNAUTHORIZED;
        descr = StringPrintf("%s: Neon error code %d = NE_AUTH, HTTP status %d: %s",
                             operation.c_str(),
                             error, code,
                             ne_get_error(m_session));
        break;
    }
    case NE_ERROR:
        if (code) {
            descr = StringPrintf("%s: Neon error code %d: %s",
                                 operation.c_str(),
                                 error,
                                 ne_get_error(m_session));
            if (code >= 500 && code <= 599 &&
                // not implemented
                code != 501 &&
                // HTTP version not supported
                code != 505) {
                // potentially temporary server failure, may try again
                retry = true;
            }
        } else if (descr.find("Secure connection truncated") != descr.npos ||
                   descr.find("decryption failed or bad record mac") != descr.npos) {
            // occasionally seen with Google server; let's retry
            // For example: "Could not read status line: SSL error: decryption failed or bad record mac"
            retry = true;
        }
        break;
    case NE_LOOKUP:
    case NE_TIMEOUT:
    case NE_CONNECT:
        retry = true;
        break;
    }

    if (code == 401) {
        if (m_settings->getCredentialsOkay()) {
            SE_LOG_DEBUG(NULL, "credential error due to throttling (?), retry");
            retry = true;
        } else {
            // give up without retrying
            SE_LOG_DEBUG(NULL, "credential error, no success with them before => report it");
        }
    }


    SE_LOG_DEBUG(NULL, "%s, %s",
                 descr.c_str(),
                 retry ? "might retry" : "must not retry");
    if (retry) {
        m_attempt++;

        if (!m_deadline) {
            SE_LOG_DEBUG(NULL, "retrying not allowed for %s (no deadline)",
                         operation.c_str());
        } else {
            Timespec now = Timespec::monotonic();
            if (now < m_deadline) {
                int retrySeconds = m_settings->retrySeconds();
                if (retrySeconds >= 0) {
                    Timespec last = m_lastRequestEnd;
                    Timespec now = Timespec::monotonic();
                    if (!last) {
                        last = now;
                    }
                    int delay = retrySeconds * (1 << (m_attempt - 1));
                    Timespec next = last + delay;
                    if (next > m_deadline) {
                        // no point in waiting (potentially much) until after our 
                        // deadline, do final attempt at that time
                        next = m_deadline;
                    }
                    if (next > now) {
                        double duration = (next - now).duration();
                        SE_LOG_DEBUG(NULL, "retry %s in %.1lfs, attempt #%d",
                                     operation.c_str(),
                                     duration,
                                     m_attempt);
                        // Inform the user, because this will take a
                        // while and we don't want to give the
                        // impression of being stuck.
                        SE_LOG_INFO(NULL, "operation temporarily (?) failed, going to retry in %.1lfs before giving up in %.1lfs: %s",
                                    duration,
                                    (m_deadline - now).duration(),
                                    descr.c_str());
                        Sleep(duration);
                    } else {
                        SE_LOG_DEBUG(NULL, "retry %s immediately (due already), attempt #%d",
                                     operation.c_str(),
                                     m_attempt);
                    }
                } else {
                    SE_LOG_DEBUG(NULL, "retry %s immediately (retry interval <= 0), attempt #%d",
                                 operation.c_str(),
                                 m_attempt);
                }

                // try same operation again?
                if (s.getState() == SuspendFlags::NORMAL) {
                    m_operation = operation;
                    return false;
                }
            } else {
                SE_LOG_DEBUG(NULL, "retry %s would exceed deadline, bailing out",
                             m_operation.c_str());
            }
        }
    }

    if (code == 401) {
        // fatal credential error, remember that
        SE_LOG_DEBUG(NULL, "credentials rejected");
        m_settings->setCredentialsOkay(false);
    }

    if (code) {
        // copy error code into exception
        SE_THROW_EXCEPTION_STATUS(TransportStatusException,
                                  descr,
                                  SyncMLStatus(code));
    } else {
        SE_THROW_EXCEPTION(TransportException,
                           descr);
    }
}

XMLParser::XMLParser()
{
    m_parser = ne_xml_create();
}

XMLParser::~XMLParser()
{
    ne_xml_destroy(m_parser);
}

XMLParser &XMLParser::pushHandler(const StartCB_t &start,
                                  const DataCB_t &data,
                                  const EndCB_t &end)
{
    m_stack.push_back(Callbacks(start, data, end));
    Callbacks &cb = m_stack.back();
    ne_xml_push_handler(m_parser,
                        startCB, dataCB, endCB,
                        &cb);
    return *this;
}

int XMLParser::startCB(void *userdata, int parent,
                       const char *nspace, const char *name,
                       const char **atts)
{
    Callbacks *cb = static_cast<Callbacks *>(userdata);
    try {
        return cb->m_start(parent, nspace, name, atts);
    } catch (...) {
        Exception::handle();
        SE_LOG_ERROR(NULL, "startCB %s %s failed", nspace, name);
        return -1;
    }
}

int XMLParser::dataCB(void *userdata, int state,
                      const char *cdata, size_t len)
{
    Callbacks *cb = static_cast<Callbacks *>(userdata);
    try {
        return cb->m_data ?
            cb->m_data(state, cdata, len) :
            0;
    } catch (...) {
        Exception::handle();
        SE_LOG_ERROR(NULL, "dataCB failed");
        return -1;
    }
}

int XMLParser::endCB(void *userdata, int state, 
                     const char *nspace, const char *name)
{
    Callbacks *cb = static_cast<Callbacks *>(userdata);
    try {
        return cb->m_end ?
            cb->m_end(state, nspace, name) :
            0;
    } catch (...) {
        Exception::handle();
        SE_LOG_ERROR(NULL, "endCB %s %s failed", nspace, name);
        return -1;
    }
}


int XMLParser::accept(const std::string &nspaceExpected,
                      const std::string &nameExpected,
                      const char *nspace,
                      const char *name)
{
    if (nspace && nspaceExpected == nspace &&
        name && nameExpected == name) {
        return 1;
    } else {
        return 0;
    }
}

int XMLParser::append(std::string &buffer,
                      const char *data,
                      size_t len)
{
    buffer.append(data, len);
    return 0;
}

int XMLParser::reset(std::string &buffer)
{
    buffer.clear();
    return 0;
}

void XMLParser::initReportParser(const ResponseEndCB_t &responseEnd)
{
    pushHandler(boost::bind(Neon::XMLParser::accept, "DAV:", "multistatus", _2, _3));
    pushHandler(boost::bind(Neon::XMLParser::accept, "DAV:", "response", _2, _3),
                Neon::XMLParser::DataCB_t(),
                boost::bind(&Neon::XMLParser::doResponseEnd,
                            this, responseEnd));
    pushHandler(boost::bind(Neon::XMLParser::accept, "DAV:", "href", _2, _3),
                boost::bind(Neon::XMLParser::append, boost::ref(m_href), _2, _3));
    pushHandler(boost::bind(Neon::XMLParser::accept, "DAV:", "propstat", _2, _3));
    pushHandler(boost::bind(Neon::XMLParser::accept, "DAV:", "status", _2, _3) /* check status? */);
    pushHandler(boost::bind(Neon::XMLParser::accept, "DAV:", "prop", _2, _3));
    pushHandler(boost::bind(Neon::XMLParser::accept, "DAV:", "getetag", _2, _3),
                boost::bind(Neon::XMLParser::append, boost::ref(m_etag), _2, _3));
}

Request::Request(Session &session,
                 const std::string &method,
                 const std::string &path,
                 const std::string &body,
                 std::string &result) :
    m_method(method),
    m_path(path),
    m_session(session),
    m_result(&result),
    m_parser(NULL)
{
    m_req = ne_request_create(session.getSession(), m_method.c_str(), path.c_str());
    ne_set_request_body_buffer(m_req, body.c_str(), body.size());
}

Request::Request(Session &session,
                 const std::string &method,
                 const std::string &path,
                 const std::string &body,
                 XMLParser &parser) :
    m_method(method),
    m_path(path),
    m_session(session),
    m_result(NULL),
    m_parser(&parser)
{
    m_req = ne_request_create(session.getSession(), m_method.c_str(), path.c_str());
    ne_set_request_body_buffer(m_req, body.c_str(), body.size());
}

Request::~Request()
{
    ne_request_destroy(m_req);
}

#ifdef NEON_COMPATIBILITY
/**
 * wrapper needed to allow lazy resolution of the ne_accept_2xx() function when needed
 * instead of when loaded
 */
static int ne_accept_2xx(void *userdata, ne_request *req, const ne_status *st)
{
    return ::ne_accept_2xx(userdata, req, st);
}
#endif

void Session::checkAuthorization()
{
    bool useOAuth2 = m_authProvider && m_authProvider->methodIsSupported(AuthProvider::AUTH_METHOD_OAUTH2);
    if (useOAuth2 &&
        m_oauth2Bearer.empty()) {
        // Count the number of times we asked for new tokens. This helps
        // the provider determine whether the token that it returns are valid.
        try {
            m_oauth2Bearer = m_authProvider->getOAuth2Bearer(m_oauthTokenRejections,
                                                             boost::bind(&Settings::updatePassword, m_settings, _1));
            SE_LOG_DEBUG(NULL, "got new OAuth2 token '%s' for next request", m_oauth2Bearer.c_str());
        } catch (...) {
            std::string explanation;
            Exception::handle(explanation);
            // Treat all errors as fatal authentication errors.
            // Our caller will abort immediately.
            SE_THROW_EXCEPTION_STATUS(FatalException,
                                      StringPrintf("logging into remote service failed: %s", explanation.c_str()),
                                      STATUS_FORBIDDEN);
        }
    }
}

bool Session::run(Request &request, const std::set<int> *expectedCodes)
{
    int error;

    // Check for authorization while we still can.
    checkAuthorization();

    std::string *result = request.getResult();
    ne_request *req = request.getRequest();
    if (result) {
        result->clear();
        ne_add_response_body_reader(req, ne_accept_2xx,
                                    Request::addResultData, &request);
        error = ne_request_dispatch(req);
    } else {
        error = ne_xml_dispatch_request(req, request.getParser()->get());
    }

    return checkError(error, request.getStatus()->code, request.getStatus(),
                      request.getResponseHeader("Location"),
                      request.getPath(),
                      expectedCodes);
}

int Request::addResultData(void *userdata, const char *buf, size_t len)
{
    Request *me = static_cast<Request *>(userdata);
    me->m_result->append(buf, len);
    return 0;
}

}

SE_END_CXX

#endif // ENABLE_DAV
