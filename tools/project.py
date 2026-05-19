#!/usr/bin/env python3
"""
tools/project.py -- Reference projection algorithm for multi-party session types.

Implements the Honda/Yoshida/Carbone projection rules for global -> local
session types. This is a standalone Python spike used to validate the
algorithm before implementing src/project.c (SS6).

Projection rules validated:
  (-> R X T) then rest  =>  R gets Send[T, project(rest, R)]
  (-> X R T) then rest  =>  R gets Recv[T, project(rest, R)]
  (-> X Y T) then rest  =>  R uninvolved: project(rest, R)
  choice deciding R     =>  R gets Choose (internal choice)
  choice involved R     =>  R gets Branch (external choice)
  choice uninvolved R   =>  merge all branch projections (fail => TUR_E0220)
  loop label body       =>  Rec[label, project(body, R)] if R is in the body
  continue label        =>  RecVar[label]

Usage:
    python3 tools/project.py
"""

from __future__ import annotations
from dataclasses import dataclass
from typing import Union

# ---------------------------------------------------------------------------
# Global protocol AST nodes
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class GMsg:
    """(-> from_role to_role msg_type) -- direct message between two roles."""
    from_role: str
    to_role: str
    msg_type: str

@dataclass(frozen=True)
class GChoice:
    """(choice deciding_role [(label body...) ...]) -- labelled choice."""
    deciding_role: str
    branches: tuple  # tuple of (str label, tuple[GlobalNode,...] body)

@dataclass(frozen=True)
class GLoop:
    """(loop label body...) -- recursive protocol."""
    label: str
    body: tuple  # tuple of GlobalNode

@dataclass(frozen=True)
class GContinue:
    """(continue label) -- recur to enclosing loop."""
    label: str

GlobalNode = Union[GMsg, GChoice, GLoop, GContinue]

# ---------------------------------------------------------------------------
# Local session type AST
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class LSend:
    """Send[T, Q] -- send a value of type T, then continue as Q."""
    msg_type: str
    cont: object  # LocalType

@dataclass(frozen=True)
class LRecv:
    """Recv[T, Q] -- receive a value of type T, then continue as Q."""
    msg_type: str
    cont: object  # LocalType

@dataclass(frozen=True)
class LChoose:
    """Choose{...} -- internal choice; this role selects the branch."""
    branches: tuple  # tuple of (str label, LocalType)

@dataclass(frozen=True)
class LBranch:
    """Branch{...} -- external choice; this role receives the branch label."""
    branches: tuple  # tuple of (str label, LocalType)

@dataclass(frozen=True)
class LRec:
    """Rec[label, body] -- recursive session type (mu-type)."""
    label: str
    body: object  # LocalType

@dataclass(frozen=True)
class LRecVar:
    """RecVar[label] -- reference to an enclosing Rec binder."""
    label: str

@dataclass(frozen=True)
class LClose:
    """Close -- protocol complete."""
    pass

LocalType = Union[LSend, LRecv, LChoose, LBranch, LRec, LRecVar, LClose]

CLOSE = LClose()

# ---------------------------------------------------------------------------
# Errors
# ---------------------------------------------------------------------------

class ProjectionError(Exception):
    """
    Raised when a global protocol is not projectable onto a role.
    Corresponds to diagnostic TUR_E0220.
    """
    pass

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _has_recvar(t: LocalType, label: str) -> bool:
    """Return True if t contains a free LRecVar with the given label."""
    if isinstance(t, LRecVar):
        return t.label == label
    if isinstance(t, (LSend, LRecv)):
        return _has_recvar(t.cont, label)
    if isinstance(t, (LChoose, LBranch)):
        return any(_has_recvar(b, label) for _, b in t.branches)
    if isinstance(t, LRec):
        # label is shadowed if it matches this binder
        return False if t.label == label else _has_recvar(t.body, label)
    return False


