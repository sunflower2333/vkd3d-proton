/* SPDX-License-Identifier: LGPL-2.1-or-later */
static unsigned stencil_ref_calls;
static uint32_t stencil_ref_seen;
static int32_t test_stencil_ref(vkdu_object *command, uint32_t reference) {
    graphics_borrow(command); ++stencil_ref_calls;
    if (graphics_hook) graphics_hook();
    stencil_ref_seen = reference;
    if (!peer(command)->references) std::abort();
    return next_result;
}
#define vkdu_command_stencil_ref test_stencil_ref
