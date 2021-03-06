/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include <forward_list>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <utility>

#include <folly/CppAttributes.h>
#include <folly/IntrusiveList.h>
#include <folly/Optional.h>
#include <folly/ssl/OpenSSLPtrTypes.h>

#include "logdevice/common/Address.h"
#include "logdevice/common/AdminCommandTable-fwd.h"
#include "logdevice/common/ConnectionInfo.h"
#include "logdevice/common/ConnectionKind.h"
#include "logdevice/common/PrincipalIdentity.h"
#include "logdevice/common/Priority.h"
#include "logdevice/common/ResourceBudget.h"
#include "logdevice/common/SocketTypes.h"
#include "logdevice/common/Timestamp.h"
#include "logdevice/common/WeakRefHolder.h"
#include "logdevice/common/configuration/NodeLocation.h"
#include "logdevice/common/configuration/TrafficClass.h"
#include "logdevice/common/configuration/TrafficShapingConfig.h"
#include "logdevice/common/protocol/Message.h"
#include "logdevice/include/Err.h"

// Think twice before adding new includes here!  This file is included in many
// translation units and increasing its transitive dependency footprint will
// slow down the build.  We use forward declaration and the pimpl idiom to
// offload most header inclusion to the .cpp file; scroll down for details.

struct event;
struct event_base;

namespace facebook { namespace logdevice {

namespace configuration { namespace nodes {
class NodesConfiguration;
}} // namespace configuration::nodes

class BWAvailableCallback;
class ClientIdxAllocator;
class Connection;
class FlowGroup;
class FlowGroupsUpdate;
class IConnectionFactory;
class Processor;
class SenderImpl;
class ShapingContainer;
class Sockaddr;
class Socket;
class SocketCallback;
class SocketProxy;
class StatsHolder;
class Worker;
struct Settings;

/**
 * @file a Sender sends Messages to Addresses by mapping an Address to a
 *       Connection then passing the Message to that Connection for sending. A
 *       Sender object is local to a Worker and only operates on Connections
 *       running on that Worker's event loop.
 *
 *       The interface for Sender is encapsulated in an abstract class to
 *       simplify the task of modifying sender behavior or intercepting
 *       outbound messages in tests.
 */

/**
 * Interface for sending messages to either a client or node.
 */
class SenderBase {
 public:
  class MessageCompletion {
   public:
    MessageCompletion(std::unique_ptr<Message> msg,
                      const Address& dest,
                      Status s,
                      SteadyTimestamp time)
        : msg_(std::move(msg)),
          enqueue_time_(time),
          destination_(dest),
          status_(s) {}

    void send();

    folly::IntrusiveListHook links;

   private:
    std::unique_ptr<Message> msg_;
    const SteadyTimestamp enqueue_time_;
    const Address destination_;
    const Status status_;
  };
  using CompletionQueue =
      folly::IntrusiveList<MessageCompletion, &MessageCompletion::links>;
  virtual ~SenderBase() {}

