/*
 * ngtcp2
 *
 * Copyright (c) 2017 ngtcp2 contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <cstdlib>
#include <cassert>
#include <cerrno>
#include <iostream>
#include <algorithm>
#include <memory>
#include <fstream>

#include <unistd.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/mman.h>

#include <http-parser/http_parser.h>

#include "client.h"
#include "network.h"
#include "debug.h"
#include "util.h"
#include "shared.h"
#include "template.h"

using namespace ngtcp2;
using namespace std::literals;

namespace {
auto randgen = util::make_mt19937();
} // namespace

Config config{};

Stream::Stream(const Request &req, int64_t stream_id)
    : req(req), stream_id(stream_id), fd(-1) {}

Stream::~Stream() {
  if (fd != -1) {
    close(fd);
  }
}

int Stream::open_file(const std::string_view &path) {
  assert(fd == -1);

  std::string_view filename;

  auto it = std::find(std::rbegin(path), std::rend(path), '/').base();
  if (it == std::end(path)) {
    filename = "index.html";
  } else {
    filename = std::string_view{it, static_cast<size_t>(std::end(path) - it)};
    if (filename == ".." || filename == ".") {
      std::cerr << "Invalid file name: " << filename << std::endl;
      return -1;
    }
  }

  auto fname = std::string{config.download};
  fname += '/';
  fname += filename;

  fd = open(fname.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
            S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
  if (fd == -1) {
    std::cerr << "open: Could not open file " << fname << ": "
              << strerror(errno) << std::endl;
    return -1;
  }

  return 0;
}

namespace {
void writecb(struct ev_loop *loop, ev_io *w, int revents) {
  ev_io_stop(loop, w);

  auto c = static_cast<Client *>(w->data);

  auto rv = c->on_write();
  switch (rv) {
  case 0:
    return;
  case NETWORK_ERR_SEND_BLOCKED:
    c->start_wev();
    return;
  }
}
} // namespace

namespace {
void readcb(struct ev_loop *loop, ev_io *w, int revents) {
  auto c = static_cast<Client *>(w->data);

  if (c->on_read() != 0) {
    return;
  }
  auto rv = c->on_write();
  switch (rv) {
  case 0:
    return;
  case NETWORK_ERR_SEND_BLOCKED:
    c->start_wev();
    return;
  }
}
} // namespace

namespace {
void timeoutcb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto c = static_cast<Client *>(w->data);

  if (!config.quiet) {
    std::cerr << "Timeout" << std::endl;
  }

  c->idle_timeout();
}
} // namespace

void Client::idle_timeout() {
  last_error_ = quic_err_idle_timeout();
  disconnect();
}

namespace {
void retransmitcb(struct ev_loop *loop, ev_timer *w, int revents) {
  int rv;
  auto c = static_cast<Client *>(w->data);

  rv = c->handle_expiry();
  if (rv != 0) {
    goto fail;
  }

  rv = c->on_write();
  if (rv != 0) {
    goto fail;
  }

  return;

fail:
  switch (rv) {
  case NETWORK_ERR_SEND_BLOCKED:
    c->start_wev();
    return;
  default:
    c->disconnect();
    return;
  }
}
} // namespace

namespace {
void change_local_addrcb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto c = static_cast<Client *>(w->data);

  c->change_local_addr();
}
} // namespace

namespace {
void key_updatecb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto c = static_cast<Client *>(w->data);

  if (c->initiate_key_update() != 0) {
    c->disconnect();
  }
}
} // namespace

namespace {
void delay_streamcb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto c = static_cast<Client *>(w->data);

  ev_timer_stop(loop, w);
  c->on_extend_max_streams();

  auto rv = c->on_write();
  switch (rv) {
  case 0:
    return;
  case NETWORK_ERR_SEND_BLOCKED:
    c->start_wev();
    return;
  }
}
} // namespace

namespace {
void siginthandler(struct ev_loop *loop, ev_signal *w, int revents) {
  ev_break(loop, EVBREAK_ALL);
}
} // namespace

Client::Client(struct ev_loop *loop)
    : local_addr_{},
      remote_addr_{},
      ecn_(0),
      max_pktlen_(0),
      loop_(loop),
      fd_(-1),
      httpconn_(nullptr),
      addr_(nullptr),
      port_(nullptr),
      sendbuf_{NGTCP2_MAX_PKTLEN_IPV4},
      nstreams_done_(0),
      nstreams_closed_(0),
      nkey_update_(0),
      version_(0),
      early_data_(false),
      should_exit_(false),
      handshake_confirmed_(false) {
  ev_io_init(&wev_, writecb, 0, EV_WRITE);
  ev_io_init(&rev_, readcb, 0, EV_READ);
  wev_.data = this;
  rev_.data = this;
  ev_timer_init(&timer_, timeoutcb, 0.,
                static_cast<double>(config.timeout) / NGTCP2_SECONDS);
  timer_.data = this;
  ev_timer_init(&rttimer_, retransmitcb, 0., 0.);
  rttimer_.data = this;
  ev_timer_init(&change_local_addr_timer_, change_local_addrcb,
                static_cast<double>(config.change_local_addr) / NGTCP2_SECONDS,
                0.);
  change_local_addr_timer_.data = this;
  ev_timer_init(&key_update_timer_, key_updatecb,
                static_cast<double>(config.key_update) / NGTCP2_SECONDS, 0.);
  key_update_timer_.data = this;
  ev_timer_init(&delay_stream_timer_, delay_streamcb,
                static_cast<double>(config.delay_stream) / NGTCP2_SECONDS, 0.);
  delay_stream_timer_.data = this;
  ev_signal_init(&sigintev_, siginthandler, SIGINT);

  application_rx_key_cb_ = [this]() { return setup_httpconn(); };
}

Client::~Client() {
  disconnect();

  ev_io_stop(loop_, &wev_);

  if (httpconn_) {
    nghttp3_conn_del(httpconn_);
    httpconn_ = nullptr;
  }

  if (fd_ != -1) {
    ::close(fd_);
    fd_ = -1;
  }
}

void Client::disconnect() {
  handle_error();

  config.tx_loss_prob = 0;

  ev_timer_stop(loop_, &delay_stream_timer_);
  ev_timer_stop(loop_, &key_update_timer_);
  ev_timer_stop(loop_, &change_local_addr_timer_);
  ev_timer_stop(loop_, &rttimer_);
  ev_timer_stop(loop_, &timer_);

  ev_io_stop(loop_, &rev_);

  ev_signal_stop(loop_, &sigintev_);
}

namespace {
int recv_crypto_data(ngtcp2_conn *conn, ngtcp2_crypto_level crypto_level,
                     uint64_t offset, const uint8_t *data, size_t datalen,
                     void *user_data) {
  if (!config.quiet && !config.no_quic_dump) {
    debug::print_crypto_data(crypto_level, data, datalen);
  }

  auto c = static_cast<Client *>(user_data);

  if (c->recv_crypto_data(crypto_level, data, datalen) != 0) {
    if (auto err = ngtcp2_conn_get_tls_error(conn); err) {
      return err;
    }
    return NGTCP2_ERR_CRYPTO;
  }

  return 0;
}
} // namespace

namespace {
int recv_stream_data(ngtcp2_conn *conn, uint32_t flags, int64_t stream_id,
                     uint64_t offset, const uint8_t *data, size_t datalen,
                     void *user_data, void *stream_user_data) {
  if (!config.quiet && !config.no_quic_dump) {
    debug::print_stream_data(stream_id, data, datalen);
  }

  auto c = static_cast<Client *>(user_data);

  if (c->recv_stream_data(flags, stream_id, data, datalen) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

namespace {
int acked_crypto_offset(ngtcp2_conn *conn, ngtcp2_crypto_level crypto_level,
                        uint64_t offset, uint64_t datalen, void *user_data) {
  auto c = static_cast<Client *>(user_data);
  c->remove_tx_crypto_data(crypto_level, offset, datalen);

  return 0;
}
} // namespace

namespace {
int acked_stream_data_offset(ngtcp2_conn *conn, int64_t stream_id,
                             uint64_t offset, uint64_t datalen, void *user_data,
                             void *stream_user_data) {
  auto c = static_cast<Client *>(user_data);
  if (c->acked_stream_data_offset(stream_id, datalen) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  return 0;
}
} // namespace

namespace {
int handshake_completed(ngtcp2_conn *conn, void *user_data) {
  auto c = static_cast<Client *>(user_data);

  if (!config.quiet) {
    debug::handshake_completed(conn, user_data);
  }

  if (c->handshake_completed() != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

int Client::handshake_completed() {
  if (early_data_ && !tls_session_.get_early_data_accepted()) {
    if (!config.quiet) {
      std::cerr << "Early data was rejected by server" << std::endl;
    }

    if (auto rv = ngtcp2_conn_early_data_rejected(conn_); rv != 0) {
      std::cerr << "ngtcp2_conn_early_data_rejected: " << ngtcp2_strerror(rv)
                << std::endl;
      return -1;
    }
  }

  if (!config.quiet) {
    std::cerr << "Negotiated cipher suite is " << tls_session_.get_cipher_name()
              << std::endl;
    std::cerr << "Negotiated ALPN is " << tls_session_.get_selected_alpn()
              << std::endl;
  }

  return 0;
}

namespace {
int handshake_confirmed(ngtcp2_conn *conn, void *user_data) {
  auto c = static_cast<Client *>(user_data);

  if (!config.quiet) {
    debug::handshake_confirmed(conn, user_data);
  }

  if (c->handshake_confirmed() != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

int Client::handshake_confirmed() {
  handshake_confirmed_ = true;

  if (config.change_local_addr) {
    start_change_local_addr_timer();
  }
  if (config.key_update) {
    start_key_update_timer();
  }
  if (config.delay_stream) {
    start_delay_stream_timer();
  }

  return 0;
}

namespace {
int stream_close(ngtcp2_conn *conn, int64_t stream_id, uint64_t app_error_code,
                 void *user_data, void *stream_user_data) {
  auto c = static_cast<Client *>(user_data);

  if (c->on_stream_close(stream_id, app_error_code) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

namespace {
int stream_reset(ngtcp2_conn *conn, int64_t stream_id, uint64_t final_size,
                 uint64_t app_error_code, void *user_data,
                 void *stream_user_data) {
  auto c = static_cast<Client *>(user_data);

  if (c->on_stream_reset(stream_id) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

namespace {
int extend_max_streams_bidi(ngtcp2_conn *conn, uint64_t max_streams,
                            void *user_data) {
  auto c = static_cast<Client *>(user_data);

  if (c->on_extend_max_streams() != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

namespace {
int rand(uint8_t *dest, size_t destlen, const ngtcp2_rand_ctx *rand_ctx,
         ngtcp2_rand_usage usage) {
  auto dis = std::uniform_int_distribution<uint8_t>(0, 255);
  std::generate(dest, dest + destlen, [&dis]() { return dis(randgen); });
  return 0;
}
} // namespace

namespace {
int get_new_connection_id(ngtcp2_conn *conn, ngtcp2_cid *cid, uint8_t *token,
                          size_t cidlen, void *user_data) {
  auto dis = std::uniform_int_distribution<uint8_t>(0, 255);
  auto f = [&dis]() { return dis(randgen); };

  std::generate_n(cid->data, cidlen, f);
  cid->datalen = cidlen;
  auto md = util::crypto_md_sha256();
  if (ngtcp2_crypto_generate_stateless_reset_token(
          token, &md, config.static_secret.data(), config.static_secret.size(),
          cid) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

namespace {
int remove_connection_id(ngtcp2_conn *conn, const ngtcp2_cid *cid,
                         void *user_data) {
  return 0;
}
} // namespace

namespace {
int do_hp_mask(uint8_t *dest, const ngtcp2_crypto_cipher *hp,
               const ngtcp2_crypto_cipher_ctx *hp_ctx, const uint8_t *sample) {
  if (ngtcp2_crypto_hp_mask(dest, hp, hp_ctx, sample) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  if (!config.quiet && config.show_secret) {
    debug::print_hp_mask(dest, NGTCP2_HP_MASKLEN, sample, NGTCP2_HP_SAMPLELEN);
  }

  return 0;
}
} // namespace

namespace {
int update_key(ngtcp2_conn *conn, uint8_t *rx_secret, uint8_t *tx_secret,
               ngtcp2_crypto_aead_ctx *rx_aead_ctx, uint8_t *rx_iv,
               ngtcp2_crypto_aead_ctx *tx_aead_ctx, uint8_t *tx_iv,
               const uint8_t *current_rx_secret,
               const uint8_t *current_tx_secret, size_t secretlen,
               void *user_data) {
  auto c = static_cast<Client *>(user_data);

  if (c->update_key(rx_secret, tx_secret, rx_aead_ctx, rx_iv, tx_aead_ctx,
                    tx_iv, current_rx_secret, current_tx_secret,
                    secretlen) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

namespace {
int path_validation(ngtcp2_conn *conn, const ngtcp2_path *path,
                    ngtcp2_path_validation_result res, void *user_data) {
  if (!config.quiet) {
    debug::path_validation(path, res);
  }
  return 0;
}
} // namespace

namespace {
int select_preferred_address(ngtcp2_conn *conn, ngtcp2_addr *dest,
                             const ngtcp2_preferred_addr *paddr,
                             void *user_data) {
  auto c = static_cast<Client *>(user_data);
  Address addr;

  if (config.no_preferred_addr) {
    return 0;
  }

  if (c->select_preferred_address(addr, paddr) != 0) {
    dest->addrlen = 0;
    return 0;
  }

  dest->addrlen = addr.len;
  memcpy(dest->addr, &addr.su, dest->addrlen);

  return 0;
}
} // namespace

namespace {
int extend_max_stream_data(ngtcp2_conn *conn, int64_t stream_id,
                           uint64_t max_data, void *user_data,
                           void *stream_user_data) {
  auto c = static_cast<Client *>(user_data);
  if (c->extend_max_stream_data(stream_id, max_data) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  return 0;
}
} // namespace

int Client::extend_max_stream_data(int64_t stream_id, uint64_t max_data) {
  if (auto rv = nghttp3_conn_unblock_stream(httpconn_, stream_id); rv != 0) {
    std::cerr << "nghttp3_conn_unblock_stream: " << nghttp3_strerror(rv)
              << std::endl;
    return -1;
  }
  return 0;
}

namespace {
int recv_new_token(ngtcp2_conn *conn, const ngtcp2_vec *token,
                   void *user_data) {
  if (config.token_file.empty()) {
    return 0;
  }

  util::write_token(config.token_file, token->base, token->len);

  return 0;
}
} // namespace

int Client::init(int fd, const Address &local_addr, const Address &remote_addr,
                 const char *addr, const char *port, uint32_t version,
                 const TLSClientContext &tls_ctx) {
  local_addr_ = local_addr;
  remote_addr_ = remote_addr;
  fd_ = fd;
  addr_ = addr;
  port_ = port;
  version_ = version;

  switch (remote_addr_.su.storage.ss_family) {
  case AF_INET:
    max_pktlen_ = NGTCP2_MAX_PKTLEN_IPV4;
    break;
  case AF_INET6:
    max_pktlen_ = NGTCP2_MAX_PKTLEN_IPV6;
    break;
  default:
    return -1;
  }

  auto callbacks = ngtcp2_callbacks{
      ngtcp2_crypto_client_initial_cb,
      nullptr, // recv_client_initial
      ::recv_crypto_data,
      ::handshake_completed,
      nullptr, // recv_version_negotiation
      ngtcp2_crypto_encrypt_cb,
      ngtcp2_crypto_decrypt_cb,
      do_hp_mask,
      ::recv_stream_data,
      acked_crypto_offset,
      ::acked_stream_data_offset,
      nullptr, // stream_open
      stream_close,
      nullptr, // recv_stateless_reset
      ngtcp2_crypto_recv_retry_cb,
      extend_max_streams_bidi,
      nullptr, // extend_max_streams_uni
      rand,
      get_new_connection_id,
      remove_connection_id,
      ::update_key,
      path_validation,
      ::select_preferred_address,
      stream_reset,
      nullptr, // extend_max_remote_streams_bidi,
      nullptr, // extend_max_remote_streams_uni,
      ::extend_max_stream_data,
      nullptr, // dcid_status
      ::handshake_confirmed,
      ::recv_new_token,
      ngtcp2_crypto_delete_crypto_aead_ctx_cb,
      ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
  };

  auto dis = std::uniform_int_distribution<uint8_t>(
      0, std::numeric_limits<uint8_t>::max());
  auto generate_cid = [&dis](ngtcp2_cid &cid, size_t len) {
    cid.datalen = len;
    std::generate(std::begin(cid.data), std::begin(cid.data) + cid.datalen,
                  [&dis]() { return dis(randgen); });
  };

  ngtcp2_cid scid, dcid;
  if (config.scid_present) {
    scid = config.scid;
  } else {
    generate_cid(scid, 17);
  }
  if (config.dcid.datalen == 0) {
    generate_cid(dcid, 18);
  } else {
    dcid = config.dcid;
  }

  ngtcp2_settings settings;
  ngtcp2_settings_default(&settings);
  settings.log_printf = config.quiet ? nullptr : debug::log_printf;
  if (!config.qlog_file.empty() || !config.qlog_dir.empty()) {
    std::string path;
    if (!config.qlog_file.empty()) {
      path = config.qlog_file;
    } else {
      path = std::string{config.qlog_dir};
      path += '/';
      path += util::format_hex(scid.data, scid.datalen);
      path += ".qlog";
    }
    qlog_ = fopen(path.c_str(), "w");
    if (qlog_ == nullptr) {
      std::cerr << "Could not open qlog file " << path << ": "
                << strerror(errno) << std::endl;
      return -1;
    }
    settings.qlog.write = qlog_write_cb;
  }
  settings.max_udp_payload_size = max_pktlen_;
  settings.cc_algo =
      config.cc == "cubic" ? NGTCP2_CC_ALGO_CUBIC : NGTCP2_CC_ALGO_RENO;
  settings.initial_ts = util::timestamp(loop_);
  settings.initial_rtt = config.initial_rtt;
  settings.max_window = config.max_window;
  settings.max_stream_window = config.max_stream_window;

  std::string token;

  if (!config.token_file.empty()) {
    std::cerr << "Reading token file " << config.token_file << std::endl;

    auto [t, rv] = util::read_token(config.token_file);
    if (rv == 0) {
      token = std::move(t);
      settings.token.base = reinterpret_cast<uint8_t *>(token.data());
      settings.token.len = token.size();
    }
  }

  ngtcp2_transport_params params;
  ngtcp2_transport_params_default(&params);
  params.initial_max_stream_data_bidi_local = config.max_stream_data_bidi_local;
  params.initial_max_stream_data_bidi_remote =
      config.max_stream_data_bidi_remote;
  params.initial_max_stream_data_uni = config.max_stream_data_uni;
  params.initial_max_data = config.max_data;
  params.initial_max_streams_bidi = config.max_streams_bidi;
  params.initial_max_streams_uni = config.max_streams_uni;
  params.max_idle_timeout = config.timeout;
  params.active_connection_id_limit = 7;

  auto path = ngtcp2_path{
      {local_addr.len, const_cast<sockaddr *>(&local_addr.su.sa)},
      {remote_addr.len, const_cast<sockaddr *>(&remote_addr.su.sa)}};
  auto rv =
      ngtcp2_conn_client_new(&conn_, &dcid, &scid, &path, version, &callbacks,
                             &settings, &params, nullptr, this);

  if (rv != 0) {
    std::cerr << "ngtcp2_conn_client_new: " << ngtcp2_strerror(rv) << std::endl;
    return -1;
  }

  if (tls_session_.init(early_data_, tls_ctx, addr_, this, AppProtocol::H3) !=
      0) {
    return -1;
  }

  ngtcp2_conn_set_tls_native_handle(conn_, tls_session_.get_native_handle());

  if (early_data_ && config.tp_file) {
    ngtcp2_transport_params params;
    if (read_transport_params(config.tp_file, &params) != 0) {
      std::cerr << "Could not read transport parameters from " << config.tp_file
                << std::endl;
      early_data_ = false;
    } else {
      ngtcp2_conn_set_early_remote_transport_params(conn_, &params);
      if (make_stream_early() != 0) {
        return -1;
      }
    }
  }

  ev_io_set(&wev_, fd_, EV_WRITE);
  ev_io_set(&rev_, fd_, EV_READ);

  ev_io_start(loop_, &rev_);
  ev_timer_again(loop_, &timer_);

  ev_signal_start(loop_, &sigintev_);

  return 0;
}

int Client::feed_data(const sockaddr *sa, socklen_t salen,
                      const ngtcp2_pkt_info *pi, uint8_t *data,
                      size_t datalen) {
  auto path =
      ngtcp2_path{{local_addr_.len, const_cast<sockaddr *>(&local_addr_.su.sa)},
                  {salen, const_cast<sockaddr *>(sa)}};
  if (auto rv = ngtcp2_conn_read_pkt(conn_, &path, pi, data, datalen,
                                     util::timestamp(loop_));
      rv != 0) {
    std::cerr << "ngtcp2_conn_read_pkt: " << ngtcp2_strerror(rv) << std::endl;
    switch (rv) {
    case NGTCP2_ERR_REQUIRED_TRANSPORT_PARAM:
    case NGTCP2_ERR_MALFORMED_TRANSPORT_PARAM:
    case NGTCP2_ERR_TRANSPORT_PARAM:
    case NGTCP2_ERR_PROTO: // with failed TP validation, we get this.
      // If rv indicates transport_parameters related error, we should
      // send TRANSPORT_PARAMETER_ERROR even if last_error_.code is
      // already set.  This is because OpenSSL might set Alert.
      last_error_ = quic_err_transport(rv);
      break;
    default:
      if (!last_error_.code) {
        last_error_ = quic_err_transport(rv);
      }
    }
    disconnect();
    return -1;
  }
  return 0;
}

int Client::on_read() {
  std::array<uint8_t, 65536> buf;
  sockaddr_union su;
  size_t pktcnt = 0;
  ngtcp2_pkt_info pi;

  iovec msg_iov;
  msg_iov.iov_base = buf.data();
  msg_iov.iov_len = buf.size();

  msghdr msg{};
  msg.msg_name = &su;
  msg.msg_iov = &msg_iov;
  msg.msg_iovlen = 1;

  uint8_t msg_ctrl[CMSG_SPACE(sizeof(uint8_t))];
  msg.msg_control = msg_ctrl;

  for (;;) {
    msg.msg_namelen = sizeof(su);
    msg.msg_controllen = sizeof(msg_ctrl);

    auto nread = recvmsg(fd_, &msg, 0);

    if (nread == -1) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        std::cerr << "recvmsg: " << strerror(errno) << std::endl;
      }
      break;
    }

    pi.ecn = msghdr_get_ecn(&msg, su.storage.ss_family);

    if (!config.quiet) {
      std::cerr << "Received packet: local="
                << util::straddr(&local_addr_.su.sa, local_addr_.len)
                << " remote=" << util::straddr(&su.sa, msg.msg_namelen)
                << " ecn=0x" << std::hex << pi.ecn << std::dec << " " << nread
                << " bytes" << std::endl;
    }

    if (debug::packet_lost(config.rx_loss_prob)) {
      if (!config.quiet) {
        std::cerr << "** Simulated incoming packet loss **" << std::endl;
      }
      break;
    }

    if (feed_data(&su.sa, msg.msg_namelen, &pi, buf.data(), nread) != 0) {
      return -1;
    }

    if (++pktcnt >= 10) {
      break;
    }
  }

  if (should_exit_) {
    last_error_ = quic_err_app(0);
    disconnect();
    return -1;
  }

  reset_idle_timer();

  return 0;
}

void Client::reset_idle_timer() {
  auto now = util::timestamp(loop_);
  auto idle_expiry = ngtcp2_conn_get_idle_expiry(conn_);
  timer_.repeat =
      idle_expiry > now
          ? static_cast<ev_tstamp>(idle_expiry - now) / NGTCP2_SECONDS
          : 1e-9;

  if (!config.quiet) {
    std::cerr << "Set idle timer=" << std::fixed << timer_.repeat << "s"
              << std::defaultfloat << std::endl;
  }

  ev_timer_again(loop_, &timer_);
}

int Client::handle_expiry() {
  auto now = util::timestamp(loop_);
  if (auto rv = ngtcp2_conn_handle_expiry(conn_, now); rv != 0) {
    std::cerr << "ngtcp2_conn_handle_expiry: " << ngtcp2_strerror(rv)
              << std::endl;
    last_error_ = quic_err_transport(NGTCP2_ERR_INTERNAL);
    disconnect();
    return -1;
  }

  return 0;
}

int Client::on_write() {
  if (sendbuf_.size() > 0) {
    if (auto rv = send_packet(); rv != NETWORK_ERR_OK) {
      if (rv != NETWORK_ERR_SEND_BLOCKED) {
        last_error_ = quic_err_transport(NGTCP2_ERR_INTERNAL);
        disconnect();
      }
      return rv;
    }
  }

  assert(sendbuf_.left() >= max_pktlen_);

  if (auto rv = write_streams(); rv != 0) {
    if (rv == NETWORK_ERR_SEND_BLOCKED) {
      schedule_retransmit();
    }
    return rv;
  }

  if (should_exit_) {
    last_error_ = quic_err_app(0);
    disconnect();
    return -1;
  }

  schedule_retransmit();
  return 0;
}

int Client::write_streams() {
  std::array<nghttp3_vec, 16> vec;
  PathStorage path;
  size_t pktcnt = 0;

  for (;;) {
    int64_t stream_id = -1;
    int fin = 0;
    nghttp3_ssize sveccnt = 0;

    if (httpconn_ && ngtcp2_conn_get_max_data_left(conn_)) {
      sveccnt = nghttp3_conn_writev_stream(httpconn_, &stream_id, &fin,
                                           vec.data(), vec.size());
      if (sveccnt < 0) {
        std::cerr << "nghttp3_conn_writev_stream: " << nghttp3_strerror(sveccnt)
                  << std::endl;
        last_error_ = quic_err_app(sveccnt);
        disconnect();
        return -1;
      }
    }

    ngtcp2_ssize ndatalen;
    auto v = vec.data();
    auto vcnt = static_cast<size_t>(sveccnt);

    uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE;
    if (fin) {
      flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
    }

    ngtcp2_pkt_info pi;

    auto nwrite = ngtcp2_conn_writev_stream(
        conn_, &path.path, &pi, sendbuf_.wpos(), max_pktlen_, &ndatalen, flags,
        stream_id, reinterpret_cast<const ngtcp2_vec *>(v), vcnt,
        util::timestamp(loop_));
    if (nwrite < 0) {
      switch (nwrite) {
      case NGTCP2_ERR_STREAM_DATA_BLOCKED:
        assert(ndatalen == -1);
        if (auto rv = nghttp3_conn_block_stream(httpconn_, stream_id);
            rv != 0) {
          std::cerr << "nghttp3_conn_block_stream: " << nghttp3_strerror(rv)
                    << std::endl;
          last_error_ = quic_err_app(rv);
          disconnect();
          return -1;
        }
        continue;
      case NGTCP2_ERR_STREAM_SHUT_WR:
        assert(ndatalen == -1);
        if (auto rv = nghttp3_conn_shutdown_stream_write(httpconn_, stream_id);
            rv != 0) {
          std::cerr << "nghttp3_conn_shutdown_stream_write: "
                    << nghttp3_strerror(rv) << std::endl;
          last_error_ = quic_err_app(rv);
          disconnect();
          return -1;
        }
        continue;
      case NGTCP2_ERR_WRITE_MORE:
        assert(ndatalen >= 0);
        if (auto rv =
                nghttp3_conn_add_write_offset(httpconn_, stream_id, ndatalen);
            rv != 0) {
          std::cerr << "nghttp3_conn_add_write_offset: " << nghttp3_strerror(rv)
                    << std::endl;
          last_error_ = quic_err_app(rv);
          disconnect();
          return -1;
        }
        continue;
      }

      assert(ndatalen == -1);

      std::cerr << "ngtcp2_conn_write_stream: " << ngtcp2_strerror(nwrite)
                << std::endl;
      last_error_ = quic_err_transport(nwrite);
      disconnect();
      return -1;
    } else if (ndatalen >= 0) {
      if (auto rv =
              nghttp3_conn_add_write_offset(httpconn_, stream_id, ndatalen);
          rv != 0) {
        std::cerr << "nghttp3_conn_add_write_offset: " << nghttp3_strerror(rv)
                  << std::endl;
        last_error_ = quic_err_app(rv);
        disconnect();
        return -1;
      }
    }

    if (nwrite == 0) {
      // We are congestion limited.
      return 0;
    }

    sendbuf_.push(nwrite);

    update_remote_addr(&path.path.remote, &pi);
    reset_idle_timer();

    if (auto rv = send_packet(); rv != NETWORK_ERR_OK) {
      if (rv != NETWORK_ERR_SEND_BLOCKED) {
        last_error_ = quic_err_transport(NGTCP2_ERR_INTERNAL);
        disconnect();
      }
      return rv;
    }

    if (++pktcnt == 10) {
      ev_io_start(loop_, &wev_);
      return 0;
    }
  }
}

void Client::schedule_retransmit() {
  auto expiry = ngtcp2_conn_get_expiry(conn_);
  auto now = util::timestamp(loop_);
  auto t = expiry < now ? 1e-9
                        : static_cast<ev_tstamp>(expiry - now) / NGTCP2_SECONDS;
  rttimer_.repeat = t;
  ev_timer_again(loop_, &rttimer_);
}

int Client::recv_crypto_data(ngtcp2_crypto_level crypto_level,
                             const uint8_t *data, size_t datalen) {
  return ngtcp2_crypto_read_write_crypto_data(conn_, crypto_level, data,
                                              datalen);
}

namespace {
int bind_addr(Address &local_addr, int fd, int family) {
  addrinfo hints{};
  addrinfo *res, *rp;

  hints.ai_family = family;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_PASSIVE;

  if (auto rv = getaddrinfo(nullptr, "0", &hints, &res); rv != 0) {
    std::cerr << "getaddrinfo: " << gai_strerror(rv) << std::endl;
    return -1;
  }

  auto res_d = defer(freeaddrinfo, res);

  for (rp = res; rp; rp = rp->ai_next) {
    if (bind(fd, rp->ai_addr, rp->ai_addrlen) != -1) {
      break;
    }
  }

  if (!rp) {
    std::cerr << "Could not bind" << std::endl;
    return -1;
  }

  socklen_t len = sizeof(local_addr.su.storage);
  if (getsockname(fd, &local_addr.su.sa, &len) == -1) {
    std::cerr << "getsockname: " << strerror(errno) << std::endl;
    return -1;
  }
  local_addr.len = len;

  return 0;
}
} // namespace

namespace {
int create_sock(Address &remote_addr, const char *addr, const char *port) {
  addrinfo hints{};
  addrinfo *res, *rp;

  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;

  if (auto rv = getaddrinfo(addr, port, &hints, &res); rv != 0) {
    std::cerr << "getaddrinfo: " << gai_strerror(rv) << std::endl;
    return -1;
  }

  auto res_d = defer(freeaddrinfo, res);

  int fd = -1;

  for (rp = res; rp; rp = rp->ai_next) {
    fd = util::create_nonblock_socket(rp->ai_family, rp->ai_socktype,
                                      rp->ai_protocol);
    if (fd == -1) {
      continue;
    }

    break;
  }

  if (!rp) {
    std::cerr << "Could not connect" << std::endl;
    return -1;
  }

  auto val = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val,
                 static_cast<socklen_t>(sizeof(val))) == -1) {
    close(fd);
    return -1;
  }

  fd_set_recv_ecn(fd, rp->ai_family);

  remote_addr.len = rp->ai_addrlen;
  memcpy(&remote_addr.su, rp->ai_addr, rp->ai_addrlen);

  return fd;
}
} // namespace

void Client::start_change_local_addr_timer() {
  ev_timer_start(loop_, &change_local_addr_timer_);
}

int Client::change_local_addr() {
  Address remote_addr, local_addr;

  if (!config.quiet) {
    std::cerr << "Changing local address" << std::endl;
  }

  auto nfd = create_sock(remote_addr, addr_, port_);
  if (nfd == -1) {
    return -1;
  }

  if (bind_addr(local_addr, nfd, remote_addr.su.sa.sa_family) != 0) {
    ::close(nfd);
    return -1;
  }

  ::close(fd_);

  fd_ = nfd;
  local_addr_ = local_addr;
  remote_addr_ = remote_addr;

  if (config.nat_rebinding) {
    ngtcp2_addr addr;
    ngtcp2_conn_set_local_addr(
        conn_,
        ngtcp2_addr_init(&addr, &local_addr.su.sa, local_addr.len, nullptr));
  } else {
    auto path = ngtcp2_path{{local_addr.len, &local_addr.su.sa},
                            {remote_addr.len, &remote_addr.su.sa}};
    if (auto rv = ngtcp2_conn_initiate_migration(conn_, &path,
                                                 util::timestamp(loop_));
        rv != 0) {
      std::cerr << "ngtcp2_conn_initiate_migration: " << ngtcp2_strerror(rv)
                << std::endl;
    }
  }

  auto wev_active = ev_is_active(&wev_);

  ev_io_stop(loop_, &wev_);
  ev_io_stop(loop_, &rev_);
  ev_io_set(&wev_, fd_, EV_WRITE);
  ev_io_set(&rev_, fd_, EV_READ);
  if (wev_active) {
    ev_io_start(loop_, &wev_);
  }
  ev_io_start(loop_, &rev_);

  return 0;
}

void Client::start_key_update_timer() {
  ev_timer_start(loop_, &key_update_timer_);
}

int Client::update_key(uint8_t *rx_secret, uint8_t *tx_secret,
                       ngtcp2_crypto_aead_ctx *rx_aead_ctx, uint8_t *rx_iv,
                       ngtcp2_crypto_aead_ctx *tx_aead_ctx, uint8_t *tx_iv,
                       const uint8_t *current_rx_secret,
                       const uint8_t *current_tx_secret, size_t secretlen) {
  if (!config.quiet) {
    std::cerr << "Updating traffic key" << std::endl;
  }

  auto crypto_ctx = ngtcp2_conn_get_crypto_ctx(conn_);
  auto aead = &crypto_ctx->aead;
  auto keylen = ngtcp2_crypto_aead_keylen(aead);
  auto ivlen = ngtcp2_crypto_packet_protection_ivlen(aead);

  ++nkey_update_;

  std::array<uint8_t, 64> rx_key, tx_key;

  if (ngtcp2_crypto_update_key(conn_, rx_secret, tx_secret, rx_aead_ctx,
                               rx_key.data(), rx_iv, tx_aead_ctx, tx_key.data(),
                               tx_iv, current_rx_secret, current_tx_secret,
                               secretlen) != 0) {
    return -1;
  }

  if (!config.quiet && config.show_secret) {
    std::cerr << "application_traffic rx secret " << nkey_update_ << std::endl;
    debug::print_secrets(rx_secret, secretlen, rx_key.data(), keylen, rx_iv,
                         ivlen);
    std::cerr << "application_traffic tx secret " << nkey_update_ << std::endl;
    debug::print_secrets(tx_secret, secretlen, tx_key.data(), keylen, tx_iv,
                         ivlen);
  }

  return 0;
}

int Client::initiate_key_update() {
  if (!config.quiet) {
    std::cerr << "Initiate key update" << std::endl;
  }

  if (auto rv = ngtcp2_conn_initiate_key_update(conn_, util::timestamp(loop_));
      rv != 0) {
    std::cerr << "ngtcp2_conn_initiate_key_update: " << ngtcp2_strerror(rv)
              << std::endl;
    return -1;
  }

  return 0;
}

void Client::start_delay_stream_timer() {
  ev_timer_start(loop_, &delay_stream_timer_);
}

void Client::update_remote_addr(const ngtcp2_addr *addr,
                                const ngtcp2_pkt_info *pi) {
  remote_addr_.len = addr->addrlen;
  memcpy(&remote_addr_.su, addr->addr, addr->addrlen);
  if (pi) {
    ecn_ = pi->ecn;
  } else {
    ecn_ = 0;
  }
}

int Client::send_packet() {
  if (debug::packet_lost(config.tx_loss_prob)) {
    if (!config.quiet) {
      std::cerr << "** Simulated outgoing packet loss **" << std::endl;
    }
    sendbuf_.reset();
    return NETWORK_ERR_OK;
  }

  iovec msg_iov;
  msg_iov.iov_base = const_cast<uint8_t *>(sendbuf_.rpos());
  msg_iov.iov_len = sendbuf_.size();

  msghdr msg{};
  msg.msg_name = const_cast<sockaddr *>(&remote_addr_.su.sa);
  msg.msg_namelen = remote_addr_.len;
  msg.msg_iov = &msg_iov;
  msg.msg_iovlen = 1;

  fd_set_ecn(fd_, remote_addr_.su.storage.ss_family, ecn_);

  ssize_t nwrite = 0;

  do {
    nwrite = sendmsg(fd_, &msg, 0);
  } while (nwrite == -1 && errno == EINTR);

  if (nwrite == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return NETWORK_ERR_SEND_BLOCKED;
    }
    std::cerr << "sendmsg: " << strerror(errno) << std::endl;
    return NETWORK_ERR_FATAL;
  }

  assert(static_cast<size_t>(nwrite) == sendbuf_.size());
  sendbuf_.reset();

  if (!config.quiet) {
    std::cerr << "Sent packet: local="
              << util::straddr(&local_addr_.su.sa, local_addr_.len)
              << " remote="
              << util::straddr(&remote_addr_.su.sa, remote_addr_.len)
              << " ecn=0x" << std::hex << ecn_ << std::dec << " " << nwrite
              << " bytes" << std::endl;
  }

  return NETWORK_ERR_OK;
}

int Client::handle_error() {
  if (!conn_ || ngtcp2_conn_is_in_closing_period(conn_)) {
    return 0;
  }

  sendbuf_.reset();
  assert(sendbuf_.left() >= max_pktlen_);

  if (last_error_.type == QUICErrorType::TransportVersionNegotiation ||
      last_error_.type == QUICErrorType::TransportIdleTimeout) {
    return 0;
  }

  PathStorage path;
  ngtcp2_pkt_info pi;
  if (last_error_.type == QUICErrorType::Transport) {
    auto n = ngtcp2_conn_write_connection_close(
        conn_, &path.path, &pi, sendbuf_.wpos(), max_pktlen_, last_error_.code,
        util::timestamp(loop_));
    if (n < 0) {
      std::cerr << "ngtcp2_conn_write_connection_close: " << ngtcp2_strerror(n)
                << std::endl;
      return -1;
    }
    sendbuf_.push(n);
  } else {
    auto n = ngtcp2_conn_write_application_close(
        conn_, &path.path, &pi, sendbuf_.wpos(), max_pktlen_, last_error_.code,
        util::timestamp(loop_));
    if (n < 0) {
      std::cerr << "ngtcp2_conn_write_application_close: " << ngtcp2_strerror(n)
                << std::endl;
      return -1;
    }
    sendbuf_.push(n);
  }

  update_remote_addr(&path.path.remote, &pi);

  return send_packet();
}

int Client::on_stream_close(int64_t stream_id, uint64_t app_error_code) {
  if (httpconn_) {
    if (app_error_code == 0) {
      app_error_code = NGHTTP3_H3_NO_ERROR;
    }
    auto rv = nghttp3_conn_close_stream(httpconn_, stream_id, app_error_code);
    switch (rv) {
    case 0:
      break;
    case NGHTTP3_ERR_STREAM_NOT_FOUND:
      // We have to handle the case when stream opened but no data is
      // transferred.  In this case, nghttp3_conn_close_stream might
      // return error.
      if (!ngtcp2_is_bidi_stream(stream_id)) {
        assert(!ngtcp2_conn_is_local_stream(conn_, stream_id));
        ngtcp2_conn_extend_max_streams_uni(conn_, 1);
      }
      break;
    default:
      std::cerr << "nghttp3_conn_close_stream: " << nghttp3_strerror(rv)
                << std::endl;
      last_error_ = quic_err_app(rv);
      return -1;
    }
  }

  return 0;
}

int Client::on_stream_reset(int64_t stream_id) {
  if (httpconn_) {
    if (auto rv = nghttp3_conn_reset_stream(httpconn_, stream_id); rv != 0) {
      std::cerr << "nghttp3_conn_reset_stream: " << nghttp3_strerror(rv)
                << std::endl;
      return -1;
    }
  }
  return 0;
}

int Client::make_stream_early() {
  if (setup_httpconn() != 0) {
    return -1;
  }

  return on_extend_max_streams();
}

int Client::on_extend_max_streams() {
  int64_t stream_id;

  if ((config.delay_stream && !handshake_confirmed_) ||
      ev_is_active(&delay_stream_timer_)) {
    return 0;
  }

  for (; nstreams_done_ < config.nstreams; ++nstreams_done_) {
    if (auto rv = ngtcp2_conn_open_bidi_stream(conn_, &stream_id, nullptr);
        rv != 0) {
      assert(NGTCP2_ERR_STREAM_ID_BLOCKED == rv);
      break;
    }

    auto stream = std::make_unique<Stream>(
        config.requests[nstreams_done_ % config.requests.size()], stream_id);

    if (submit_http_request(stream.get()) != 0) {
      break;
    }

    if (!config.download.empty()) {
      stream->open_file(stream->req.path);
    }
    streams_.emplace(stream_id, std::move(stream));
  }
  return 0;
}

namespace {
nghttp3_ssize read_data(nghttp3_conn *conn, int64_t stream_id, nghttp3_vec *vec,
                        size_t veccnt, uint32_t *pflags, void *user_data,
                        void *stream_user_data) {
  vec[0].base = config.data;
  vec[0].len = config.datalen;
  *pflags |= NGHTTP3_DATA_FLAG_EOF;

  return 1;
}
} // namespace

int Client::submit_http_request(const Stream *stream) {
  std::string content_length_str;

  const auto &req = stream->req;

  std::array<nghttp3_nv, 6> nva{
      util::make_nv(":method", config.http_method),
      util::make_nv(":scheme", req.scheme),
      util::make_nv(":authority", req.authority),
      util::make_nv(":path", req.path),
      util::make_nv("user-agent", "nghttp3/ngtcp2 client"),
  };
  size_t nvlen = 5;
  if (config.fd != -1) {
    content_length_str = std::to_string(config.datalen);
    nva[nvlen++] = util::make_nv("content-length", content_length_str);
  }

  if (!config.quiet) {
    debug::print_http_request_headers(stream->stream_id, nva.data(), nvlen);
  }

  nghttp3_data_reader dr{};
  dr.read_data = read_data;

  if (auto rv = nghttp3_conn_submit_request(
          httpconn_, stream->stream_id, nva.data(), nvlen,
          config.fd == -1 ? nullptr : &dr, nullptr);
      rv != 0) {
    std::cerr << "nghttp3_conn_submit_request: " << nghttp3_strerror(rv)
              << std::endl;
    return -1;
  }

  return 0;
}

int Client::recv_stream_data(uint32_t flags, int64_t stream_id,
                             const uint8_t *data, size_t datalen) {
  auto nconsumed = nghttp3_conn_read_stream(
      httpconn_, stream_id, data, datalen, flags & NGTCP2_STREAM_DATA_FLAG_FIN);
  if (nconsumed < 0) {
    std::cerr << "nghttp3_conn_read_stream: " << nghttp3_strerror(nconsumed)
              << std::endl;
    last_error_ = quic_err_app(nconsumed);
    return -1;
  }

  ngtcp2_conn_extend_max_stream_offset(conn_, stream_id, nconsumed);
  ngtcp2_conn_extend_max_offset(conn_, nconsumed);

  return 0;
}

int Client::acked_stream_data_offset(int64_t stream_id, uint64_t datalen) {
  if (auto rv = nghttp3_conn_add_ack_offset(httpconn_, stream_id, datalen);
      rv != 0) {
    std::cerr << "nghttp3_conn_add_ack_offset: " << nghttp3_strerror(rv)
              << std::endl;
    return -1;
  }

  return 0;
}

int Client::select_preferred_address(Address &selected_addr,
                                     const ngtcp2_preferred_addr *paddr) {
  int af;
  const uint8_t *binaddr;
  uint16_t port;
  constexpr uint8_t empty_addr[] = {0, 0, 0, 0, 0, 0, 0, 0,
                                    0, 0, 0, 0, 0, 0, 0, 0};
  if (local_addr_.su.sa.sa_family == AF_INET &&
      memcmp(empty_addr, paddr->ipv4_addr, sizeof(paddr->ipv4_addr)) != 0) {
    af = AF_INET;
    binaddr = paddr->ipv4_addr;
    port = paddr->ipv4_port;
  } else if (local_addr_.su.sa.sa_family == AF_INET6 &&
             memcmp(empty_addr, paddr->ipv6_addr, sizeof(paddr->ipv6_addr)) !=
                 0) {
    af = AF_INET6;
    binaddr = paddr->ipv6_addr;
    port = paddr->ipv6_port;
  } else {
    return -1;
  }

  char host[NI_MAXHOST];
  if (inet_ntop(af, binaddr, host, sizeof(host)) == nullptr) {
    std::cerr << "inet_ntop: " << strerror(errno) << std::endl;
    return -1;
  }

  if (!config.quiet) {
    std::cerr << "selected server preferred_address is [" << host
              << "]:" << port << std::endl;
  }

  addrinfo hints{};
  addrinfo *res;

  hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
  hints.ai_family = af;
  hints.ai_socktype = SOCK_DGRAM;

  if (auto rv = getaddrinfo(host, std::to_string(port).c_str(), &hints, &res);
      rv != 0) {
    std::cerr << "getaddrinfo: " << gai_strerror(rv) << std::endl;
    return -1;
  }

  assert(res);

  selected_addr.len = res->ai_addrlen;
  memcpy(&selected_addr.su, res->ai_addr, res->ai_addrlen);

  freeaddrinfo(res);

  return 0;
}

void Client::start_wev() { ev_io_start(loop_, &wev_); }

namespace {
int http_acked_stream_data(nghttp3_conn *conn, int64_t stream_id,
                           size_t datalen, void *user_data,
                           void *stream_user_data) {
  auto c = static_cast<Client *>(user_data);
  if (c->http_acked_stream_data(stream_id, datalen) != 0) {
    return NGHTTP3_ERR_CALLBACK_FAILURE;
  }
  return 0;
}
} // namespace

int Client::http_acked_stream_data(int64_t stream_id, size_t datalen) {
  return 0;
}

namespace {
int http_recv_data(nghttp3_conn *conn, int64_t stream_id, const uint8_t *data,
                   size_t datalen, void *user_data, void *stream_user_data) {
  if (!config.quiet && !config.no_http_dump) {
    debug::print_http_data(stream_id, data, datalen);
  }
  auto c = static_cast<Client *>(user_data);
  c->http_consume(stream_id, datalen);
  c->http_write_data(stream_id, data, datalen);
  return 0;
}
} // namespace

namespace {
int http_deferred_consume(nghttp3_conn *conn, int64_t stream_id,
                          size_t nconsumed, void *user_data,
                          void *stream_user_data) {
  auto c = static_cast<Client *>(user_data);
  c->http_consume(stream_id, nconsumed);
  return 0;
}
} // namespace

void Client::http_consume(int64_t stream_id, size_t nconsumed) {
  ngtcp2_conn_extend_max_stream_offset(conn_, stream_id, nconsumed);
  ngtcp2_conn_extend_max_offset(conn_, nconsumed);
}

void Client::http_write_data(int64_t stream_id, const uint8_t *data,
                             size_t datalen) {
  auto it = streams_.find(stream_id);
  if (it == std::end(streams_)) {
    return;
  }

  auto &stream = (*it).second;

  if (stream->fd == -1) {
    return;
  }

  ssize_t nwrite;
  do {
    nwrite = write(stream->fd, data, datalen);
  } while (nwrite == -1 && errno == EINTR);
}

namespace {
int http_begin_headers(nghttp3_conn *conn, int64_t stream_id, void *user_data,
                       void *stream_user_data) {
  if (!config.quiet) {
    debug::print_http_begin_response_headers(stream_id);
  }
  return 0;
}
} // namespace

namespace {
int http_recv_header(nghttp3_conn *conn, int64_t stream_id, int32_t token,
                     nghttp3_rcbuf *name, nghttp3_rcbuf *value, uint8_t flags,
                     void *user_data, void *stream_user_data) {
  if (!config.quiet) {
    debug::print_http_header(stream_id, name, value, flags);
  }
  return 0;
}
} // namespace

namespace {
int http_end_headers(nghttp3_conn *conn, int64_t stream_id, void *user_data,
                     void *stream_user_data) {
  if (!config.quiet) {
    debug::print_http_end_headers(stream_id);
  }
  return 0;
}
} // namespace

namespace {
int http_begin_trailers(nghttp3_conn *conn, int64_t stream_id, void *user_data,
                        void *stream_user_data) {
  if (!config.quiet) {
    debug::print_http_begin_trailers(stream_id);
  }
  return 0;
}
} // namespace

namespace {
int http_recv_trailer(nghttp3_conn *conn, int64_t stream_id, int32_t token,
                      nghttp3_rcbuf *name, nghttp3_rcbuf *value, uint8_t flags,
                      void *user_data, void *stream_user_data) {
  if (!config.quiet) {
    debug::print_http_header(stream_id, name, value, flags);
  }
  return 0;
}
} // namespace

namespace {
int http_end_trailers(nghttp3_conn *conn, int64_t stream_id, void *user_data,
                      void *stream_user_data) {
  if (!config.quiet) {
    debug::print_http_end_trailers(stream_id);
  }
  return 0;
}
} // namespace

namespace {
int http_begin_push_promise(nghttp3_conn *conn, int64_t stream_id,
                            int64_t push_id, void *user_data,
                            void *stream_user_data) {
  if (!config.quiet) {
    debug::print_http_begin_push_promise(stream_id, push_id);
  }
  return 0;
}
} // namespace

namespace {
int http_recv_push_promise(nghttp3_conn *conn, int64_t stream_id,
                           int64_t push_id, int32_t token, nghttp3_rcbuf *name,
                           nghttp3_rcbuf *value, uint8_t flags, void *user_data,
                           void *stream_user_data) {
  if (!config.quiet) {
    debug::print_http_push_promise(stream_id, push_id, name, value, flags);
  }
  return 0;
}
} // namespace

namespace {
int http_end_push_promise(nghttp3_conn *conn, int64_t stream_id,
                          int64_t push_id, void *user_data,
                          void *stream_user_data) {
  if (!config.quiet) {
    debug::print_http_end_push_promise(stream_id, push_id);
  }
  return 0;
}
} // namespace

namespace {
int http_send_stop_sending(nghttp3_conn *conn, int64_t stream_id,
                           uint64_t app_error_code, void *user_data,
                           void *stream_user_data) {
  auto c = static_cast<Client *>(user_data);
  if (c->send_stop_sending(stream_id, app_error_code) != 0) {
    return NGHTTP3_ERR_CALLBACK_FAILURE;
  }
  return 0;
}
} // namespace

int Client::send_stop_sending(int64_t stream_id, uint64_t app_error_code) {
  if (auto rv =
          ngtcp2_conn_shutdown_stream_read(conn_, stream_id, app_error_code);
      rv != 0) {
    std::cerr << "ngtcp2_conn_shutdown_stream_read: " << ngtcp2_strerror(rv)
              << std::endl;
    if (rv == NGTCP2_ERR_STREAM_NOT_FOUND) {
      return 0;
    }
    return -1;
  }
  return 0;
}

namespace {
int http_cancel_push(nghttp3_conn *conn, int64_t push_id, int64_t stream_id,
                     void *user_data, void *stream_user_data) {
  if (!config.quiet) {
    debug::cancel_push(push_id, stream_id);
  }
  return 0;
}
} // namespace

namespace {
int http_push_stream(nghttp3_conn *conn, int64_t push_id, int64_t stream_id,
                     void *user_data) {
  if (!config.quiet) {
    debug::push_stream(push_id, stream_id);
  }
  return 0;
}
} // namespace

namespace {
int http_stream_close(nghttp3_conn *conn, int64_t stream_id,
                      uint64_t app_error_code, void *conn_user_data,
                      void *stream_user_data) {
  auto c = static_cast<Client *>(conn_user_data);
  if (c->http_stream_close(stream_id, app_error_code) != 0) {
    return NGHTTP3_ERR_CALLBACK_FAILURE;
  }
  return 0;
}
} // namespace

int Client::http_stream_close(int64_t stream_id, uint64_t app_error_code) {
  if (ngtcp2_is_bidi_stream(stream_id)) {
    assert(ngtcp2_conn_is_local_stream(conn_, stream_id));

    ++nstreams_closed_;

    if (config.exit_on_first_stream_close ||
        (config.exit_on_all_streams_close &&
         config.nstreams == nstreams_done_ &&
         nstreams_closed_ == nstreams_done_)) {
      should_exit_ = true;
    }
  } else {
    assert(!ngtcp2_conn_is_local_stream(conn_, stream_id));
    ngtcp2_conn_extend_max_streams_uni(conn_, 1);
  }

  if (auto it = streams_.find(stream_id); it != std::end(streams_)) {
    if (!config.quiet) {
      std::cerr << "HTTP stream " << stream_id << " closed with error code "
                << app_error_code << std::endl;
    }
    streams_.erase(it);
  }

  return 0;
}

int Client::setup_httpconn() {
  if (httpconn_) {
    return 0;
  }

  if (ngtcp2_conn_get_max_local_streams_uni(conn_) < 3) {
    std::cerr << "peer does not allow at least 3 unidirectional streams."
              << std::endl;
    return -1;
  }

  nghttp3_callbacks callbacks{
      ::http_acked_stream_data,  ::http_stream_close,
      ::http_recv_data,          ::http_deferred_consume,
      ::http_begin_headers,      ::http_recv_header,
      ::http_end_headers,        ::http_begin_trailers,
      ::http_recv_trailer,       ::http_end_trailers,
      ::http_begin_push_promise, ::http_recv_push_promise,
      ::http_end_push_promise,   ::http_cancel_push,
      ::http_send_stop_sending,  ::http_push_stream,
  };
  nghttp3_settings settings;
  nghttp3_settings_default(&settings);
  settings.qpack_max_table_capacity = 4096;
  settings.qpack_blocked_streams = 100;
  settings.max_pushes = 100;

  auto mem = nghttp3_mem_default();

  if (auto rv =
          nghttp3_conn_client_new(&httpconn_, &callbacks, &settings, mem, this);
      rv != 0) {
    std::cerr << "nghttp3_conn_client_new: " << nghttp3_strerror(rv)
              << std::endl;
    return -1;
  }

  int64_t ctrl_stream_id;

  if (auto rv = ngtcp2_conn_open_uni_stream(conn_, &ctrl_stream_id, nullptr);
      rv != 0) {
    std::cerr << "ngtcp2_conn_open_uni_stream: " << ngtcp2_strerror(rv)
              << std::endl;
    return -1;
  }

  if (auto rv = nghttp3_conn_bind_control_stream(httpconn_, ctrl_stream_id);
      rv != 0) {
    std::cerr << "nghttp3_conn_bind_control_stream: " << nghttp3_strerror(rv)
              << std::endl;
    return -1;
  }

  if (!config.quiet) {
    fprintf(stderr, "http: control stream=%" PRIx64 "\n", ctrl_stream_id);
  }

  int64_t qpack_enc_stream_id, qpack_dec_stream_id;

  if (auto rv =
          ngtcp2_conn_open_uni_stream(conn_, &qpack_enc_stream_id, nullptr);
      rv != 0) {
    std::cerr << "ngtcp2_conn_open_uni_stream: " << ngtcp2_strerror(rv)
              << std::endl;
    return -1;
  }

  if (auto rv =
          ngtcp2_conn_open_uni_stream(conn_, &qpack_dec_stream_id, nullptr);
      rv != 0) {
    std::cerr << "ngtcp2_conn_open_uni_stream: " << ngtcp2_strerror(rv)
              << std::endl;
    return -1;
  }

  if (auto rv = nghttp3_conn_bind_qpack_streams(httpconn_, qpack_enc_stream_id,
                                                qpack_dec_stream_id);
      rv != 0) {
    std::cerr << "nghttp3_conn_bind_qpack_streams: " << nghttp3_strerror(rv)
              << std::endl;
    return -1;
  }

  if (!config.quiet) {
    fprintf(stderr,
            "http: QPACK streams encoder=%" PRIx64 " decoder=%" PRIx64 "\n",
            qpack_enc_stream_id, qpack_dec_stream_id);
  }

  return 0;
}

namespace {
int run(Client &c, const char *addr, const char *port,
        const TLSClientContext &tls_ctx) {
  Address remote_addr, local_addr;

  auto fd = create_sock(remote_addr, addr, port);
  if (fd == -1) {
    return -1;
  }

  if (bind_addr(local_addr, fd, remote_addr.su.sa.sa_family) != 0) {
    close(fd);
    return -1;
  }

  if (c.init(fd, local_addr, remote_addr, addr, port, config.version,
             tls_ctx) != 0) {
    return -1;
  }

  // TODO Do we need this ?
  if (auto rv = c.on_write(); rv != 0) {
    return rv;
  }

  ev_run(EV_DEFAULT, 0);

  return 0;
}
} // namespace

namespace {
std::string_view get_string(const char *uri, const http_parser_url &u,
                            http_parser_url_fields f) {
  auto p = &u.field_data[f];
  return {uri + p->off, p->len};
}
} // namespace

namespace {
int parse_uri(Request &req, const char *uri) {
  http_parser_url u;

  http_parser_url_init(&u);
  if (http_parser_parse_url(uri, strlen(uri), /* is_connect = */ 0, &u) != 0) {
    return -1;
  }

  if (!(u.field_set & (1 << UF_SCHEMA)) || !(u.field_set & (1 << UF_HOST))) {
    return -1;
  }

  req.scheme = get_string(uri, u, UF_SCHEMA);

  req.authority = get_string(uri, u, UF_HOST);
  if (util::numeric_host(req.authority.c_str())) {
    req.authority = '[' + req.authority + ']';
  }
  if (u.field_set & (1 << UF_PORT)) {
    req.authority += ':';
    req.authority += get_string(uri, u, UF_PORT);
  }

  if (u.field_set & (1 << UF_PATH)) {
    req.path = get_string(uri, u, UF_PATH);
  } else {
    req.path = "/";
  }

  if (u.field_set & (1 << UF_QUERY)) {
    req.path += '?';
    req.path += get_string(uri, u, UF_QUERY);
  }

  return 0;
}
} // namespace

