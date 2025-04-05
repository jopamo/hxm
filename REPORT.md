# LLM Context Report

Generated at: 2025-12-24T07:00:12.533010Z

## 1. Project Overview

- **Root**: `/home/me/projects/bbox`
- **Total Symbols**: 2014
- **Total Coverage**: 85%
- **Clang-Tidy Issues**: 60

## 2. Code Coverage

| File | Coverage | Missing Lines |
|---|---|---|
| `tests/dummy_client.c` | 0% | 6-8,10-11,13-14,16,19-20,22-25,28-29 |
| `src/main.c` | 13% | 17-25,27-31,34,36-42,45-46,48,50-54,57-59,61,64... |
| `src/core.c` | 18% | 20,22,31-32,34-37,39-40,42,44-46,48,50,53-56,62... |
| `src/wm_input_keys.c` | 45% | 40-41,44-49,51-52,54,57-58,151-156,185-186,189-... |
| `src/config.c` | 49% | 156,158,160,163-170,177-183,186-188,190-191,193... |
| `src/xcb_utils.c` | 51% | 133-134,137,142-143,145-147,149-150,152-154,156... |
| `src/menu.c` | 55% | 57-58,70-71,180-181,219-222,225-227,229-234,236... |
| `src/wm.c` | 59% | 74-76,78-85,90-94,96,105-112,168-169,171-176,18... |
| `src/render.c` | 64% | 16-18,20-21,23,25-29,32-36,38-43,45-49,53-57,60... |
| `src/wm_desktop.c` | 71% | 18-20,23-27,29,31-40,42,44-52,203-208,211-212,2... |
| `src/wm_reply.c` | 71% | 47,52-54,90-101,106-110,113,116-117,119,127,135... |
| `src/event.c` | 74% | 65-66,79-81,88-89,104-105,119-120,154-155,189-1... |
| `tests/test_txn_serialization.c` | 75% | 17-23 |
| `tests/xcb_stubs.c` | 79% | 244-246,269-270,301-302,304-305,307,362,402,417... |
| `src/stack.c` | 82% | 170,200-202,206,229-230,240-243,258-259,261-263... |
| `src/client.c` | 86% | 31-32,49-50,58-59,68,97-98,106-107,360-361,369-... |
| `tests/test_menu.c` | 86% | 72-76,113-117 |
| `src/frame.c` | 87% | 97,130-135,137-138,140,159 |
| `src/wm_dirty.c` | 88% | 70,113-114,116,250-253,392-395,397-398,401-403,... |
| `tests/integration_client.c` | 88% | 18-20,23-30,96-97,163-164,185-186,246-250,259,2... |
| `tests/test_ewmh_check.c` | 91% | 28-31,33-34,42-43,61-62,66-67,71-73 |
| `tests/test_save_set.c` | 91% | 36-42 |
| `src/log.c` | 92% | 59,76-77,81 |
| `include/slotmap.h` | 94% | 47-51 |
| `tests/test_focus.c` | 94% | 60,65 |
| `tests/test_workarea_compute.c` | 94% | 15-16 |
| `src/cookie_jar.c` | 96% | 43-44,108-109 |
| `src/ds.c` | 96% | 46-47,75-76,162-163,232-233 |
| `tests/test_transient_cycle.c` | 96% | 91-92,126-127 |
| `include/hxm.h` | 97% | 107-108 |
| `tests/test_gtk_decorations.c` | 97% | 46-48 |
| `tests/test_workspaces.c` | 97% | 299-303 |
| `src/focus.c` | 98% | 28 |
| `tests/test_frame_destroy.c` | 98% | 67-68 |
| `tests/test_resize.c` | 98% | 36-37 |
| `tests/test_stacking.c` | 98% | 32-33 |
| `tests/test_cookie_jar.c` | 99% | 116-118 |
| `tests/test_ds.c` | 99% | 31-32 |
| `tests/test_ewmh_ext.c` | 99% | 111-112 |
| `tests/test_wm_focus_policy.c` | 99% | 705-709 |
| `include/client.h` | 100% | - |
| `include/cookie_jar.h` | 100% | - |
| `include/event.h` | 100% | - |
| `include/handle.h` | 100% | - |
| `include/handle_conv.h` | 100% | - |
| `tests/stress_lifecycle.c` | 100% | - |
| `tests/test_colormap.c` | 100% | - |
| `tests/test_configure_geometry.c` | 100% | - |
| `tests/test_dirty_region.c` | 100% | - |
| `tests/test_event_ingest.c` | 100% | - |
| `tests/test_ewmh_negative.c` | 100% | - |
| `tests/test_ewmh_props.c` | 100% | - |
| `tests/test_expose_damage.c` | 100% | - |
| `tests/test_fullscreen.c` | 100% | - |
| `tests/test_gtk_extents.c` | 100% | - |
| `tests/test_icccm.c` | 100% | - |
| `tests/test_icccm_fuzz.c` | 100% | - |
| `tests/test_icccm_props_extra.c` | 100% | - |
| `tests/test_input_interaction.c` | 100% | - |
| `tests/test_manage_unmanage.c` | 100% | - |
| `tests/test_net_close.c` | 100% | - |
| `tests/test_position_hints.c` | 100% | - |
| `tests/test_race_conditions.c` | 100% | - |
| `tests/test_rules.c` | 100% | - |
| `tests/test_size_hints.c` | 100% | - |
| `tests/test_transients.c` | 100% | - |
| `tests/test_unmanage_race.c` | 100% | - |
| `tests/test_wm_class.c` | 100% | - |
| `tests/test_wm_config_theme.c` | 100% | - |
| `tests/test_wm_icon.c` | 100% | - |
| `tests/test_wm_icon_invalid.c` | 100% | - |
| `tests/test_wm_name_fallback.c` | 100% | - |