  /**
   * Attempts to send a message to a specified Address, which can identify
   * either a client or a server connection. The method asserts that it
   * is called on this Sender's Worker thread.
   *
   * @param msg  message to send. The caller loses ownership of msg if the
   *             call succeeds, retains ownership on failure.
   *
   * @param addr address to send the message to. If this is a client address,
   *             the call succeeds only if a Connection wrapping a TCP
   *             Connection accepted from that client is already running on this
   *             Worker. If addr is a server address for which there is no
   *             Connection on this Worker, the function attempts to create one.
   *             If it's a server address, and generation()
   *             == 0, takes the generation from the current config.
   *
   * @param onclose an optional callback functor to push onto the list of
   *                callbacks that the Connection through which the message gets
   *                sent will call on this Worker thread when that Connection is
   *                closed. The callback MUST NOT yet be on any callback list.
   *                The callback will NOT be installed if the call fails,
   *                except CBREGISTERED. Note that if you provide both
   *                `on_bw_avail` and `onclose`, and the Connection closes while
   *                waiting for bandwidth, then both callbacks will be notified:
   *                first `on_bw_avail->cancelled()`, then `(*onclose)()`.
   *
   *                NOTE a: the Sender does NOT take ownership of the callback.
   *                NOTE b: it is safe for caller to destroy the callback while
   *                        it is installed. Callback's destructor will remove
   *                        it from list.
   *                NOTE c: Connection::close() removes callbacks from list
   *                before calling them (one at a time)
   *
   * @return 0 if the message was successfully queued up on a Connection send
   *         queue. Note that this does not guarantee that the Message will be
   *         written into a TCP connection, much less delivered. The final
   *         disposition of this attempt to send will be communicated through
   *         msg.onSent(). On failure -1 is returned. err is set to
   *
   *            CBREGISTERED   traffic throttling is in effect. The provided
   *                           callback will be invoked once bandwidth is
   *                           available.
   *            UNREACHABLE    addr is a client address for which no connected
   *                           and handshaken Connection exists on this Worker.
   *            NOTINCONFIG    if addr is a NodeID that is not present
   *                           in the current cluster config of Processor to
   *                           which this Sender belongs, or if it is a
   *                           node_index_t that is greater than the largest
   *                           node index in config.
   *            NOSSLCONFIG    Connection to the recipient must use SSL but the
   *                           recipient is not configured for SSL connections.
   *            UNROUTABLE     if addr is a NodeID and the corresponding IP
   *                           address in the cluster config has no route.
   *                           This may happen if a network interface has been
   *                           taken down, e.g., during system shutdown.
   *            NOBUFS         Connection send queue limit was reached
   *            DISABLED       destination is temporarily marked down
   *            TOOBIG         message is too big (exceeds payload size limit)
   *            SYSLIMIT       out of file descriptors or ephemeral ports
   *            NOMEM          out of kernel memory for Connections, or malloc()
   *                           failed
   *            CANCELLED      msg.cancelled() requested message to be cancelled
   *            SHUTDOWN       Sender has been shut down
   *            INTERNAL       bufferevent unexpectedly failed to initiate
   *                           connection, unexpected error from socket(2).
   *            PROTONOSUPPORT the handshaken protocol is not compatible with
   *                           this message.
   *            NOTCONN        Underlying socket is disconnected.
   */
  template <typename MessageT>
  int sendMessage(typename std::unique_ptr<MessageT>&& msg,
                  const Address& addr,
                  BWAvailableCallback* on_bw_avail = nullptr,
                  SocketCallback* onclose = nullptr) {
    // Ensure the temporary necessary to convert from
    // unique_ptr<DerivedMessage> to unique_ptr<Message>
    // is explicitly controlled by SenderBase, and the
    // result of a failed sendMessage (no move should
    // occur) is properly reflected to the caller.
    std::unique_ptr<Message> base = std::move(msg);
    int rv = sendMessageImpl(std::move(base), addr, on_bw_avail, onclose);
    msg.reset(static_cast<MessageT*>(base.release()));
    return rv;
  }

  template <typename MessageT>
  int sendMessage(typename std::unique_ptr<MessageT>&& msg,
                  ClientID cid,
                  BWAvailableCallback* on_bw_avail = nullptr,
                  SocketCallback* onclose = nullptr) {
    return sendMessage(std::move(msg), Address(cid), on_bw_avail, onclose);
  }

  template <typename MessageT>
  int sendMessage(typename std::unique_ptr<MessageT>&& msg,
                  NodeID nid,
                  BWAvailableCallback* on_bw_avail = nullptr,
                  SocketCallback* onclose = nullptr) {
    return sendMessage(std::move(msg), Address(nid), on_bw_avail, onclose);
  }

  template <typename MessageT>
  int sendMessage(typename std::unique_ptr<MessageT>&& msg,
                  ClientID cid,
                  SocketCallback* onclose) {
    return sendMessage(std::move(msg), Address(cid), nullptr, onclose);
  }

  template <typename MessageT>
  int sendMessage(typename std::unique_ptr<MessageT>&& msg,
                  NodeID nid,
                  SocketCallback* onclose) {
    return sendMessage(std::move(msg), Address(nid), nullptr, onclose);
  }

