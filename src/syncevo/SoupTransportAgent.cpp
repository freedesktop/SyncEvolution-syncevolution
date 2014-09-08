/*
 * Copyright (C) 2009 Intel Corporation
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

#include <syncevo/SoupTransportAgent.h>

#ifdef ENABLE_LIBSOUP

#include <algorithm>
#include <libsoup/soup-status.h>
#include <syncevo/Logging.h>

#ifdef HAVE_LIBSOUP_SOUP_GNOME_FEATURES_H
#include <libsoup/soup-gnome-features.h>
#endif

#include <syncevo/declarations.h>
SE_BEGIN_CXX

SoupTransportAgent::SoupTransportAgent(GMainLoop *loop) :
    m_verifySSL(false),
    m_session(soup_session_async_new()),
    m_loop(loop ?
           g_main_loop_ref(loop) :
           g_main_loop_new(NULL, TRUE),
           "Soup main loop"),
    m_status(INACTIVE),
    m_timeoutSeconds(0),
    m_response(0)
{
#ifdef HAVE_LIBSOUP_SOUP_GNOME_FEATURES_H
    // use default GNOME proxy settings
    soup_session_add_feature_by_type(m_session.get(), SOUP_TYPE_PROXY_RESOLVER_GNOME);
#endif
}

SoupTransportAgent::~SoupTransportAgent()
{
    if (m_session.get()) {
        // ensure that no callbacks for this session will be triggered
        // in the future, they would use a stale pointer to this agent
        // instance
        soup_session_abort(m_session.get());
    }
}

void SoupTransportAgent::setURL(const std::string &url)
{
    m_URL = url;
}

void SoupTransportAgent::setProxy(const std::string &proxy)
{
    if (!proxy.empty()) {
        eptr<SoupURI, SoupURI, GLibUnref> uri(soup_uri_new(proxy.c_str()), "Proxy URI");
        g_object_set(m_session.get(),
                     SOUP_SESSION_PROXY_URI, uri.get(),
                     NULL);
    } else {
        g_object_set(m_session.get(),
                     SOUP_SESSION_PROXY_URI, NULL,
                     NULL);
    }
}

void SoupTransportAgent::setProxyAuth(const std::string &user, const std::string &password)
{
    /**
     * @TODO: handle "authenticate" signal for both proxy and HTTP server
     * (https://sourceforge.net/tracker/index.php?func=detail&aid=2056162&group_id=146288&atid=764733).
     *
     * Proxy authentication is available, but still needs to be hooked up
     * with libsoup. Should we make this interactive? Needs an additional
     * API for TransportAgent into caller.
     *
     * HTTP authentication is not available.
     */
    m_proxyUser = user;
    m_proxyPassword = password;
}

void SoupTransportAgent::setSSL(const std::string &cacerts,
                                bool verifyServer,
                                bool verifyHost)
{
    m_verifySSL = verifyServer || verifyHost;
    m_cacerts = cacerts;
}

void SoupTransportAgent::setContentType(const std::string &type)
{
    m_contentType = type;
}

void SoupTransportAgent::setUserAgent(const std::string &agent)
{
    g_object_set(m_session.get(),
                 SOUP_SESSION_USER_AGENT, agent.c_str(),
                 NULL);
}

void SoupTransportAgent::setTimeout(int seconds)
{
    m_timeoutSeconds = seconds;
}

void SoupTransportAgent::send(const char *data, size_t len)
{
    // ownership is transferred to libsoup in soup_session_queue_message()
    eptr<SoupMessage, GObject> message(soup_message_new("POST", m_URL.c_str()));
    if (!message.get()) {
        SE_THROW_EXCEPTION(TransportException, "could not allocate SoupMessage");
    }

    // use CA certificates if available and needed,
    // otherwise let soup use system default certificates
    if (m_verifySSL) {
        if (!m_cacerts.empty()) {
            g_object_set(m_session.get(), SOUP_SESSION_SSL_CA_FILE, m_cacerts.c_str(), NULL);
        }
    }

    soup_message_set_request(message.get(), m_contentType.c_str(),
                             SOUP_MEMORY_TEMPORARY, data, len);
    m_status = ACTIVE;
    if (m_timeoutSeconds) {
        m_message = message.get();
        m_timeoutEventSource = g_timeout_add_seconds(m_timeoutSeconds, TimeoutCallback, static_cast<gpointer> (this));
    }
    soup_session_queue_message(m_session.get(), message.release(),
                               SessionCallback, static_cast<gpointer>(this));
}

