#ifndef SRC_NODE_QUIC_SESSION_H_
#define SRC_NODE_QUIC_SESSION_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include "aliased_buffer.h"
#include "async_wrap.h"
#include "env.h"
#include "handle_wrap.h"
#include "node.h"
#include "node_crypto.h"
#include "node_mem.h"
#include "node_quic_util.h"
#include "v8.h"
#include "uv.h"

#include <ngtcp2/ngtcp2.h>
#include <openssl/ssl.h>

#include <functional>
#include <map>
#include <vector>

namespace node {
namespace quic {

class QuicClientSession;
class QuicServerSession;
class QuicSocket;
class QuicStream;

constexpr int ERR_INVALID_REMOTE_TRANSPORT_PARAMS = -1;
constexpr int ERR_INVALID_TLS_SESSION_TICKET = -2;

#define QUICSESSION_CONFIG(V)                                                 \
  V(MAX_STREAM_DATA_BIDI_LOCAL, max_stream_data_bidi_local, 256 * 1024)       \
  V(MAX_STREAM_DATA_BIDI_REMOTE, max_stream_data_bidi_remote, 256 * 1024)     \
  V(MAX_STREAM_DATA_UNI, max_stream_data_uni, 256 * 1024)                     \
  V(MAX_DATA, max_data, 1 * 1024 * 1024)                                      \
  V(MAX_STREAMS_BIDI, max_streams_bidi, 100)                                  \
  V(MAX_STREAMS_UNI, max_streams_uni, 3)                                      \
  V(IDLE_TIMEOUT, idle_timeout, 10 * 1000)                                    \
  V(MAX_PACKET_SIZE, max_packet_size, NGTCP2_MAX_PKT_SIZE)                    \
  V(MAX_ACK_DELAY, max_ack_delay, NGTCP2_DEFAULT_MAX_ACK_DELAY)

#define V(idx, name, def)                                                     \
  constexpr uint64_t IDX_QUIC_SESSION_## idx ##_DEFAULT = def;
  QUICSESSION_CONFIG(V)
#undef V

// The QuicSessionConfig class holds the initial transport parameters and
// configuration options set by the JavaScript side when either a
// QuicClientSession or QuicServerSession is created. Instances are
// stack created and use a combination of an AliasedBuffer to pass
// the numeric settings quickly (see node_quic_state.h) and passed
// in non-numeric settings (e.g. preferred_addr).
class QuicSessionConfig {
 public:
  QuicSessionConfig() = default;

  void ResetToDefaults();

  // QuicSessionConfig::Set() is where the magic happens. It pulls
  // values out of the AliasedBuffer defined in node_quic_state.h
  // and stores the values. If the preferred_addr is set, it will
  // be copied into preferred_address_.
  void Set(
      Environment* env,
      const struct sockaddr* preferred_addr = nullptr);

  // When a ngtcp2 connection is created, ToSettings is used to
  // populate the given ngtcp2_settings object with the stored
  // parameters. These are translated into QUIC transport params.
  void ToSettings(
      ngtcp2_settings* settings,
      ngtcp2_cid* pscid,
      bool stateless_reset_token = false);

  size_t GetMaxCidLen() { return max_cid_len_; }
  size_t GetMinCidLen() { return min_cid_len_; }

 private:
#define V(idx, name, def) uint64_t name##_ = def;
  QUICSESSION_CONFIG(V)
#undef V
  bool preferred_address_set_ = false;
  size_t max_cid_len_ = NGTCP2_MAX_CIDLEN;
  size_t min_cid_len_ = NGTCP2_MIN_CIDLEN;
  SocketAddress preferred_address_;
};

// The QuicSessionState enums are used with the QuicSession's
// private state_ array. This is exposed to JavaScript via an
// aliased buffer and is used to communicate various types of
// state efficiently across the native/JS boundary.
enum QuicSessionState {
  // Communicates the number of available connection ID's that
  // have been created and associated with the session. This
  // is used, for instance, to enable migration of a QuicSession
  // from one QuicSocket to another (when count > 0).
  IDX_QUIC_SESSION_STATE_CONNECTION_ID_COUNT,

  // Communicates whether a 'keylog' event listener has been
  // registered on the JavaScript QuicSession object. The
  // value will be either 1 or 0. When set to 1, the native
  // code will emit TLS keylog entries to the JavaScript
  // side triggering the 'keylog' event once for each line.
  IDX_QUIC_SESSION_STATE_KEYLOG_ENABLED,

  // Communicates whether a 'clientHello' event listener has
  // been registered on the JavaScript QuicServerSession.
  // The value will be either 1 or 0. When set to 1, the
  // native code will callout to the JavaScript side causing
  // the 'clientHello' event to be emitted. This is only
  // used on QuicServerSession instances.
  IDX_QUIC_SESSION_STATE_CLIENT_HELLO_ENABLED,