def _merge(t1: LocalType, t2: LocalType) -> LocalType:
    """
    Merge two projected local types for an uninvolved role.

    Per Honda/Yoshida/Carbone, two types are mergeable when they are
    structurally equal up to recursive merge on sub-terms.  Raises
    ProjectionError (TUR_E0220) when the types cannot be merged.
    """
    if type(t1) is not type(t2):
        raise ProjectionError(
            f"Cannot merge incompatible local types: {_fmt(t1)} vs {_fmt(t2)}"
        )
    if isinstance(t1, LClose):
        return CLOSE
    if isinstance(t1, LRecVar):
        if t1.label != t2.label:
            raise ProjectionError(
                f"Cannot merge RecVar[{t1.label}] with RecVar[{t2.label}]"
            )
        return t1
    if isinstance(t1, LSend):
        if t1.msg_type != t2.msg_type:
            raise ProjectionError(
                f"Cannot merge Send[{t1.msg_type}] with Send[{t2.msg_type}]"
            )
        return LSend(t1.msg_type, _merge(t1.cont, t2.cont))
    if isinstance(t1, LRecv):
        if t1.msg_type != t2.msg_type:
            raise ProjectionError(
                f"Cannot merge Recv[{t1.msg_type}] with Recv[{t2.msg_type}]"
            )
        return LRecv(t1.msg_type, _merge(t1.cont, t2.cont))
    if isinstance(t1, LBranch):
        ls1 = [l for l, _ in t1.branches]
        ls2 = [l for l, _ in t2.branches]
        if ls1 != ls2:
            raise ProjectionError(
                f"Cannot merge Branch with different labels: {ls1} vs {ls2}"
            )
        merged = tuple(
            (l, _merge(b1, b2))
            for (l, b1), (_, b2) in zip(t1.branches, t2.branches)
        )
        return LBranch(merged)
    if isinstance(t1, LChoose):
        ls1 = [l for l, _ in t1.branches]
        ls2 = [l for l, _ in t2.branches]
        if ls1 != ls2:
            raise ProjectionError(
                f"Cannot merge Choose with different labels: {ls1} vs {ls2}"
            )
        merged = tuple(
            (l, _merge(b1, b2))
            for (l, b1), (_, b2) in zip(t1.branches, t2.branches)
        )
        return LChoose(merged)
    if isinstance(t1, LRec):
        if t1.label != t2.label:
            raise ProjectionError(
                f"Cannot merge Rec[{t1.label}] with Rec[{t2.label}]"
            )
        return LRec(t1.label, _merge(t1.body, t2.body))
    raise ProjectionError(f"Unknown LocalType: {type(t1)}")


def _involves(body: tuple, role: str) -> bool:
    """
    Return True if role participates directly in the top-level interactions
    of body.  Used to decide whether a role gets Branch (external choice) or
    the uninvolved-merge treatment for a choice it does not decide.

    "Direct" means a top-level GMsg or being the deciding_role of a top-level
    GChoice.  We recurse into GLoop bodies (sequential, not branching) but do
    NOT recurse into the branches of nested GChoice nodes -- that would falsely
    mark a role as notified when it is only reachable through an inner choice
    that the role never learns about.  The non-recursive treatment of nested
    GChoice branches is what enables merge-failure detection for uninvolved
    roles (Case 4).
    """
    for node in body:
        if isinstance(node, GMsg):
            if node.from_role == role or node.to_role == role:
                return True
        elif isinstance(node, GChoice):
            if node.deciding_role == role:
                return True
            # Do NOT recurse into nested choice branches here.
        elif isinstance(node, GLoop):
            if _involves(node.body, role):
                return True
    return False

# ---------------------------------------------------------------------------
# Projection
# ---------------------------------------------------------------------------

def project(interactions: tuple, role: str) -> LocalType:
    """
    Project a global protocol (tuple of GlobalNode) onto a local role.

    Returns the LocalType for that role.
    Raises ProjectionError (TUR_E0220) if the protocol is not projectable.

    Limitations of this reference implementation:
    - GLoop nodes must be the last element in their enclosing sequence; any
      interactions following a Loop are not threaded into the loop body.
      The four validated test cases are all self-contained loops, so this
      simplification does not affect the validation goal.
    """
    if not interactions:
        return CLOSE

    first = interactions[0]
    rest = interactions[1:]

    # -- Direct message --------------------------------------------------
    if isinstance(first, GMsg):
        if first.from_role == role:
            return LSend(first.msg_type, project(rest, role))
        if first.to_role == role:
            return LRecv(first.msg_type, project(rest, role))
        # role is uninvolved: skip this interaction
        return project(rest, role)

    # -- Labelled choice -------------------------------------------------
    if isinstance(first, GChoice):
        deciding = first.deciding_role

        if deciding == role:
            # Internal choice: role picks the branch (Choose)
            projected = tuple(
                (label, project(body + rest, role))
                for label, body in first.branches
            )
            return LChoose(projected)

        # Is role directly involved in any branch?
        involved = any(_involves(body, role) for _, body in first.branches)

        if involved:
            # External choice: role is notified and must select a branch (Branch)
            projected = tuple(
                (label, project(body + rest, role))
                for label, body in first.branches
            )
            return LBranch(projected)

        # Role is uninvolved in all branches: project each branch and merge.
        # Merge failure => TUR_E0220.
        projected_types = [
            project(body + rest, role) for _, body in first.branches
        ]
        if not projected_types:
            return project(rest, role)
        result = projected_types[0]
        for other in projected_types[1:]:
            result = _merge(result, other)
        return result

    # -- Recursive protocol ----------------------------------------------
    if isinstance(first, GLoop):
        projected_body = project(first.body, role)
        if _has_recvar(projected_body, first.label):
            # Role participates in the loop: wrap in a recursive type
            return LRec(first.label, projected_body)
        # Role is not involved in the loop body: return its projection as-is
        return projected_body

    # -- Recursion variable ----------------------------------------------
    if isinstance(first, GContinue):
        return LRecVar(first.label)

    raise ProjectionError(f"Unknown GlobalNode: {type(first)}")