namespace {
int parse_requests(char **argv, size_t argvlen) {
  for (size_t i = 0; i < argvlen; ++i) {
    auto uri = argv[i];
    Request req;
    if (parse_uri(req, uri) != 0) {
      std::cerr << "Could not parse URI: " << uri << std::endl;
      return -1;
    }
    config.requests.emplace_back(std::move(req));
  }
  return 0;
}
} // namespace

std::ofstream keylog_file;

namespace {
void print_usage() {
  std::cerr << "Usage: client [OPTIONS] <HOST> <PORT> [<URI>...]" << std::endl;
}
} // namespace

namespace {
void config_set_default(Config &config) {
  config = Config{};
  config.tx_loss_prob = 0.;
  config.rx_loss_prob = 0.;
  config.fd = -1;
  config.ciphers = util::crypto_default_ciphers();
  config.groups = util::crypto_default_groups();
  config.nstreams = 0;
  config.data = nullptr;
  config.datalen = 0;
  config.version = NGTCP2_PROTO_VER_MIN;
  config.timeout = 30 * NGTCP2_SECONDS;
  config.http_method = "GET"sv;
  config.max_data = 1_m;
  config.max_stream_data_bidi_local = 256_k;
  config.max_stream_data_bidi_remote = 256_k;
  config.max_stream_data_uni = 256_k;
  config.max_window = 6_m;
  config.max_stream_window = 6_m;
  config.max_streams_uni = 100;
  config.cc = "cubic"sv;
  config.initial_rtt = NGTCP2_DEFAULT_INITIAL_RTT;
}
} // namespace

