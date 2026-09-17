# TODO

Planned work items for treenity. Completed items are removed.

## Future

- **End-to-end delivery acknowledgement.** Optional ACK from the final
  destination so `TX_DONE` means real delivery (today it only confirms the first
  hop). Bigger protocol change; deferred.
- **Scalability: Trickle redundancy suppression.** Every node beacons every
  interval (no `k` suppression), which does not scale to hundreds/thousands of
  nodes. Add suppression and/or a larger `BEACON_MAX_MS`, and document the
  beacon airtime budget (`N x ToA / interval`).
- **Route-table next-hop repair.** The fast repair covers the parent and the
  direct-neighbour shortcut; downward route-table next hops could be handled the
  same way.
- **Sleepy leaves.** The `LEAF` role exists but does not sleep the radio; add
  store-and-forward buffering on the parent so a sleeping leaf can still
  receive.
