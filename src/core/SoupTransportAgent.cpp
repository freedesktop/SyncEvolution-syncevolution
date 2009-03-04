/*
 * Copyright (C) 2009 Patrick Ohly
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "SoupTransportAgent.h"

#ifdef ENABLE_LIBSOUP

#include <algorithm>

namespace SyncEvolution {

SoupTransportAgent::SoupTransportAgent(GMainLoop *loop) :
    m_session(soup_session_async_new()),
    m_loop(loop ?
           loop :
           g_main_loop_new(NULL, TRUE),
           "Soup main loop"),
    m_status(INACTIVE),
    m_response(NULL)
{
}

SoupTransportAgent::~SoupTransportAgent()
{
}

void SoupTransportAgent::setURL(const std::string &url)
{
    m_URL = url;
}

void SoupTransportAgent::setProxy(const std::string &proxy)
{
    eptr<SoupURI, SoupURI, GLibUnref> uri(soup_uri_new(proxy.c_str()), "Proxy URI");
    g_object_set(m_session.get(),
                 SOUP_SESSION_PROXY_URI, uri.get(),
                 NULL);
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

void SoupTransportAgent::send(const char *data, size_t len)
{
    // ownership is transferred to libsoup in soup_session_queue_message()
    SoupMessage *message = soup_message_new("POST", m_URL.c_str());
    if (!message) {
        SE_THROW_EXCEPTION(TransportException, "could not allocate SoupMessage");
    }
    soup_message_set_request(message, m_contentType.c_str(),
                             SOUP_MEMORY_TEMPORARY, data, len);
    m_status = ACTIVE;
    soup_session_queue_message(m_session.get(), message,
                               SessionCallback, static_cast<gpointer>(this));
}

void SoupTransportAgent::cancel()
{
    /** @TODO: implement if needed */
}

TransportAgent::Status SoupTransportAgent::wait()
{
    if (!m_failure.empty()) {
        std::string failure;
        std::swap(failure, m_failure);
        SE_THROW_EXCEPTION(TransportException, failure);
    }

    switch (m_status) {
    case ACTIVE:
        // block in main loop until our HandleSessionCallback() stops the loop
        g_main_loop_run(m_loop.get());
        break;
    default:
        break;
    }

    if (!m_failure.empty()) {
        std::string failure;
        std::swap(failure, m_failure);
        SE_THROW_EXCEPTION(TransportException, failure);
    }

    return m_status;
}

void SoupTransportAgent::getReply(const char *&data, size_t &len)
{
    if (m_response) {
        data = m_response->data;
        len = m_response->length;
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
    if (msg->response_body) {
        m_response = soup_message_body_flatten(msg->response_body);
    } else {
        m_response = NULL;
    }
    if (msg->status_code != 200) {
        m_failure = msg->reason_phrase ? msg->reason_phrase : "";
        m_status = FAILED;
    } else {
        m_status = GOT_REPLY;
    }

    g_main_loop_quit(m_loop.get());
}

} // namespace SyncEvolution

#endif // ENABLE_LIBSOUP