void SoupTransportAgent::cancel()
{
    m_status = CANCELED;
    soup_session_abort(m_session.get());
    if(g_main_loop_is_running(m_loop.get()))
      g_main_loop_quit(m_loop.get());
}

TransportAgent::Status SoupTransportAgent::wait(bool noReply)
{
    if (!m_failure.empty()) {
        std::string failure;
        std::swap(failure, m_failure);
        SE_THROW_EXCEPTION(TransportException, failure);
    }

    switch (m_status) {
    case CLOSED:
        return CLOSED;
        break;
    case ACTIVE:
        // block in main loop until our HandleSessionCallback() stops the loop
        g_main_loop_run(m_loop.get());
        break;
    default:
        break;
    }

    /** For a canceled message, does not throw exception, just print a warning, the 
     * upper layer may decide to retry
     */
    if(m_status == TIME_OUT || m_status == FAILED){
        std::string failure;
        std::swap(failure, m_failure);
        SE_LOG_INFO(NULL, "SoupTransport Failure: %s", failure.c_str());
    }
    if (!m_failure.empty()) {
        std::string failure;
        std::swap(failure, m_failure);
        SE_THROW_EXCEPTION(TransportException, failure);
    }

    m_timeoutEventSource.set(0);
    return m_status;
}

void SoupTransportAgent::getReply(const char *&data, size_t &len, std::string &contentType)
{
    if (m_response) {
        data = m_response->data;
        len = m_response->length;
        contentType = m_responseContentType;
    } else {
        data = NULL;
        len = 0;
    }
}

void SoupTransportAgent::SessionCallback(SoupSession *session,
                                         SoupMessage *msg,
                                         gpointer user_data)
{
    static_cast<SoupTransportAgent *>(user_data)->HandleSessionCallback(session, msg);
}

void SoupTransportAgent::HandleSessionCallback(SoupSession *session,
                                               SoupMessage *msg)
{
    // keep a reference to the data 
    m_responseContentType = "";
    if (msg->response_body) {
        m_response = soup_message_body_flatten(msg->response_body);
        const char *soupContentType = soup_message_headers_get(msg->response_headers,
                                                               "Content-Type");
        if (soupContentType) {
            m_responseContentType = soupContentType;
        }
    } else {
        m_response = NULL;
    }
    if (msg->status_code != 200) {
        m_failure = m_URL;
        m_failure += " via libsoup: ";
        m_failure += msg->reason_phrase ? msg->reason_phrase : "failed";
        m_status = FAILED;

        if (m_responseContentType.find("text") != std::string::npos) {
            SE_LOG_DEBUG(NULL, "unexpected HTTP response: status %d/%s, content type %s, body:\n%.*s",
                         msg->status_code, msg->reason_phrase ? msg->reason_phrase : "<no reason>",
                         m_responseContentType.c_str(),
                         m_response ? (int)m_response->length : 0,
                         m_response ? m_response->data : "");
        }
    } else {
        m_status = GOT_REPLY;
    }

    g_main_loop_quit(m_loop.get());
}

gboolean SoupTransportAgent::processCallback()
{
    //stop the message processing and mark status as timeout
    guint message_status = SOUP_STATUS_CANCELLED;
    soup_session_cancel_message(m_session.get(), m_message, message_status);
    m_status = TIME_OUT;
    return FALSE;
}

gboolean SoupTransportAgent::TimeoutCallback(gpointer transport)
{
    SoupTransportAgent * sTransport = static_cast<SoupTransportAgent *>(transport);
    return sTransport->processCallback();
}

SE_END_CXX

#endif // ENABLE_LIBSOUP

