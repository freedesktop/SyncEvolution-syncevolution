/*
 * Copyright (C) 2010 Intel Corporation
 */

#include "WebDAVSource.h"

#include <boost/bind.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/scoped_ptr.hpp>

#include <syncevo/LogRedirect.h>

#include <stdio.h>
#include <errno.h>

SE_BEGIN_CXX

BoolConfigProperty WebDAVCredentialsOkay("webDAVCredentialsOkay", "credentials were accepted before");

#ifdef ENABLE_DAV

/**
 * Retrieve settings from SyncConfig.
 * NULL pointer for config is allowed.
 */
class ContextSettings : public Neon::Settings {
    boost::shared_ptr<SyncConfig> m_context;
    std::string m_url;
    bool m_googleUpdateHack;
    bool m_googleChildHack;
    bool m_googleAlarmHack;
    // credentials were valid in the past: stored persistently in tracking node
    bool m_credentialsOkay;


public:
    ContextSettings(const boost::shared_ptr<SyncConfig> &context) :
        m_context(context),
        m_googleUpdateHack(false),
        m_googleChildHack(false),
        m_googleAlarmHack(false),
        m_credentialsOkay(false)
    {
        if (m_context) {
            vector<string> urls = m_context->getSyncURL();
            if (!urls.empty()) {
                m_url = urls.front();
                std::string username = m_context->getSyncUsername();
                boost::replace_all(m_url, "%u", Neon::URI::escape(username));
            }
            Neon::URI uri = Neon::URI::parse(m_url);
            typedef boost::split_iterator<string::iterator> string_split_iterator;
            for (string_split_iterator arg =
                     boost::make_split_iterator(uri.m_query, boost::first_finder("&", boost::is_iequal()));
                 arg != string_split_iterator();
                 ++arg) {
                static const std::string keyword = "SyncEvolution=";
                if (boost::istarts_with(*arg, keyword)) {
                    std::string params(arg->begin() + keyword.size(), arg->end());
                    for (string_split_iterator flag =
                             boost::make_split_iterator(params,
                                                        boost::first_finder(",", boost::is_iequal()));
                         flag != string_split_iterator();
                         ++flag) {
                        if (boost::iequals(*flag, "UpdateHack")) {
                            m_googleUpdateHack = true;
                        } else if (boost::iequals(*flag, "ChildHack")) {
                            m_googleChildHack = true;
                        } else if (boost::iequals(*flag, "AlarmHack")) {
                            m_googleAlarmHack = true;
                        } else if (boost::iequals(*flag, "Google")) {
                            m_googleUpdateHack =
                                m_googleChildHack =
                                m_googleAlarmHack = true;
                        } else {
                            SE_THROW(StringPrintf("unknown SyncEvolution flag %s in URL %s",
                                                  std::string(flag->begin(), flag->end()).c_str(),
                                                  m_url.c_str()));
                        }
                    }
                } else if (arg->end() != arg->begin()) {
                    SE_THROW(StringPrintf("unknown parameter %s in URL %s",
                                          std::string(arg->begin(), arg->end()).c_str(),
                                          m_url.c_str()));
                }
            }
            boost::shared_ptr<FilterConfigNode> node = m_context->getNode(WebDAVCredentialsOkay);
            m_credentialsOkay = WebDAVCredentialsOkay.getPropertyValue(*node);
        }
    }

    void setURL(const std::string url) { m_url = url; }
    virtual std::string getURL() { return m_url; }

    virtual bool verifySSLHost()
    {
        return !m_context || m_context->getSSLVerifyHost();
    }

    virtual bool verifySSLCertificate()
    {
        return !m_context || m_context->getSSLVerifyServer();
    }

    virtual std::string proxy()
    {
        if (!m_context ||
            !m_context->getUseProxy()) {
            return "";
        } else {
            return m_context->getProxyHost();
        }
    }

    virtual bool googleUpdateHack() const { return m_googleUpdateHack; }
    virtual bool googleChildHack() const { return m_googleChildHack; }
    virtual bool googleAlarmHack() const { return m_googleChildHack; }

    virtual int timeoutSeconds() const { return m_context->getRetryDuration(); }
    virtual int retrySeconds() const {
        int seconds = m_context->getRetryInterval();
        if (seconds >= 0) {
            seconds /= (120 / 5); // default: 2min => 5s
        }
        return seconds;
    }

    virtual void getCredentials(const std::string &realm,
                                std::string &username,
                                std::string &password)
    {
        if (m_context) {
            username = m_context->getSyncUsername();
            password = m_context->getSyncPassword();
        }
    }

    virtual bool getCredentialsOkay() { return m_credentialsOkay; }
    virtual void setCredentialsOkay(bool okay) {
        if (m_credentialsOkay != okay && m_context) {
            boost::shared_ptr<FilterConfigNode> node = m_context->getNode(WebDAVCredentialsOkay);
            WebDAVCredentialsOkay.setProperty(*node, okay);
            node->flush();
        }
    }

    virtual int logLevel()
    {
        return m_context ?
            m_context->getLogLevel() :
            0;
    }
};

WebDAVSource::WebDAVSource(const SyncSourceParams &params,
                           const boost::shared_ptr<Neon::Settings> &settings) :
    TrackingSyncSource(params),
    m_settings(settings)
{
    if (!m_settings) {
        m_contextSettings.reset(new ContextSettings(params.m_context));
        m_settings = m_contextSettings;
    }
}