  /**
   * Verify that transmission to the given address is expected to be successful.
   *
   * When testing a client address:
   *   - The client is still connected.
   *   - Traffic shapping will allow at least one message of the specified
   *     traffic class to be transmitted.
   *
   * When testing a server node address:
   *   - The node is a member of the current config.
   *   - Either a connection doesn't yet exist for the node or
   *     Traffic shapping will allow at least one message of the specified
   *     traffic class to be transmitted.
   *
   * The method asserts that it is called on this Sender's Worker thread.
   *
   * @param addr address for sending a future message.
   *
   * @param tc   The traffic class that will be used to construct the message
   *             that will be sent.
   *
   * @return true if the future message transmission is expected to be
   *         successfull. Otherwise false with one of the following error
   *         codes set:
   *
   *         CBREGISTERED traffic throttling is in effect. The provided
   *                      callback will be invoked once bandwidth is
   *                      available.
   *
   *         NOTINCONFIG  if addr is a NodeID that is not present
   *                      in the current cluster config of Processor to which
   *                      this Sender belongs, or if it is a node_index_t
   *                      that is greater than the largest node index in
   *                      config.
   *
   *         NOTCONN      if addr is a ClientID and there is no current
   *                      connection to that client.
   */
  bool canSendTo(NodeID nid,
                 TrafficClass tc,
                 BWAvailableCallback& on_bw_avail) {
    return canSendToImpl(Address(nid), tc, on_bw_avail);
  }

  bool canSendTo(ClientID cid,
                 TrafficClass tc,
                 BWAvailableCallback& on_bw_avail) {
    return canSendToImpl(Address(cid), tc, on_bw_avail);
  }

 protected:
  // These two methods need to be implemented in concrete implementations
  // of Sender behavior.
  virtual bool canSendToImpl(const Address&,
                             TrafficClass,
                             BWAvailableCallback&) = 0;

  virtual int sendMessageImpl(std::unique_ptr<Message>&& msg,
                              const Address& addr,
                              BWAvailableCallback* on_bw_avail,
                              SocketCallback* onclose) = 0;
};

/**
 * Implements the standard practice of routing all concrete
 * Sender APIs through the Sender associated with the current
 * thread's worker.
 *
 * For classes that can only be tested by modifying Sender
 * behavior, a SenderProxy is instantiated and used for all
 * Sender API calls. During normal operation, the SenderProxy
 * will forward to the standard Sender owned by the current
 * thread's worker. When the class is under test, the SenderProxy
 * is replaced by a SenderTestProxy so the test can intercept
 * all API calls.
 */
class SenderProxy : public SenderBase {
 protected:
  bool canSendToImpl(const Address&,
                     TrafficClass,
                     BWAvailableCallback&) override;

  int sendMessageImpl(std::unique_ptr<Message>&& msg,
                      const Address& addr,
                      BWAvailableCallback* on_bw_avail,
                      SocketCallback* onclose) override;
};

/**
 * The standard Sender implementation.
 */
class Sender : public SenderBase {
 public:
  /**
   * @param node_count   the number of nodes in cluster configuration at the
   *                     time this Sender was created
   */

  Sender(Worker* worker,
         Processor* processor,
         std::shared_ptr<const Settings> settings,
         struct event_base* base,
         const configuration::ShapingConfig& tsc,
         ClientIdxAllocator* client_id_allocator,
         bool is_gossip_sender,
         std::shared_ptr<const configuration::nodes::NodesConfiguration> nodes,
         node_index_t my_node_index,
         folly::Optional<NodeLocation> my_location,
         std::unique_ptr<IConnectionFactory> connection_factory,
         StatsHolder* stats);

  ~Sender() override;

  Sender(const Sender&) = delete;
  Sender(Sender&&) noexcept = delete;
  Sender& operator=(const Sender&) = delete;
  Sender& operator=(Sender&&) = delete;

  bool canSendToImpl(const Address&,
                     TrafficClass,
                     BWAvailableCallback&) override;

  int sendMessageImpl(std::unique_ptr<Message>&& msg,
                      const Address& addr,
                      BWAvailableCallback* on_bw_avail,
                      SocketCallback* onclose) override;

  /**
   * Get client socket token for the socket associated with client-id 'cid'.
   */
  std::shared_ptr<const std::atomic<bool>> getConnectionToken(ClientID) const;

  /**
   * Dispatch any accumulated message completions. Must be called from
   * a context that can tolerate a completion re-entering Sender.
   */
  void deliverCompletedMessages();

  ShapingContainer* getNwShapingContainer() {
    return nw_shaping_container_.get();
  }

