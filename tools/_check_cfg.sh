#!/bin/bash
# Show the init config that lives inside out/vendor.img (temporary helper).
cd "$(dirname "$0")/.." || exit 1
debugfs -R "stat /etc/init/hmos2sweet.cfg" out/vendor.img 2>/dev/null | head -22
echo "--- jobs ---"
debugfs -R "cat /etc/init/hmos2sweet.cfg" out/vendor.img 2>/dev/null > /tmp/cfg.check.json
python3 - <<'EOF'
import json
d = json.load(open('/tmp/cfg.check.json'))
for j in d['jobs']:
    print(f"{j['name']}: {j['cmds']}")
EOF
