/*
 * Copyright (C) 2010 Patrick Ohly <patrick.ohly@gmx.de>
 */

/**
 * Simplifies usage of neon in C++ by wrapping some calls in C++
 * classes. Includes all neon header files relevant for the backend.
 */

#ifndef INCL_NEONCXX
#define INCL_NEONCXX

#include <ne_session.h>
#include <ne_utils.h>
#include <ne_basic.h>
#include <ne_props.h>
#include <ne_request.h>

#include <string>
#include <list>

// TODO: remove this again
using namespace std;

#include <boost/shared_ptr.hpp>
#include <boost/function.hpp>


#include <syncevo/util.h>
#include <syncevo/declarations.h>
SE_BEGIN_CXX

class AuthProvider;

namespace Neon {
#if 0
}
#endif

/** comma separated list of features supported by libneon in use */
std::string features();

class Request;

/**
 * Throwing this will stop all further attempts to use the
 * remote service.
 */
class FatalException : public StatusException
{
public:
    FatalException(const std::string &file,
                   int line,
                   const std::string &what,
                   SyncMLStatus status) :
        StatusException(file, line, what, status)
    {}
};

class Settings {
 public:
    /**
     * base URL for WebDAV service
     */
    virtual std::string getURL() = 0;

    /**
     * host name must match for SSL?
     */
    virtual bool verifySSLHost() = 0;

    /**
     * SSL certificate must be valid?
     */
    virtual bool verifySSLCertificate() = 0;

    /**
     * proxy URL, empty for system default
     */
    virtual std::string proxy() = 0;

    /**
     * fill in username and password for specified realm (URL?),
     * throw error if not available
     */
    virtual void getCredentials(const std::string &realm,
                                std::string &username,
                                std::string &password) = 0;

    /**
     * Grant access to AuthProvider. In addition to plain username/password
     * in getCredentials, this one here might also be used for OAuth2.
     */
    virtual boost::shared_ptr<AuthProvider> getAuthProvider() = 0;

    /**
     * Updates password to new one returned during OAuth2 authorization.
     */
    virtual void updatePassword(const std::string& password) = 0;

    /**
     * Google returns a 401 error even if the credentials
     * are valid. It seems to use that to throttle request
     * rates. This read/write setting remembers whether the
     * credentials were used successfully in the past,
     * in which case we try harder to get a failed request
     * executed. Otherwise we give up immediately.
     */
    virtual bool getCredentialsOkay() = 0;
    virtual void setCredentialsOkay(bool okay) = 0;

    /**
     * standard SyncEvolution log level, see
     * Session::Session() how that is mapped to neon debugging
     */
    virtual int logLevel() = 0;

    /**
     * if true, then manipulate SEQUENCE and LAST-MODIFIED properties
     * so that Google CalDAV server accepts updates
     */
    virtual bool googleUpdateHack() const = 0;

    /**
     * if true, then check whether server has added an unwanted alarm
     * and resend to get rid of it
     */
    virtual bool googleAlarmHack() const = 0;

    /**
     * duration in seconds after which communication with a server
     * fails with a timeout error; <= 0 picks a large default value
     */
    virtual int timeoutSeconds() const = 0;

    /**
     * for network operations which fail before reaching timeoutSeconds()
     * and can/should be retried: try again if > 0
     */
    virtual int retrySeconds() const = 0;

    /**
     * use this to create a boost_shared pointer for a
     * Settings instance which needs to be freed differently
     */
    struct NullDeleter {
        void operator()(Settings *) const {}
    };
};

struct URI {
    std::string m_scheme;
    std::string m_host;
    std::string m_userinfo;
    unsigned int m_port;
    std::string m_path;
    std::string m_query;
    std::string m_fragment;

    URI() : m_port(0) {}

    /**
     * Split URL into parts. Throws TransportAgentException on
     * invalid url.  Port will be set to default for scheme if not set
     * in URL. Path is normalized.
     *
     * @param collection    set to true if the normalized path is for
     *                      a collection and shall have a trailing sl
     */
    static URI parse(const std::string &url, bool collection=false);

    static URI fromNeon(const ne_uri &other, bool collection=false);

    /**
     * produce new URI from current path and new one (may be absolute
     * and relative)
     */
    URI resolve(const std::string &path) const;

    /** compose URL from parts */
    std::string toURL() const;

    /**
     * URL-escape string
     */
    static std::string escape(const std::string &text);
    static std::string unescape(const std::string &text);

    /**
     * Removes differences caused by escaping different characters.
     * Appends slash if path is a collection (or meant to be one) and
     * doesn't have a trailing slash. Removes double slashes.
     *
     * @param path   an absolute path (leading slash)
     */
    static std::string normalizePath(const std::string &path, bool collection);

