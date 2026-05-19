# Verifying Turmeric's Multiparty Session Types with Leanstral

## Introduction

Turmeric's multiparty session types (MPST) ensure type-safe communication between distributed processes. To verify the correctness of these types, we use **Leanstral**, a formal verification framework built on Lean 4. This document explains how Leanstral can be applied to Turmeric's MPST implementation.

## Prerequisites

1. **Lean 4**: Install Lean 4 from [leanprover.github.io](https://leanprover.github.io/lean4/).
2. **Leanstral**: Clone the Leanstral repository and follow its setup instructions.
3. **Turmeric**: Ensure you have the latest Turmeric source code.

## Key Concepts

- **Multiparty Session Types (MPST)**: Define communication protocols for multiple participants.
- **Leanstral**: A Lean 4 library for verifying session-typed programs.
- **Turmeric's MPST**: Implemented in `stdlib/session.tur` and related files.

## Verification Workflow

### 1. Extract MPST Definitions

Turmeric's MPST definitions are in:
- `stdlib/session.tur`
- `stdlib/session/**.tur`

Key constructs:
- `Session` type
- `send`, `recv`, `choose`, `branch` operations
- Global type definitions

### 2. Translate to Leanstral

Leanstral expects session types in a specific format. You'll need to:

1. **Map Turmeric types to Leanstral**:
   - Turmeric's `Session` → Leanstral's `SessionType`
   - Turmeric's `send`/`recv` → Leanstral's `!`/`?`
   - Turmeric's `choose`/`branch` → Leanstral's `&`/`⊕`

2. **Example Translation**:
   ```turmeric
   (defprotocol ClientServer
     (send :int)
     (recv :bool))
   ```
   becomes in Leanstral:
   ```lean
   def clientServer : SessionType := !Int ⊸ ?Bool ⊸ End
   ```

### 3. Verify Properties

Use Leanstral to verify:
- **Type Safety**: No communication errors (e.g., sending when expecting to receive).
- **Progress**: No deadlocks.
- **Protocol Fidelity**: Implementation matches the protocol.

Example verification:
```lean
theorem client_server_type_safe : TypeSafe clientServer := by
  -- proof steps here
```

### 4. Connect to Turmeric

To verify Turmeric's implementation:

1. **Extract Runtime Behavior**: Use Turmeric's interpreter to generate traces.
2. **Compare with Leanstral Model**: Ensure traces match the verified protocol.

## Example: Verifying a Simple Protocol

### Turmeric Protocol
```turmeric
(defprotocol Auth
  (send :username)
  (recv :token))
```

### Leanstral Verification
```lean
def authProtocol : SessionType := !String ⊸ ?String ⊸ End

-- Verify type safety
example : TypeSafe authProtocol := by
  apply typeSafe_send
  apply typeSafe_recv
  apply typeSafe_end

-- Verify progress
example : Progress authProtocol := by
  apply progress_send
  apply progress_recv
  apply progress_end
```

## Advanced: Verifying Global Types

For multiparty protocols:

1. **Define Global Type** in Turmeric:
   ```turmeric
   (defglobal Chat [A B C]
     (from A (send :msg) to B)
     (from B (send :ack) to C))
   ```

2. **Project to Leanstral**:
   ```lean
   def chatA : SessionType := !String ⊸ End
   def chatB : SessionType := ?String ⊸ !String ⊸ End
   def chatC : SessionType := ?String ⊸ End
   ```

3. **Verify Global Consistency**:
   ```lean
   theorem chat_consistent : GlobalConsistent [chatA, chatB, chatC] := by
     -- proof steps here
   ```

## Tools and Scripts

- **`tools/verify-session.py`**: Automates translation from Turmeric to Leanstral.
- **`tests/leanstral/`**: Contains reference verification examples.

## Common Pitfalls

1. **Mismatched Types**: Ensure Turmeric's types align with Leanstral's expectations.
2. **Incomplete Projections**: All roles in a global type must be projected.
3. **Non-Termination**: Leanstral may not terminate on recursive protocols.

## References

- [Leanstral Documentation](https://leanstral.github.io)
- [Turmeric Session Types](stdlib/session.tur)
- [Session Types: A Logical Foundation](https://arxiv.org/abs/1701.08195)