namespace {
void print_help() {
  print_usage();

  config_set_default(config);

  std::cout << R"(
  <HOST>      Remote server host (DNS name or IP address).  In case of
              DNS name, it will be sent in TLS SNI extension.
  <PORT>      Remote server port
  <URI>       Remote URI
Options:
  -t, --tx-loss=<P>
              The probability of losing outgoing packets.  <P> must be
              [0.0, 1.0],  inclusive.  0.0 means no  packet loss.  1.0
              means 100% packet loss.
  -r, --rx-loss=<P>
              The probability of losing incoming packets.  <P> must be
              [0.0, 1.0],  inclusive.  0.0 means no  packet loss.  1.0
              means 100% packet loss.
  -d, --data=<PATH>
              Read data from <PATH>, and send them as STREAM data.
  -n, --nstreams=<N>
              The number of requests.  <URI>s are used in the order of
              appearance in the command-line.   If the number of <URI>
              list  is  less than  <N>,  <URI>  list is  wrapped.   It
              defaults to 0 which means the number of <URI> specified.
  -v, --version=<HEX>
              Specify QUIC version to use in hex string.
              Default: )"
            << std::hex << "0x" << config.version << std::dec << R"(
  -q, --quiet Suppress debug output.
  -s, --show-secret
              Print out secrets unless --quiet is used.
  --timeout=<DURATION>
              Specify idle timeout.
              Default: )"
            << util::format_duration(config.timeout) << R"(
  --ciphers=<CIPHERS>
              Specify the cipher suite list to enable.
              Default: )"
            << config.ciphers << R"(
  --groups=<GROUPS>
              Specify the supported groups.
              Default: )"
            << config.groups << R"(
  --session-file=<PATH>
              Read/write  TLS session  from/to  <PATH>.   To resume  a
              session, the previous session must be supplied with this
              option.
  --tp-file=<PATH>
              Read/write QUIC transport parameters from/to <PATH>.  To
              send 0-RTT data, the  transport parameters received from
              the previous session must be supplied with this option.
  --dcid=<DCID>
              Specify  initial  DCID.   <DCID> is  hex  string.   When
              decoded as binary, it should be  at least 8 bytes and at
              most 18 bytes long.
  --scid=<SCID>
              Specify source connection ID.  <SCID> is hex string.  If
              an empty string  is given, zero length  connection ID is
              assumed.
  --change-local-addr=<DURATION>
              Client  changes  local  address when  <DURATION>  elapse
              after handshake completes.
  --nat-rebinding
              When   used  with   --change-local-addr,  simulate   NAT
              rebinding.   In   other  words,  client   changes  local
              address, but it does not start path validation.
  --key-update=<DURATION>
              Client initiates key update when <DURATION> elapse after
              handshake completes.
  -m, --http-method=<METHOD>
              Specify HTTP method.  Default: )"
            << config.http_method << R"(
  --delay-stream=<DURATION>
              Delay sending STREAM data  in 1-RTT for <DURATION> after
              handshake completes.
  --no-preferred-addr
              Do not try to use preferred address offered by server.
  --key=<PATH>
              The path to client private key PEM file.
  --cert=<PATH>
              The path to client certificate PEM file.
  --download=<PATH>
              The path to the directory  to save a downloaded content.
              It is  undefined if 2  concurrent requests write  to the
              same file.   If a request  path does not contain  a path
              component  usable  as  a   file  name,  it  defaults  to
              "index.html".
  --no-quic-dump
              Disables printing QUIC STREAM and CRYPTO frame data out.
  --no-http-dump
              Disables printing HTTP response body out.
  --qlog-file=<PATH>
              The path to write qlog.   This option and --qlog-dir are
              mutually exclusive.
  --qlog-dir=<PATH>
              Path to  the directory where  qlog file is  stored.  The
              file name  of each qlog  is the Source Connection  ID of
              client.   This  option   and  --qlog-file  are  mutually
              exclusive.
  --max-data=<SIZE>
              The initial connection-level flow control window.
              Default: )"
            << util::format_uint_iec(config.max_data) << R"(
  --max-stream-data-bidi-local=<SIZE>
              The  initial  stream-level  flow control  window  for  a
              bidirectional stream that the local endpoint initiates.
              Default: )"
            << util::format_uint_iec(config.max_stream_data_bidi_local) << R"(
  --max-stream-data-bidi-remote=<SIZE>
              The  initial  stream-level  flow control  window  for  a
              bidirectional stream that the remote endpoint initiates.
              Default: )"
            << util::format_uint_iec(config.max_stream_data_bidi_remote) << R"(
  --max-stream-data-uni=<SIZE>
              The  initial  stream-level  flow control  window  for  a
              unidirectional stream.
              Default: )"
            << util::format_uint_iec(config.max_stream_data_uni) << R"(
  --max-streams-bidi=<N>
              The number of the concurrent bidirectional streams.
              Default: )"
            << config.max_streams_bidi << R"(
  --max-streams-uni=<N>
              The number of the concurrent unidirectional streams.
              Default: )"
            << config.max_streams_uni << R"(
  --exit-on-first-stream-close
              Exit  when  a first  client  initialted  HTTP stream  is
              closed.
  --exit-on-all-streams-close
              Exit when all client initiated HTTP streams are closed.
  --disable-early-data
              Disable early data.
  --cc=(<cubic>|<reno>)
              The name of congestion controller algorithm.
  --token-file=<PATH>
              Read/write token from/to <PATH>.  Token is obtained from
              NEW_TOKEN frame from server.
  --sni=<DNSNAME>
              Send  <DNSNAME>  in TLS  SNI,  overriding  the DNS  name
              specified in <HOST>.
  --initial-rtt=<DURATION>
              Set an initial RTT.
              Default: )"
            << util::format_duration(config.initial_rtt) << R"(
  --max-window=<SIZE>
              Maximum connection-level flow  control window size.  The
              window auto-tuning is enabled if nonzero value is given,
              and window size is scaled up to this value.
              Default: )"
            << util::format_uint_iec(config.max_window) << R"(
  --max-stream-window=<SIZE>
              Maximum stream-level flow control window size.  The
              window auto-tuning is enabled if nonzero value is given,
              and window size is scaled up to this value.
              Default: )"
            << util::format_uint_iec(config.max_stream_window) << R"(
  -h, --help  Display this help and exit.

