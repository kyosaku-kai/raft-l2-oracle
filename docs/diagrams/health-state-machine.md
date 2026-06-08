# Health State Machine

## Node Health States

```mermaid
stateDiagram-v2
    [*] --> UNKNOWN: Boot

    UNKNOWN --> UP: MSG_NODE_ANNOUNCE received
    UP --> SUSPECT: 3 missed heartbeats (30ms)
    SUSPECT --> DOWN: 50ms no recovery + corroboration
    SUSPECT --> UP: Heartbeat received
    DOWN --> UP: Heartbeat received (node recovered)

    note right of SUSPECT
        Proposed to Raft as log entry.
        Committed when majority replicate.
        Observation sent to leader via
        AE response sideband.
    end note

    note right of DOWN
        Committed only after K=2
        independent observers agree.
        Triggers FAILURE_EVENT to
        oracle-agent for fencing.
    end note
```

## Timing Diagram

```mermaid
gantt
    title Failure Detection Timeline (worst case)
    dateFormat X
    axisFormat %Lms

    section Heartbeats
    Normal (10ms interval)     :done, 0, 30
    Missed beat 1              :crit, 30, 40
    Missed beat 2              :crit, 40, 50
    Missed beat 3              :crit, 50, 60

    section Detection
    SUSPECT (local)            :active, 60, 65
    Raft propose SUSPECT       :65, 70
    Raft commit SUSPECT        :70, 75

    section Corroboration
    Wait for recovery          :75, 110
    DOWN (local)               :crit, 110, 115
    Raft propose DOWN          :115, 120
    Raft commit DOWN           :120, 125

    section Fencing
    FAILURE_EVENT sent         :milestone, 125, 125
    oracle-agent receives      :125, 130
    kubectl cordon             :130, 200
```

## Decision Flow

```mermaid
flowchart TD
    A[Heartbeat Timer Fires<br/>every 10ms] --> B{Any node<br/>missed 3 beats?}
    B -->|No| A
    B -->|Yes| C[Mark SUSPECT locally]
    C --> D{Am I the<br/>Raft leader?}
    D -->|Yes| E[Propose SUSPECT<br/>via raft_recv_entry]
    D -->|No| F[Piggyback observation<br/>on next AE response]
    E --> G[Wait for Raft commit<br/>majority must replicate]
    F --> H[Leader receives<br/>observation]
    H --> I{K observers<br/>agree?}
    I -->|No| J[Wait for more<br/>observations]
    I -->|Yes| E
    G --> K[Apply to health table<br/>all nodes update]
    K --> L{Status = DOWN?}
    L -->|No| M[Continue monitoring<br/>50ms timeout for DOWN]
    L -->|Yes| N[Emit 0x88B6<br/>FAILURE_EVENT]
    N --> O[oracle-agent<br/>executes fencing]
    M --> P{Recovery<br/>within 50ms?}
    P -->|Yes| Q[Mark UP, cancel]
    P -->|No| R[Propose DOWN]
    R --> E
```

## What Gets Replicated vs Local

| State | Scope | Mechanism |
|-------|-------|-----------|
| Heartbeat timestamps | Local only | Per-node `last_seen_ms` in health_monitor |
| Miss counters | Local only | Per-node `miss_count` array |
| SUSPECT transition | **Raft-committed** | Proposed as log entry, all nodes apply |
| DOWN transition | **Raft-committed** | Requires corroboration before proposal |
| Health table | **Raft-committed** | Updated in `applylog` callback |
| Cluster state broadcast | Leader-originated | Sent every 1s to compute nodes |
