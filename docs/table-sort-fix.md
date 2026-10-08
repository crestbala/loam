# Table Sort Improvements - Summary

## Overview

Fixed two critical table sorting bugs and added new string utility functions.

## Changes Made

### 1. Fixed Reactivity Bug ✅
**File:** `packages/zeus-components/std/organisms.loam`

**Problem:** Sorting required multiple clicks to update the UI.

**Root Cause:** Used `effect()` instead of `computed()` for derived state.

**Solution:** 
- Changed `Table` component to use `computed()` instead of `effect()` + `signal()`
- Sort changes now immediately propagate to the UI in ONE click

### 2. Fixed Value Ordering (Case-Insensitive Sort) ✅
**File:** `packages/zeus-components/std/atoms.loam`

**Problem:** String sorting was case-sensitive: "Banana" < "apple" < "cherry"

**Root Cause:** `str_cmp` compared raw ASCII bytes (A=65 < a=97)

**Solution:**
- Added `lowercase(s: string) -> string` function
- Added `uppercase(s: string) -> string` function  
- Updated `str_cmp` to use `lowercase()` for case-insensitive comparison
- Result: "apple" < "Banana" < "cherry" (correct case-insensitive order)

### 3. Added String Functions ✅
**File:** `packages/zeus-components/std/atoms.loam`

New API (accessible via `zui.lowercase()` / `zui.uppercase()`):

```loam
/// A copy of `s` with ASCII letters lowercased ("Hello" → "hello").
fn lowercase(s: string) -> string

/// A copy of `s` with ASCII letters uppercased ("hello" → "HELLO").
fn uppercase(s: string) -> string
```

Helper functions:
- `lower_byte(c: int) -> int`: Convert ASCII byte to lowercase
- `upper_byte(c: int) -> int`: Convert ASCII byte to uppercase

### 4. Reverted Hover Behavior ✅
**File:** `packages/zeus-components/std/organisms.loam`

- Restored click-based sort icons (reverted hover change)
- Sort icons show only when column is actively sorted

## Test Files

### New Tests (all passing ✓)
1. **`zeus_table_sort_columns.loam`** - Tests column switching in ONE click
   - Validates Name ↔ Qty switching
   - Ensures row nodes are reused (not rebuilt)

2. **`zeus_table_ordering.loam`** - Tests case-insensitive ordering
   - alpha < Beta < Delta < epsilon < gamma (case-insensitive)
   - -5 < 0 < 3 < 10 < 100 (numeric with negatives)

3. **`zeus_string_case.loam`** - Tests new string functions
   - `lowercase()`: "Hello" → "hello", "WORLD" → "world"
   - `uppercase()`: "hello" → "HELLO", "Apple" → "APPLE"
   - `str_cmp()`: case-insensitive comparison

### Existing Tests (all passing ✓)
- `zeus_table_sort.loam` - basic sort functionality
- `zeus_table.loam` - basic table rendering

## Validation Results

All 5 tests pass:
```
✓ zeus_table_sort.loam
✓ zeus_table_sort_columns.loam
✓ zeus_table_ordering.loam  
✓ zeus_table.loam
✓ zeus_string_case.loam
```

## Key Behavior

### Sorting
- **Numbers**: Sort by parsed value (-5 < 0 < 3 < 10 < 100)
- **Strings**: Case-insensitive via `lowercase()` (apple < Banana < cherry)
- **Mixed**: Numbers first when one parses and other doesn't
- **Stable**: Equal values maintain input order
- **Reactive**: Sort changes update UI immediately in ONE click

### String Functions
- `lowercase("Hello")` → `"hello"`
- `uppercase("hello")` → `"HELLO"`
- Both work on ASCII letters only (A-Z, a-z)

## Design Patterns

Following reactive system design (`packages/zeus/docs/component-model.md`):
- **`signal()`**: Independent state you mutate directly
- **`computed()`**: Derived values that depend on other signals ✓
- **`effect()`**: Side effects only (logging, imperative calls) ✗

## Files Changed

1. `packages/zeus-components/std/atoms.loam` (+73 lines)
   - Added `lower_byte()`, `upper_byte()`, `lowercase()`, `uppercase()`
   - Updated `str_cmp()` to use `lowercase()` for case-insensitive comparison

2. `packages/zeus-components/std/organisms.loam` (+7 lines)
   - Changed `Table` to use `computed()` instead of `effect()`
   - Reverted `TableColHead` to click-based sort icons

3. Documentation: `docs/table-sort-fix.md`





