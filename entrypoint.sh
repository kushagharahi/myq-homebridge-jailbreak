#!/bin/sh
set -e

MYQ_SERIAL="${MYQ_SERIAL:-2100001E7E}"
MYQ_PSK="${MYQ_PSK}"

if [ -z "$MYQ_PSK" ]; then
    echo "[-] ERROR: MYQ_PSK environment variable is required!"
    echo "    Usage example:"
    echo "      docker run -d -p 8883:8883 -e MYQ_SERIAL=2100001E7E -e MYQ_PSK=<extracted_aes_key> myq-broker"
    exit 1
fi

echo "=========================================================="
echo "  LiftMaster MyQ Local Mosquitto Cloud Broker"
echo "=========================================================="
echo "[*] Device Serial : ${MYQ_SERIAL}"
echo "[*] PSK Length    : ${#MYQ_PSK} characters"

# Create secure PSK credential file
echo "${MYQ_SERIAL}:${MYQ_PSK}" > /tmp/psk.txt
chmod 0600 /tmp/psk.txt
chown mosquitto:mosquitto /tmp/psk.txt 2>/dev/null || true

# Generate dynamic Mosquitto configuration
cat <<EOF > /tmp/mosquitto.conf
# Internal loopback listener for startup pre-seeding
listener 1883 127.0.0.1
allow_anonymous true

# External TLS-PSK listener for MyQ 819LMB bridge
listener 8883 0.0.0.0
psk_hint ${MYQ_SERIAL}
psk_file /tmp/psk.txt
ciphers PSK-AES128-CBC-SHA:@SECLEVEL=0
tls_version tlsv1.1
allow_anonymous true
EOF

echo "[+] Launching Mosquitto daemon..."
/usr/sbin/mosquitto -c /tmp/mosquitto.conf &
MOSQ_PID=$!

# Wait for local port 1883 to be ready to accept pre-seed pub
echo "[*] Waiting for Mosquitto internal listener on 127.0.0.1:1883..."
for i in $(seq 1 30); do
    if nc -z 127.0.0.1 1883 2>/dev/null; then
        break
    fi
    sleep 0.2
done

# Pre-seed retained message to disarm the 12-second firmware downlink watchdog
echo "[*] Pre-seeding retained message to topic G/${MYQ_SERIAL}..."
mosquitto_pub -h 127.0.0.1 -p 1883 -t "G/${MYQ_SERIAL}" -m "1" -r
echo "[+] Retained message pre-seeded successfully!"
echo "[+] MyQ local broker is ready on 0.0.0.0:8883 (TLS-PSK)."

# Forward termination signals gracefully
trap "echo '[*] Shutting down...'; kill -TERM $MOSQ_PID" TERM INT
wait $MOSQ_PID