  // Communicates whether a 'cert' event listener has been
  // registered on the JavaScript QuicSession. The value will
  // be either 1 or 0. When set to 1, then native code will
  // callout to the JavaScript side causing the 'cert' event
  // to be emitted.
  IDX_QUIC_SESSION_STATE_CERT_ENABLED,

  // Just the number of session state enums for use when
  // creating the AliasedBuffer.
  IDX_QUIC_SESSION_STATE_COUNT
};

// The QuicSession class is an virtual class that serves as
// the basis for both QuicServerSession and QuicClientSession.
// It implements the functionality that is shared for both
// QUIC clients and servers.
//
// QUIC sessions are virtual connections that exchange data
// back and forth between peer endpoints via UDP. Every QuicSession
// has an associated TLS context and all data transfered between
// the peers is always encrypted. Unlike TLS over TCP, however,
// The QuicSession uses a session identifier that is independent
// of both the local *and* peer IP address, allowing a QuicSession
// to persist across changes in the network (one of the key features
// of QUIC). QUIC sessions also support 0RTT, implement error
// correction mechanisms to recover from lost packets, and flow
// control. In other words, there's quite a bit going on within
// a QuicSession object.
class QuicSession : public AsyncWrap,
                    public std::enable_shared_from_this<QuicSession>,
                    public mem::Tracker {
 public:
  static const int kInitialClientBufferLength = 4096;

  QuicSession(
      // The QuicSocket that created this session. Note that
      // it is possible to replace this socket later, after
      // the TLS handshake has completed. The QuicSession
      // should never assume that the socket will always
      // remain the same.
      QuicSocket* socket,
      v8::Local<v8::Object> wrap,
      crypto::SecureContext* ctx,
      AsyncWrap::ProviderType provider,
      // QUIC is generally just a transport. The ALPN identifier
      // is used to specify the application protocol that is
      // layered on top. If not specified, this will default
      // to the HTTP/3 identifier.
      const std::string& alpn);
  ~QuicSession() override;

  void AddStream(QuicStream* stream);
  void Close();
  void Closing();
  void Destroy();
  void ExtendStreamOffset(QuicStream* stream, size_t amount);
  const std::string& GetALPN();
  inline QuicError GetLastError();
  void GetLocalTransportParams(
      ngtcp2_transport_params* params);
  uint32_t GetNegotiatedVersion();
  SocketAddress* GetRemoteAddress();
  bool IsClosing();
  bool IsDestroyed();
  bool IsHandshakeCompleted();
  int OpenBidirectionalStream(
      int64_t* stream_id);
  int OpenUnidirectionalStream(
      int64_t* stream_id);
  size_t ReadPeerHandshake(
      uint8_t* buf,
      size_t buflen);
  int ReceiveStreamData(
      int64_t stream_id,
      int fin,
      const uint8_t* data,
      size_t datalen,
      uint64_t offset);
  void RemoveStream(
      int64_t stream_id);
  int Send0RTTStreamData(
      QuicStream* stream);
  int SendStreamData(
      QuicStream* stream);
  inline void SetLastError(
      QuicError error = { QUIC_ERROR_SESSION, NGTCP2_NO_ERROR }) {
    last_error_ = error;
  }
  inline void SetLastError(QuicErrorFamily family, int code) {
    SetLastError(InitQuicError(family, code));
  }
  int SetRemoteTransportParams(
      ngtcp2_transport_params* params);
  void SetTLSAlert(
      int err);
  int ShutdownStreamRead(
      int64_t stream_id,
      uint16_t code = NGTCP2_APP_NOERROR);
  int ShutdownStreamWrite(
      int64_t stream_id,
      uint16_t code = NGTCP2_APP_NOERROR);
  QuicSocket* Socket();
  SSL* ssl() { return ssl_.get(); }
  void WriteHandshake(
      const uint8_t* data,
      size_t datalen);

  const ngtcp2_cid* scid() const;

  // These may be implemented by QuicSession types
  virtual bool IsServer() const { return false; }
  virtual int OnClientHello() { return 0; }
  virtual void OnClientHelloDone() {}
  virtual int OnCert() { return 1; }
  virtual void OnCertDone(
      crypto::SecureContext* context,
      v8::Local<v8::Value> ocsp_response) {}

  // These must be implemented by QuicSession types
  virtual void AddToSocket(QuicSocket* socket) = 0;
  virtual int DoHandshake(
      const ngtcp2_path* path,
      const uint8_t* data,
      size_t datalen) = 0;
  virtual int HandleError() = 0;
  virtual void MaybeTimeout() = 0;
  virtual void OnIdleTimeout() = 0;
  virtual int OnKey(
      int name,
      const uint8_t* secret,
      size_t secretlen) = 0;
  virtual int OnTLSStatus() = 0;
  virtual int Receive(
      ngtcp2_pkt_hd* hd,
      ssize_t nread,
      const uint8_t* data,
      const struct sockaddr* addr,
      unsigned int flags) = 0;
  virtual void RemoveFromSocket() = 0;
  virtual bool SendConnectionClose() = 0;
  virtual int SendPendingData() = 0;
  virtual int TLSHandshake_Complete() = 0;
  virtual int TLSHandshake_Initial() = 0;
  virtual int TLSRead() = 0;
  virtual int VerifyPeerIdentity(const char* hostname) = 0;

  static void SetupTokenContext(
      CryptoContext* context);
  static int GenerateRetryToken(
      uint8_t* token,
      size_t* tokenlen,
      const sockaddr* addr,
      const ngtcp2_cid* ocid,
      CryptoContext* context,
      std::array<uint8_t, TOKEN_SECRETLEN>* token_secret);
  static int VerifyRetryToken(
      Environment* env,
      ngtcp2_cid* ocid,
      const ngtcp2_pkt_hd* hd,
      const sockaddr* addr,
      CryptoContext* context,
      std::array<uint8_t, TOKEN_SECRETLEN>* token_secret,
      uint64_t verification_expiration);

  static void DebugLog(
      void* user_data,
      const char* fmt, ...);

  void CheckAllocatedSize(size_t previous_size) override;
  void IncrementAllocatedSize(size_t size) override;
  void DecrementAllocatedSize(size_t size) override;

 private:
  void AckedCryptoOffset(
      ngtcp2_crypto_level crypto_level,
      uint64_t offset,
      size_t datalen);
  void AckedStreamDataOffset(
      int64_t stream_id,
      uint64_t offset,
      size_t datalen);
  void AssociateCID(
      ngtcp2_cid* cid);
  int DoHandshakeReadOnce(
      const ngtcp2_path* path,
      const uint8_t* data,
      size_t datalen);
  int DoHandshakeWriteOnce();
  QuicStream* FindStream(
      int64_t id);
  void HandshakeCompleted();
  inline bool IsInClosingPeriod();
  inline bool IsInDrainingPeriod();
  int PathValidation(
    const ngtcp2_path* path,
    ngtcp2_path_validation_result res);
  inline void ScheduleRetransmit();
  inline int SendPacket();
  inline void SetHandshakeCompleted();

  int StreamOpen(
      int64_t stream_id);
  int TLSHandshake();
  int WritePeerHandshake(
      ngtcp2_crypto_level crypto_level,
      const uint8_t* data,
      size_t datalen);

  virtual void DisassociateCID(
      const ngtcp2_cid* cid) {}
  virtual int ExtendMaxStreamsUni(
      uint64_t max_streams) { return 0; }
  virtual int ExtendMaxStreamsBidi(
      uint64_t max_streams) { return 0; }
  virtual int ReceiveRetry() { return 0; }
  virtual void StoreRemoteTransportParams(
      ngtcp2_transport_params* params) {}


  // ngtcp2 callbacks
  static int OnClientInitial(
      ngtcp2_conn* conn,
      void* user_data);
  static int OnReceiveClientInitial(
      ngtcp2_conn* conn,
      const ngtcp2_cid* dcid,
      void* user_data);
  static int OnReceiveCryptoData(
      ngtcp2_conn* conn,
      ngtcp2_crypto_level crypto_level,
      uint64_t offset,
      const uint8_t* data,
      size_t datalen,
      void* user_data);
  static int OnHandshakeCompleted(
      ngtcp2_conn* conn,
      void* user_data);
  static ssize_t OnDoHSEncrypt(
      ngtcp2_conn* conn,
      uint8_t* dest,
      size_t destlen,
      const uint8_t* plaintext,
      size_t plaintextlen,
      const uint8_t* key,
      size_t keylen,
      const uint8_t* nonce,
      size_t noncelen,
      const uint8_t* ad,
      size_t adlen,
      void* user_data);
  static ssize_t OnDoHSDecrypt(
      ngtcp2_conn* conn,
      uint8_t* dest,
      size_t destlen,
      const uint8_t* ciphertext,
      size_t ciphertextlen,
      const uint8_t* key,
      size_t keylen,
      const uint8_t* nonce,
      size_t noncelen,
      const uint8_t* ad,
      size_t adlen,
      void* user_data);
  static ssize_t OnDoEncrypt(
      ngtcp2_conn* conn,
      uint8_t* dest,
      size_t destlen,
      const uint8_t* plaintext,
      size_t plaintextlen,
      const uint8_t* key,
      size_t keylen,
      const uint8_t* nonce,
      size_t noncelen,
      const uint8_t* ad,
      size_t adlen,
      void* user_data);
  static ssize_t OnDoDecrypt(
      ngtcp2_conn* conn,
      uint8_t* dest,
      size_t destlen,
      const uint8_t* ciphertext,
      size_t ciphertextlen,
      const uint8_t* key,
      size_t keylen,
      const uint8_t* nonce,
      size_t noncelen,
      const uint8_t* ad,
      size_t adlen,
      void* user_data);
  static ssize_t OnDoInHPMask(
      ngtcp2_conn* conn,
      uint8_t* dest,
      size_t destlen,
      const uint8_t* key,
      size_t keylen,
      const uint8_t* sample,
      size_t samplelen,
      void* user_data);
  static ssize_t OnDoHPMask(
      ngtcp2_conn* conn,
      uint8_t* dest,
      size_t destlen,
      const uint8_t* key,
      size_t keylen,
      const uint8_t* sample,
      size_t samplelen,
      void* user_data);
  static int OnReceiveStreamData(
      ngtcp2_conn* conn,
      int64_t stream_id,
      int fin,
      uint64_t offset,
      const uint8_t* data,
      size_t datalen,
      void* user_data,
      void* stream_user_data);
  static int OnReceiveRetry(
      ngtcp2_conn* conn,
      const ngtcp2_pkt_hd* hd,
      const ngtcp2_pkt_retry* retry,
      void* user_data);
  static int OnAckedCryptoOffset(
      ngtcp2_conn* conn,
      ngtcp2_crypto_level crypto_level,
      uint64_t offset,
      size_t datalen,
      void* user_data);
  static int OnAckedStreamDataOffset(
      ngtcp2_conn* conn,
      int64_t stream_id,
      uint64_t offset,
      size_t datalen,
      void* user_data,
      void* stream_user_data);
  static int OnSelectPreferredAddress(
      ngtcp2_conn* conn,
      ngtcp2_addr* dest,
      const ngtcp2_preferred_addr* paddr,
      void* user_data);
  static int OnStreamClose(
      ngtcp2_conn* conn,
      int64_t stream_id,
      uint16_t app_error_code,
      void* user_data,
      void* stream_user_data);
  static int OnStreamOpen(
      ngtcp2_conn* conn,
      int64_t stream_id,
      void* user_data);
  static int OnStreamReset(
      ngtcp2_conn* conn,
      int64_t stream_id,
      uint64_t final_size,
      uint16_t app_error_code,
      void* user_data,
      void* stream_user_data);
  static int OnRand(
      ngtcp2_conn* conn,
      uint8_t* dest,
      size_t destlen,
      ngtcp2_rand_ctx ctx,
      void* user_data);
  static int OnGetNewConnectionID(
      ngtcp2_conn* conn,
      ngtcp2_cid* cid,
      uint8_t* token,
      size_t cidlen,
      void* user_data);
  static int OnRemoveConnectionID(
      ngtcp2_conn* conn,
      const ngtcp2_cid* cid,
      void* user_data);
  static int OnUpdateKey(
      ngtcp2_conn* conn,
      void* user_data);
  static int OnPathValidation(
      ngtcp2_conn* conn,
      const ngtcp2_path* path,
      ngtcp2_path_validation_result res,
      void* user_data);
  static void OnIdleTimeout(
      uv_timer_t* timer);
  static int OnExtendMaxStreamsUni(
      ngtcp2_conn* conn,
      uint64_t max_streams,
      void* user_data);
  static int OnExtendMaxStreamsBidi(
      ngtcp2_conn* conn,
      uint64_t max_streams,
      void* user_data);
  static int OnExtendMaxStreamData(
      ngtcp2_conn* conn,
      int64_t stream_id,
      uint64_t max_data,
      void* user_data,
      void* stream_user_data);

  static void OnKeylog(const SSL* ssl, const char* line);

  virtual void InitTLS_Post() = 0;

  int ReceiveClientInitial(
      const ngtcp2_cid* dcid);
  int ReceiveCryptoData(
      ngtcp2_crypto_level crypto_level,
      uint64_t offset,
      const uint8_t* data,
      size_t datalen);
  ssize_t DoHSEncrypt(
      uint8_t* dest,
      size_t destlen,
      const uint8_t* plaintext,
      size_t plaintextlen,
      const uint8_t* key,
      size_t keylen,
      const uint8_t* nonce,
      size_t noncelen,
      const uint8_t* ad,
      size_t adlen);
  ssize_t DoHSDecrypt(
      uint8_t* dest,
      size_t destlen,
      const uint8_t* ciphertext,
      size_t ciphertextlen,
      const uint8_t* key,
      size_t keylen,
      const uint8_t* nonce,
      size_t noncelen,
      const uint8_t* ad,
      size_t adlen);
  ssize_t DoEncrypt(
      uint8_t* dest,
      size_t destlen,
      const uint8_t* plaintext,
      size_t plaintextlen,
      const uint8_t* key,
      size_t keylen,
      const uint8_t* nonce,
      size_t noncelen,
      const uint8_t* ad,
      size_t adlen);
  ssize_t DoDecrypt(
      uint8_t* dest,
      size_t destlen,
      const uint8_t* ciphertext,
      size_t ciphertextlen,
      const uint8_t* key,
      size_t keylen,
      const uint8_t* nonce,
      size_t noncelen,
      const uint8_t* ad,
      size_t adlen);
  ssize_t DoInHPMask(
      uint8_t* dest,
      size_t destlen,
      const uint8_t* key,
      size_t keylen,
      const uint8_t* sample,
      size_t samplelen);
  ssize_t DoHPMask(
      uint8_t* dest,
      size_t destlen,
      const uint8_t* key,
      size_t keylen,
      const uint8_t* sample,
      size_t samplelen);
  void InitTLS();
  void Keylog(const char* line);
  void StreamClose(
      int64_t stream_id,
      uint16_t app_error_code);
  void StreamReset(
      int64_t stream_id,
      uint64_t final_size,
      uint16_t app_error_code);
  int UpdateKey();
  void RemoveConnectionID(
      const ngtcp2_cid* cid);
  inline int GetNewConnectionID(
      ngtcp2_cid* cid,
      uint8_t* token,
      size_t cidlen);
  void ExtendMaxStreamData(
      int64_t stream_id,
      uint64_t max_data);
  int WritePackets();

  inline QuicStream* CreateStream(
      int64_t stream_id);

  void SetLocalAddress(
      const ngtcp2_addr* addr);

  typedef ssize_t(*ngtcp2_close_fn)(
    ngtcp2_conn* conn,
    ngtcp2_path* path,
    uint8_t* dest,
    size_t destlen,
    uint16_t error_code,
    ngtcp2_tstamp ts);

  static inline ngtcp2_close_fn SelectCloseFn(QuicErrorFamily family) {
    if (family == QUIC_ERROR_APPLICATION)
      return ngtcp2_conn_write_application_close;
    return ngtcp2_conn_write_connection_close;
  }

  inline bool IsHandshakeSuspended() {
    return client_hello_cb_running_ || cert_cb_running_;
  }

  virtual ngtcp2_crypto_level GetServerCryptoLevel() = 0;
  virtual ngtcp2_crypto_level GetClientCryptoLevel() = 0;
  virtual void SetServerCryptoLevel(ngtcp2_crypto_level level) = 0;
  virtual void SetClientCryptoLevel(ngtcp2_crypto_level level) = 0;
  virtual void SetLocalCryptoLevel(ngtcp2_crypto_level level) = 0;
  virtual int Start() { return 0; }
  virtual int SelectPreferredAddress(
    ngtcp2_addr* dest,
    const ngtcp2_preferred_addr* paddr) { return 0; }

  ngtcp2_crypto_level rx_crypto_level_;
  ngtcp2_crypto_level tx_crypto_level_;
  QuicError last_error_;
  bool closing_;
  bool destroyed_;
  bool initial_;
  crypto::SSLPointer ssl_;
  ngtcp2_conn* connection_;
  SocketAddress remote_address_;
  size_t max_pktlen_;

  Timer* idle_;
  Timer* retransmit_;

  QuicSocket* socket_;
  CryptoContext hs_crypto_ctx_;
  CryptoContext crypto_ctx_;
  std::vector<uint8_t> tx_secret_;
  std::vector<uint8_t> rx_secret_;
  ngtcp2_cid scid_;

  // The sendbuf_ is a temporary holding for data being collected
  // to send. On send, the contents of the sendbuf_ will be
  // transfered to the txbuf_
  QuicBuffer sendbuf_;

  // The handshake_ is a temporary holding for outbound TLS handshake
  // data. On send, the contents of the handshake_ will be
  // transfered to the txbuf_
  QuicBuffer handshake_;

  // The txbuf_ contains all of the data that has been passed off
  // to the QuicSocket. The data will remain in the txbuf_ until
  // it is successfully sent. This is a std::shared_ptr because
  // references of txbuf_ are shared with QuicSocket::SendWrap
  // instances that are responsible for actually sending the data.
  // Each QuicSocket::SendWrap uses a std::weak_ptr. When the
  // QuicSession object is destroyed, those QuicSocket::SendWrap
  // instances may still be alive but will not invoke the Done
  // callback.
  std::shared_ptr<QuicBuffer> txbuf_;

  // Temporary holding for inbound TLS handshake data.
  std::vector<uint8_t> peer_handshake_;
  size_t ncread_;

  std::map<int64_t, QuicStream*> streams_;

  AliasedFloat64Array state_;

  // The amount of memory allocated by ngtcp2 internals
  uint64_t current_ngtcp2_memory_;
  size_t max_cid_len_;
  size_t min_cid_len_;

  std::string alpn_;

  mem::Allocator<ngtcp2_mem> allocator_;
  bool cert_cb_running_;
  bool client_hello_cb_running_;
  bool is_tls_callback_;

  struct session_stats {
    // The timestamp at which the session was created
    uint64_t created_at;
    // The timestamp at which the handshake was started
    uint64_t handshake_start_at;
    // The timestamp at which the most recent handshake
    // message was sent
    uint64_t handshake_send_at;
    // The timestamp at which the most recent handshake
    // message was received
    uint64_t handshake_continue_at;
    // The timestamp at which handshake completed
    uint64_t handshake_completed_at;
    // The timestamp at which the most recently sent
    // non-handshake packets were sent
    uint64_t session_sent_at;
    // The timestamp at which the most recently received
    // non-handshake packets were received
    uint64_t session_received_at;
    // The timestamp at which a graceful close was started
    uint64_t closing_at;
    // The total number of bytes received (and not ignored)
    // by this QuicSession
    uint64_t bytes_received;
    // The total number of bytes sent by this QuicSession
    uint64_t bytes_sent;
    // The total bidirectional stream count
    uint64_t bidi_stream_count;
    // The total unidirectional stream count
    uint64_t uni_stream_count;
    // The total number of peer-initiated streams
    uint64_t streams_in_count;
    // The total number of local-initiated streams
    uint64_t streams_out_count;
    // The total number of keyupdates
    uint64_t keyupdate_count;
  };
  session_stats session_stats_{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

  AliasedBigUint64Array stats_buffer_;

  template <typename... Members>
  void IncrementSocketStat(
      uint64_t amount,
      session_stats* a,
      Members... mems) {
    IncrementStat<session_stats, Members...>(amount, a, mems...);
  }

  class TLSHandshakeCallbackScope {
   public:
    explicit TLSHandshakeCallbackScope(QuicSession* session) :
        session_(session) {
      session_->is_tls_callback_ = true;
    }

    ~TLSHandshakeCallbackScope() {
      session_->is_tls_callback_ = false;
    }

    static bool IsInTLSHandshakeCallback(QuicSession* session) {
      return session->is_tls_callback_;
    }

   private:
    QuicSession* session_;
  };

  class TLSHandshakeScope {
   public:
    TLSHandshakeScope(QuicSession* session, bool* monitor) :
        session_{session},
        monitor_(monitor) {}

    ~TLSHandshakeScope() {
      if (session_->IsHandshakeSuspended()) {
        // There are a couple of monitor fields in QuicSession
        // (cert_cb_running_ and client_hello_cb_running_).
        // When one of those are true, IsHandshakeSuspended
        // will be true. We set the monitor to false so we
        // can keep the handshake going when the TLS Handshake
        // is continued.
        *monitor_ = false;
        // Only continue the TLS handshake if we are not currently running
        // synchronously within the TLS handshake function. This can happen
        // when the callback function passed to the clientHello and cert
        // event handlers is called synchronously. If the function is called
        // asynchronously, then we have to manually continue the handshake.
        if (!TLSHandshakeCallbackScope::IsInTLSHandshakeCallback(session_)) {
          session_->TLSHandshake();
          session_->SendPendingData();
        }
      }
    }

   private:
    QuicSession* session_;
    bool* monitor_;
  };

  // SendScope will cause the session to flush it's
  // current pending data queue to the underlying
  // socket.
  class SendScope {
   public:
    explicit SendScope(QuicSession* session) : session_(session) {}
    ~SendScope() {
      if (session_->IsDestroyed())
        return;
      session_->SendPendingData();
      session_->idle_->Update();
    }
   private:
    QuicSession* session_;
  };

  friend class QuicServerSession;
  friend class QuicClientSession;
};

class QuicServerSession : public QuicSession {
 public:
  static void Initialize(
      Environment* env,
      v8::Local<v8::Object> target,
      v8::Local<v8::Context> context);

  static std::shared_ptr<QuicSession> New(
      QuicSocket* socket,
      const ngtcp2_cid* rcid,
      const struct sockaddr* addr,
      const ngtcp2_cid* dcid,
      const ngtcp2_cid* ocid,
      uint32_t version,
      const std::string& alpn = NGTCP2_ALPN_H3,
      bool reject_unauthorized = true,
      bool request_cert_ = true);

  void AddToSocket(QuicSocket* socket) override;

  void Init(
      const struct sockaddr* addr,
      const ngtcp2_cid* dcid,
      const ngtcp2_cid* ocid,
      uint32_t version);

  bool IsDraining();
  bool IsServer() const override { return true; }
  int OnClientHello() override;
  void OnClientHelloDone() override;
  int OnTLSStatus() override;

  void MaybeTimeout() override;

  int OnCert() override;
  void OnCertDone(
      crypto::SecureContext* context,
      v8::Local<v8::Value> ocsp_response) override;

  int VerifyPeerIdentity(const char* hostname) override;

  const ngtcp2_cid* rcid() const;
  ngtcp2_cid* pscid();

  void MemoryInfo(MemoryTracker* tracker) const override {}
  SET_MEMORY_INFO_NAME(QuicServerSession)
  SET_SELF_SIZE(QuicServerSession)

 private:
  QuicServerSession(
      QuicSocket* socket,
      v8::Local<v8::Object> wrap,
      const ngtcp2_cid* rcid,
      const struct sockaddr* addr,
      const ngtcp2_cid* dcid,
      const ngtcp2_cid* ocid,
      uint32_t version,
      const std::string& alpn,
      bool reject_unauthorized,
      bool request_cert);

  void DisassociateCID(
      const ngtcp2_cid* cid) override;
  int DoHandshake(
      const ngtcp2_path* path,
      const uint8_t* data,
      size_t datalen) override;
  int HandleError() override;
  void InitTLS_Post() override;
  void OnIdleTimeout() override;
  int OnKey(
      int name,
      const uint8_t* secret,
      size_t secretlen) override;
  int Receive(
      ngtcp2_pkt_hd* hd,
      ssize_t nread,
      const uint8_t* data,
      const struct sockaddr* addr,
      unsigned int flags) override;
  void RemoveFromSocket() override;
  bool SendConnectionClose() override;
  int SendPendingData() override;
  int TLSHandshake_Complete() override;
  int TLSHandshake_Initial() override;
  int TLSRead() override;

  int StartClosingPeriod();
  void StartDrainingPeriod();

  ngtcp2_crypto_level GetServerCryptoLevel() override;
  ngtcp2_crypto_level GetClientCryptoLevel() override;
  void SetServerCryptoLevel(ngtcp2_crypto_level level) override;
  void SetClientCryptoLevel(ngtcp2_crypto_level level) override;
  void SetLocalCryptoLevel(ngtcp2_crypto_level level) override;

  ngtcp2_cid pscid_;
  ngtcp2_cid rcid_;
  bool draining_;
  bool reject_unauthorized_;
  bool request_cert_;

  MallocedBuffer<uint8_t> conn_closebuf_;
  v8::Global<v8::ArrayBufferView> ocsp_response_;

  const ngtcp2_conn_callbacks callbacks_ = {
    nullptr,
    OnReceiveClientInitial,
    OnReceiveCryptoData,
    OnHandshakeCompleted,
    nullptr,  // recv_version_negotiation
    OnDoHSEncrypt,
    OnDoHSDecrypt,
    OnDoEncrypt,
    OnDoDecrypt,
    OnDoInHPMask,
    OnDoHPMask,
    OnReceiveStreamData,
    OnAckedCryptoOffset,
    OnAckedStreamDataOffset,
    OnStreamOpen,
    OnStreamClose,
    nullptr,  // recv_stateless_reset
    nullptr,  // recv_retry
    nullptr,  // extend_max_streams_bidi
    nullptr,  // extend_max_streams_uni
    OnRand,
    OnGetNewConnectionID,
    OnRemoveConnectionID,
    OnUpdateKey,
    OnPathValidation,
    nullptr,  // select_preferred_addr
    OnStreamReset,
    OnExtendMaxStreamsBidi,
    OnExtendMaxStreamsUni,
    OnExtendMaxStreamData
  };

  friend class QuicSession;
};

class QuicClientSession : public QuicSession {
 public:
  static void Initialize(
      Environment* env,
      v8::Local<v8::Object> target,
      v8::Local<v8::Context> context);

  static std::shared_ptr<QuicSession> New(
      QuicSocket* socket,
      const struct sockaddr* addr,
      uint32_t version,
      crypto::SecureContext* context,
      const char* hostname,
      uint32_t port,
      v8::Local<v8::Value> early_transport_params,
      v8::Local<v8::Value> session_ticket,
      v8::Local<v8::Value> dcid,
      int select_preferred_address_policy =
          QUIC_PREFERRED_ADDRESS_IGNORE,
      const std::string& alpn = NGTCP2_ALPN_H3,
      bool request_ocsp = false);

  QuicClientSession(
      QuicSocket* socket,
      v8::Local<v8::Object> wrap,
      const struct sockaddr* addr,
      uint32_t version,
      crypto::SecureContext* context,
      const char* hostname,
      uint32_t port,
      v8::Local<v8::Value> early_transport_params,
      v8::Local<v8::Value> session_ticket,
      v8::Local<v8::Value> dcid,
      int select_preferred_address_policy,
      const std::string& alpn,
      bool request_ocsp);

  void AddToSocket(QuicSocket* socket) override;

  void MaybeTimeout() override;

  int OnTLSStatus() override;

  int SetSocket(
      QuicSocket* socket,
      bool nat_rebinding = false);
  int SetSession(
      SSL_SESSION* session);
  int SetEarlyTransportParams(
      v8::Local<v8::Value> buffer);
  int SetSession(
      v8::Local<v8::Value> buffer);

  int VerifyPeerIdentity(const char* hostname) override;

  void MemoryInfo(MemoryTracker* tracker) const override {}

  SET_MEMORY_INFO_NAME(QuicClientSession)
  SET_SELF_SIZE(QuicClientSession)

 private:
  int DoHandshake(
      const ngtcp2_path* path,
      const uint8_t* data,
      size_t datalen) override;
  int ExtendMaxStreamsUni(
      uint64_t max_streams) override;
  int ExtendMaxStreamsBidi(
      uint64_t max_streams) override;
  int HandleError() override;
  void InitTLS_Post() override;
  void OnIdleTimeout() override;
  int OnKey(
      int name,
      const uint8_t* secret,
      size_t secretlen) override;
  int Receive(
      ngtcp2_pkt_hd* hd,
      ssize_t nread,
      const uint8_t* data,
      const struct sockaddr* addr,
      unsigned int flags) override;
  int ReceiveRetry() override;
  void RemoveFromSocket() override;
  int SelectPreferredAddress(
    ngtcp2_addr* dest,
    const ngtcp2_preferred_addr* paddr) override;
  bool SendConnectionClose() override;
  int SendPendingData() override;
  int Start() override;
  void StoreRemoteTransportParams(
      ngtcp2_transport_params* params) override;
  int TLSHandshake_Complete() override;
  int TLSHandshake_Initial() override;
  int TLSRead() override;

  int Init(
      const struct sockaddr* addr,
      uint32_t version,
      v8::Local<v8::Value> early_transport_params,
      v8::Local<v8::Value> session_ticket,
      v8::Local<v8::Value> dcid);
  int ExtendMaxStreams(
      bool bidi,
      uint64_t max_streams);
  int SetupInitialCryptoContext();

  ngtcp2_crypto_level GetServerCryptoLevel() override;
  ngtcp2_crypto_level GetClientCryptoLevel() override;
  void SetServerCryptoLevel(ngtcp2_crypto_level level) override;
  void SetClientCryptoLevel(ngtcp2_crypto_level level) override;
  void SetLocalCryptoLevel(ngtcp2_crypto_level level) override;

  bool resumption_;
  std::string hostname_;
  uint32_t port_;

  MaybeStackBuffer<char> transportParams_;
  int select_preferred_address_policy_;
  bool request_ocsp_;

  const ngtcp2_conn_callbacks callbacks_ = {
    OnClientInitial,
    nullptr,
    OnReceiveCryptoData,
    OnHandshakeCompleted,
    nullptr,
    OnDoHSEncrypt,
    OnDoHSDecrypt,
    OnDoEncrypt,
    OnDoDecrypt,
    OnDoInHPMask,
    OnDoHPMask,
    OnReceiveStreamData,
    OnAckedCryptoOffset,
    OnAckedStreamDataOffset,
    OnStreamOpen,
    OnStreamClose,
    nullptr,
    OnReceiveRetry,
    OnExtendMaxStreamsBidi,
    OnExtendMaxStreamsUni,
    OnRand,
    OnGetNewConnectionID,
    OnRemoveConnectionID,
    OnUpdateKey,
    OnPathValidation,
    OnSelectPreferredAddress,  // select_preferred_addr
    nullptr,  // stream_reset
    nullptr,  // extend_max_remote_streams_bidi
    nullptr,  // extend_max_remote_streams_uni
    OnExtendMaxStreamData
  };

  friend class QuicSession;
};

}  // namespace quic
}  // namespace node

#endif  // NODE_WANT_INTERNALS
#endif  // SRC_NODE_QUIC_SESSION_H_