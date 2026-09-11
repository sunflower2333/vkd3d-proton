/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "vkd3d.h"

/* These queries normally live in the application-facing d3d12core factory.
 * An embedded UMD has no IVKD3DDebugControlInterface factory. Use its unchanged
 * production defaults: retain all validation messages and normal spec behavior.
 * This is independent of feature/capability reporting and GPU selection. */
bool vkd3d_debug_control_is_test_suite(void) { return false; }
bool vkd3d_debug_control_explode_on_vvl_error(void) { return false; }
bool vkd3d_debug_control_has_out_of_spec_test_behavior(VKD3D_DEBUG_CONTROL_OUT_OF_SPEC_BEHAVIOR behavior)
{ (void)behavior; return false; }
VKD3D_DEBUG_CONTROL_BEHAVIOR_FLAGS vkd3d_debug_control_get_behavior_flags(void)
{ return (VKD3D_DEBUG_CONTROL_BEHAVIOR_FLAGS)0; }
bool vkd3d_debug_control_mute_message_id(const char *vuid)
{ (void)vuid; return false; }
