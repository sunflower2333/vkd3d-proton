/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef VKDU_TOPOLOGY_OBSERVER_TEST_H
#define VKDU_TOPOLOGY_OBSERVER_TEST_H
struct vkdu_topology_observation {
    uint64_t setter_before, setter_after, draw_before, draw_after, indexed_before, indexed_after;
};
struct vkdu_topology_observation vkdu_test_topology_observation(void);
#endif
