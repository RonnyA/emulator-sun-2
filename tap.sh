#!/bin/bash
#
# tap.sh - Manage TAP device for Sun-2 emulator ethernet (3c400)
#
# The emulated 3c400 Multibus ethernet card connects to a Linux TAP
# device for layer-2 (raw ethernet frame) network access. The TAP
# device must be created before starting the emulator.
#
# Network setup overview:
#
#   Linux host                          Emulated Sun-2
#   ----------                          --------------
#   tap0: 10.0.2.1/24  <-- TAP -->  ec0: 10.0.2.2
#
#   The host and emulated Sun-2 share a point-to-point link via the
#   TAP device. The default MAC address of the emulated 3c400 is
#   08:00:20:01:06:e0.
#
# Quick start:
#
#   1. Create the TAP device (run once, before starting the emulator):
#
#      sudo ./tap.sh create tap0
#      sudo ip addr add 10.0.2.1/24 dev tap0
#
#   2. Start the emulator:
#
#      ./run.sh --ether=tap0
#
#   3. Inside SunOS (after boot), configure the network:
#
#      ifconfig ec0 10.0.2.2 up
#
#   4. Test connectivity:
#
#      From SunOS:  ping 10.0.2.1
#      From Linux:  ping 10.0.2.2
#
# Routing to the outside world (optional):
#
#   To let the Sun-2 reach hosts beyond the Linux machine, enable
#   IP forwarding and masquerading on the host:
#
#      sudo sysctl -w net.ipv4.ip_forward=1
#      sudo iptables -t nat -A POSTROUTING -s 10.0.2.0/24 -j MASQUERADE
#
#   Then set the default gateway inside SunOS:
#
#      route add default 10.0.2.1 1
#
# Persistent SunOS 3.2 network config:
#
#   To have SunOS configure the network at boot:
#
#     /etc/hosts          - add: 10.0.2.2  mysun
#     /etc/hostname.ec0   - contains: mysun
#
#   SunOS 3.2 runs /etc/rc.boot and /etc/rc.local at startup.
#   The hostname.ec0 file tells rc.boot to bring up ec0 using
#   the IP mapped to that hostname in /etc/hosts.
#
#   For a default route, add to /etc/rc.local:
#     route add default 10.0.2.1 1
#
# Cleanup:
#
#   sudo ./tap.sh destroy tap0
#
# Usage:
#   sudo ./tap.sh create [device] [bridge]
#   sudo ./tap.sh destroy [device]
#   ./tap.sh status [device]
#

TAP_DEV="${2:-tap0}"
BRIDGE="${3:-}"
TAP_USER="${SUDO_USER:-$(whoami)}"
TAP_IP="10.0.2.1/24"

create_tap() {
    if ip link show "$TAP_DEV" > /dev/null 2>&1; then
        echo "Error: $TAP_DEV already exists"
        echo "Use '$0 status $TAP_DEV' to check or '$0 destroy $TAP_DEV' to remove it"
        exit 1
    fi

    echo "Creating TAP device $TAP_DEV for user $TAP_USER"
    ip tuntap add dev "$TAP_DEV" mode tap user "$TAP_USER"
    if [ $? -ne 0 ]; then
        echo "Error: failed to create TAP device (are you root?)"
        exit 1
    fi

    echo "Assigning $TAP_IP to $TAP_DEV"
    ip addr add "$TAP_IP" dev "$TAP_DEV"
    ip link set "$TAP_DEV" up

    if [ -n "$BRIDGE" ]; then
        if ip link show "$BRIDGE" > /dev/null 2>&1; then
            echo "Adding $TAP_DEV to bridge $BRIDGE"
            ip link set "$TAP_DEV" master "$BRIDGE"
        else
            echo "Warning: bridge $BRIDGE does not exist, skipping"
        fi
    fi

    echo ""
    echo "TAP device $TAP_DEV is ready:"
    ip -brief link show "$TAP_DEV"
    ip -brief addr show "$TAP_DEV"
    echo ""
    echo "===== WHAT TO DO NEXT ====="
    echo ""
    echo "  Step 1: Start the emulator"
    echo "  -------------------------"
    echo "  ./run.sh --ether=$TAP_DEV"
    echo ""
    echo "  Step 2: Inside SunOS (at the # prompt), type:"
    echo "  -----------------------------------------------"
    echo "  ifconfig ec0 10.0.2.2 up"
    echo ""
    echo "  Step 3: Test that it works, type:"
    echo "  ----------------------------------"
    echo "  ping 10.0.2.1"
    echo ""
    echo "  You should see '10.0.2.1 is alive'"
    echo "  You can also ping the Sun from Linux: ping 10.0.2.2"
    echo ""
    echo "  Optional: route SunOS traffic to the internet"
    echo "  ----------------------------------------------"
    echo "  On Linux host:"
    echo "    sudo sysctl -w net.ipv4.ip_forward=1"
    echo "    sudo iptables -t nat -A POSTROUTING -s 10.0.2.0/24 -j MASQUERADE"
    echo "  Inside SunOS:"
    echo "    route add default 10.0.2.1 1"
    echo ""
    echo "  To make SunOS network config permanent (survives reboot):"
    echo "  ----------------------------------------------------------"
    echo "  Inside SunOS, add to /etc/hosts:"
    echo "    10.0.2.2  mysun"
    echo "  Create /etc/hostname.ec0 containing:"
    echo "    mysun"
    echo "============================="
}

