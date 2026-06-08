# Network Topology

## Hackathon Setup (Single Box)

```mermaid
graph TB
    subgraph "CRS326-24G-2S+ (Flat L2 Bridge)"
        direction LR
        P1[Port 1]
        P2[Port 2]
        P3[Port 3]
        P4[Port 4]
        P5[Port 5]
    end

    subgraph "Nucleo-F207ZG (Node 1)"
        STM1[STM32 Oracle<br/>02:CA:FE:01:00:01]
    end

    subgraph "Monitoring Laptop"
        SNIFF[frame_sniffer.py<br/>heartbeat_sender.py]
    end

    STM1 --- P2
    SNIFF --- P4

    style STM1 fill:#4a9,stroke:#333,color:#fff
    style SNIFF fill:#49a,stroke:#333,color:#fff
```

## Production Setup (3-Box Cluster)

```mermaid
graph TB
    subgraph "L2 Switch Fabric (CRS326 or ASIC backplane)"
        direction LR
        SW[Flat Bridge<br/>All ports same VLAN]
    end

    subgraph "Box 1"
        STM1[STM32 Node 1<br/>02:CA:FE:01:00:01]
        C1[Compute 101<br/>oracle-agent]
        C2[Compute 102<br/>oracle-agent]
    end

    subgraph "Box 2"
        STM2[STM32 Node 2<br/>02:CA:FE:02:00:02]
        C3[Compute 201<br/>oracle-agent]
        C4[Compute 202<br/>oracle-agent]
    end

    subgraph "Box 3"
        STM3[STM32 Node 3<br/>02:CA:FE:03:00:03]
        C5[Compute 301<br/>oracle-agent]
        C6[Compute 302<br/>oracle-agent]
    end

    STM1 --- SW
    STM2 --- SW
    STM3 --- SW
    C1 --- SW
    C2 --- SW
    C3 --- SW
    C4 --- SW
    C5 --- SW
    C6 --- SW

    C1 -.->|0x88B7 Heartbeat| STM1
    C2 -.->|0x88B7 Heartbeat| STM1
    STM1 ==>|0x88B5 Raft| STM2
    STM1 ==>|0x88B5 Raft| STM3
    STM1 -.->|0x88B6 Health| C1
    STM1 -.->|0x88B6 Health| C2

    style STM1 fill:#4a9,stroke:#333,color:#fff
    style STM2 fill:#4a9,stroke:#333,color:#fff
    style STM3 fill:#4a9,stroke:#333,color:#fff
    style C1 fill:#49a,stroke:#333,color:#fff
    style C2 fill:#49a,stroke:#333,color:#fff
    style C3 fill:#49a,stroke:#333,color:#fff
    style C4 fill:#49a,stroke:#333,color:#fff
    style C5 fill:#49a,stroke:#333,color:#fff
    style C6 fill:#49a,stroke:#333,color:#fff
```

## MAC Address Scheme

| Component | MAC Format | Example |
|-----------|-----------|---------|
| STM32 oracle node | `02:CA:FE:<box>:00:<node>` | `02:CA:FE:01:00:01` (box 1, node 1) |
| Compute node | `02:CA:FE:<box>:00:<id>` | `02:CA:FE:01:00:65` (box 1, node 101) |
| Broadcast | `FF:FF:FF:FF:FF:FF` | All heartbeats use broadcast |

The `02:` prefix indicates locally-administered unicast (IEEE bit 1 of first octet).

## Frame Routing

All oracle frames use **broadcast** destination MAC (`FF:FF:FF:FF:FF:FF`). On a flat L2 bridge:
- Every port receives every frame (no MAC learning needed)
- STM32 nodes filter by EtherType in software
- No configuration needed on the switch (just plug in)

This is intentional: it matches the production model where the service processor sees ALL management frames transiting the switch backplane via frame-extract. It also means adding/removing nodes requires zero switch configuration.
