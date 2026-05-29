#!/usr/bin/env bash
set -u

SRC_NAME="MPK mini 3"
DST_NAME="CVpal"

find_port() {
  local target="$1"
  aconnect -l | awk -v target="$target" '
    /^client / {
      client=$2
      gsub(":", "", client)
      line=$0
      next
    }
    /^[[:space:]]+[0-9]+[[:space:]]/ {
      port=$1
      if (line ~ target) {
        print client ":" port
        exit
      }
    }
  '
}

is_connected() {
  local src_client="$1"
  local dst_client="$2"
  aconnect -l | awk -v src="$src_client" -v dst="$dst_client" '
    $0 ~ "^client " src ":" { in_src=1; next }
    in_src && $0 ~ "^client " { in_src=0 }
    in_src && $0 ~ "Connecting To:.*" dst ":0" { found=1 }
    END { exit(found ? 0 : 1) }
  '
}

while true; do
  SRC_PORT="$(find_port "$SRC_NAME")"
  DST_PORT="$(find_port "$DST_NAME")"

  if [[ -n "$SRC_PORT" && -n "$DST_PORT" ]]; then
    SRC_CLIENT="${SRC_PORT%%:*}"
    DST_CLIENT="${DST_PORT%%:*}"

    if ! is_connected "$SRC_CLIENT" "$DST_CLIENT"; then
      echo "Connecting $SRC_PORT -> $DST_PORT"
      aconnect "$SRC_PORT" "$DST_PORT"
    fi
  fi

  sleep 1
done