    int compare(const URI &other) const {
        int res;
        res = m_scheme.compare(other.m_scheme);
        if (!res) {
            res = m_host.compare(other.m_host);
            if (!res) {
                res = m_userinfo.compare(other.m_userinfo);
                if (!res) {
                    res = other.getPort() - getPort();
                    if (!res) {
                        res = m_path.compare(other.m_path);
                        if (!res) {
                            res = m_query.compare(other.m_query);
                            if (!res) {
                                res = m_fragment.compare(other.m_fragment);
                            }
                        }
                    }
                }
            }
        }
        return res;
    }

    bool operator == (const URI &other) const { return compare(other) == 0; }
    bool operator < (const URI &other) const { return compare(other) < 0; }
    bool operator <= (const URI &other) const { return compare(other) <= 0; }
    bool operator > (const URI &other) const { return compare(other) > 0; }
    bool operator >= (const URI &other) const { return compare(other) >= 0; }

    int getPort() const {
        return m_port ? m_port :
            m_scheme == "https" ? 443 :
            m_scheme == "http" ? 80 :
            0;
    }

    bool empty() const {
        return m_scheme.empty() &&
        m_host.empty() &&
        m_userinfo.empty() &&
        m_port == 0 &&
        m_path.empty() &&
        m_query.empty() &&
        m_fragment.empty();
    }
};

/** produce debug string for status, which may be NULL */
std::string Status2String(const ne_status *status);

/**
 * Wraps all session related activities.
 * Throws transport errors for fatal problems.
 */
class Session {
    /**
     * @param settings    must provide information about settings on demand
     */
    Session(const boost::shared_ptr<Settings> &settings);
    static boost::shared_ptr<Session> m_cachedSession;

    bool m_forceAuthorizationOnce;
    boost::shared_ptr<AuthProvider> m_authProvider;

    /**
     * Count how often a request was sent with credentials.
     * If the request succeeds, we assume that the credentials
     * were okay. A bit fuzzy because forcing authorization
     * might succeed despite invalid credentials if the
     * server doesn't check them.
     */
    bool m_credentialsSent;

    /**
     * Count the number of consecutive times that an OAuth2 token
     * failed to get accepted. This can happen when the current one
     * expired and needs to be refreshed or we need re-authorization
     * by the user.
     */
    int m_oauthTokenRejections;

    /**
     * Cached token for OAuth2. Obtained before starting the request in run(),
     * used in preSend(), invalidated when it caused an authentication error
     * in checkError().
     */
    std::string m_oauth2Bearer;

    /**
     * current operation; used for debugging output
     */
    string m_operation;

    /**
     * current deadline for operation
     */
    Timespec m_deadline;

 public:
    /**
     * Create or reuse Session instance.
     * 
     * One Session instance is kept alive throughout the life of the process,
     * to reuse proxy information (libproxy has a considerably delay during
     * initialization) and HTTP connection/authentication.
     */
    static boost::shared_ptr<Session> create(const boost::shared_ptr<Settings> &settings);
    ~Session();

#ifdef HAVE_LIBNEON_OPTIONS
    /** ne_options2() for a specific path*/
    unsigned int options(const std::string &path);
#endif

    /**
     * called with URI and complete result set; exceptions are logged, but ignored
     */
    typedef boost::function<void (const URI &, const ne_prop_result_set *)> PropfindURICallback_t;

    /**
     * called with URI and specific property, value string may be NULL (error case);
     * exceptions are logged and abort iterating over properties (but not URIs)
     */
    typedef boost::function<void (const URI &, const ne_propname *, const char *, const ne_status *)> PropfindPropCallback_t;

    /** ne_simple_propfind(): invoke callback for each URI */
    void propfindURI(const std::string &path, int depth,
                     const ne_propname *props,
                     const PropfindURICallback_t &callback,
                     const Timespec &deadline);

    /**
     * ne_simple_propfind(): invoke callback for each property of each URI
     * @param deadline      stop resending after that point in time,
     *                      zero disables resending
     * @param retrySeconds  number of seconds to wait between resending,
     *                      must not be negative
     */
    void propfindProp(const std::string &path, int depth,
                      const ne_propname *props,
                      const PropfindPropCallback_t &callback,
                      const Timespec &deadline);

    /** URL which is in use */
    std::string getURL() const { return m_uri.toURL(); }

    /** same as getURL() split into parts */
    const URI &getURI() const { return m_uri; }

