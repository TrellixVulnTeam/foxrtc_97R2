// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_QUIC_CORE_CONGESTION_CONTROL_SIMULATION_SWITCH_H_
#define NET_QUIC_CORE_CONGESTION_CONTROL_SIMULATION_SWITCH_H_

#include <unordered_map>

#include "net/quic/core/congestion_control/simulation/queue.h"

namespace net {
namespace simulation {

typedef size_t SwitchPortNumber;

// Simulates a network switch with simple persistent learning scheme and queues
// on every output port.
class Switch {
 public:
  Switch(Simulator* simulator,
         std::string name,
         SwitchPortNumber port_count,
         QuicByteCount queue_capacity);
  ~Switch();

  // Returns Endpoint associated with the port under number |port_number|.  Just
  // like on most real switches, port numbering starts with 1.
  inline Endpoint* port(SwitchPortNumber port_number) {
    DCHECK_NE(port_number, 0u);
    return &ports_[port_number - 1];
  }

 private:
  class Port : public Endpoint, public UnconstrainedPortInterface {
   public:
    Port(Simulator* simulator,
         std::string name,
         Switch* parent,
         SwitchPortNumber port_number,
         QuicByteCount queue_capacity);
    Port(Port&&) = delete;
    ~Port() override {}

    // Accepts packet to be routed into the switch.
    void AcceptPacket(std::unique_ptr<Packet> packet) override;
    // Enqueue packet to be routed out of the switch.
    void EnqueuePacket(std::unique_ptr<Packet> packet);

    UnconstrainedPortInterface* GetRxPort() override;
    void SetTxPort(ConstrainedPortInterface* port) override;

    void Act() override;

    inline bool connected() { return connected_; }

   private:
    Switch* parent_;
    SwitchPortNumber port_number_;
    bool connected_;

    Queue queue_;

    DISALLOW_COPY_AND_ASSIGN(Port);
  };

  // Sends the packet to the appropriate port, or to all ports if the
  // appropriate port is not known.
  void DispatchPacket(SwitchPortNumber port_number,
                      std::unique_ptr<Packet> packet);

  std::deque<Port> ports_;
  std::unordered_map<std::string, Port*> switching_table_;

  DISALLOW_COPY_AND_ASSIGN(Switch);
};

}  // namespace simulation
}  // namespace net

#endif  // NET_QUIC_CORE_CONGESTION_CONTROL_SIMULATION_SWITCH_H_
