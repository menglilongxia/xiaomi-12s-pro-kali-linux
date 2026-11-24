#!/usr/bin/env bash
set -euo pipefail

CMD="${1:-status}"
Q="qmicli -d qrtr://0"

detect_apn() {
    local imsi=""
    imsi=$($Q --dms-get-ids 2>/dev/null | grep -i imsi || true)
    local mm_op=""
    mm_op=$(mmcli -m any 2>/dev/null | awk -F: '/operator id/ {gsub(/[^0-9]/,"",$2); print $2; exit}' || true)
    case "$mm_op" in
        46000|46002|46004|46007|46008) echo "cmnet" ;;
        46001|46006|46009)             echo "3gnet" ;;
        46003|46005|46011)             echo "ctnet" ;;
        *)                             echo "cmnet" ;;
    esac
}

case "$CMD" in
    status)
        echo "=== 5G RF Operating Mode ==="
        $Q --dms-get-operating-mode 2>/dev/null || true
        echo "=== Serving System ==="
        $Q --nas-get-serving-system 2>/dev/null || true
        echo "=== Signal Info ==="
        $Q --nas-get-signal-info 2>/dev/null || true
        echo "=== ModemManager ==="
        mmcli -m any 2>/dev/null | grep -E "manufacturer:|model:|equipment id:|own:|state:|power state:|access tech:|signal quality:|operator name:|operator id:|registration:" || true
        echo "=== NetworkManager ==="
        nmcli -f DEVICE,TYPE,STATE,CONNECTION dev status 2>/dev/null | grep -E "DEVICE|qrtr|wwan|rmnet" || true
        ;;
    online)
        $Q --dms-set-operating-mode=online
        $Q --nas-set-system-selection-preference="gsm|umts|lte|5gnr" || true
        mmcli -m any -e || true
        ;;
    scan)
        echo "Scanning live 4G/5G networks over the air (may take 15-40s)..."
        $Q --nas-network-scan
        ;;
    connect)
        APN="${2:-$(detect_apn)}"
        ROAM="${3:-yes}"
        echo "Bringing up 5G/LTE data connection (APN=${APN}, roaming=${ROAM})..."
        $Q --dms-set-operating-mode=online >/dev/null 2>&1 || true
        mmcli -m any -e >/dev/null 2>&1 || true
        nmcli con delete unicorn-5g >/dev/null 2>&1 || true
        HOME_ONLY="no"
        [ "$ROAM" = "no" ] && HOME_ONLY="yes"
        nmcli con add type gsm ifname qrtr0 con-name unicorn-5g apn "$APN" gsm.home-only "$HOME_ONLY"
        nmcli con up unicorn-5g
        ip -br addr show | grep -E "rmnet|wwan" || true
        ;;
    disconnect)
        nmcli con down unicorn-5g 2>/dev/null || true
        echo "Disconnected unicorn-5g."
        ;;
    *)
        echo "Usage: $0 {status|online|scan|connect [apn] [roaming:yes|no]|disconnect}"
        exit 1
        ;;
esac