  /**
   * If addr identifies a Connection managed by this Sender, pushes cb
   * onto the on_close_ callback list of that Connection, to be called when
   * the Connection is closed. See sendMessage() docblock above for important
   * notes and restrictions.
   * @return 0 on success, -1 if callback could not be installed. Sets err
   *         to NOTFOUND if addr does not identify a Connection managed by this
   *                     Sender. NOTFOUND is set also if
   *                     the incoming Connection was already closed.
   * INVALID_PARAM  if cb is already on
   * some callback list (debug build asserts)
   */
  int registerOnSocketClosed(const Address& addr, SocketCallback& cb);

  /**
   * Close the Connection for this server
   *
   * @param peer   Address for which to close the Connection.
   * @param reason Reason for closing the Connection.
   *
   * @return 0 on success, or -1 if there is no Connection for address `peer`,
   * in which case err is set to E::NOTFOUND.
   */
  int closeSocket(Address peer, Status reason);

  /**
   * Close the Connection for this server
   *
   * @param peer   NodeID for which to close the Connection.
   * @param reason Reason for closing the Connection.
   *
   * @return 0 on success, or -1 if there is no Connection for address `peer`,
   * in which case err is set to E::NOTFOUND.
   */
  int closeServerSocket(NodeID peer, Status reason);

  /**
   * Close the Connection for a client.
   *
   * @param cid    Client for which to close the Connection.
   * @param reason Reason for closing the Connection.
   *
   * @return 0 on success, or -1 if there is no Connection for client `cid`, in
   *         which case err is set to E::NOTFOUND.
   */
  int closeClientSocket(ClientID cid, Status reason);

  /**
   * Flushes buffered data to network and disallows initiating new connections
   * or sending messages.
   *
   * isShutdownCompleted() can be used to find out when this operation
   * completes.
   */
  void beginShutdown();

  /**
   * Esnures all connections are closed. Must be be called as last step of
   * Sender shutdown. Normally you want to call beginShutdown() first to flush
   * buffers to network and shutdown gracefully but you do not have to, for
   * example when shutting down Sender on emergency path or in tests.
   */
  void forceShutdown();

  /**
   * @return true iff all owned Connections are closed.
   */
  bool isShutdownCompleted() const;

  bool isClosed(const Address& addr) const;

  /**
   * Check if a working connection to a given node exists.
   *
   * If connection to nid is currently in progress, this method will report
   * either ALREADY or NOTCONN, depending on whether this is the first attempt
   * to connect to the given node or not.
   *
   * @return  0 if a connection to nid is already established and handshake
   *          completed, -1 otherwise with err set to
   *          NOTFOUND   nid does not identify a server Connection managed
   *                     by this Sender
   *          ALREADY    Initial connection attempt to nid is in progress
   *          NOTCONN    No active connection to nid is available
   *          NOBUFS     Connection to destination is valid but reaches its
   *                     buffer limit
   *          DISABLED   Connection is currently marked down after an
   *                     unsuccessful connection attempt INVALID_PARAM  nid is
   *                     invalid (debug build asserts)
   */
  int checkServerConnection(NodeID nid) const;

  /**
   * Check if a working connection to a give client exists. If peer_is_client is
   * set extra checks is made to make sure peer is logdevice client.
   *
   * @return   0 if a connection to nid is already established and handshake
   *          completed, -1 otherwise with err set to
   *          NOTFOUND     cid does not exist the socket must be closed.
   *          NOTCONN      The socket is not connected or not hanshaken.
   *          NOTANODE     If check_peer_is_node is set, peer is not a
   *                       logdevice node.
   */
  int checkClientConnection(ClientID cid, bool check_peer_is_node);

  /**
   * Initiate an asynchronous attempt to connect to a given node unless a
   * working connection already exists or is in progress.
   *
   * @return 0 on sucesss, -1 otherwise with err set to
   *         SHUTDOWN      Sender has been shut down
   *         NOTINCONFIG   if nid is not present in the current cluster config
   *         NOSSLCONFIG   Connection to nid must use SSL but SSL is not
   *                       configured for nid
   *         see Connection::connect() for the rest of possible error codes
   */
  int connect(NodeID nid);

  /**
   * Returns connection info for the connection with given address. Retuned
   * pointer should not outlive the connection itself so if caller needs to
   * store information somwhere for the future usage it must be copied.
   *
   * @return Pointer to info struct if connection exists or nullptr
   *         otherwise.
   */
  const ConnectionInfo* FOLLY_NULLABLE getConnectionInfo(const Address&) const;