    /**
     * to be called *once* before executing a request
     * or retrying it
     *
     * call sequence is this:
     * - startOperation()
     * - repeat until success or final failure: create request, run(), checkError()
     *
     * @param operation    internal descriptor for debugging (for example, PROPFIND)
     * @param deadline     time at which the operation must be completed, otherwise it'll be considered failed;
     *                     empty if the operation is only meant to be attempted once
     */
    void startOperation(const string &operation, const Timespec &deadline);

    /**
     * Run one attempt to execute the request. May be called multiple times.
     *
     * Uses checkError() underneath to detect fatal errors and throw
     * exceptions.
     *
     * @return result of Session::checkError()
     */
    bool run(Request &request, const std::set<int> *expectedCodes);

    /**
     * to be called after each operation which might have produced debugging output by neon;
     * automatically called by checkError()
     */
    void flush();

    ne_session *getSession() const { return m_session; }

    /**
     * Force next request in this session to have Basic authorization
     * (when username/password are provided by AuthProvider) or all
     * requests to use OAuth2 authentication.
     */
    void forceAuthorization(const boost::shared_ptr<AuthProvider> &authProvider);

 private:
    boost::shared_ptr<Settings> m_settings;
    bool m_debugging;
    ne_session *m_session;
    URI m_uri;
    std::string m_proxyURL;
    /** time when last successul request completed, maintained by checkError() */
    Timespec m_lastRequestEnd;
    /** number of times a request was sent, maintained by startOperation(), the credentials callback, and checkError() */
    int m_attempt;

    void checkAuthorization();

    /**
     * throw error if error code indicates failure;
     * pass additional status code from a request whenever possible
     *
     * @param error      return code from Neon API call
     * @param code       HTTP status code
     * @param status     optional ne_status pointer, non-NULL for all requests
     * @param location   optional "Location" header value
     * @param expectedCodes   set of codes which are normal and must not result
     *                        in retrying or an exception (returns true, as if the
     *                        operation succeeded)
     *
     * @return true for success, false if retry needed (only if deadline not empty);
     *         errors reported via exceptions
     */
    bool checkError(int error, int code = 0, const ne_status *status = NULL,
                    const string &newLocation = "",
                    const std::string &oldLocation = "",
                    const std::set<int> *expectedCodes = NULL);

    /** ne_set_server_auth() callback */
    static int getCredentials(void *userdata, const char *realm, int attempt, char *username, char *password) throw();

    /** ne_ssl_set_verify() callback */
    static int sslVerify(void *userdata, int failures, const ne_ssl_certificate *cert) throw();

    /** ne_props_result callback which invokes a PropfindURICallback_t as user data */
    static void propsResult(void *userdata, const ne_uri *uri,
                            const ne_prop_result_set *results) throw();

    /** iterate over properties in result set */
    static void propsIterate(const URI &uri, const ne_prop_result_set *results,
                             const PropfindPropCallback_t &callback);

    /** ne_propset_iterator callback which invokes pair<URI, PropfindPropCallback_t> */
    static int propIterator(void *userdata,
                            const ne_propname *pname,
                            const char *value,
                            const ne_status *status) throw();

    // use pointers here, g++ 4.2.3 has issues with references (which was used before)
    typedef std::pair<const URI *, const PropfindPropCallback_t *> PropIteratorUserdata_t;

    /** Neon callback for preSend() */
    static void preSendHook(ne_request *req, void *userdata, ne_buffer *header) throw();
    /** implements forced Basic authentication, if requested */
    void preSend(ne_request *req, ne_buffer *header);
};

/**
 * encapsulates a ne_xml_parser
 */
class XMLParser
{
 public:
    XMLParser();
    ~XMLParser();

    ne_xml_parser *get() const { return m_parser; }

    /**
     * See ne_xml_startelm_cb:
     * arguments are parent state, namespace, name, attributes (NULL terminated)
     * @return < 0 abort, 0 decline, > 0 accept
     */
    typedef boost::function<int (int, const char *, const char *, const char **)> StartCB_t;

    /**
     * See ne_xml_cdata_cb:
     * arguments are state of element, data and data len
     * May be NULL.
     * @return != 0 to abort
     */
    typedef boost::function<int (int, const char *, size_t)> DataCB_t;

    /**
     * See ne_xml_endelm_cb:
     * arguments are state of element, namespace, name
     * May be NULL.
     * @return != 0 to abort
     */
    typedef boost::function<int (int, const char *, const char *)> EndCB_t;

    /**
     * add new handler, see ne_xml_push_handler()
     */
    XMLParser &pushHandler(const StartCB_t &start,
                           const DataCB_t &data = DataCB_t(),
                           const EndCB_t &end = EndCB_t());

    /**
     * StartCB_t: accepts a new element if namespace and name match
     */
    static int accept(const std::string &nspaceExpected,
                      const std::string &nameExpected,
                      const char *nspace,
                      const char *name);

