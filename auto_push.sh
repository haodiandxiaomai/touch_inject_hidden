#!/bin/bash
cd /root/.openclaw/workspace/touch_inject_hidden
for i in $(seq 1 60); do
  git push origin main 2>/dev/null && echo "OK" && exit 0
  sleep 60
done
echo "FAIL: 60 次重试均失败"
