#!/bin/bash
# Register the test subscriber in Open5GS via MongoDB
# IMSI: 001010000000001 | K: 00112233445566778899AABBCCDDEEFF | OPc: 63BFA50EE6523365FF14C1F45F88737D

set -e

echo "=== Registering subscriber in Open5GS ==="

# Wait for MongoDB to be ready
until mongosh --quiet --eval "db.runCommand({ping:1})" > /dev/null 2>&1; do
    echo "Waiting for MongoDB..."
    sleep 1
done

mongosh open5gs --quiet --eval '
var sub = db.subscribers.findOne({"imsi": "001010000000001"});
if (sub) {
    print("Subscriber already exists, skipping.");
} else {
    db.subscribers.insertOne({
        "imsi": "001010000000001",
        "msisdn": [],
        "imeisv": "4370816125816151",
        "mme_host": [],
        "mme_realm": [],
        "purge_flag": [],
        "security": {
            "k": "00112233445566778899AABBCCDDEEFF",
            "op": null,
            "opc": "63BFA50EE6523365FF14C1F45F88737D",
            "amf": "8000",
            "sqn": NumberLong("1")
        },
        "ambr": {
            "downlink": {"value": 1, "unit": 3},
            "uplink": {"value": 1, "unit": 3}
        },
        "slice": [{
            "sst": 1,
            "default_indicator": true,
            "session": [{
                "name": "internet",
                "type": 1,
                "qos": {"index": 9, "arp": {"priority_level": 8, "pre_emption_capability": 1, "pre_emption_vulnerability": 1}},
                "ambr": {
                    "downlink": {"value": 1, "unit": 3},
                    "uplink": {"value": 1, "unit": 3}
                },
                "ue": {"addr": "10.45.0.2"},
                "pcc_rule": []
            }]
        }],
        "access_restriction_data": 32,
        "subscriber_status": 0,
        "network_access_mode": 0,
        "subscribed_rau_tau_timer": 12,
        "__v": 0
    });
    print("Subscriber registered successfully.");
}
'
echo "=== Done ==="