## 3. Clang-Tidy Diagnostics

### Summary by Type

- **clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling**: 54
- **clang-analyzer-security.ArrayBound**: 2
- **clang-analyzer-deadcode.DeadStores**: 3
- **clang-analyzer-optin.core.EnumCastOutOfRange**: 1

### Top Issues

- **../src/client.c**:35346: Call to function 'memset' is insecure as it does not provide security checks introduced in the C11 standard. Replace with analogous functions that support length arguments or provides boundary checks such as 'memset_s' in case of C11
- **../src/client.c**:35959: Call to function 'memset' is insecure as it does not provide security checks introduced in the C11 standard. Replace with analogous functions that support length arguments or provides boundary checks such as 'memset_s' in case of C11
- **../src/config.c**:1119: Call to function 'memset' is insecure as it does not provide security checks introduced in the C11 standard. Replace with analogous functions that support length arguments or provides boundary checks such as 'memset_s' in case of C11
- **../src/config.c**:3069: Call to function 'snprintf' is insecure as it does not provide security checks introduced in the C11 standard. Replace with analogous functions that support length arguments or provides boundary checks such as 'snprintf_s' in case of C11
- **../src/config.c**:4749: Potential out of bound access to the region with tainted index
- **../src/config.c**:14082: Although the value stored to 'read' is used in the enclosing expression, the value is never actually read from 'read'
- **../src/config.c**:18731: Although the value stored to 'read' is used in the enclosing expression, the value is never actually read from 'read'
- **../src/cookie_jar.c**:3081: Call to function 'memset' is insecure as it does not provide security checks introduced in the C11 standard. Replace with analogous functions that support length arguments or provides boundary checks such as 'memset_s' in case of C11
- **../src/core.c**:594: Call to function 'memset' is insecure as it does not provide security checks introduced in the C11 standard. Replace with analogous functions that support length arguments or provides boundary checks such as 'memset_s' in case of C11
- **../src/ds.c**:2516: Call to function 'memcpy' is insecure as it does not provide security checks introduced in the C11 standard. Replace with analogous functions that support length arguments or provides boundary checks such as 'memcpy_s' in case of C11
- **../src/ds.c**:3740: Call to function 'memcpy' is insecure as it does not provide security checks introduced in the C11 standard. Replace with analogous functions that support length arguments or provides boundary checks such as 'memcpy_s' in case of C11
- **../src/event.c**:1784: Call to function 'memset' is insecure as it does not provide security checks introduced in the C11 standard. Replace with analogous functions that support length arguments or provides boundary checks such as 'memset_s' in case of C11
- **../src/event.c**:11623: Call to function 'memset' is insecure as it does not provide security checks introduced in the C11 standard. Replace with analogous functions that support length arguments or provides boundary checks such as 'memset_s' in case of C11
- **../src/event.c**:12128: Call to function 'snprintf' is insecure as it does not provide security checks introduced in the C11 standard. Replace with analogous functions that support length arguments or provides boundary checks such as 'snprintf_s' in case of C11
- **../src/event.c**:12342: Call to function 'snprintf' is insecure as it does not provide security checks introduced in the C11 standard. Replace with analogous functions that support length arguments or provides boundary checks such as 'snprintf_s' in case of C11
- **../src/event.c**:13419: Call to function 'snprintf' is insecure as it does not provide security checks introduced in the C11 standard. Replace with analogous functions that support length arguments or provides boundary checks such as 'snprintf_s' in case of C11
- **../src/event.c**:13589: Call to function 'snprintf' is insecure as it does not provide security checks introduced in the C11 standard. Replace with analogous functions that support length arguments or provides boundary checks such as 'snprintf_s' in case of C11
- **../src/event.c**:13900: Call to function 'snprintf' is insecure as it does not provide security checks introduced in the C11 standard. Replace with analogous functions that support length arguments or provides boundary checks such as 'snprintf_s' in case of C11
- **../src/event.c**:14072: Call to function 'snprintf' is insecure as it does not provide security checks introduced in the C11 standard. Replace with analogous functions that support length arguments or provides boundary checks such as 'snprintf_s' in case of C11
- **../src/event.c**:18982: Call to function 'memcpy' is insecure as it does not provide security checks introduced in the C11 standard. Replace with analogous functions that support length arguments or provides boundary checks such as 'memcpy_s' in case of C11

*(...and 40 more)*

## 4. Symbol Statistics

- **enumerator**: 143
- **macro**: 81
- **member**: 657
- **heredoc**: 1
- **variable**: 111
- **struct**: 83
- **enum**: 20
- **function**: 848
- **typedef**: 70
