#!/bin/bash
# pmtu-lab.sh up|down  -- a two-namespace path with a 1400-byte hop, on the host's LAN
# host(ens18 192.168.1.136) --pmh0 10.9.1.1/24 [mtu 1400]-- pmr0 10.9.1.2 [R] pmr1 10.9.2.1 --[1500]-- pms0 10.9.2.2 [S]
set -e
case "${1:-up}" in
  up)
    ip netns add pmtuR 2>/dev/null || true
    ip netns add pmtuS 2>/dev/null || true
    ip link add pmh0 type veth peer name pmr0 2>/dev/null || true
    ip link set pmr0 netns pmtuR
    ip link add pmr1 type veth peer name pms0 2>/dev/null || true
    ip link set pmr1 netns pmtuR; ip link set pms0 netns pmtuS
    ip addr add 10.9.1.1/24 dev pmh0 2>/dev/null || true
    ip link set pmh0 mtu 1400 up
    ip netns exec pmtuR ip addr add 10.9.1.2/24 dev pmr0 2>/dev/null || true
    ip netns exec pmtuR ip link set pmr0 mtu 1400 up
    ip netns exec pmtuR ip addr add 10.9.2.1/24 dev pmr1 2>/dev/null || true
    ip netns exec pmtuR ip link set pmr1 up
    ip netns exec pmtuR ip link set lo up
    ip netns exec pmtuR sh -c "echo 1 > /proc/sys/net/ipv4/ip_forward"
    ip netns exec pmtuR ip route add default via 10.9.1.1 2>/dev/null || true
    ip netns exec pmtuS ip addr add 10.9.2.2/24 dev pms0 2>/dev/null || true
    ip netns exec pmtuS ip link set pms0 up
    ip netns exec pmtuS ip link set lo up
    ip netns exec pmtuS ip route add default via 10.9.2.1 2>/dev/null || true
    ip route add 10.9.2.0/24 via 10.9.1.2 2>/dev/null || true
    echo 1 > /proc/sys/net/ipv4/ip_forward
    for i in ens18 pmh0 all; do echo 2 > /proc/sys/net/ipv4/conf/$i/rp_filter; done
    echo "lab up"; ip -br addr show pmh0; ip netns exec pmtuR ip -br addr; ip netns exec pmtuS ip -br addr
    ;;
  down)
    ip netns del pmtuS 2>/dev/null || true
    ip netns del pmtuR 2>/dev/null || true
    ip link del pmh0 2>/dev/null || true
    ip route del 10.9.2.0/24 2>/dev/null || true
    echo "lab down"
    ;;
esac