destroy_tap() {
    if ! ip link show "$TAP_DEV" > /dev/null 2>&1; then
        echo "Error: $TAP_DEV does not exist"
        exit 1
    fi

    echo "Destroying TAP device $TAP_DEV"
    ip link set "$TAP_DEV" down 2>/dev/null
    ip tuntap del dev "$TAP_DEV" mode tap
    if [ $? -eq 0 ]; then
        echo "TAP device $TAP_DEV removed"
    else
        echo "Error: failed to remove $TAP_DEV (are you root?)"
        exit 1
    fi
}

status_tap() {
    if ip link show "$TAP_DEV" > /dev/null 2>&1; then
        echo "TAP device $TAP_DEV:"
        echo ""
        echo "Link:"
        ip -brief link show "$TAP_DEV"
        echo ""
        echo "Addresses:"
        ip -brief addr show "$TAP_DEV"
        echo ""
        echo "Details:"
        ip -d link show "$TAP_DEV"
        echo ""

        RX=$(cat /sys/class/net/"$TAP_DEV"/statistics/rx_packets 2>/dev/null)
        TX=$(cat /sys/class/net/"$TAP_DEV"/statistics/tx_packets 2>/dev/null)
        if [ -n "$RX" ]; then
            echo "Packets: RX=$RX TX=$TX"
        fi
    else
        echo "TAP device $TAP_DEV does not exist"
        exit 1
    fi
}

case "$1" in
    create)
        create_tap
        ;;
    destroy)
        destroy_tap
        ;;
    status)
        status_tap
        ;;
    *)
        echo "Usage: $0 {create|destroy|status} [device] [bridge]"
        echo ""
        echo "Manage TAP network device for the Sun-2 emulator ethernet."
        echo "The TAP device must exist before starting the emulator."
        echo ""
        echo "Commands:"
        echo "  create   Create TAP device and assign 10.0.2.1/24 (requires root)"
        echo "  destroy  Remove a TAP device (requires root)"
        echo "  status   Show TAP device status"
        echo ""
        echo "Arguments:"
        echo "  device   TAP device name (default: tap0)"
        echo "  bridge   Optional bridge to attach TAP to (create only)"
        echo ""
        echo "Examples:"
        echo "  sudo $0 create                  # Create tap0 with 10.0.2.1/24"
        echo "  sudo $0 create tap0 br0         # Create tap0 and add to bridge br0"
        echo "  sudo $0 destroy tap0            # Remove tap0"
        echo "  $0 status tap0                  # Show tap0 status"
        echo ""
        echo "Quick start:"
        echo "  sudo ./tap.sh create tap0"
        echo "  ./run.sh --ether=tap0"
        echo "  # Inside SunOS: ifconfig ec0 10.0.2.2 up"
        echo "  # Inside SunOS: ping 10.0.2.1"
        exit 1
        ;;
esac
