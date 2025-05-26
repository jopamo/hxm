# LLM Context Report

Generated at: 2025-12-24T11:13:53.227116Z

## 1. Project Overview

- **Root**: `/home/me/projects/bbox`
- **Total Symbols**: 2279
- **Total Coverage**: 85%
- **Clang-Tidy Issues**: 60

## 2. Code Coverage

| File | Coverage | Missing Lines |
|---|---|---|
| `tests/dummy_client.c` | 0% | 6-8,10-11,13-14,16,19-20,22-25,28-29 |
| `tests/parity_client.c` | 0% | 46-48,51-58,61-64,67-70,72-78,81-86,88-89,91-95... |
| `src/main.c` | 13% | 17-25,27-31,34,36-42,45-46,48,50-54,57-59,61,64... |
| `src/xcb_utils.c` | 51% | 136-137,140,145-146,148-150,152-153,155-157,159... |
| `src/menu.c` | 55% | 70-71,180-181,219-222,225-227,229-234,236-238,2... |
| `src/render.c` | 64% | 16-18,20-21,23,25-29,32-36,38-43,45-49,53-57,60... |
| `src/wm.c` | 64% | 74-76,78-85,90-94,96,168-169,171-176,186-188,21... |
| `src/wm_input_keys.c` | 68% | 46-47,50-55,57-58,60,63-64,202-204,227-229,231-... |
| `src/wm_reply.c` | 71% | 47,52-54,90-101,106-110,113,116-117,119,127,135... |
| `src/wm_desktop.c` | 72% | 18-20,23-27,29,31-40,42,44-52,203-208,211-212,2... |
| `tests/test_txn_serialization.c` | 75% | 17-23 |
| `src/config.c` | 82% | 156,158,160,163-170,181-183,201,203,207,213,251... |
| `src/stack.c` | 82% | 170,200-202,206,229-230,240-243,258-259,261-263... |
| `src/event.c` | 84% | 78-79,84-85,255-256,280-281,341-346,413-417,419... |
| `tests/xcb_stubs.c` | 85% | 172-173,253-255,282,379,419,434,444,499,503,525... |
| `tests/test_menu.c` | 86% | 72-76,113-117 |
| `tests/test_server_init_extensions.c` | 86% | 118,130-131,147-148,164-165 |
| `tests/test_wm_input_keys.c` | 86% | 262,274,280,288-292,320-322,324-326,332-336,339... |
| `src/client.c` | 87% | 31-32,49-50,58-59,68,360-361,369-370,448-449,47... |
| `src/frame.c` | 87% | 97,130-135,137-138,140,159 |
| `tests/integration_client.c` | 88% | 18-20,23-30,96-97,163-164,185-186,246-250,259,2... |
| `src/wm_dirty.c` | 91% | 113-114,116,250-253,395-398,400-401,404-406,408... |
| `tests/test_ewmh_check.c` | 91% | 28-31,33-34,42-43,61-62,66-67,71-73 |
| `tests/test_save_set.c` | 91% | 36-42 |
| `tests/test_ds.c` | 92% | 51-52,469-470,472-475,483-484,492-494,499,502,5... |
| `tests/test_server_init_failures.c` | 93% | 169-171 |
| `tests/test_workarea_compute.c` | 94% | 15-16 |
| `src/ds.c` | 96% | 50-51,79-80,166-167,236-237 |
| `tests/test_transient_cycle.c` | 96% | 91-92,126-127 |
| `tests/test_cookie_jar.c` | 97% | 127-129,555,565-566,588,598-599 |
| `tests/test_focus.c` | 97% | 138,143 |
| `tests/test_gtk_decorations.c` | 97% | 46-48 |
| `tests/test_workspaces.c` | 97% | 299-303 |
| `tests/test_frame_destroy.c` | 98% | 67-68 |
| `tests/test_resize.c` | 98% | 36-37 |
| `tests/test_stacking.c` | 98% | 32-33 |
| `tests/test_ewmh_ext.c` | 99% | 111-112 |
| `tests/test_wm_focus_policy.c` | 99% | 705-709 |

*44 files with 100% coverage are hidden*

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
- **../src/event.c**:11851: Call to function 'memset' is insecure as it does not provide security checks introduced in the C11 standard. Replace with analogous functions that support length arguments or provides boundary checks such as 'memset_s' in case of C11
- **../src/event.c**:12356: Call to function 'snprintf' is insecure as it does not provide security checks introduced in the C11 standard. Replace with analogous functions that support length arguments or provides boundary checks such as 'snprintf_s' in case of C11
- **../src/event.c**:12570: Call to function 'snprintf' is insecure as it does not provide security checks introduced in the C11 standard. Replace with analogous functions that support length arguments or provides boundary checks such as 'snprintf_s' in case of C11
- **../src/event.c**:13647: Call to function 'snprintf' is insecure as it does not provide security checks introduced in the C11 standard. Replace with analogous functions that support length arguments or provides boundary checks such as 'snprintf_s' in case of C11
- **../src/event.c**:13817: Call to function 'snprintf' is insecure as it does not provide security checks introduced in the C11 standard. Replace with analogous functions that support length arguments or provides boundary checks such as 'snprintf_s' in case of C11
- **../src/event.c**:14128: Call to function 'snprintf' is insecure as it does not provide security checks introduced in the C11 standard. Replace with analogous functions that support length arguments or provides boundary checks such as 'snprintf_s' in case of C11
- **../src/event.c**:14300: Call to function 'snprintf' is insecure as it does not provide security checks introduced in the C11 standard. Replace with analogous functions that support length arguments or provides boundary checks such as 'snprintf_s' in case of C11
- **../src/event.c**:19462: Call to function 'memcpy' is insecure as it does not provide security checks introduced in the C11 standard. Replace with analogous functions that support length arguments or provides boundary checks such as 'memcpy_s' in case of C11

*(...and 40 more)*

## 4. Symbol Statistics

- **enumerator**: 171
- **macro**: 99
- **member**: 692
- **heredoc**: 1
- **variable**: 161
- **struct**: 94
- **enum**: 23
- **function**: 950
- **typedef**: 88
