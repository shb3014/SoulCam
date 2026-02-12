#!/bin/sh
i=0
while [ $i -lt 30 ]; do
  if [ -e /dev/video8 ]; then
    exec /usr/bin/v4l2-ctl -d /dev/video8 \
      --set-ctrl=auto_exposure=0 \
      --set-ctrl=gain_automatic=1 \
      --set-ctrl=white_balance_automatic=1
  fi
  i=$((i+1))
  sleep 1
done
exit 1