  /**
   * Updates connection info by replacing with given version.
   *
   * @return whether connection info was successfully updated. This operation
   * may fail only if connection with given address is not found.
   */
  bool setConnectionInfo(const Address&, const ConnectionInfo&);

  /**
   * @param addr  peer name of a client or server Connection expected to be
   *              under the management of this Sender.
   * @return a peer Sockaddr for the Connection, or an invalid Sockaddr if
   *         no Connections known to this Sender match addr
   */
  Sockaddr getSockaddr(const Address& addr);

  /**
   * @param addr  peer name of a client or server Connection expected to be
   *              under the management of this Sender.
   * @return the type of the connection (SSL/PLAIN)
   */
  ConnectionType getSockConnType(const Address& addr);

  /**
   * @param addr    peer name of a client or server Connection expected to be
   *                under the management of this Sender.
   * @return a pointer to the principal held in the Connection matching the addr
   * or nullptr if no Connection is known to this Sender match addr
   */
  const PrincipalIdentity* getPrincipal(const Address&);

  // An enum representing the result of the peer identity extractions.
  enum class ExtractPeerIdentityResult {
    // Indicates that the connection wasn't found, and translates to
    // PrincipalIdentity::UNAUTHENTICATED.
    NOT_FOUND,
    // Indicates that the connection wasnt SSL, and translates to
    // PrincipalIdentity::UNAUTHENTICATED.
    NOT_SSL,
    // Indicates that the parsing the certificate fails and translates to
    // PrincipalIdentity::INVALID.
    PARSING_FAILED,
    // Indicates that the parsing was successfull and the parsed identity is
    // returned.
    SUCCESS,
  };

  /**
   * Check the documentation of Connection::extractPeerIdentity.
   *
   * @param addr  peer name of a client or server Connection expected to be
   *              under the management of this Sender.
   *
   * @returns     An enum representing the result of the parsing and the
   * corresponding identity. Check documentation ExtractPeerIdentityResult for
   * what each result translates to.
   */
  std::pair<ExtractPeerIdentityResult, PrincipalIdentity>
  extractPeerIdentity(const Address& addr);

  /**
   * Returns the node index of the peer with the given address.
   *
   * @return if addr is a server address or corresponds to a client connection
   *         from another node, that id will be returned; otherwise, this method
   *         returns folly::none
   */
  folly::Optional<node_index_t> getNodeIdx(const Address& addr) const;

  /**
   * @return getSockaddr()  for this thread's worker (if exists), otherwise it
   * returns an INVALID Sockaddr instance. Useful for trace loggers.
   */
  static Sockaddr sockaddrOrInvalid(const Address& addr);

  /**
   * @param addr  address that should (but does not have to) identify a
   * Connection on this Worker thread
   *
   * @return a string of the form "W<idx>:<socket-id> (<ip>)" where <idx>
   *         is the index of Worker in its Processor, <socket-id> is
   *         addr.briefString(), and <ip> is the IP of peer to which the
   * Connection is connected to, or UNKNOWN if there is no such Connection on
   * this Worker. If this method is called on a thread other than a Worker
   *         thread, it returns addr.briefString().
   *
   */
  static std::string describeConnection(const Address& addr);
  static std::string describeConnection(const ClientID& client) {
    return describeConnection(Address(client));
  }
  static std::string describeConnection(const NodeID& node) {
    return describeConnection(Address(node));
  }
  static std::string describeConnection(node_index_t node) {
    return describeConnection(Address(NodeID(node, 0)));
  }

  /**
   * Creates a new client Connection for a newly accepted client connection and
   * inserts it into the client_sockets_ map. Must be called on the thread
   * running this Sender's Worker.
   *
   * @param fd          TCP socket that we got from accept(2)
   * @param client_addr sockaddr we got from accept(2)
   * @param conn_token  an object used for accepted connection accounting
   * @param type        type of socket (DATA/GOSSIP)
   * @param conntype    type of connection (PLAIN/SSL)
   *
   * @return  0 on success, -1 if we failed to create a Connection, sets err to:
   *     EXISTS          a Connection for this ClientID already exists
   *     NOMEM           a libevent function could not allocate memory
   *     NOBUFS          reached internal limit on the number of client
   * Connections (TODO: implement) INTERNAL        failed to set fd non-blocking
   * (unlikely)
   */
  int addClient(int fd,
                const Sockaddr& client_addr,
                ResourceBudget::Token conn_token,
                SocketType type,
                ConnectionType conntype,
                ConnectionKind connection_kind);