    /**
     * DataCB_t: append to std::string
     */
    static int append(std::string &buffer,
                      const char *data,
                      size_t len);

    /**
     * EndCB_t: clear std::string
     */
    static int reset(std::string &buffer);

    /**
     * called once a response is completely parse
     *
     * @param href     the path for which the response was sent
     * @param etag     it's etag, empty if not requested or available
     */
    typedef boost::function<void (const std::string &, const std::string &)> ResponseEndCB_t;

    /**
     * Setup parser for handling REPORT result.
     * Already deals with href and etag, caching it
     * for each response and passing it to the "response
     * complete" callback.
     *
     * Caller still needs to push a handler for
     * "urn:ietf:params:xml:ns:caldav", "calendar-data",
     * or any other elements that it wants to know about.
     * 
     * @param responseEnd   called at the end of processing each response;
     *                      this is the only time when all relevant
     *                      parts of the response are guaranteed to have been seen;
     *                      when expecting only one response, the callback
     *                      is not needed
     */
    void initReportParser(const ResponseEndCB_t &responseEnd = ResponseEndCB_t());

 private:
    ne_xml_parser *m_parser;
    struct Callbacks {
        Callbacks(const StartCB_t &start,
                  const DataCB_t &data = DataCB_t(),
                  const EndCB_t &end = EndCB_t()) :
            m_start(start),
            m_data(data),
            m_end(end)
        {}
        StartCB_t m_start;
        DataCB_t m_data;
        EndCB_t m_end;
    };
    std::list<Callbacks> m_stack;

    /** buffers for initReportParser() */
    std::string m_href, m_etag;

    int doResponseEnd(const ResponseEndCB_t &responseEnd) {
        if (responseEnd) {
            responseEnd(m_href, m_etag);
        }
        // clean up for next response
        m_href.clear();
        m_etag.clear();
        return 0;
    }

    static int startCB(void *userdata, int parent,
                       const char *nspace, const char *name,
                       const char **atts);
    static int dataCB(void *userdata, int state,
                      const char *cdata, size_t len);
    static int endCB(void *userdata, int state, 
                     const char *nspace, const char *name);

};

/**
 * encapsulates a ne_request, with std::string as read and write buffer
 */
class Request
{
 public:
    /**
     * read and write buffers owned by caller
     */
    Request(Session &session,
            const std::string &method,
            const std::string &path,
            const std::string &body,
            std::string &result);
    /**
     * read from buffer (owned by caller!) and
     * parse result as XML
     */
    Request(Session &session,
            const std::string &method,
            const std::string &path,
            const std::string &body,
            XMLParser &parser);
    ~Request();

    void setFlag(ne_request_flag flag, int value) { ne_set_request_flag(m_req, flag, value); }
    void addHeader(const std::string &name, const std::string &value) {
        ne_add_request_header(m_req, name.c_str(), value.c_str());
    }

    /**
     * Execute the request. See Session::run().
     */
    bool run(const std::set<int> *expectedCodes = NULL) { return m_session.run(*this, expectedCodes); }

    std::string getResponseHeader(const std::string &name) {
        const char *value = ne_get_response_header(m_req, name.c_str());
        return value ? value : "";
    }
    int getStatusCode() { return ne_get_status(m_req)->code; }
    const ne_status *getStatus() { return ne_get_status(m_req); }

    ne_request *getRequest() const { return m_req; }
    std::string *getResult() const { return m_result; }
    XMLParser *getParser() const { return m_parser; }
    std::string getPath() const { return m_path; }

    /** ne_block_reader implementation */
    static int addResultData(void *userdata, const char *buf, size_t len);

 private:
    // buffers for string (copied by ne_request_create(),
    // but due to a bug in neon, our method string is still used
    // for credentials)
    std::string m_method;

    // Path used when creating the request. Copied by ne_request_create(),
    // but cannot be accessed later via the request.
    std::string m_path;

    Session &m_session;
    ne_request *m_req;
    std::string *m_result;
    XMLParser *m_parser;
};

/** thrown for 301 HTTP status */
class RedirectException : public TransportException
{
    const int m_code;
    const std::string m_url;

 public:
    RedirectException(const std::string &file,
                      int line,
                      const std::string &what,
                      int code,
                      const std::string &url) :
    TransportException(file, line, what),
    m_code(code),
    m_url(url)
    {}
    ~RedirectException() throw() {}

    /** returns exact HTTP status code (301, 302, ...) */
    int getCode() const { return m_code; }

    /** returns URL to where the request was redirected */
    std::string getLocation() const { return m_url; }
};

}
SE_END_CXX

#endif // INCL_NEONCXX