---

  The <SIZE> argument is an integer and an optional unit (e.g., 10K is
  10 * 1024).  Units are K, M and G (powers of 1024).

  The <DURATION> argument is an integer and an optional unit (e.g., 1s
  is 1 second and 500ms is 500  milliseconds).  Units are h, m, s, ms,
  us, or ns (hours,  minutes, seconds, milliseconds, microseconds, and
  nanoseconds respectively).  If  a unit is omitted, a  second is used
  as unit.)" << std::endl;
}
} // namespace

int main(int argc, char **argv) {
  config_set_default(config);
  char *data_path = nullptr;
  const char *private_key_file = nullptr;
  const char *cert_file = nullptr;

  for (;;) {
    static int flag = 0;
    constexpr static option long_opts[] = {
        {"help", no_argument, nullptr, 'h'},
        {"tx-loss", required_argument, nullptr, 't'},
        {"rx-loss", required_argument, nullptr, 'r'},
        {"data", required_argument, nullptr, 'd'},
        {"http-method", required_argument, nullptr, 'm'},
        {"nstreams", required_argument, nullptr, 'n'},
        {"version", required_argument, nullptr, 'v'},
        {"quiet", no_argument, nullptr, 'q'},
        {"show-secret", no_argument, nullptr, 's'},
        {"ciphers", required_argument, &flag, 1},
        {"groups", required_argument, &flag, 2},
        {"timeout", required_argument, &flag, 3},
        {"session-file", required_argument, &flag, 4},
        {"tp-file", required_argument, &flag, 5},
        {"dcid", required_argument, &flag, 6},
        {"change-local-addr", required_argument, &flag, 7},
        {"key-update", required_argument, &flag, 8},
        {"nat-rebinding", no_argument, &flag, 9},
        {"delay-stream", required_argument, &flag, 10},
        {"no-preferred-addr", no_argument, &flag, 11},
        {"key", required_argument, &flag, 12},
        {"cert", required_argument, &flag, 13},
        {"download", required_argument, &flag, 14},
        {"no-quic-dump", no_argument, &flag, 15},
        {"no-http-dump", no_argument, &flag, 16},
        {"qlog-file", required_argument, &flag, 17},
        {"max-data", required_argument, &flag, 18},
        {"max-stream-data-bidi-local", required_argument, &flag, 19},
        {"max-stream-data-bidi-remote", required_argument, &flag, 20},
        {"max-stream-data-uni", required_argument, &flag, 21},
        {"max-streams-bidi", required_argument, &flag, 22},
        {"max-streams-uni", required_argument, &flag, 23},
        {"exit-on-first-stream-close", no_argument, &flag, 24},
        {"disable-early-data", no_argument, &flag, 25},
        {"qlog-dir", required_argument, &flag, 26},
        {"cc", required_argument, &flag, 27},
        {"exit-on-all-streams-close", no_argument, &flag, 28},
        {"token-file", required_argument, &flag, 29},
        {"sni", required_argument, &flag, 30},
        {"initial-rtt", required_argument, &flag, 31},
        {"max-window", required_argument, &flag, 32},
        {"max-stream-window", required_argument, &flag, 33},
        {"scid", required_argument, &flag, 34},
        {nullptr, 0, nullptr, 0},
    };

    auto optidx = 0;
    auto c = getopt_long(argc, argv, "d:him:n:qr:st:v:", long_opts, &optidx);
    if (c == -1) {
      break;
    }
    switch (c) {
    case 'd':
      // --data
      data_path = optarg;
      break;
    case 'h':
      // --help
      print_help();
      exit(EXIT_SUCCESS);
    case 'm':
      // --http-method
      config.http_method = optarg;
      break;
    case 'n':
      // --streams
      config.nstreams = strtol(optarg, nullptr, 10);
      break;
    case 'q':
      // --quiet
      config.quiet = true;
      break;
    case 'r':
      // --rx-loss
      config.rx_loss_prob = strtod(optarg, nullptr);
      break;
    case 's':
      // --show-secret
      config.show_secret = true;
      break;
    case 't':
      // --tx-loss
      config.tx_loss_prob = strtod(optarg, nullptr);
      break;
    case 'v':
      // --version
      config.version = strtol(optarg, nullptr, 16);
      break;
    case '?':
      print_usage();
      exit(EXIT_FAILURE);
    case 0:
      switch (flag) {
      case 1:
        // --ciphers
        config.ciphers = optarg;
        break;
      case 2:
        // --groups
        config.groups = optarg;
        break;
      case 3:
        // --timeout
        if (auto [t, rv] = util::parse_duration(optarg); rv != 0) {
          std::cerr << "timeout: invalid argument" << std::endl;
          exit(EXIT_FAILURE);
        } else {
          config.timeout = t;
        }
        break;
      case 4:
        // --session-file
        config.session_file = optarg;
        break;
      case 5:
        // --tp-file
        config.tp_file = optarg;
        break;
      case 6: {
        // --dcid
        auto dcidlen2 = strlen(optarg);
        if (dcidlen2 % 2 || dcidlen2 / 2 < 8 || dcidlen2 / 2 > 18) {
          std::cerr << "dcid: wrong length" << std::endl;
          exit(EXIT_FAILURE);
        }
        auto dcid = util::decode_hex(optarg);
        ngtcp2_cid_init(&config.dcid,
                        reinterpret_cast<const uint8_t *>(dcid.c_str()),
                        dcid.size());
        break;
      }
      case 7:
        // --change-local-addr
        if (auto [t, rv] = util::parse_duration(optarg); rv != 0) {
          std::cerr << "change-local-addr: invalid argument" << std::endl;
          exit(EXIT_FAILURE);
        } else {
          config.change_local_addr = t;
        }
        break;
      case 8:
        // --key-update
        if (auto [t, rv] = util::parse_duration(optarg); rv != 0) {
          std::cerr << "key-update: invalid argument" << std::endl;
          exit(EXIT_FAILURE);
        } else {
          config.key_update = t;
        }
        break;
      case 9:
        // --nat-rebinding
        config.nat_rebinding = true;
        break;
      case 10:
        // --delay-stream
        if (auto [t, rv] = util::parse_duration(optarg); rv != 0) {
          std::cerr << "delay-stream: invalid argument" << std::endl;
          exit(EXIT_FAILURE);
        } else {
          config.delay_stream = t;
        }
        break;
      case 11:
        // --no-preferred-addr
        config.no_preferred_addr = true;
        break;
      case 12:
        // --key
        private_key_file = optarg;
        break;
      case 13:
        // --cert
        cert_file = optarg;
        break;
      case 14:
        // --download
        config.download = optarg;
        break;
      case 15:
        // --no-quic-dump
        config.no_quic_dump = true;
        break;
      case 16:
        // --no-http-dump
        config.no_http_dump = true;
        break;
      case 17:
        // --qlog-file
        config.qlog_file = optarg;
        break;
      case 18:
        // --max-data
        if (auto [n, rv] = util::parse_uint_iec(optarg); rv != 0) {
          std::cerr << "max-data: invalid argument" << std::endl;
          exit(EXIT_FAILURE);
        } else {
          config.max_data = n;
        }
        break;
      case 19:
        // --max-stream-data-bidi-local
        if (auto [n, rv] = util::parse_uint_iec(optarg); rv != 0) {
          std::cerr << "max-stream-data-bidi-local: invalid argument"
                    << std::endl;
          exit(EXIT_FAILURE);
        } else {
          config.max_stream_data_bidi_local = n;
        }
        break;
      case 20:
        // --max-stream-data-bidi-remote
        if (auto [n, rv] = util::parse_uint_iec(optarg); rv != 0) {
          std::cerr << "max-stream-data-bidi-remote: invalid argument"
                    << std::endl;
          exit(EXIT_FAILURE);
        } else {
          config.max_stream_data_bidi_remote = n;
        }
        break;
      case 21:
        // --max-stream-data-uni
        if (auto [n, rv] = util::parse_uint_iec(optarg); rv != 0) {
          std::cerr << "max-stream-data-uni: invalid argument" << std::endl;
          exit(EXIT_FAILURE);
        } else {
          config.max_stream_data_uni = n;
        }
        break;
      case 22:
        // --max-streams-bidi
        config.max_streams_bidi = strtoull(optarg, nullptr, 10);
        break;
      case 23:
        // --max-streams-uni
        config.max_streams_uni = strtoull(optarg, nullptr, 10);
        break;
      case 24:
        // --exit-on-first-stream-close
        config.exit_on_first_stream_close = true;
        break;
      case 25:
        // --disable-early-data
        config.disable_early_data = true;
        break;
      case 26:
        // --qlog-dir
        config.qlog_dir = optarg;
        break;
      case 27:
        // --cc
        if (strcmp("cubic", optarg) == 0 || strcmp("reno", optarg) == 0) {
          config.cc = optarg;
          break;
        }
        std::cerr << "cc: specify cubic or reno" << std::endl;
        exit(EXIT_FAILURE);
      case 28:
        // --exit-on-all-streams-close
        config.exit_on_all_streams_close = true;
        break;
      case 29:
        // --token-file
        config.token_file = optarg;
        break;
      case 30:
        // --sni
        config.sni = optarg;
        break;
      case 31:
        // --initial-rtt
        if (auto [t, rv] = util::parse_duration(optarg); rv != 0) {
          std::cerr << "initial-rtt: invalid argument" << std::endl;
          exit(EXIT_FAILURE);
        } else {
          config.initial_rtt = t;
        }
        break;
      case 32:
        // --max-window
        if (auto [n, rv] = util::parse_uint_iec(optarg); rv != 0) {
          std::cerr << "max-window: invalid argument" << std::endl;
          exit(EXIT_FAILURE);
        } else {
          config.max_window = n;
        }
        break;
      case 33:
        // --max-stream-window
        if (auto [n, rv] = util::parse_uint_iec(optarg); rv != 0) {
          std::cerr << "max-stream-window: invalid argument" << std::endl;
          exit(EXIT_FAILURE);
        } else {
          config.max_stream_window = n;
        }
        break;
      case 34: {
        // --scid
        auto scid = util::decode_hex(optarg);
        ngtcp2_cid_init(&config.scid,
                        reinterpret_cast<const uint8_t *>(scid.c_str()),
                        scid.size());
        config.scid_present = true;
        break;
      }
      }
      break;
    default:
      break;
    };
  }

  if (argc - optind < 2) {
    std::cerr << "Too few arguments" << std::endl;
    print_usage();
    exit(EXIT_FAILURE);
  }

  if (!config.qlog_file.empty() && !config.qlog_dir.empty()) {
    std::cerr << "qlog-file and qlog-dir are mutually exclusive" << std::endl;
    exit(EXIT_FAILURE);
  }

  if (config.exit_on_first_stream_close && config.exit_on_all_streams_close) {
    std::cerr << "exit-on-first-stream-close and exit-on-all-streams-close are "
                 "mutually exclusive"
              << std::endl;
    exit(EXIT_FAILURE);
  }

  if (data_path) {
    auto fd = open(data_path, O_RDONLY);
    if (fd == -1) {
      std::cerr << "data: Could not open file " << data_path << ": "
                << strerror(errno) << std::endl;
      exit(EXIT_FAILURE);
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
      std::cerr << "data: Could not stat file " << data_path << ": "
                << strerror(errno) << std::endl;
      exit(EXIT_FAILURE);
    }
    config.fd = fd;
    config.datalen = st.st_size;
    auto addr = mmap(nullptr, config.datalen, PROT_READ, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
      std::cerr << "data: Could not mmap file " << data_path << ": "
                << strerror(errno) << std::endl;
      exit(EXIT_FAILURE);
    }
    config.data = static_cast<uint8_t *>(addr);
  }

  auto addr = argv[optind++];
  auto port = argv[optind++];

  if (parse_requests(&argv[optind], argc - optind) != 0) {
    exit(EXIT_FAILURE);
  }

  if (config.nstreams == 0) {
    config.nstreams = config.requests.size();
  }

  TLSClientContext tls_ctx;
  if (tls_ctx.init(private_key_file, cert_file) != 0) {
    exit(EXIT_FAILURE);
  }

  auto ev_loop_d = defer(ev_loop_destroy, EV_DEFAULT);

  auto keylog_filename = getenv("SSLKEYLOGFILE");
  if (keylog_filename) {
    keylog_file.open(keylog_filename, std::ios_base::app);
    if (keylog_file) {
      tls_ctx.enable_keylog();
    }
  }

  if (util::generate_secret(config.static_secret.data(),
                            config.static_secret.size()) != 0) {
    std::cerr << "Unable to generate static secret" << std::endl;
    exit(EXIT_FAILURE);
  }

  Client c(EV_DEFAULT);

  if (run(c, addr, port, tls_ctx) != 0) {
    exit(EXIT_FAILURE);
  }

  return EXIT_SUCCESS;
}