  /**
   * Called by a Connection managed by this Sender when bytes are added to one
   * of Connection's queues. These calls should be matched by
   * noteBytesDrained() calls, such that for each message_type (including
   * folly::none) the (queued - drained) value reflects the current in-flight
   * situation.
   * @param peer_type CLIENT or NODE.
   * @param message_type If message bytes are added to a queue, message_type is
   *        the type of that message. If message is passed to a evbuffer,
   *        message_type is folly::none.
   *
   * @param nbytes   how many bytes were appended
   */
  void noteBytesQueued(size_t nbytes,
                       PeerType peer_type,
                       folly::Optional<MessageType> message_type);

  /**
   * Called by a Connection managed by this Sender when some bytes from
   * the Connection output buffer have been drained into the
   * underlying TCP socket.
   * @param peer_type CLIENT or NODE.
   * @param message_type is treated the same way as in
   * noteBytesQueued()
   *
   * @param nbytes   how many bytes were sent
   */
  void noteBytesDrained(size_t nbytes,
                        PeerType peer_type,
                        folly::Optional<MessageType> message_type);

  /**
   * @return   the current total number of bytes in the output evbuffers of
   *           all Connections of all peer types managed by this Sender.
   */
  size_t getBytesPending() const {
    return bytes_pending_;
  }

  /**
   * @return true iff the total number of bytes in the output evbuffers of
   *              all Connections managed by this Sender exceeds the limit set
   *              in this Processor's configuration
   */
  bool bytesPendingLimitReached(PeerType peer_type) const;

  /**
   * Queue a message for a deferred completion. Used from contexts that
   * must be protected from re-entrance into Sender.
   */
  void queueMessageCompletion(std::unique_ptr<Message>,
                              const Address&,
                              Status,
                              SteadyTimestamp t);

  /**
   * Proxy for Connection::getTcpSendBufSize() for a client Connection.  Returns
   * -1 if Connection not found (should never happen).
   */
  ssize_t getTcpSendBufSizeForClient(ClientID client_id) const;

  /**
   * Proxy for Connection::getTcpSendBufOccupancy() for a client Connection.
   * Returns -1 if Connection not found.
   */
  ssize_t getTcpSendBufOccupancyForClient(ClientID client_id) const;

  /**
   * @return if this Sender manages a Connection for the node at configuration
   *         position idx, return that Connection. Otherwise return nullptr.
   *         Deprecated : Do not use this API to get Connection. Use of
   * Connection outside Sender is deprecated.
   */
  Connection* findServerConnection(node_index_t idx) const;

  /**
   * @return protocol version of the Connection.
   */
  folly::Optional<uint16_t> getSocketProtocolVersion(node_index_t idx) const;

  /**
   * @return get ID assigned by client.
   */
  ClientID getOurNameAtPeer(node_index_t node_index) const;

  /**
   * Resets the server Connection's connect throttle.
   */
  void resetServerSocketConnectThrottle(NodeID node_id);

  /**
   * Sets the server Connection's peer_shuttingdown_ to true
   * indicating that peer is going to go down soon
   */
  void setPeerShuttingDown(NodeID node_id);

  /**
   * Called when configuration has changed.  Sender closes any open
   * connections to nodes that are no longer in the config.
   */
  void noteConfigurationChanged(
      std::shared_ptr<const configuration::nodes::NodesConfiguration>);

  void onSettingsUpdated(std::shared_ptr<const Settings> new_settings);

  /**
   * Add a client id to the list of Connections to be erased from
   * .client_sockets_ next time this object tries to add a new entry to that
   * map.
   *
   * @param client_name   id of client Connection to erase
   *
   * @return  0 on success, -1 if client_name is not in .client_sockets_, err is
   *          set to NOTFOUND.
   */
  int noteDisconnectedClient(ClientID client_name);

  // Dumps a human-readable frequency map of queued messages by type.  If
  // `addr` is valid, only messages queued in that Connection are counted.
  // Otherwise, messages in all Connections are counted.
  std::string dumpQueuedMessages(Address addr) const;

  /**
   * Invokes a callback for each Connection with its info structure.
   */
  void
  forEachConnection(const std::function<void(const ConnectionInfo&)>& cb) const;

