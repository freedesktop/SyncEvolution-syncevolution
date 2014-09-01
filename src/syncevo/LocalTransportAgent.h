/*
 * Copyright (C) 2010 Patrick Ohly
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

#ifndef INCL_LOCALTRANSPORTAGENT
#define INCL_LOCALTRANSPORTAGENT

#include "config.h"
#include <syncevo/TransportAgent.h>
#include <syncevo/SyncML.h>
#include <syncevo/SmartPtr.h>
#include <syncevo/GLibSupport.h>
#include <syncevo/SyncConfig.h>
#include <syncevo/ForkExec.h>
// This needs to be defined before including gdbus-cxx-bridge.h!
#define DBUS_CXX_EXCEPTION_HANDLER SyncEvo::SyncEvoHandleException
#include "gdbus-cxx-bridge.h"
#include <string>
#include <stdint.h>
#include <pcrecpp.h>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

class SyncContext;

/**
 * The main() function of the local transport helper.
 * Implements the child side of local sync.
 */
int LocalTransportMain(int argc, char **argv);

// internal in LocalTransportAgent.cpp
class LocalTransportChild;

/**
 * message send/receive with a forked process as peer
 *
 * Uses pipes to send a message and then get the response.
 * Limited to server forking the client. Because the client
 * has access to the full server setup after the fork,
 * no SAN message is needed and the first message goes
 * from client to server.
 *
 * Most messages will be SyncML message and response. In addition,
 * password requests also need to be passed through the server via
 * dedicated messages, because it is the one with a UI.
 */
class LocalTransportAgent : public TransportAgent
{
 private:
    LocalTransportAgent(SyncContext *server,
                        const std::string &clientConfig,
                        void *loop = NULL);
    boost::weak_ptr<LocalTransportAgent> m_self;

 public:
    /**
     * @param server          the server side of the sync;
     *                        must remain valid while transport exists
     * @param clientConfig    name of the target sync config or context (in which case
     *                        the target-config in that context is used)
     * @param loop            optional glib loop to use when waiting for IO;
     *                        transport will *not* increase the reference count
     */
    static boost::shared_ptr<LocalTransportAgent> create(SyncContext *server,
                                                         const std::string &clientConfig,
                                                         void *loop = NULL);
    ~LocalTransportAgent();

    /**
     * Set up message passing and fork the client.
     */
    void start();

    /**
     * Copies the client's sync report. If the client terminated
     * unexpectedly or shutdown() hasn't completed yet, the
     * STATUS_DIED_PREMATURELY sync result code will be set.
     */
    void getClientSyncReport(SyncReport &report);

    // TransportAgent implementation
    virtual void setURL(const std::string &url) {}
    virtual void setContentType(const std::string &type);
    virtual void shutdown();
    virtual void setFreeze(bool freeze);
    virtual void send(const char *data, size_t len);
    virtual void cancel();
    virtual Status wait(bool noReply = false);
    virtual void getReply(const char *&data, size_t &len, std::string &contentType);
    virtual void setTimeout(int seconds);

 private:
    SyncContext *m_server;
    std::string m_clientConfig;
    Status m_status;
    SyncReport m_clientReport;
    GMainLoopCXX m_loop;
    boost::shared_ptr<ForkExecParent> m_forkexec;
    std::string m_contentType;
    std::string m_replyContentType;
    pcrecpp::StringPiece m_replyMsg;

    /**
     * provides the D-Bus API expected by the forked process:
     * - password requests
     * - store the child's sync report
     */
    boost::shared_ptr<GDBusCXX::DBusObjectHelper> m_parent;

    /**
     * provides access to the forked process' D-Bus API
     * - start sync (returns child's first message)
     * - send server reply (returns child's next message or empty when done)
     * - emits output via signal
     *
     * Only non-NULL when child is running and connected.
     */
    boost::shared_ptr<LocalTransportChild> m_child;

    void logChildOutput(const std::string &level, const std::string &message);
    void onChildConnect(const GDBusCXX::DBusConnectionPtr &conn);
    void onFailure(const std::string &error);
    void onChildQuit(int status);
    void askPassword(const std::string &passwordName,
                     const std::string &descr,
                     const ConfigPasswordKey &key,
                     const boost::shared_ptr< GDBusCXX::Result1<const std::string &> > &reply);
    void storeSyncReport(const std::string &report);
    void storeReplyMsg(const std::string &contentType,
                       size_t offset, size_t len,
                       const std::string &error);

    /**
     * utility function: calculate deadline for operation starting now
     *
     * @param seconds      timeout in seconds, 0 for the default
     */
    Timespec deadline(unsigned seconds) { return seconds ? (Timespec::monotonic() + seconds) : Timespec(); }
};

SE_END_CXX

#endif // INCL_LOCALTRANSPORTAGENT
