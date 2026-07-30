#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
netmonitor="$root_dir/buildroot/package/thirdreality/tr-proj-ha-speaker/script/netmonitor"

NETMONITOR_LIBRARY_ONLY=1
# shellcheck disable=SC1090
. "$netmonitor"

restart_count=0
restart_dhcp_client() {
    restart_count=$((restart_count + 1))
}

LAST_DHCP_REPAIR=0
repair_link_configuration 100
[ "$restart_count" -eq 1 ]
[ "$LAST_DHCP_REPAIR" -eq 100 ]

repair_link_configuration 110
[ "$restart_count" -eq 1 ]
[ "$LAST_DHCP_REPAIR" -eq 100 ]

repair_link_configuration 115
[ "$restart_count" -eq 2 ]
[ "$LAST_DHCP_REPAIR" -eq 115 ]

echo "netmonitor DHCP repair tests passed"
