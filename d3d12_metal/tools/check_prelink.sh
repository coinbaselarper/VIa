#!/bin/bash
# usage: check_prelink.sh PRELINKED.o
# Fails when the prelinked object leaves a reference to the layer's own code undefined: it would bind to
# gta6rt_d3d12_vulkan's copy at the host link. Allowed: the shared portable code (PSO streams, root signatures),
# shims_core, and the shared-state namespaces the module exports on purpose (d3d12_metal.cmake).
obj="$1"
bad=$(nm -u "$obj" | c++filt | grep -E '^(rt::|leonida::)' |
    grep -vE '^rt::(d3d12_xs::(PsoStreamRecord|attach_pso_record|audit_pso_stream|copy_root_signature_desc_clear_xbox_flags|copy_rts0_blob_clear_xbox_flags|copy_versioned_root_signature_desc_clear_xbox_flags|derive_stream|downgrade_root_signature_1_2_to_1_1|dxbc_checksum|find_pso_record|serialize_legacy_keeping_heap_flags|serialize_versioned_keeping_heap_flags|share_pso_record)|rootsig::|shims::|xmem::|xg_xs::|startup_trace::|d3d12_trace::)' |
    grep -vE '^leonida::(label|title_gpu_views|title_texel_writes|title_xg_layouts|value_sink|kernel_writes|dstorage|console_texture)::')
if [ -n "$bad" ]; then
    echo "check_prelink: $obj leaves layer symbols undefined (they would bind to the Vulkan layer):" >&2
    echo "$bad" | sed 's/^/  /' >&2
    exit 1
fi
echo "check_prelink: no layer symbol left to the Vulkan layer"
