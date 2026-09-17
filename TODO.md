# TODO

Tracked work items for treenity. `[x]` done, `[ ]` planned.

## ACK robustness and fast local repair

- [x] **A. Accept ACK from any neighbour.**
  `tn_handle_ack` used to require the ACK to come from the intended next hop
  (`ack_dst`). Because frames are physically broadcast, an opportunistic
  forwarder can deliver the frame and ACK it, but the originator ignored that
  ACK, retransmitted and reported `TX_FAILED`. It now matches by sequence
  number and accepts the ACK from any known neighbour. Test:
  `test_ack_from_any_neighbor`.

- [x] **B. Fast local repair on ACK failure.**
  A dead parent was only noticed after the beacon timeout (`PARENT_TIMEOUT`,
  ~36 s). Added an ACK-based fast path: the intended next hop that does not
  acknowledge accumulates failures; after `TREENET_ACK_FAIL_THRESHOLD` it is
  marked "suspect" for `TREENET_LINK_SUSPECT_MS` and excluded from parent
  selection, and the parent is re-selected immediately. Cuts the blind window
  from ~36 s to a few seconds for nodes that are actively sending. Test:
  `test_fast_repair_on_ack_failure`.

## C. Repeater report: blind-window metric

- [ ] Extend `tests/sim/test_repeaters.c` scenario 2 to also measure, for a node
  whose transit parent is powered off, how many datagrams are lost (`TX_FAILED`)
  before it reconnects, and the reconnection time under active traffic (not just
  idle beacon timeout). Update the docs.

## Future

- [ ] **End-to-end delivery acknowledgement.** Optional ACK from the final
  destination so `TX_DONE` means real delivery (today it only confirms the first
  hop). Bigger protocol change; deferred.
- [ ] **Scalability: Trickle redundancy suppression.** Every node beacons every
  interval (no `k` suppression), which does not scale to hundreds/thousands of
  nodes. Add suppression and/or a larger `BEACON_MAX_MS`, and document the
  beacon airtime budget (`N x ToA / interval`).
- [ ] **Route-table next-hop repair.** The fast repair covers the parent and the
  direct-neighbour shortcut; downward route-table next hops could be handled the
  same way.
- [ ] **Sleepy leaves.** The `LEAF` role exists but does not sleep the radio;
  add store-and-forward buffering on the parent so a sleeping leaf can still
  receive.