  /**
   * Fills give table with debug information about all connections managed by
   * this sender.
   */
  void fillDebugInfo(InfoSocketsTable& table) const;

  /**
   * @param addr    peer name of a client or server Connection expected to be
   *                under the management of this Sender.
   * @return the CSID held in the Connection matching the addr or
   *         folly::none if no Connection is known to this Sender match address
   *         or CSID for connection is not known
   */
  folly::Optional<std::string> getCSID(const Address& addr) const;

  std::string getClientLocation(const ClientID& cid) const;

 private:
  // Worker owning this Sender
  Worker* worker_;
  Processor* processor_;
  std::shared_ptr<const Settings> settings_;

  std::unique_ptr<IConnectionFactory> connection_factory_;

  // Network Traffic Shaping
  std::unique_ptr<ShapingContainer> nw_shaping_container_;

  // Pimpl
  friend class SenderImpl;
  std::unique_ptr<SenderImpl> impl_;

  bool is_gossip_sender_;

  std::shared_ptr<const configuration::nodes::NodesConfiguration> nodes_;

  // ids of disconnected Connections to be erased from .client_sockets_
  std::forward_list<ClientID> disconnected_clients_;

  // To avoid re-entering Sender::sendMessage() when low priority messages
  // are trimmed from a FlowGroup's priority queue, trimmed messages are
  // accumulated in a deferred completion queue and then processed from
  // the event loop.
  CompletionQueue completed_messages_;

  std::atomic<bool> delivering_completed_messages_{false};

  // current number of bytes in all output buffers.
  std::atomic<size_t> bytes_pending_{0};
  // current number of bytes in all output buffers of logdevice client
  // connections.
  std::atomic<size_t> bytes_pending_client_{0};
  // current number of bytes in all output buffers of node connections.
  std::atomic<size_t> bytes_pending_node_{0};

  // if true, disallow sending messages and initiating connections
  bool shutting_down_ = false;

  // The id of this node.
  // If running on the client, this will be set to NODE_INDEX_INVALID.
  const node_index_t my_node_index_;

  // The location of this node (or client). Used to determine whether to use
  // SSL when connecting.
  const folly::Optional<NodeLocation> my_location_;

  void disconnectFromNodesThatChangedAddressOrGeneration();

  /**
   * Tells all open Connections to flush output and close, asynchronously.
   * isShutdownCompleted() can be used to find out when this operation
   * completes. Used during shutdown.
   */
  void flushOutputAndClose(Status reason);

  /**
   * Unconditionally close all server and clients Connections.
   */
  void closeAllSockets();

  /**
   * A helper method for sending a message to a connected Connection.
   *
   * @param msg   message to send. Ownership is not transferred unless the call
   *              succeeds.
   * @param conn  client server Connection to send the message into
   * @param onclose an optional callback to push onto the list of callbacks
   *                that the Connection through which the message gets sent will
   *                call when that Connection is closed. The callback will NOT
   * be installed if the call fails.
   *
   * @return 0 on success, -1 on failure. err is set to
   *    NOBUFS       Connection send queue limit was reached
   *    TOOBIG       message is too big (exceeds payload size limit)
   *    NOMEM        out of kernel memory for sockets, or malloc() failed
   *    CANCELLED    msg.cancelled() requested message to be cancelled
   *    INTERNAL     bufferevent unexpectedly failed to initiate connection,
   *                 unexpected error from socket(2).
   */
  int sendMessageImpl(std::unique_ptr<Message>&& msg,
                      Connection& conn,
                      BWAvailableCallback* on_bw_avail = nullptr,
                      SocketCallback* onclose = nullptr);

  /**
   * Returns a Connection to a given node in the cluster config. If a Connection
   * doesn't exist yet, it will be created.
   *
   * This will rely on ssl_boundary setting and target/own location to determine
   * whether to use SSL.
   *
   * @return a pointer to the valid Connection object; on failure returns
   * nullptr and sets err to NOTINCONFIG  nid is not present in the current
   * cluster config NOSSLCONFIG  Connection to nid must use SSL but SSL is not
   *                      configured for nid
   *         INTERNAL     internal error (debug builds assert)
   */
  Connection* initServerConnection(NodeID nid, SocketType sock_type);