# ---------------------------------------------------------------------------
# Pretty printer
# ---------------------------------------------------------------------------

def _fmt(t: LocalType) -> str:
    """Format a local type as a readable string."""
    if isinstance(t, LClose):
        return "Close"
    if isinstance(t, LRecVar):
        return f"RecVar[{t.label}]"
    if isinstance(t, LSend):
        return f"Send[{t.msg_type}, {_fmt(t.cont)}]"
    if isinstance(t, LRecv):
        return f"Recv[{t.msg_type}, {_fmt(t.cont)}]"
    if isinstance(t, LChoose):
        branches = ", ".join(f"{l}: {_fmt(b)}" for l, b in t.branches)
        return f"Choose{{{branches}}}"
    if isinstance(t, LBranch):
        branches = ", ".join(f"{l}: {_fmt(b)}" for l, b in t.branches)
        return f"Branch{{{branches}}}"
    if isinstance(t, LRec):
        return f"Rec[{t.label}, {_fmt(t.body)}]"
    return repr(t)

# ---------------------------------------------------------------------------
# Test runner
# ---------------------------------------------------------------------------

def _run_tests() -> bool:
    passed = 0
    failed = 0

    def check(name: str, got: LocalType, expected: LocalType) -> None:
        nonlocal passed, failed
        if got == expected:
            print(f"  PASS  {name}")
            passed += 1
        else:
            print(f"  FAIL  {name}")
            print(f"         got:      {_fmt(got)}")
            print(f"         expected: {_fmt(expected)}")
            failed += 1

    def check_error(name: str, fn) -> None:
        nonlocal passed, failed
        try:
            result = fn()
            print(f"  FAIL  {name}  (expected ProjectionError, got {_fmt(result)})")
            failed += 1
        except ProjectionError as exc:
            print(f"  PASS  {name}  (ProjectionError: {exc})")
            passed += 1

    # -------------------------------------------------------------------
    # Case 1 -- ThreeWayHandshake (Example 4 from session-types-plan.md)
    #
    # (defprotocol Handshake [A B C]
    #   (-> A B Syn)
    #   (-> B A SynAck)
    #   (-> A C Ack)
    #   (-> C A AckAck))
    #
    # Expected local types:
    #   A: Send[Syn, Recv[SynAck, Send[Ack, Recv[AckAck, Close]]]]
    #   B: Recv[Syn, Send[SynAck, Close]]
    #   C: Recv[Ack, Send[AckAck, Close]]
    # -------------------------------------------------------------------
    print("\nCase 1: ThreeWayHandshake -- straightforward sequential interactions")
    handshake = (
        GMsg("A", "B", "Syn"),
        GMsg("B", "A", "SynAck"),
        GMsg("A", "C", "Ack"),
        GMsg("C", "A", "AckAck"),
    )
    check("A",
        project(handshake, "A"),
        LSend("Syn", LRecv("SynAck", LSend("Ack", LRecv("AckAck", CLOSE)))),
    )
    check("B",
        project(handshake, "B"),
        LRecv("Syn", LSend("SynAck", CLOSE)),
    )
    check("C",
        project(handshake, "C"),
        LRecv("Ack", LSend("AckAck", CLOSE)),
    )

    # -------------------------------------------------------------------
    # Case 2 -- Uninvolved-role merge
    #
    # (defprotocol LoggedRPC [client server logger]
    #   (choice client
    #     [(ask (-> client server Request)
    #           (-> server client Response))
    #      (quit)])
    #   (-> server logger Log))
    #
    # logger is uninvolved in the choice (client decides; neither ask nor
    # quit branch contains a direct message to/from logger).  logger only
    # appears in rest, so both branch projections for logger are identical:
    # Recv[Log, Close].  Merge succeeds.
    #
    # Expected:
    #   client: Choose{ask: Send[Request, Recv[Response, Close]], quit: Close}
    #   server: Branch{ask: Recv[Request, Send[Response, Send[Log, Close]]],
    #                  quit: Send[Log, Close]}
    #   logger: Recv[Log, Close]
    # -------------------------------------------------------------------
    print("\nCase 2: Choice with uninvolved-role merge")
    logged_rpc = (
        GChoice("client", (
            ("ask", (
                GMsg("client", "server", "Request"),
                GMsg("server", "client", "Response"),
            )),
            ("quit", ()),
        )),
        GMsg("server", "logger", "Log"),
    )
    check("client",
        project(logged_rpc, "client"),
        LChoose((
            ("ask", LSend("Request", LRecv("Response", CLOSE))),
            ("quit", CLOSE),
        )),
    )
    check("server",
        project(logged_rpc, "server"),
        LBranch((
            ("ask", LRecv("Request", LSend("Response", LSend("Log", CLOSE)))),
            ("quit", LSend("Log", CLOSE)),
        )),
    )
    check("logger",
        project(logged_rpc, "logger"),
        LRecv("Log", CLOSE),
    )

    # -------------------------------------------------------------------
    # Case 3 -- Recursive echo protocol (exercises loop/continue projection)
    #
    # (defprotocol EchoServer [client server]
    #   (loop echo
    #     (choice client
    #       [(again (-> client server int)
    #               (-> server client int)
    #               (continue echo))
    #        (quit)])))
    #
    # Expected:
    #   client: Rec[echo, Choose{again: Send[int, Recv[int, RecVar[echo]]],
    #                            quit: Close}]
    #   server: Rec[echo, Branch{again: Recv[int, Send[int, RecVar[echo]]],
    #                            quit: Close}]
    # -------------------------------------------------------------------
    print("\nCase 3: Recursive echo protocol -- loop/continue projection")
    echo_server = (
        GLoop("echo", (
            GChoice("client", (
                ("again", (
                    GMsg("client", "server", "int"),
                    GMsg("server", "client", "int"),
                    GContinue("echo"),
                )),
                ("quit", ()),
            )),
        )),
    )
    check("client",
        project(echo_server, "client"),
        LRec("echo", LChoose((
            ("again", LSend("int", LRecv("int", LRecVar("echo")))),
            ("quit", CLOSE),
        ))),
    )
    check("server",
        project(echo_server, "server"),
        LRec("echo", LBranch((
            ("again", LRecv("int", LSend("int", LRecVar("echo")))),
            ("quit", CLOSE),
        ))),
    )

    # -------------------------------------------------------------------
    # Case 4 -- Non-projectable protocol (TUR_E0220)
    #
    # (defprotocol BadProtocol [A B C]
    #   (choice A
    #     [(left  (choice B [(go (-> B C TypeA)) (skip)]))
    #      (right (choice B [(go (-> B C TypeB)) (skip)]))]))
    #
    # C is uninvolved in the outer choice (A decides; C does not appear at
    # the top level of either branch -- only inside nested B-choices).
    # The uninvolved-merge rule therefore applies to C:
    #
    #   left  projects to Branch{go: Recv[TypeA, Close], skip: Close} for C
    #   right projects to Branch{go: Recv[TypeB, Close], skip: Close} for C
    #
    # Merging those types fails because Recv[TypeA] != Recv[TypeB].
    # => ProjectionError (TUR_E0220)
    #
    # A and B are still projectable:
    #   A: Choose{left: Choose{go: Close, skip: Close},
    #             right: Choose{go: Close, skip: Close}}
    #   B: Branch{left: Choose{go: Send[TypeA, Close], skip: Close},
    #             right: Choose{go: Send[TypeB, Close], skip: Close}}
    # -------------------------------------------------------------------
    print("\nCase 4: Non-projectable protocol -- merge failure (TUR_E0220)")
    bad_protocol = (
        GChoice("A", (
            ("left", (
                GChoice("B", (
                    ("go",   (GMsg("B", "C", "TypeA"),)),
                    ("skip", ()),
                )),
            )),
            ("right", (
                GChoice("B", (
                    ("go",   (GMsg("B", "C", "TypeB"),)),
                    ("skip", ()),
                )),
            )),
        )),
    )
    check_error("C not projectable -- merge fails (Recv[TypeA] vs Recv[TypeB])",
        lambda: project(bad_protocol, "C"),
    )
    # A decides the outer choice but has no further interactions inside either
    # outer branch (B decides the inner choices).  So A's inner projection
    # for each outer branch is simply Close.
    check("A projects fine",
        project(bad_protocol, "A"),
        LChoose((
            ("left",  CLOSE),
            ("right", CLOSE),
        )),
    )
    check("B projects fine",
        project(bad_protocol, "B"),
        LBranch((
            ("left",  LChoose((("go", LSend("TypeA", CLOSE)), ("skip", CLOSE)))),
            ("right", LChoose((("go", LSend("TypeB", CLOSE)), ("skip", CLOSE)))),
        )),
    )

    # -------------------------------------------------------------------
    print(f"\nResults: {passed} passed, {failed} failed out of {passed + failed} checks")
    return failed == 0


if __name__ == "__main__":
    import sys
    sys.exit(0 if _run_tests() else 1)
