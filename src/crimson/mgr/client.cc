// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "client.h"

#include "crimson/common/log.h"
#include "crimson/net/Connection.h"
#include "crimson/net/Messenger.h"
#include "messages/MMgrConfigure.h"
#include "messages/MMgrMap.h"
#include "messages/MMgrOpen.h"

namespace {
  seastar::logger& logger()
  {
    return crimson::get_logger(ceph_subsys_mgrc);
  }
}

using crimson::common::local_conf;

namespace crimson::mgr
{

Client::Client(crimson::net::Messenger& msgr,
                 WithStats& with_stats)
  : msgr{msgr},
    with_stats{with_stats},
    tick_timer{[this] {tick();}}
{}

seastar::future<> Client::start()
{
  return seastar::now();
}

seastar::future<> Client::stop()
{
  return gate.close().then([this] {
    if (conn) {
      return conn->close();
    } else {
      return seastar::now();
    }
  });
}

seastar::future<> Client::ms_dispatch(crimson::net::Connection* conn,
                                      MessageRef m)
{
  switch(m->get_type()) {
  case MSG_MGR_MAP:
    return handle_mgr_map(conn, boost::static_pointer_cast<MMgrMap>(m));
  case MSG_MGR_CONFIGURE:
    return handle_mgr_conf(conn, boost::static_pointer_cast<MMgrConfigure>(m));
  default:
    return seastar::now();
  }
}

seastar::future<> Client::ms_handle_reset(crimson::net::ConnectionRef c)
{
  if (conn == c) {
    conn = nullptr;
  }
  return seastar::now();
}

seastar::future<> Client::reconnect()
{
  return (conn ? conn->close() : seastar::now()).then([this] {
    if (!mgrmap.get_available()) {
      logger().warn("No active mgr available yet");
      return seastar::now();
    }
    auto peer = mgrmap.get_active_addrs().front();
    conn = msgr.connect(peer, CEPH_ENTITY_TYPE_MGR);
    // ask for the mgrconfigure message
    auto m = ceph::make_message<MMgrOpen>();
    m->daemon_name = local_conf()->name.get_id();
    return conn->send(std::move(m));
  });
}

seastar::future<> Client::handle_mgr_map(crimson::net::Connection*,
                                         Ref<MMgrMap> m)
{
  mgrmap = m->get_map();
  if (!conn) {
    return reconnect();
  } else if (conn->get_peer_addr() !=
             mgrmap.get_active_addrs().legacy_addr()) {
    return reconnect();
  } else {
    return seastar::now();
  }
}

seastar::future<> Client::handle_mgr_conf(crimson::net::Connection* conn,
                                          Ref<MMgrConfigure> m)
{
  logger().info("{} {}", __func__, *m);
  tick_period = std::chrono::seconds{m->stats_period};
  if (tick_period.count() && !tick_timer.armed() ) {
    tick();
  }
  return seastar::now();
}

void Client::tick()
{
  (void) seastar::with_gate(gate, [=] {
    if (conn) {
      auto pg_stats = with_stats.get_stats();
      return conn->send(std::move(pg_stats)).finally([this] {
	if (tick_period.count()) {
	  tick_timer.arm(tick_period);
	}
      });
    } else {
      return reconnect().finally([this] {
	if (tick_period.count()) {
	  tick_timer.arm(tick_period);
	}
      });;
    }
  });
}

}