  /**
   * This method gets the Connection associated with a given ClientID. The
   * connection must already exist for this method to succeed.
   *
   * @param cid  peer name of a client Connection expected to be under the
   *             management of this Sender.
   * @return the Connection connected to cid or nullptr if an error occured
   */
  Connection* FOLLY_NULLABLE getConnection(const ClientID& cid);

  /**
   * This method gets the Connection associated with a given NodeID. It will
   * attempt to create the connection if it doesn't already exist.
   *
   * @param nid        peer name of a client Connection expected to be under the
   *                   management of this Sender.
   * @param msg        the Message either being sent or received on the
   *                   Connection.
   * @return the Connection connected to nid or nullptr if an error occured
   */
  Connection* FOLLY_NULLABLE getConnection(const NodeID& nid,
                                           const Message& msg);

  /**
   * This method gets the Connection associated with a given Address. If addr is
   * a NodeID, it will attempt to create the connection if it doesn't already
   * exist.  Otherwise, the connection must already exist.
   *
   * @param addr       peer name of a Connection expected to be under the
   *                   management of this Sender.
   * @param msg        the Message either being sent or received on the
   *                   Connection.
   * @return the Connection connected to nid or nullptr if an error occured
   */
  Connection* FOLLY_NULLABLE getConnection(const Address& addr,
                                           const Message& msg);

  /**
   * This method finds any existing Connection associated with a given Address.
   *
   * @param addr       peer name of a Connection expected to be under the
   *                   management of this Sender.
   * @return the Connection connected to addr or nullptr, with err set, if
   *         the Connection couldn't be found.
   */
  Connection* FOLLY_NULLABLE findConnection(const Address& addr);

  /**
   * @return true iff called on the thread that is running this Sender's
   *         Worker. Used in asserts.
   */
  bool onMyWorker() const;

  /**
   * Remove client Connections on the disconnected client list from the client
   * map. Destroy the Connection objects.
   */
  void eraseDisconnectedClients();

  /**
   * Initializes my_node_id_ and my_location_ from the current config and
   * settings.
   */
  void initMyLocation();

  /**
   * Returns true if SSL should be used with the specified node.
   * If cross_boundary_out or authentication_out are given, outputs in them
   * whether SSL should be used for data encryption or for authentication
   * accordingly.
   */
  bool useSSLWith(NodeID nid,
                  bool* cross_boundary = nullptr,
                  bool* authentication = nullptr) const;

  /**
   * Closes all Connections in sockets_to_close_ with the specified error codes
   * and destroys them
   */
  void processSocketsToClose();

  /**
   * Detects and closes connections that are either slow or idle for prolonged
   * period of time.
   */
  void cleanupConnections();

  static void onCompletedMessagesAvailable(void* self, short);

  /**
   * Increment the bytes pending for a given peer type.
   */
  void incrementBytesPending(size_t nbytes, PeerType peer_type) {
    bytes_pending_ += nbytes;
    switch (peer_type) {
      case PeerType::CLIENT:
        bytes_pending_client_ += nbytes;
        return;
      case PeerType::NODE:
        bytes_pending_node_ += nbytes;
        return;
      default:
        ld_check(false);
    }
  }

  /**
   * Decrement the bytes pending for a given peer type.
   */
  void decrementBytesPending(size_t nbytes, PeerType peer_type) {
    ld_check(bytes_pending_ >= nbytes);
    ld_check(getBytesPending(peer_type) >= nbytes);
    bytes_pending_ -= nbytes;
    switch (peer_type) {
      case PeerType::CLIENT:
        bytes_pending_client_ -= nbytes;
        return;
      case PeerType::NODE:
        bytes_pending_node_ -= nbytes;
        return;
      default:
        ld_check(false);
    }
  }

  size_t getBytesPending(PeerType peer_type) const {
    switch (peer_type) {
      case PeerType::CLIENT:
        return bytes_pending_client_;
      case PeerType::NODE:
        return bytes_pending_node_;
      default:
        ld_check(false);
    }
    return 0;
  }

  Connection* findConnection(const Address& addr) const;
  Connection* findClientConnection(const ClientID& client_id) const;

  // Identifies right DSCP marking for connection based in connection properties
  folly::Optional<uint8_t> detectDSCP(const ConnectionInfo&);

  // Iterates over all connections and invokes a callback for each of them.
  void
  forAllConnections(const std::function<void(const Connection&)>& cb) const;
};

}} // namespace facebook::logdevice
