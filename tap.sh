#!/bin/bash
#
# tap.sh - Manage TAP device for Sun-2 emulator ethernet (3c400)
#
# Usage:
#   sudo ./tap.sh create [device] [bridge]
#   sudo ./tap.sh destroy [device]
#   ./tap.sh status [device]
#
# Default device: tap0
# The TAP device must be created before running the emulator with --ether=
#

TAP_DEV="${2:-tap0}"
BRIDGE="${3:-}"
# Use SUDO_USER if run via sudo, otherwise current user
TAP_USER="${SUDO_USER:-$(whoami)}"

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
    echo "Run the emulator with:  ./run.sh --ether=$TAP_DEV"
    echo ""
    echo "To assign an IP to the TAP interface:"
    echo "  sudo ip addr add 10.0.2.1/24 dev $TAP_DEV"
    echo ""
    echo "To route between TAP and your network (enable forwarding):"
    echo "  sudo sysctl -w net.ipv4.ip_forward=1"
    echo "  sudo iptables -t nat -A POSTROUTING -s 10.0.2.0/24 -j MASQUERADE"
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

        # Show packet counters
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
        echo "  create   Create a TAP device (requires root)"
        echo "  destroy  Remove a TAP device (requires root)"
        echo "  status   Show TAP device status"
        echo ""
        echo "Arguments:"
        echo "  device   TAP device name (default: tap0)"
        echo "  bridge   Optional bridge to attach TAP to (create only)"
        echo ""
        echo "Examples:"
        echo "  sudo $0 create                  # Create tap0"
        echo "  sudo $0 create tap0 br0         # Create tap0 and add to bridge br0"
        echo "  sudo $0 destroy tap0            # Remove tap0"
        echo "  $0 status tap0                  # Show tap0 status"
        echo ""
        echo "Typical workflow:"
        echo "  sudo ./tap.sh create tap0"
        echo "  sudo ip addr add 10.0.2.1/24 dev tap0"
        echo "  ./run.sh --ether=tap0"
        exit 1
        ;;
esac