void WebDAVSource::replaceHTMLEntities(std::string &item)
{
    while (true) {
        bool found = false;

        std::string decoded;
        size_t last = 0; // last character copied
        size_t next = 0; // next character to be looked at
        while (true) {
            next = item.find('&', next);
            size_t start = next;
            if (next == item.npos) {
                // finish decoding
                if (found) {
                    decoded.append(item, last, item.size() - last);
                }
                break;
            }
            next++;
            size_t end = next;
            while (end != item.size()) {
                char c = item[end];
                if ((c >= 'a' && c <= 'z') ||
                    (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') ||
                    (c == '#')) {
                    end++;
                } else {
                    break;
                }
            }
            if (end == item.size() || item[end] != ';') {
                // Invalid character between & and ; or no
                // proper termination? No entity, continue
                // decoding in next loop iteration.
                next = end;
                continue;
            }
            unsigned char c = 0;
            if (next < end) {
                if (item[next] == '#') {
                    // decimal or hexadecimal number
                    next++;
                    if (next < end) {
                        int base;
                        if (item[next] == 'x') {
                            // hex
                            base = 16;
                            next++;
                        } else {
                            base = 10;
                        }
                        while (next < end) {
                            unsigned char v = tolower(item[next]);
                            if (v >= '0' && v <= '9') {
                                next++;
                                c = c * base + (v - '0');
                            } else if (base == 16 && v >= 'a' && v <= 'f') {
                                next++;
                                c = c * base + (v - 'a') + 10;
                            } else {
                                // invalid character, abort scanning of this entity
                                break;
                            }
                        }
                    }
                } else {
                    // check for entities
                    struct {
                        const char *m_name;
                        unsigned char m_character;
                    } entities[] = {
                        // core entries, extend as needed...
                        { "quot", '"' },
                        { "amp", '&' },
                        { "apos", '\'' },
                        { "lt", '<' },
                        { "gt", '>' },
                        { NULL, 0 }
                    };
                    int i = 0;
                    while (true) {
                        const char *name = entities[i].m_name;
                        if (!name) {
                            break;
                        }
                        if (!item.compare(next, end - next, name)) {
                            c = entities[i].m_character;
                            next += strlen(name);
                            break;
                        }
                        i++;
                    }
                }
                if (next == end) {
                    // swallowed all characters in entity, must be valid:
                    // copy all uncopied characters plus the new one
                    found = true;
                    decoded.reserve(item.size());
                    decoded.append(item, last, start - last);
                    decoded.append(1, c);
                    last = end + 1;
                }
            }
            next = end + 1;
        }
        if (found) {
            item = decoded;
        } else {
            break;
        }
    }
}

void WebDAVSource::open()
{
    // Nothing to do here, expensive initialization is in contactServer().
}

void WebDAVSource::contactServer()
{
    if (!m_calendar.empty() &&
        m_session) {
        // we have done this work before, no need to repeat it
    }

    int timeoutSeconds = m_settings->timeoutSeconds();
    int retrySeconds = m_settings->retrySeconds();
    SE_LOG_DEBUG(this, NULL, "timout %ds, retry %ds => %s",
                 timeoutSeconds, retrySeconds,
                 (timeoutSeconds <= 0 ||
                  retrySeconds <= 0) ? "resending disabled" : "resending allowed");

    // ignore the "Request ends, status 207 class 2xx, error line:" printed by neon
    LogRedirect::addIgnoreError(", error line:");
    // ignore error messages in returned data
    LogRedirect::addIgnoreError("Read block (");

    SE_LOG_DEBUG(NULL, NULL, "using libneon %s with %s",
                 ne_version_string(), Neon::features().c_str());

    std::string username, password;
    m_contextSettings->getCredentials("", username, password);

    // If no URL was configured, then try DNS SRV lookup.
    // syncevo-webdav-lookup and at least one of the tools
    // it depends on (host, nslookup, adnshost, ...) must
    // be in the shell search path.
    //
    // Only our own m_contextSettings allows overriding the
    // URL. Not an issue, in practice it is always used.
    std::string url = m_settings->getURL();
    if (url.empty() && m_contextSettings) {
        size_t pos = username.find('@');
        if (pos == username.npos) {
            // throw authentication error to indicate that the credentials are wrong
            throwError(STATUS_UNAUTHORIZED, StringPrintf("syncURL not configured and username %s does not contain a domain", username.c_str()));
        }
        std::string domain = username.substr(pos + 1);

        FILE *in = NULL;
        try {
            Timespec startTime = Timespec::monotonic();

        retry:
            in = popen(StringPrintf("syncevo-webdav-lookup '%s' '%s'",
                                    serviceType().c_str(),
                                    domain.c_str()).c_str(),
                       "r");
            if (!in) {
                throwError("syncURL not configured and starting syncevo-webdav-lookup for DNS SRV lookup failed", errno);
            }
            // ridicuously long URLs are truncated...
            char buffer[1024];
            size_t read = fread(buffer, 1, sizeof(buffer) - 1, in);
            buffer[read] = 0;
            if (read > 0 && buffer[read - 1] == '\n') {
                read--;
            }
            buffer[read] = 0;
            m_contextSettings->setURL(buffer);
            SE_LOG_DEBUG(this, NULL, "found syncURL '%s' via DNS SRV", buffer);
            int res = pclose(in);
            in = NULL;
            switch (res) {
            case 0:
                break;
            case 2:
                throwError(StringPrintf("syncURL not configured and syncevo-webdav-lookup did not find a DNS utility to search for %s in %s", serviceType().c_str(), domain.c_str()));
                break;
            case 3:
                throwError(StringPrintf("syncURL not configured and DNS SRV search for %s in %s did not find the service", serviceType().c_str(), domain.c_str()));
                break;
            default: {
                Timespec now = Timespec::monotonic();
                if (retrySeconds > 0 &&
                    timeoutSeconds > 0) {
                    if (now < startTime + timeoutSeconds) {
                        SE_LOG_DEBUG(this, NULL, "DNS SRV search failed due to network issues, retry in %d seconds",
                                     retrySeconds);
                        Sleep(retrySeconds);
                        goto retry;
                    } else {
                        SE_LOG_INFO(this, NULL, "DNS SRV search timed out after %d seconds", timeoutSeconds);
                    }
                }

                // probably network problem
                throwError(STATUS_TRANSPORT_FAILURE, StringPrintf("syncURL not configured and DNS SRV search for %s in %s failed", serviceType().c_str(), domain.c_str()));
                break;
            }
            }
        } catch (...) {
            if (in) {
                pclose(in);
            }
            throw;
        }
    }

    // start talking to host defined by m_settings->getURL()
    m_session = Neon::Session::create(m_settings);

    // Find default calendar. Same for address book, with slightly
    // different parameters.
    //
    // Stops when:
    // - current path is calendar collection (= contains VEVENTs)
    // Gives up:
    // - when running in circles
    // - nothing else to try out
    // - tried 10 times
    // Follows:
    // - current-user-principal
    // - CalDAV calendar-home-set
    // - collections
    //
    // TODO: hrefs and redirects are assumed to be on the same host - support switching host
    // TODO: support more than one calendar. Instead of stopping at the first one,
    // scan more throroughly, then decide deterministically.
    int counter = 0;
    const int limit = 10;
    std::set<std::string> tried;
    std::list<std::string> candidates;
    std::string path = m_session->getURI().m_path;
    Neon::Session::PropfindPropCallback_t callback =
        boost::bind(&WebDAVSource::openPropCallback,
                    this, _1, _2, _3, _4);

    // With Yahoo! the initial connection often failed with 50x
    // errors.  Retrying individual requests is error prone because at
    // least one (asking for .well-known/[caldav|carddav]) always
    // results in 502. Let the PROPFIND requests be resent, but in
    // such a way that the overall discovery will never take longer
    // than the total configured timeout period.
    //
    // The PROPFIND with openPropCallback is idempotent, because it
    // will just overwrite previously found information in m_davProps.
    // Therefore resending is okay.
    Timespec finalDeadline = createDeadline(); // no resending if left empty

    while (true) {
        std::string next;

        // must normalize so that we can compare against results from server
        path = Neon::URI::normalizePath(path, true);
        SE_LOG_DEBUG(NULL, NULL, "testing %s", path.c_str());
        tried.insert(path);

        // Accessing the well-known URIs should lead to a redirect, but
        // with Yahoo! Calendar all I got was a 502 "connection refused".
        // Yahoo! Contacts also doesn't redirect. Instead on ends with
        // a Principal resource - perhaps reading that would lead further.
        //
        // So anyway, let's try the well-known URI first, but also add
        // the root path as fallback.
        if (path == "/.well-known/caldav/" ||
            path == "/.well-known/carddav/") {
            // remove trailing slash added by normalization, to be aligned with draft-daboo-srv-caldav-10
            path.resize(path.size() - 1);

            // Yahoo! Calendar returns no redirect. According to rfc4918 appendix-E,
            // a client may simply try the root path in case of such a failure,
            // which happens to work for Yahoo.
            candidates.push_back("/");
            // TODO: Google Calendar, with workarounds
            // candidates.push_back(StringPrintf("/calendar/dav/%s/user/", Neon::URI::escape(username).c_str()));
        }

        bool success = false;
        try {
            // disable resending for some known cases where it never succeeds
            Timespec deadline = finalDeadline;
            if (boost::starts_with(path, "/.well-known") &&
                m_settings->getURL().find("yahoo.com") != string::npos) {
                deadline = Timespec();
            }

            if (LoggerBase::instance().getLevel() >= Logger::DEV) {
                // First dump WebDAV "allprops" properties (does not contain
                // properties which must be asked for explicitly!). Only
                // relevant for debugging.
                SE_LOG_DEBUG(NULL, NULL, "debugging: read all WebDAV properties of %s", path.c_str());
                Neon::Session::PropfindPropCallback_t callback =
                    boost::bind(&WebDAVSource::openPropCallback,
                                this, _1, _2, _3, _4);
                m_session->propfindProp(path, 0, NULL, callback, deadline);
            }
        
            // Now ask for some specific properties of interest for us.
            // Using CALDAV:allprop would be nice, but doesn't seem to
            // be possible with Neon.
            //
            // The "current-user-principal" is particularly relevant,
            // because it leads us from
            // "/.well-known/[carddav/caldav]" (or whatever that
            // redirected to) to the current user and its
            // "[calendar/addressbook]-home-set".
            //
            // Apple Calendar Server only returns that information if
            // we force authorization to be used. Otherwise it returns
            // <current-user-principal>
            //    <unauthenticated/>
            // </current-user-principal>
            //
            // We send valid credentials here, using Basic authorization.
            // The rationale is that this cuts down on the number of
            // requests for https while still being secure. For
            // http the setup already is insecure if the transport
            // isn't trusted (sends PIM data as plain text).
            //
            // See also:
            // http://tools.ietf.org/html/rfc4918#appendix-E
            // http://lists.w3.org/Archives/Public/w3c-dist-auth/2005OctDec/0243.html
            // http://thread.gmane.org/gmane.comp.web.webdav.neon.general/717/focus=719
            std::string user, pw;
            m_settings->getCredentials("", user, pw);
            m_session->forceAuthorization(user, pw);
            m_davProps.clear();
            static const ne_propname caldav[] = {
                // WebDAV ACL
                { "DAV:", "alternate-URI-set" },
                { "DAV:", "principal-URL" },
                { "DAV:", "current-user-principal" },
                { "DAV:", "group-member-set" },
                { "DAV:", "group-membership" },
                { "DAV:", "displayname" },
                { "DAV:", "resourcetype" },
                // CalDAV
                { "urn:ietf:params:xml:ns:caldav", "calendar-home-set" },
                { "urn:ietf:params:xml:ns:caldav", "calendar-description" },
                { "urn:ietf:params:xml:ns:caldav", "calendar-timezone" },
                { "urn:ietf:params:xml:ns:caldav", "supported-calendar-component-set" },
                { "urn:ietf:params:xml:ns:caldav", "supported-calendar-data" },
                { "urn:ietf:params:xml:ns:caldav", "max-resource-size" },
                { "urn:ietf:params:xml:ns:caldav", "min-date-time" },
                { "urn:ietf:params:xml:ns:caldav", "max-date-time" },
                { "urn:ietf:params:xml:ns:caldav", "max-instances" },
                { "urn:ietf:params:xml:ns:caldav", "max-attendees-per-instance" },
                // CardDAV
                { "urn:ietf:params:xml:ns:carddav", "addressbook-home-set" },
                { "urn:ietf:params:xml:ns:carddav", "principal-address" },
                { "urn:ietf:params:xml:ns:carddav", "addressbook-description" },
                { "urn:ietf:params:xml:ns:carddav", "supported-address-data" },
                { "urn:ietf:params:xml:ns:carddav", "max-resource-size" },
                { NULL, NULL }
            };
            SE_LOG_DEBUG(NULL, NULL, "read relevant properties of %s", path.c_str());
            m_session->propfindProp(path, 0, caldav, callback, deadline);
            success = true;
        } catch (const Neon::RedirectException &ex) {
            // follow to new location
            Neon::URI next = Neon::URI::parse(ex.getLocation());
            Neon::URI old = m_session->getURI();
            if (next.m_scheme != old.m_scheme ||
                next.m_host != old.m_host ||
                next.m_port != old.m_port) {
                SE_LOG_DEBUG(NULL, NULL, "ignore redirection to different server (not implemented): %s",
                             ex.getLocation().c_str());
                if (candidates.empty()) {
                    // nothing left to try, bail out with this error
                    throw;
                }
            } else {
                candidates.push_front(next.m_path);
            }
        } catch (const TransportStatusException &ex) {
            SE_LOG_DEBUG(NULL, NULL, "TransportStatusException: %s", ex.what());
            if (ex.syncMLStatus() == 404) {
                // We're actually looking at an authentication error: the path to the calendar has
                // not been found, so the username was wrong. Let's hijack the error message and
                // code of the exception by throwing a new one.
                string descr = StringPrintf("Path not found: %s. Is the username '%s' correct?",
                                            path.c_str(), username.c_str());
                int code = 401;
                SE_THROW_EXCEPTION_STATUS(TransportStatusException, descr, SyncMLStatus(code));
            }
        } catch (const Exception &ex) {
            if (candidates.empty()) {
                // nothing left to try, bail out with this error
                throw;
            } else {
                // ignore the error (whatever it was!), try next
                // candidate; needed to handle 502 "Connection
                // refused" for /.well-known/caldav/ from Yahoo!
                // Calendar
                SE_LOG_DEBUG(NULL, NULL, "ignore error for URI candidate: %s", ex.what());
            }
        }

        if (success) {
            if (m_davProps.find(path) == m_davProps.end()) {
                // No reply for requested path? Happens with Yahoo Calendar server,
                // which returns information about "/dav" when asked about "/".
                // Move to that path.
                if (!m_davProps.empty()) {
                    string newpath = m_davProps.begin()->first;
                    SE_LOG_DEBUG(NULL, NULL, "use properties for '%s' instead of '%s'",
                                 newpath.c_str(), path.c_str());
                    path = newpath;
                }
            }
            StringMap &props = m_davProps[path];
            if (typeMatches(props)) {
                // found it
                break;
            }

            // find next path:
            // prefer CardDAV:calendar-home-set or CalDAV:addressbook-home-set
            std::string home = extractHREF(props[homeSetProp()]);
            if (!home.empty() &&
                tried.find(home) == tried.end()) {
                next = home;
                SE_LOG_DEBUG(NULL, NULL, "follow home-set property to %s", next.c_str());
            }
            // alternatively, follow principal URL
            if (next.empty()) {
                std::string principal = extractHREF(props["DAV::current-user-principal"]);
                if (!principal.empty() &&
                    tried.find(principal) == tried.end()) {
                    next = principal;
                    SE_LOG_DEBUG(NULL, NULL, "follow current-user-prinicipal to %s", next.c_str());
                }
            }
            // finally, recursively descend into collections
            if (next.empty()) {
                const std::string &type = props["DAV::resourcetype"];
                if (type.find("<DAV:collection></DAV:collection>") != type.npos) {
                    // List members and find new candidates.
                    // Yahoo! Calendar does not return resources contained in /dav/<user>/Calendar/
                    // if <allprops> is used. Properties must be requested explicitly.
                    SE_LOG_DEBUG(NULL, NULL, "list items in %s", path.c_str());
                    static const ne_propname props[] = {
                        { "DAV:", "displayname" },
                        { "DAV:", "resourcetype" },
                        { "urn:ietf:params:xml:ns:caldav", "calendar-home-set" },
                        { "urn:ietf:params:xml:ns:caldav", "calendar-description" },
                        { "urn:ietf:params:xml:ns:caldav", "calendar-timezone" },
                        { "urn:ietf:params:xml:ns:caldav", "supported-calendar-component-set" },
                        { "urn:ietf:params:xml:ns:carddav", "addressbook-home-set" },
                        { "urn:ietf:params:xml:ns:carddav", "addressbook-description" },
                        { "urn:ietf:params:xml:ns:carddav", "supported-address-data" },
                        { NULL, NULL }
                    };
                    m_davProps.clear();
                    m_session->propfindProp(path, 1, props, callback, finalDeadline);
                    BOOST_FOREACH(Props_t::value_type &entry, m_davProps) {
                        const std::string &sub = entry.first;
                        const std::string &subType = entry.second["DAV::resourcetype"];
                        // new candidates are:
                        // - untested
                        // - not already a candidate
                        // - a resource
                        // - not shared ("global-addressbook" in Apple Calendar Server),
                        //   because these are unlikely to be the right "default" collection
                        //
                        // Trying to prune away collections here which are not of the
                        // right type *and* cannot contain collections of the right
                        // type (example: Apple Calendar Server "inbox" under
                        // calendar-home-set URL with type "CALDAV:schedule-inbox") requires
                        // knowledge not current provided by derived classes. TODO (?).
                        if (tried.find(sub) == tried.end() &&
                            std::find(candidates.begin(), candidates.end(), sub) == candidates.end() &&
                            subType.find("<DAV:collection></DAV:collection>") != subType.npos &&
                            subType.find("<http://calendarserver.org/ns/shared") == subType.npos) {
                            // insert before other candidates (depth-first search)
                            candidates.push_front(sub);
                            if (next.empty() && typeMatches(entry.second)) {
                                // try this one before or all other candidates
                                next = sub;
                            }
                            SE_LOG_DEBUG(NULL, NULL, "new candidate: %s", sub.c_str());
                        }
                    }
                }
            }
        }

        if (next.empty()) {
            // use next candidate
            if (candidates.empty()) {
                throwError(StringPrintf("no collection found in %s", m_settings->getURL().c_str()));
            }
            next = candidates.front();
            candidates.pop_front();
            SE_LOG_DEBUG(NULL, NULL, "follow candidate %s", next.c_str());
        }

        counter++;
        if (counter > limit) {
            throwError(StringPrintf("giving up search for collection after %d attempts", limit));
        }
        path = next;
    }

    // Pick final path.
    m_calendar = m_session->getURI();
    m_calendar.m_path = path;
    SE_LOG_DEBUG(NULL, NULL, "picked final path %s", m_calendar.m_path.c_str());

    // Check some server capabilities. Purely informational at this
    // point, doesn't have to succeed either (Google 401 throttling
    // workaround not active here, so it may really fail!).
#ifdef HAVE_LIBNEON_OPTIONS
    if (LoggerBase::instance().getLevel() >= Logger::DEV) {
        try {
            SE_LOG_DEBUG(NULL, NULL, "read capabilities of %s", m_calendar.toURL().c_str());
            m_session->startOperation("OPTIONS", Timespec());
            int caps = m_session->options(path);
            static const Flag descr[] = {
                { NE_CAP_DAV_CLASS1, "Class 1 WebDAV (RFC 2518)" },
                { NE_CAP_DAV_CLASS2, "Class 2 WebDAV (RFC 2518)" },
                { NE_CAP_DAV_CLASS3, "Class 3 WebDAV (RFC 4918)" },
                { NE_CAP_MODDAV_EXEC, "mod_dav 'executable' property" },
                { NE_CAP_DAV_ACL, "WebDAV ACL (RFC 3744)" },
                { NE_CAP_VER_CONTROL, "DeltaV version-control" },
                { NE_CAP_CO_IN_PLACE, "DeltaV checkout-in-place" },
                { NE_CAP_VER_HISTORY, "DeltaV version-history" },
                { NE_CAP_WORKSPACE, "DeltaV workspace" },
                { NE_CAP_UPDATE, "DeltaV update" },
                { NE_CAP_LABEL, "DeltaV label" },
                { NE_CAP_WORK_RESOURCE, "DeltaV working-resouce" },
                { NE_CAP_MERGE, "DeltaV merge" },
                { NE_CAP_BASELINE, "DeltaV baseline" },
                { NE_CAP_ACTIVITY, "DeltaV activity" },
                { NE_CAP_VC_COLLECTION, "DeltaV version-controlled-collection" },
                { 0, NULL }
            };
            SE_LOG_DEBUG(NULL, NULL, "%s WebDAV capabilities: %s",
                         m_session->getURL().c_str(),
                         Flags2String(caps, descr).c_str());
        } catch (...) {
            Exception::handle();
        }
    }
#endif // HAVE_LIBNEON_OPTIONS
}

std::string WebDAVSource::extractHREF(const std::string &propval)
{
    // all additional parameters after opening resp. closing tag
    static const std::string hrefStart = "<DAV:href";
    static const std::string hrefEnd = "</DAV:href";
    size_t start = propval.find(hrefStart);
    start = propval.find('>', start);
    if (start != propval.npos) {
        start++;
        size_t end = propval.find(hrefEnd, start);
        if (end != propval.npos) {
            return propval.substr(start, end - start);
        }
    }
    return "";
}

void WebDAVSource::openPropCallback(const Neon::URI &uri,
                                    const ne_propname *prop,
                                    const char *value,
                                    const ne_status *status)
{
    // TODO: recognize CALDAV:calendar-timezone and use it for local time conversion of events
    std::string name;
    if (prop->nspace) {
        name = prop->nspace;
    }
    name += ":";
    name += prop->name;
    if (value) {
        m_davProps[uri.m_path][name] = value;
        boost::trim_if(m_davProps[uri.m_path][name],
                       boost::is_space());
    }
}

bool WebDAVSource::isEmpty()
{
    contactServer();

    // listing all items is relatively efficient, let's use that
    // TODO: use truncated result search
    RevisionMap_t revisions;
    listAllItems(revisions);
    return revisions.empty();
}

void WebDAVSource::close()
{
    m_session.reset();
}

WebDAVSource::Databases WebDAVSource::getDatabases()
{
    Databases result;

    // TODO: scan for right collections
    result.push_back(Database("select database via relative URI",
                              "<path>"));
    return result;
}

void WebDAVSource::getSynthesisInfo(SynthesisInfo &info,
                                    XMLConfigFragments &fragments)
{
    TrackingSyncSource::getSynthesisInfo(info, fragments);
    // TODO: instead of identifying the peer based on the
    // session URI, use some information gathered about
    // it during open()
    if (m_session) {
        string host = m_session->getURI().m_host;
        if (host.find("google") != host.npos) {
            info.m_backendRule = "GOOGLE";
            fragments.m_remoterules["GOOGLE"] =
                "      <remoterule name='GOOGLE'>\n"
                "          <deviceid>none</deviceid>\n"
                // enable extensions, just in case (not relevant yet for calendar)
                "          <include rule=\"ALL\"/>\n"
                "      </remoterule>";
        } else if (host.find("yahoo") != host.npos) {
            info.m_backendRule = "YAHOO";
            fragments.m_remoterules["YAHOO"] =
                "      <remoterule name='YAHOO'>\n"
                "          <deviceid>none</deviceid>\n"
                // Yahoo! Contacts reacts with a "500 - internal server error"
                // to an empty X-GENDER property. In general, empty properties
                // should never be necessary in CardDAV and CalDAV, because
                // sent items conceptually replace the one on the server, so
                // disable them all.
                "          <noemptyproperties>yes</noemptyproperties>\n"
                // BDAY is ignored if it has the compact 19991231 instead of
                // 1999-12-31, although both are valid.
                "          <include rule='EXTENDED-DATE-FORMAT'/>\n"
                // Yahoo accepts extensions, so send them. However, it
                // doesn't seem to store the X-EVOLUTION-UI-SLOT parameter
                // extensions.
                "          <include rule=\"ALL\"/>\n"
                "      </remoterule>";
        } else {
            // fallback: generic CalDAV/CardDAV, with all properties
            // enabled (for example, X-EVOLUTION-UI-SLOT)
            info.m_backendRule = "WEBDAV";
            fragments.m_remoterules["WEBDAV"] =
                "      <remoterule name='WEBDAV'>\n"
                "          <deviceid>none</deviceid>\n"
                "          <include rule=\"ALL\"/>\n"
                "      </remoterule>";
        }
        SE_LOG_DEBUG(this, NULL, "using data conversion rules for '%s'", info.m_backendRule.c_str());
    }
}

static const ne_propname getetag[] = {
    { "DAV:", "getetag" },
    { "DAV:", "resourcetype" },
    { NULL, NULL }
};

void WebDAVSource::listAllItems(RevisionMap_t &revisions)
{
    bool failed = false;
    Timespec deadline = createDeadline();
    m_session->propfindURI(m_calendar.m_path, 1, getetag,
                           boost::bind(&WebDAVSource::listAllItemsCallback,
                                       this, _1, _2, boost::ref(revisions),
                                       boost::ref(failed)),
                           deadline);
    if (failed) {
        SE_THROW("incomplete listing of all items");
    }
}

void WebDAVSource::listAllItemsCallback(const Neon::URI &uri,
                                        const ne_prop_result_set *results,
                                        RevisionMap_t &revisions,
                                        bool &failed)
{
    static const ne_propname prop = {
        "DAV:", "getetag"
    };
    static const ne_propname resourcetype = {
        "DAV:", "resourcetype"
    };

    const char *type = ne_propset_value(results, &resourcetype);
    if (type && strstr(type, "<DAV:collection></DAV:collection>")) {
        // skip collections
        return;
    }

    std::string uid = path2luid(uri.m_path);
    if (uid.empty()) {
        // skip collection itself (should have been detected as collection already)
        return;
    }

    const char *etag = ne_propset_value(results, &prop);
    if (etag) {
        std::string rev = ETag2Rev(etag);
        SE_LOG_DEBUG(NULL, NULL, "item %s = rev %s",
                     uid.c_str(), rev.c_str());
        revisions[uid] = rev;
    } else {
        failed = true;
        SE_LOG_ERROR(NULL, NULL,
                     "%s: %s",
                     uri.toURL().c_str(),
                     Neon::Status2String(ne_propset_status(results, &prop)).c_str());
    }
}

std::string WebDAVSource::path2luid(const std::string &path)
{
    if (boost::starts_with(path, m_calendar.m_path)) {
        return Neon::URI::unescape(path.substr(m_calendar.m_path.size()));
    } else {
        return path;
    }
}

std::string WebDAVSource::luid2path(const std::string &luid)
{
    if (boost::starts_with(luid, "/")) {
        return luid;
    } else {
        return m_calendar.resolve(Neon::URI::escape(luid)).m_path;
    }
}

void WebDAVSource::readItem(const string &uid, std::string &item, bool raw)
{
    Timespec deadline = createDeadline();
    m_session->startOperation("GET", deadline);
    while (true) {
        item.clear();
        Neon::Request req(*m_session, "GET", luid2path(uid),
                          "", item);
        // useful with CardDAV: server might support more than vCard 3.0, but we don't
        req.addHeader("Accept", contentType());
        if (req.run()) {
            break;
        }
    }
}

TrackingSyncSource::InsertItemResult WebDAVSource::insertItem(const string &uid, const std::string &item, bool raw)
{
    std::string new_uid;
    std::string rev;
    bool update = false;  /* true if adding item was turned into update */

    Timespec deadline = createDeadline(); // no resending if left empty
    m_session->startOperation("PUT", deadline);
    std::string result;
    int counter = 0;
 retry:
    counter++;
    result = "";
    if (uid.empty()) {
        // Pick a resource name (done by derived classes, by default random),
        // catch unexpected conflicts via If-None-Match: *.
        std::string buffer;
        const std::string *data = createResourceName(item, buffer, new_uid);
        Neon::Request req(*m_session, "PUT", luid2path(new_uid),
                          *data, result);
        // Clearing the idempotent flag would allow us to clearly
        // distinguish between a connection error (no changes made
        // on server) and a server failure (may or may not have
        // changed something) because it'll close the connection
        // first.
        //
        // But because we are going to try resending
        // the PUT anyway in case of 5xx errors we might as well 
        // treat it like an idempotent request (which it is,
        // in a way, because we'll try to get our data onto
        // the server no matter what) and keep reusing an
        // existing connection.
        // req.setFlag(NE_REQFLAG_IDEMPOTENT, 0);

        // For this to work we must allow the server to overwrite
        // an item that we might have created before. Don't allow
        // that in the first attempt.
        if (counter == 1) {
            req.addHeader("If-None-Match", "*");
        }
        req.addHeader("Content-Type", contentType().c_str());
        if (!req.run()) {
            goto retry;
        }
        SE_LOG_DEBUG(NULL, NULL, "add item status: %s",
                     Neon::Status2String(req.getStatus()).c_str());
        switch (req.getStatusCode()) {
        case 204:
            // stored, potentially in a different resource than requested
            // when the UID was recognized
            break;
        case 201:
            // created
            break;
        default:
            SE_THROW_EXCEPTION_STATUS(TransportStatusException,
                                      std::string("unexpected status for insert: ") +
                                      Neon::Status2String(req.getStatus()),
                                      SyncMLStatus(req.getStatus()->code));
            break;
        }
        rev = getETag(req);
        std::string real_luid = getLUID(req);
        if (!real_luid.empty()) {
            // Google renames the resource automatically to something of the form
            // <UID>.ics. Interestingly enough, our 1234567890!@#$%^&*()<>@dummy UID
            // test case leads to a resource path which Google then cannot find
            // via CalDAV. client-test must run with CLIENT_TEST_SIMPLE_UID=1...
            SE_LOG_DEBUG(NULL, NULL, "new item mapped to %s", real_luid.c_str());
            new_uid = real_luid;
            // TODO: find a better way of detecting unexpected updates.
            // update = true;
        } else if (!rev.empty()) {
            // Yahoo Contacts returns an etag, but no href. For items
            // that were really created as requested, that's okay. But
            // Yahoo Contacts silently merges the new contact with an
            // existing one, presumably if it is "similar" enough. The
            // web interface allows creating identical contacts
            // multiple times; not so CardDAV.  We are not even told
            // the path of that other contact...  Detect this by
            // checking whether the item really exists.
            RevisionMap_t revisions;
            bool failed = false;
            m_session->propfindURI(luid2path(new_uid), 0, getetag,
                                   boost::bind(&WebDAVSource::listAllItemsCallback,
                                               this, _1, _2, boost::ref(revisions),
                                               boost::ref(failed)),
                                   deadline);
            // Turns out we get a result for our original path even in
            // the case of a merge, although the original path is not
            // listed when looking at the collection.  Let's use that
            // to return the "real" uid to our caller.
            if (revisions.size() == 1 &&
                revisions.begin()->first != new_uid) {
                SE_LOG_DEBUG(NULL, NULL, "%s mapped to %s by peer",
                             new_uid.c_str(),
                             revisions.begin()->first.c_str());
                new_uid = revisions.begin()->first;
                update = true;
            }
        }
    } else {
        new_uid = uid;
        std::string buffer;
        const std::string *data = setResourceName(item, buffer, new_uid);
        Neon::Request req(*m_session, "PUT", luid2path(new_uid),
                          *data, result);
        // See above for discussion of idempotent and PUT.
        // req.setFlag(NE_REQFLAG_IDEMPOTENT, 0);
        req.addHeader("Content-Type", contentType());
        // TODO: match exactly the expected revision, aka ETag,
        // or implement locking. Note that the ETag might not be
        // known, for example in this case:
        // - PUT succeeds
        // - PROPGET does not
        // - insertItem() fails
        // - Is retried? Might need slow sync in this case!
        //
        // req.addHeader("If-Match", etag);
        if (!req.run()) {
            goto retry;
        }
        SE_LOG_DEBUG(NULL, NULL, "update item status: %s",
                     Neon::Status2String(req.getStatus()).c_str());
        switch (req.getStatusCode()) {
        case 204:
            // the expected outcome, as we were asking for an overwrite
            break;
        case 201:
            // Huh? Shouldn't happen, but Google sometimes reports it
            // even when updating an item. Accept it.
            // SE_THROW("unexpected creation instead of update");
            break;
        default:
            SE_THROW_EXCEPTION_STATUS(TransportStatusException,
                                      std::string("unexpected status for update: ") +
                                      Neon::Status2String(req.getStatus()),
                                      SyncMLStatus(req.getStatus()->code));
            break;
        }
        rev = getETag(req);
        std::string real_luid = getLUID(req);
        if (!real_luid.empty() && real_luid != new_uid) {
            SE_THROW(StringPrintf("updating item: real luid %s does not match old luid %s",
                                  real_luid.c_str(), new_uid.c_str()));
        }
    }

    if (rev.empty()) {
        // Server did not include etag header. Must request it
        // explicitly (leads to race condition!). Google Calendar
        // assigns a new ETag even if the body has not changed,
        // so any kind of caching of ETag would not work either.
        bool failed = false;
        RevisionMap_t revisions;
        m_session->propfindURI(luid2path(new_uid), 0, getetag,
                               boost::bind(&WebDAVSource::listAllItemsCallback,
                                           this, _1, _2, boost::ref(revisions),
                                           boost::ref(failed)),
                               deadline);
        rev = revisions[new_uid];
        if (failed || rev.empty()) {
            SE_THROW("could not retrieve ETag");
        }
    }

    return InsertItemResult(new_uid, rev, update);
}

std::string WebDAVSource::ETag2Rev(const std::string &etag)
{
    std::string res = etag;
    if (boost::starts_with(res, "W/")) {
        res.erase(0, 2);
    }
    if (res.size() >= 2) {
        res = res.substr(1, res.size() - 2);
    }
    return res;
}

std::string WebDAVSource::getLUID(Neon::Request &req)
{
    std::string location = req.getResponseHeader("Location");
    if (location.empty()) {
        return location;
    } else {
        return path2luid(Neon::URI::parse(location).m_path);
    }
}

Timespec WebDAVSource::createDeadline() const
{
    int timeoutSeconds = m_settings->timeoutSeconds();
    int retrySeconds = m_settings->retrySeconds();
    if (timeoutSeconds > 0 &&
        retrySeconds > 0) {
        return Timespec::monotonic() + timeoutSeconds;
    } else {
        return Timespec();
    }
}

void WebDAVSource::removeItem(const string &uid)
{
    Timespec deadline = createDeadline();
    m_session->startOperation("DELETE", deadline);
    std::string item, result;
    boost::scoped_ptr<Neon::Request> req;
    while (true) {
        req.reset(new Neon::Request(*m_session, "DELETE", luid2path(uid),
                                    item, result));
        // TODO: match exactly the expected revision, aka ETag,
        // or implement locking.
        // req.addHeader("If-Match", etag);
        if (req->run()) {
            break;
        }
    }
    SE_LOG_DEBUG(NULL, NULL, "remove item status: %s",
                 Neon::Status2String(req->getStatus()).c_str());
    switch (req->getStatusCode()) {
    case 204:
        // the expected outcome
        break;
    case 404:
        // possibly already removed, ignore
        SE_LOG_DEBUG(this, NULL, "404 - already removed?");
        break;
    default:
        SE_THROW_EXCEPTION_STATUS(TransportStatusException,
                                  std::string("unexpected status for removal: ") +
                                  Neon::Status2String(req->getStatus()),
                                  SyncMLStatus(req->getStatus()->code));
        break;
    }
}

#endif /* ENABLE_DAV */

SE_END_CXX


#ifdef ENABLE_MODULES
# include "WebDAVSourceRegister.cpp"
#endif
